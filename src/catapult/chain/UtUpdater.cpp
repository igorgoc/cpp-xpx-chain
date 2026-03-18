/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#include "UtUpdater.h"
#include "ChainResults.h"
#include "ProcessingNotificationSubscriber.h"
#include "catapult/cache/CatapultCache.h"
#include "catapult/cache/RelockableDetachedCatapultCache.h"
#include "catapult/cache_tx/UtCache.h"
#include "catapult/model/FeeUtils.h"

namespace catapult { namespace chain {

	namespace {

		struct FailureInfo {
			const model::Transaction& Transaction;
			const Hash256& Hash;
			Height EffectiveHeight;
			validators::ValidationResult Result;
		};

		struct ApplyState {
			constexpr ApplyState(cache::UtCacheModifierProxy& modifier,
					cache::CatapultCacheDelta& unconfirmedCatapultCache, std::vector<FailureInfo>& failureTransactions)
					: Modifier(modifier)
					, UnconfirmedCatapultCache(unconfirmedCatapultCache)
					, FailureTransactions(failureTransactions)
			{}

			cache::UtCacheModifierProxy& Modifier;
			cache::CatapultCacheDelta& UnconfirmedCatapultCache;
			std::vector<FailureInfo>& FailureTransactions;
		};
	}

	class UtUpdater::Impl final {
	public:
		Impl(
				cache::UtCache& transactionsCache,
				const cache::CatapultCache& confirmedCatapultCache,
				const ExecutionConfiguration& executionConfig,
				const TimeSupplier& timeSupplier,
				const FailedTransactionSink& failedTransactionSink,
				const Throttle& throttle)
				: m_transactionsCache(transactionsCache)
				, m_detachedCatapultCache(confirmedCatapultCache)
				, m_executionConfig(executionConfig)
				, m_timeSupplier(timeSupplier)
				, m_failedTransactionSink(failedTransactionSink)
				, m_throttle(throttle)
		{}

	public:
		std::vector<UtUpdateResult> update(const std::vector<model::TransactionInfo>& utInfos) {
			std::vector<FailureInfo> failureTransactions;
			std::vector<UtUpdateResult> results;
			results.resize(utInfos.size());

			{
				// 1. lock the UT cache and lock the unconfirmed copy
				auto modifier = m_transactionsCache.modifier();
				auto pUnconfirmedCatapultCache = m_detachedCatapultCache.getAndTryLock();
				if (!pUnconfirmedCatapultCache) {
					// if there is no unconfirmed cache state, it means that a block update is forthcoming
					// add non-expired transactions to the cache; they will be fully validated later
					auto currentTime = m_timeSupplier();
					addAll(modifier, utInfos, currentTime);
					for (auto i = 0u; i < utInfos.size(); ++i) {
						results[i].Type = UtUpdateResult::UpdateType::Neutral;
					}
					return results;
				}

				auto applyState = ApplyState(modifier, *pUnconfirmedCatapultCache, failureTransactions);
				apply(applyState, utInfos, TransactionSource::New);
			}

			for (const auto& info : failureTransactions) {
				m_failedTransactionSink(info.Transaction, info.EffectiveHeight, info.Hash, info.Result);
			}
			int j = 0;
			for (auto i = 0u; i < utInfos.size() && j < failureTransactions.size(); ++i) {
				if (utInfos[i].EntityHash == failureTransactions[j].Hash) {
					results[i].Type = UtUpdateResult::UpdateType::Invalid;
					++j;
				}
			}

			return results;
		}

		void update(const utils::HashPointerSet& confirmedTransactionHashes, const std::vector<model::TransactionInfo>& utInfos) {
			if (!confirmedTransactionHashes.empty() || !utInfos.empty()) {
				CATAPULT_LOG(debug)
						<< "confirmed " << confirmedTransactionHashes.size() << " transactions, "
						<< "reverted " << utInfos.size() << " transactions";
			}

			// 1. lock and clear the UT cache - UT cache must be locked before catapult cache to prevent race condition whereby
			//    other update overload applies transactions to rebased cache before UT lock is held
			auto modifier = m_transactionsCache.modifier();
			auto originalTransactionInfos = modifier.removeAll();
			std::vector<FailureInfo> failureTransactions;

			{
				// 2. lock the catapult cache and rebase the unconfirmed catapult cache
				auto pUnconfirmedCatapultCache = m_detachedCatapultCache.rebaseAndLock();

				// 3. add back reverted txes
				auto applyState = ApplyState(modifier, *pUnconfirmedCatapultCache, failureTransactions);
				apply(applyState, utInfos, TransactionSource::Reverted);

				// 4. add back original txes that have not been confirmed
				apply(applyState, originalTransactionInfos, TransactionSource::Existing, [&confirmedTransactionHashes](const auto& info) {
				  return confirmedTransactionHashes.cend() == confirmedTransactionHashes.find(&info.EntityHash);
				});
			}

			// 5. Notify about failed transactions
			for (const auto& info : failureTransactions) {
				m_failedTransactionSink(info.Transaction, info.EffectiveHeight, info.Hash, info.Result);
			}
		}

	private:
		void apply(const ApplyState& applyState, const std::vector<model::TransactionInfo>& utInfos, TransactionSource transactionSource) {
			apply(applyState, utInfos, transactionSource, [](const auto&) { return true; });
		}

		void apply(
				const ApplyState& applyState,
				const std::vector<model::TransactionInfo>& utInfos,
				TransactionSource transactionSource,
				const predicate<const model::TransactionInfo&>& filter) {
			using validators::ValidatorContext;
			using observers::ObserverContext;

			auto currentTime = m_timeSupplier();

			auto readOnlyCache = applyState.UnconfirmedCatapultCache.toReadOnly();

			// note that the validator and observer context height is one larger than the chain height
			// since the validation and observation has to be for the *next* block
			auto effectiveHeight = m_detachedCatapultCache.height() + Height(1);
			auto resolverContext = m_executionConfig.ResolverContextFactory(readOnlyCache);
			const auto& config = m_executionConfig.ConfigSupplier(effectiveHeight);
			auto validatorContext = ValidatorContext(config, effectiveHeight, currentTime, resolverContext, readOnlyCache);

			// note that the "real" state is currently only required by block observers, so a dummy state can be used
			auto& cache = applyState.UnconfirmedCatapultCache;
			state::CatapultState dummyState;
			std::vector<std::unique_ptr<model::Notification>> notifications;
			observers::ObserverState observerState{ cache, dummyState, notifications };
			auto observerContext = ObserverContext(observerState, config, effectiveHeight, currentTime, observers::NotifyMode::Commit, resolverContext);
			for (const auto& utInfo : utInfos) {
				const auto& entity = *utInfo.pEntity;
				const auto& entityHash = utInfo.EntityHash;

				if (entity.Deadline <= currentTime) {
					CATAPULT_LOG(warning) << "dropping transaction " << entityHash << " " << entity.Type << " due to expiration";
					applyState.FailureTransactions.emplace_back(FailureInfo{ entity, entityHash, effectiveHeight, Failure_Chain_Transaction_Expired });
					continue;
				}

				if (!filter(utInfo))
					continue;

				auto minTransactionFee = m_executionConfig.pTransactionFeeCalculator->calculateTransactionFee(
					config.Node.MinFeeMultiplier,
					entity,
					config.Node.FeeInterest,
					config.Node.FeeInterestDenominator,
					Height(-1));
				if (entity.MaxFee < minTransactionFee) {
					// don't log reverted transactions that could have been included by harvester with lower min fee multiplier
					if (TransactionSource::New == transactionSource) {
						CATAPULT_LOG(debug)
								<< "dropping transaction " << entityHash << " " << entity.Type << " with max fee " << entity.MaxFee
								<< " because min fee is " << minTransactionFee;
					}

					continue;
				}

				if (throttle(utInfo, transactionSource, applyState, readOnlyCache)) {
					CATAPULT_LOG(warning) << "dropping transaction " << entityHash << " " << entity.Type << " due to throttle";
					applyState.FailureTransactions.emplace_back(FailureInfo{ entity, entityHash, effectiveHeight, Failure_Chain_Unconfirmed_Cache_Too_Full });
					continue;
				}

				if (!applyState.Modifier.add(utInfo))
					continue;

				// notice that subscriber is created within loop because aggregate result needs to be reset each iteration
				const auto& validator = *m_executionConfig.pValidator;
				const auto& observer = *m_executionConfig.pObserver;
				ProcessingNotificationSubscriber sub(validator, validatorContext, observer, observerContext);
				model::WeakEntityInfo entityInfo(entity, entityHash, effectiveHeight);
				cache.backupChanges(true);
				m_executionConfig.pNotificationPublisher->publish(entityInfo, sub);
				if (!IsValidationResultSuccess(sub.result())) {
					CATAPULT_LOG_LEVEL(validators::MapToLogLevel(sub.result()))
							<< "dropping transaction " << entityHash << ": " << sub.result();

					// only forward failure (not neutral) results
					if (IsValidationResultFailure(sub.result()))
						applyState.FailureTransactions.emplace_back(FailureInfo{ entity, entityHash, effectiveHeight, sub.result() });

					cache.restoreChanges();
					applyState.Modifier.remove(entityHash);
					continue;
				}
			}
		}

		bool throttle(
				const model::TransactionInfo& utInfo,
				TransactionSource transactionSource,
				const ApplyState& applyState,
				cache::ReadOnlyCatapultCache& cache) const {
			return m_throttle(utInfo, { transactionSource, m_detachedCatapultCache.height(), cache, applyState.Modifier });
		}

		void addAll(cache::UtCacheModifierProxy& modifier, const std::vector<model::TransactionInfo>& utInfos, Timestamp currentTime) {
			for (const auto& utInfo : utInfos) {
				if (utInfo.pEntity->Deadline <= currentTime)
					continue;

				modifier.add(utInfo);
			}
		}

	private:
		cache::UtCache& m_transactionsCache;
		cache::RelockableDetachedCatapultCache m_detachedCatapultCache;
		ExecutionConfiguration m_executionConfig;
		TimeSupplier m_timeSupplier;
		FailedTransactionSink m_failedTransactionSink;
		UtUpdater::Throttle m_throttle;
	};

	UtUpdater::UtUpdater(
			cache::UtCache& transactionsCache,
			const cache::CatapultCache& confirmedCatapultCache,
			const ExecutionConfiguration& executionConfig,
			const TimeSupplier& timeSupplier,
			const FailedTransactionSink& failedTransactionSink,
			const Throttle& throttle)
			: m_pImpl(std::make_unique<Impl>(
					transactionsCache,
					confirmedCatapultCache,
					executionConfig,
					timeSupplier,
					failedTransactionSink,
					throttle))
	{}

	UtUpdater::~UtUpdater() = default;

	std::vector<UtUpdateResult> UtUpdater::update(const std::vector<model::TransactionInfo>& utInfos) {
		return m_pImpl->update(utInfos);
	}

	void UtUpdater::update(const utils::HashPointerSet& confirmedTransactionHashes, const std::vector<model::TransactionInfo>& utInfos) {
		m_pImpl->update(confirmedTransactionHashes, utInfos);
	}
}}
