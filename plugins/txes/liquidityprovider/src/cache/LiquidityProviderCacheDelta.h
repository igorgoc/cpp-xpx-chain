/**
*** Copyright 2021 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#pragma once
#include "LiquidityProviderKeyCollector.h"
#include "LiquidityProviderBaseSets.h"
#include "catapult/cache/CacheMixinAliases.h"
#include "catapult/cache/ReadOnlyArtifactCache.h"
#include "catapult/cache/ReadOnlyViewSupplier.h"
#include "catapult/config_holder/BlockchainConfigurationHolder.h"
#include "catapult/deltaset/BaseSetDelta.h"
#include "src/config/LiquidityProviderConfiguration.h"

namespace catapult { namespace cache {

	/// Mixins used by the LiquidityProvider cache delta.
	using LiquidityProviderCacheDeltaMixins = PatriciaTreeCacheMixins<
			LiquidityProviderCacheTypes::PrimaryTypes::BaseSetDeltaType, LiquidityProviderCacheDescriptor>;

	/// Basic delta on top of the LiquidityProvider cache.
	class BasicLiquidityProviderCacheDelta
			: public utils::MoveOnly
			, public LiquidityProviderCacheDeltaMixins::Size
			, public LiquidityProviderCacheDeltaMixins::Contains
			, public LiquidityProviderCacheDeltaMixins::ConstAccessor
			, public LiquidityProviderCacheDeltaMixins::MutableAccessor
			, public LiquidityProviderCacheDeltaMixins::PatriciaTreeDelta
			, public LiquidityProviderCacheDeltaMixins::BasicInsertRemove
			, public LiquidityProviderCacheDeltaMixins::DeltaElements
			, public LiquidityProviderCacheDeltaMixins::ConfigBasedEnable<config::LiquidityProviderConfiguration> {
	public:
		using ReadOnlyView = LiquidityProviderCacheTypes::CacheReadOnlyType;

	public:
		/// Creates a delta around \a LiquidityProviderSets.
		explicit BasicLiquidityProviderCacheDelta(
			const LiquidityProviderCacheTypes::BaseSetDeltaPointers& LiquidityProviderSets,
			std::shared_ptr<config::BlockchainConfigurationHolder> pConfigHolder)
				: LiquidityProviderCacheDeltaMixins::Size(*LiquidityProviderSets.pPrimary)
				, LiquidityProviderCacheDeltaMixins::Contains(*LiquidityProviderSets.pPrimary)
				, LiquidityProviderCacheDeltaMixins::ConstAccessor(*LiquidityProviderSets.pPrimary)
				, LiquidityProviderCacheDeltaMixins::MutableAccessor(*LiquidityProviderSets.pPrimary)
				, LiquidityProviderCacheDeltaMixins::PatriciaTreeDelta(*LiquidityProviderSets.pPrimary, LiquidityProviderSets.pPatriciaTree)
				, LiquidityProviderCacheDeltaMixins::BasicInsertRemove(*LiquidityProviderSets.pPrimary)
				, LiquidityProviderCacheDeltaMixins::DeltaElements(*LiquidityProviderSets.pPrimary)
				, LiquidityProviderCacheDeltaMixins::ConfigBasedEnable<config::LiquidityProviderConfiguration>(
					pConfigHolder, [](const auto& config) { return config.Enabled; })
				, m_pLiquidityProviderEntries(LiquidityProviderSets.pPrimary)
				, m_pDeltaKeys(LiquidityProviderSets.pDeltaKeys)
				, m_PrimaryKeys(LiquidityProviderSets.PrimaryKeys)
		{}

	public:
		using LiquidityProviderCacheDeltaMixins::ConstAccessor::find;
		using LiquidityProviderCacheDeltaMixins::MutableAccessor::find;

	public:
		/// Inserts the LiquidityProvider \a entry into the cache.
		void insert(const state::LiquidityProviderEntry& entry) {
			LiquidityProviderCacheDeltaMixins::BasicInsertRemove::insert(entry);
			insertKey(entry.mosaicId());
		}

		/// Inserts the \a key into the cache.
		void insertKey(const UnresolvedMosaicId& key) {
			if (!m_pDeltaKeys->contains(key))
				m_pDeltaKeys->insert(key);
		}

		/// Removes the LiquidityProvider \a entry into the cache.
		void remove(const UnresolvedMosaicId& key) {
			LiquidityProviderCacheDeltaMixins::BasicInsertRemove::remove(key);
			if (m_pDeltaKeys->contains(key))
				m_pDeltaKeys->remove(key);
		}

		/// Returns keys available after commit
		auto keys() const {
			std::unordered_set<UnresolvedMosaicId, utils::BaseValueHasher<UnresolvedMosaicId>> result;
			for (const auto& key : deltaset::MakeIterableView(m_PrimaryKeys)) {
				result.insert(key);
			}

			auto deltas = m_pDeltaKeys->deltas();

			for (const auto& key : deltas.Added) {
				result.insert(key);
			}

			for (const auto& key : deltas.Removed) {
				result.erase(key);
			}

			return result;
		}

		void updateKeyCollector(const std::shared_ptr<LiquidityProviderKeyCollector>& pKeyCollector) const {
			if (!pKeyCollector)
				return;
			pKeyCollector->keys().clear();
			for (const auto& key : keys()) {
				auto iter = m_pLiquidityProviderEntries->find(key);
				if (auto pEntry = iter.get())
					pKeyCollector->addKey(*pEntry);
			}
		}

	private:
		LiquidityProviderCacheTypes::PrimaryTypes::BaseSetDeltaPointerType m_pLiquidityProviderEntries;
		LiquidityProviderCacheTypes::KeyTypes::BaseSetDeltaPointerType m_pDeltaKeys;
		const LiquidityProviderCacheTypes::KeyTypes::BaseSetType& m_PrimaryKeys;
	};

	/// Delta on top of the LiquidityProvider cache.
	class LiquidityProviderCacheDelta : public ReadOnlyViewSupplier<BasicLiquidityProviderCacheDelta> {
	public:
		/// Creates a delta around \a LiquidityProviderSets.
		explicit LiquidityProviderCacheDelta(
			const LiquidityProviderCacheTypes::BaseSetDeltaPointers& LiquidityProviderSets,
			std::shared_ptr<config::BlockchainConfigurationHolder> pConfigHolder)
				: ReadOnlyViewSupplier(LiquidityProviderSets, pConfigHolder)
		{}
	};
}}
