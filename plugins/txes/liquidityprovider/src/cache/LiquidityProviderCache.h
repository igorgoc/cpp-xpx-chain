/**
*** Copyright 2021 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#pragma once
#include "LiquidityProviderCacheDelta.h"
#include "LiquidityProviderCacheView.h"
#include "catapult/cache/BasicCache.h"
#include "catapult/config_holder/BlockchainConfigurationHolder.h"

namespace catapult { namespace cache {

	using LiquidityProviderBasicCache = BasicCache<LiquidityProviderCacheDescriptor,
			LiquidityProviderCacheTypes::BaseSets, std::shared_ptr<config::BlockchainConfigurationHolder>>;

	/// Cache composed of LiquidityProvider information.
	class BasicLiquidityProviderCache : public LiquidityProviderBasicCache {
	public:
		/// Creates a cache.
		explicit BasicLiquidityProviderCache(
			const CacheConfiguration& config,
			const std::shared_ptr<LiquidityProviderKeyCollector>& pKeyCollector,
			std::shared_ptr<config::BlockchainConfigurationHolder> pConfigHolder)
				: LiquidityProviderBasicCache(config, std::move(pConfigHolder))
				, m_pKeyCollector(pKeyCollector)
		{}

	public:
		/// Initializes the cache with \a keys.
		void init(const std::unordered_set<UnresolvedMosaicId, utils::BaseValueHasher<UnresolvedMosaicId>>& keys) {
			auto delta = createDelta(Height());
			for (const auto& key : keys)
				delta.insertKey(key);
			commit(delta);
		}

		void commit(const CacheDeltaType& delta) {
			if (m_pKeyCollector)
				delta.updateKeyCollector(m_pKeyCollector);
			LiquidityProviderBasicCache::commit(delta);
		}

	private:
		std::shared_ptr<LiquidityProviderKeyCollector> m_pKeyCollector;
	};

	/// Synchronized cache composed of LiquidityProvider information.
	class LiquidityProviderCache : public SynchronizedCacheWithInit<BasicLiquidityProviderCache> {
	public:
		DEFINE_CACHE_CONSTANTS(LiquidityProvider)

	public:
		/// Creates a cache around \a config.
		explicit LiquidityProviderCache(
			const CacheConfiguration& config,
			const std::shared_ptr<LiquidityProviderKeyCollector>& pKeyCollector,
			std::shared_ptr<config::BlockchainConfigurationHolder> pConfigHolder)
				: SynchronizedCacheWithInit(BasicLiquidityProviderCache(config, pKeyCollector, pConfigHolder))
		{}
	};
}}
