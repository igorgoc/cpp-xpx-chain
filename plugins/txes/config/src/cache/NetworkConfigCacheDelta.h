/**
*** Copyright 2019 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#pragma once
#include "NetworkConfigBaseSets.h"
#include "catapult/cache/CacheMixinAliases.h"
#include "catapult/cache/ReadOnlyArtifactCache.h"
#include "catapult/cache/ReadOnlyViewSupplier.h"
#include "catapult/config_holder/BlockchainConfigurationHolder.h"
#include "catapult/deltaset/BaseSetDelta.h"

namespace catapult { namespace cache {

	/// Mixins used by the network config cache delta.
	using NetworkConfigCacheDeltaMixins = PatriciaTreeCacheMixins<NetworkConfigCacheTypes::PrimaryTypes::BaseSetDeltaType, NetworkConfigCacheDescriptor>;

	/// Basic delta on top of the network config cache.
	class BasicNetworkConfigCacheDelta
			: public utils::MoveOnly
			, public NetworkConfigCacheDeltaMixins::Size
			, public NetworkConfigCacheDeltaMixins::Contains
			, public NetworkConfigCacheDeltaMixins::ConstAccessor
			, public NetworkConfigCacheDeltaMixins::MutableAccessor
			, public NetworkConfigCacheDeltaMixins::PatriciaTreeDelta
			, public NetworkConfigCacheDeltaMixins::BasicInsertRemove
			, public NetworkConfigCacheDeltaMixins::DeltaElements
			, public NetworkConfigCacheDeltaMixins::Enable
			, public NetworkConfigCacheDeltaMixins::Height {
	public:
		using ReadOnlyView = NetworkConfigCacheTypes::CacheReadOnlyType;

	public:
		/// Creates a delta around \a networkConfigSets.
		explicit BasicNetworkConfigCacheDelta(const NetworkConfigCacheTypes::BaseSetDeltaPointers& networkConfigSets)
				: NetworkConfigCacheDeltaMixins::Size(*networkConfigSets.pPrimary)
				, NetworkConfigCacheDeltaMixins::Contains(*networkConfigSets.pPrimary)
				, NetworkConfigCacheDeltaMixins::ConstAccessor(*networkConfigSets.pPrimary)
				, NetworkConfigCacheDeltaMixins::MutableAccessor(*networkConfigSets.pPrimary)
				, NetworkConfigCacheDeltaMixins::PatriciaTreeDelta(*networkConfigSets.pPrimary, networkConfigSets.pPatriciaTree)
				, NetworkConfigCacheDeltaMixins::BasicInsertRemove(*networkConfigSets.pPrimary)
				, NetworkConfigCacheDeltaMixins::DeltaElements(*networkConfigSets.pPrimary)
				, m_pNetworkConfigEntries(networkConfigSets.pPrimary)
				, m_pDeltaHeights(networkConfigSets.pDeltaHeights)
				, m_PrimaryHeights(networkConfigSets.PrimaryHeights)
		{}

	public:
		using NetworkConfigCacheDeltaMixins::ConstAccessor::find;
		using NetworkConfigCacheDeltaMixins::MutableAccessor::find;

	public:
		/// Inserts the network config \a entry into the cache.
		void insert(const state::NetworkConfigEntry& entry) {
			NetworkConfigCacheDeltaMixins::BasicInsertRemove::insert(entry);
			insertHeight(entry.height());
		}

		/// Inserts the \a height of network config into the cache.
		void insertHeight(const Height& height) {
			if (!m_pDeltaHeights->contains(height))
				m_pDeltaHeights->insert(height);
		}

		/// Removes the network config \a entry into the cache.
		void remove(const Height& height) {
			NetworkConfigCacheDeltaMixins::BasicInsertRemove::remove(height);
			if (m_pDeltaHeights->contains(height))
				m_pDeltaHeights->remove(height);
		}

		/// Returns heights available after commit
		std::set<Height> heights() const {
			std::set<Height> result;
			for (const auto& height : deltaset::MakeIterableView(m_PrimaryHeights)) {
				result.insert(height);
			}

			auto deltas = m_pDeltaHeights->deltas();

			for (const auto& height : deltas.Added) {
				result.insert(height);
			}

			for (const auto& height : deltas.Removed) {
				result.erase(height);
			}

			return result;
		}

		void updateConfigHolder(std::shared_ptr<config::BlockchainConfigurationHolder> pConfigHolder) const {
			auto deltas = m_pDeltaHeights->deltas();
			auto added = deltas.Added;
			added.insert(deltas.Copied.begin(), deltas.Copied.end());
			auto removed = deltas.Removed;

			for (const auto& height : added) {
				if (removed.find(height) != removed.end()) {
					removed.erase(height);
				} else {
					auto iter = m_pNetworkConfigEntries->find(height);
					if (auto entry = iter.get()) {
						pConfigHolder->InsertConfig(height, entry->networkConfig(), entry->supportedEntityVersions());
					}
				}
			}

			for (const auto& height : removed) {
				pConfigHolder->RemoveConfig(height);
			}
		}

	private:
		NetworkConfigCacheTypes::PrimaryTypes::BaseSetDeltaPointerType m_pNetworkConfigEntries;
		NetworkConfigCacheTypes::HeightTypes::BaseSetDeltaPointerType m_pDeltaHeights;
		const NetworkConfigCacheTypes::HeightTypes::BaseSetType& m_PrimaryHeights;
	};

	/// Delta on top of the network config cache.
	class NetworkConfigCacheDelta : public ReadOnlyViewSupplier<BasicNetworkConfigCacheDelta> {
	public:
		/// Creates a delta around \a networkConfigSets.
		explicit NetworkConfigCacheDelta(const NetworkConfigCacheTypes::BaseSetDeltaPointers& networkConfigSets)
				: ReadOnlyViewSupplier(networkConfigSets)
		{}
	};
}}
