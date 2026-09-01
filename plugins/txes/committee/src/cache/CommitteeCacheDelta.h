/**
*** Copyright 2020 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#pragma once
#include "CommitteeAccountCollector.h"
#include "CommitteeBaseSets.h"
#include "catapult/cache/CacheMixinAliases.h"
#include "catapult/cache/ReadOnlyArtifactCache.h"
#include "catapult/cache/ReadOnlyViewSupplier.h"
#include "catapult/config_holder/BlockchainConfigurationHolder.h"
#include "catapult/deltaset/BaseSetDelta.h"
#include "src/config/CommitteeConfiguration.h"

namespace catapult { namespace cache {

	/// Mixins used by the committee cache delta.
	using CommitteeCacheDeltaMixins = PatriciaTreeCacheMixins<CommitteeCacheTypes::PrimaryTypes::BaseSetDeltaType, CommitteeCacheDescriptor>;

	/// Basic delta on top of the committee cache.
	class BasicCommitteeCacheDelta
			: public utils::MoveOnly
			, public CommitteeCacheDeltaMixins::Size
			, public CommitteeCacheDeltaMixins::Contains
			, public CommitteeCacheDeltaMixins::ConstAccessor
			, public CommitteeCacheDeltaMixins::MutableAccessor
			, public CommitteeCacheDeltaMixins::PatriciaTreeDelta
			, public CommitteeCacheDeltaMixins::BasicInsertRemove
			, public CommitteeCacheDeltaMixins::DeltaElements
			, public CommitteeCacheDeltaMixins::ConfigBasedEnable<config::CommitteeConfiguration> {
	public:
		using ReadOnlyView = CommitteeCacheTypes::CacheReadOnlyType;

	public:
		/// Creates a delta around \a committeeSets.
		explicit BasicCommitteeCacheDelta(
			const CommitteeCacheTypes::BaseSetDeltaPointers& committeeSets,
			std::shared_ptr<config::BlockchainConfigurationHolder> pConfigHolder)
				: CommitteeCacheDeltaMixins::Size(*committeeSets.pPrimary)
				, CommitteeCacheDeltaMixins::Contains(*committeeSets.pPrimary)
				, CommitteeCacheDeltaMixins::ConstAccessor(*committeeSets.pPrimary)
				, CommitteeCacheDeltaMixins::MutableAccessor(*committeeSets.pPrimary)
				, CommitteeCacheDeltaMixins::PatriciaTreeDelta(*committeeSets.pPrimary, committeeSets.pPatriciaTree)
				, CommitteeCacheDeltaMixins::BasicInsertRemove(*committeeSets.pPrimary)
				, CommitteeCacheDeltaMixins::DeltaElements(*committeeSets.pPrimary)
				, CommitteeCacheDeltaMixins::ConfigBasedEnable<config::CommitteeConfiguration>(
					pConfigHolder, [](const auto& config) { return config.Enabled; })
				, m_pCommitteeEntries(committeeSets.pPrimary)
				, m_pDeltaKeys(committeeSets.pDeltaKeys)
				, m_PrimaryKeys(committeeSets.PrimaryKeys)
		{}

	public:
		using CommitteeCacheDeltaMixins::ConstAccessor::find;
		using CommitteeCacheDeltaMixins::MutableAccessor::find;

	public:
		/// Inserts the committee \a entry into the cache.
		void insert(const state::CommitteeEntry& entry) {
			CommitteeCacheDeltaMixins::BasicInsertRemove::insert(entry);
			insertKey(entry.key());
		}

		/// Inserts the \a key into the cache.
		void insertKey(const Key& key) {
			if (!m_pDeltaKeys->contains(key))
				m_pDeltaKeys->insert(key);
		}

		/// Removes the committee \a entry from the cache.
		void remove(const Key& key) {
			CommitteeCacheDeltaMixins::BasicInsertRemove::remove(key);
			if (m_pDeltaKeys->contains(key))
				m_pDeltaKeys->remove(key);
		}

		/// Returns keys available after commit
		std::set<Key> keys() const {
			std::set<Key> result;
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

		void updateAccountCollector(const std::shared_ptr<CommitteeAccountCollector>& pAccountCollector) const {
			pAccountCollector->clear();
			try {
				const auto& config = pluginConfig();
				for (const auto& key : keys()) {
					auto iter = m_pCommitteeEntries->find(key);
					if (auto pEntry = iter.get())
						pAccountCollector->addAccount(*pEntry, config);
				}
			} catch (const catapult_invalid_argument&) {
				// Plugin configuration not yet initialized during intermediate cache boot phase
			}
		}

	private:
		CommitteeCacheTypes::PrimaryTypes::BaseSetDeltaPointerType m_pCommitteeEntries;
		CommitteeCacheTypes::KeyTypes::BaseSetDeltaPointerType m_pDeltaKeys;
		const CommitteeCacheTypes::KeyTypes::BaseSetType& m_PrimaryKeys;
	};

	/// Delta on top of the committee cache.
	class CommitteeCacheDelta : public ReadOnlyViewSupplier<BasicCommitteeCacheDelta> {
	public:
		/// Creates a delta around \a committeeSets.
		explicit CommitteeCacheDelta(
			const CommitteeCacheTypes::BaseSetDeltaPointers& committeeSets,
			const std::shared_ptr<config::BlockchainConfigurationHolder>& pConfigHolder)
				: ReadOnlyViewSupplier(committeeSets, pConfigHolder)
		{}
	};
}}
