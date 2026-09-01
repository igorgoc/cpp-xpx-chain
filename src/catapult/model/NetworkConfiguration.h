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

#pragma once
#include "NetworkInfo.h"
#include "PluginConfiguration.h"
#include "BlockTimeUpdateStrategy.h"
#include "catapult/dbrb/DbrbDefinitions.h"
#include "catapult/utils/ConfigurationBag.h"
#include "catapult/utils/FileSize.h"
#include "catapult/utils/MemoryUtils.h"
#include "catapult/utils/Hashers.h"
#include "catapult/utils/TimeSpan.h"
#include "catapult/exceptions.h"
#include "catapult/types.h"
#include <unordered_map>
#include <cstdint>

namespace catapult { namespace model {

	struct NetworkConfiguration;

	using NetworkConfigurations = std::map<Height, NetworkConfiguration>;

	/// Network configuration settings.
	struct NetworkConfiguration {
	public:
		/// Block chain network info.
		NetworkInfo Info;

		/// Targeted time between blocks.
		utils::TimeSpan BlockGenerationTargetTime;

		/// Smoothing factor in thousandths.
		/// If this value is non-zero, the network will be biased in favor of evenly spaced blocks.
		/// \note A higher value makes the network more biased.
		/// \note This can lower security because it will increase the influence of time relative to importance.
		uint32_t BlockTimeSmoothingFactor;

		/// Greed smoothing parameter.
		double GreedDelta;

		/// Greed exponent parameter.
		double GreedExponent;

		/// Active inflation amount
		Amount Inflation;

		/// Number of blocks that should be treated as a group for importance purposes.
		/// \note Importances will only be calculated at blocks that are multiples of this grouping number.
		uint64_t ImportanceGrouping;

		/// Maximum number of blocks that can be rolled back.
		uint32_t MaxRollbackBlocks;

		/// Maximum number of blocks to use in a difficulty calculation.
		uint32_t MaxDifficultyBlocks;

		/// Maximum lifetime a transaction can have before it expires.
		utils::TimeSpan MaxTransactionLifetime;

		/// Maximum future time of a block that can be accepted.
		utils::TimeSpan MaxBlockFutureTime;

		/// Maximum atomic units (total-supply * 10 ^ divisibility) of a mosaic allowed in the network.
		Amount MaxMosaicAtomicUnits;

		/// Total whole importance units available in the network.
		Importance TotalChainImportance;

		/// Minimum number of harvesting mosaic atomic units needed for an account to be eligible for harvesting.
		Amount MinHarvesterBalance;

		/// Percentage of the harvested fee that is collected by the beneficiary account.
		uint8_t HarvestBeneficiaryPercentage;

		/// Number of blocks between cache pruning.
		uint32_t BlockPruneInterval;

		/// Maximum number of transactions per block.
		uint32_t MaxTransactionsPerBlock;

		/// Allows validation of unconfirmed transactions before including it into UT cache
		/// that transaction fee is not less than the minimum calculated with minFeeMultiplier.
		bool EnableUnconfirmedTransactionMinFeeValidation;

		/// Allows validate deadline of transaction. It means that you can send old transactions to blockchain if it is false.
		bool EnableDeadlineValidation;

		/// Allows block rollback.
		bool EnableUndoBlock;

		/// Allows block synchronization using block synchronizer.
		bool EnableBlockSync;

		/// Allows weighted voting consensus.
		bool EnableWeightedVoting;

		/// Number of nodes in committee.
		uint32_t CommitteeSize;

		/// Sum of positive vote weights compared to total sum required for committee approval.
		double CommitteeApproval;

		/// Time per each committee phase.
		utils::TimeSpan CommitteePhaseTime;

		/// Minimal time per each committee phase.
		utils::TimeSpan MinCommitteePhaseTime;

		/// Maximum time per each committee phase.
		utils::TimeSpan MaxCommitteePhaseTime;

		/// Time interval at the end of committee phase without message requests.
		utils::TimeSpan CommitteeSilenceInterval;

		/// Time interval between committee message requests.
		utils::TimeSpan CommitteeRequestInterval;

		/// Time interval between committee chain height requests.
		utils::TimeSpan CommitteeChainHeightRequestInterval;

		/// Time adjustment after each committee round.
		double CommitteeTimeAdjustment;

		/// Sum of positive vote weights of running nodes compared to total sum of all polled nodes
		/// required for synchronization approval.
		double CommitteeEndSyncApproval;

		/// Amount of importance added to the node's importance during the approval stage of node synchronization.
		uint64_t CommitteeBaseTotalImportance;

		/// Weight of the node in synchronization state during the approval stage of node synchronization.
		double CommitteeNotRunningContribution;

		/// Time period after registration when a network node is allowed to be a DBRB process.
		utils::TimeSpan DbrbRegistrationDuration;

		/// Grace period during which time a network node is expected to prolong its registration as a DBRB process.
		utils::TimeSpan DbrbRegistrationGracePeriod;

		/// Public keys of the DBRB bootstrap processes.
		dbrb::ViewData DbrbBootstrapProcesses;

		/// Allows expiration of harvesters.
		bool EnableHarvesterExpiration;

		/// Allows removing DBRB process on node shutdown.
		bool EnableRemovingDbrbProcessOnShutdown;

		/// Special group of harvesters that never expire.
		std::set<Key> EmergencyHarvesters;

		/// Special group of harvesters that never expire mapped to bootstrap keys of the nodes the harvesters are set up on.
		std::unordered_map<Key, Key, utils::ArrayHasher<Key>> BootstrapHarvesters;

		/// Unparsed map of plugin configuration bags.
		std::unordered_map<std::string, utils::ConfigurationBag> Plugins;

		/// Enables DBRB sharding.
		bool EnableDbrbSharding;

		/// DBRB shard size.
		uint32_t DbrbShardSize;

		/// Allows block confirmation using DBRB protocol without Weighted Voting.
		bool EnableDbrbFastFinality;

		/// Number of failed rounds of block generation before checking the network height and synchronizing if necessary.
		uint32_t CheckNetworkHeightInterval;

		/// Block time update strategy.
		model::BlockTimeUpdateStrategy BlockTimeUpdateStrategy;

		/// The size of the queue of harvesters.
		uint8_t HarvestersQueueSize;

		/// Allows round time calculation improvement in committee manager.
		bool EnableCommitteeManagerImprovement;

	private:
		/// Map of plugin configurations.
		mutable std::array<std::shared_ptr<PluginConfiguration>, size_t(config::ConfigId::Latest) + 1> m_pluginConfigs;

	private:
		NetworkConfiguration() = default;

	public:
		/// Creates an uninitialized block chain configuration.
		static NetworkConfiguration Uninitialized();

		/// Loads a block chain configuration from \a bag.
		static NetworkConfiguration LoadFromBag(const utils::ConfigurationBag& bag);

		/// Loads plugin configuration for plugin.
		template<typename T>
		T LoadPluginConfiguration() const {
			auto iter = Plugins.find(T::Name);
			if (Plugins.cend() == iter) {
				CATAPULT_LOG(info) << "can't load plugin " << T::Name;
				return T::Uninitialized();
			}

			return T::LoadFromBag(iter->second);
		}

		/// Sets \a config of plugin.
		template<typename T>
		void SetPluginConfiguration(const T& config) {
			if (T::Id >= m_pluginConfigs.size())
				CATAPULT_THROW_AND_LOG_1(catapult_invalid_argument, "plugin has wrong Id", std::string(T::Name));

			m_pluginConfigs[T::Id] = std::make_shared<T>(config);
		}

		/// Inits config of plugin with \a pluginNameHash.
		template<typename T>
		void InitPluginConfiguration() {
			SetPluginConfiguration<T>(LoadPluginConfiguration<T>());
		}

		/// Returns plugin configuration for plugin with \a pluginNameHash.
		template<typename T>
		const T& GetPluginConfiguration() const {
			if (T::Id >= m_pluginConfigs.size() || !m_pluginConfigs[T::Id])
				CATAPULT_THROW_AND_LOG_1(catapult_invalid_argument, "plugin configuration not found", std::string(T::Name));

			return *static_cast<const T*>(m_pluginConfigs[T::Id].get());
		}

		/// Removes all plugin configs.
		void ClearPluginConfigurations() const {
			for (auto i = 0u; i < m_pluginConfigs.size(); ++i)
				m_pluginConfigs[i] = nullptr;
		}
	};

	/// Calculates the duration of a full rollback for the block chain described by \a config.
	utils::TimeSpan CalculateFullRollbackDuration(const NetworkConfiguration& config);

	/// Calculates the duration of the rollback variability buffer for the block chain described by \a config.
	utils::TimeSpan CalculateRollbackVariabilityBufferDuration(const NetworkConfiguration& config);

	/// Calculates the duration of time that expired transactions should be cached for the block chain described by \a config.
	utils::TimeSpan CalculateTransactionCacheDuration(const NetworkConfiguration& config);

	/// Calculates the number of historical difficulties to cache in memory for the block chain described by \a config.
	uint64_t CalculateDifficultyHistorySize(const NetworkConfiguration& config);

	/// Loads plugin configuration for plugin named \a pluginName from \a config.
	template<typename T>
	T LoadPluginConfiguration(const NetworkConfiguration& config) {
		return config.LoadPluginConfiguration<T>();
	}
}}
