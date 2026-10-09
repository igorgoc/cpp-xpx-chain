/**
*** Copyright 2024 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#pragma once
#include "FastFinalityRound.h"
#include "catapult/model/EntityHasher.h"
#include "catapult/plugins/PluginManager.h"
#include <atomic>
#include <map>
#include <memory>
#include <shared_mutex>
#include <set>
#include <future>
#include <boost/filesystem/path.hpp>

namespace catapult {
	namespace crypto { class KeyPair; }
	namespace harvesting { class UnlockedAccounts; }
	namespace model { class Block; }
}

namespace catapult { namespace fastfinality {

	class FastFinalityData {
	public:
		explicit FastFinalityData(const plugins::PluginManager& pluginManager)
			: m_pluginManager(pluginManager)
			, m_round(FastFinalityRound{})
			, m_pBlockProducer(nullptr)
			, m_unexpectedBlockHeight(false)
			, m_isBlockBroadcastEnabled(false)
			, m_failedNodeStateRetrievalCount(0)
		{}

	public:
		void setBeneficiary(const Key& key) {
			m_beneficiary = key;
		}

		const auto& beneficiary() const {
			return m_beneficiary;
		}

		void setUnlockedAccounts(const std::shared_ptr<harvesting::UnlockedAccounts>& pUnlockedAccounts) {
			m_pUnlockedAccounts = pUnlockedAccounts;
		}

		const auto& unlockedAccounts() const {
			return m_pUnlockedAccounts;
		}

		void setDelegatedKeysDirectory(const boost::filesystem::path& keysDir) {
			m_delegatedKeysDirectory = keysDir;
		}

		const auto& delegatedKeysDirectory() const {
			return m_delegatedKeysDirectory;
		}

		void setRound(const FastFinalityRound& round) {
			m_round = round;
		}

		auto round() const {
			return m_round.load();
		}

		void setPrimaryHarvestKey(const Key& primaryHarvestKey) {
			m_primaryHarvestKey = primaryHarvestKey;
		}

		const auto& primaryHarvestKey() const {
			return m_primaryHarvestKey;
		}

		void setBlockProducer(const crypto::KeyPair* pBlockProducer) {
			m_pBlockProducer = pBlockProducer;
		}

		const auto* blockProducer() const {
			return m_pBlockProducer;
		}

		void setProposedBlockHash(const Hash256& hash) {
			std::unique_lock guard(m_mutex);
			m_proposedBlockHash = hash;
		}

		auto proposedBlockHash() const {
			std::shared_lock guard(m_mutex);
			return m_proposedBlockHash;
		}

		std::future<bool> startWaitForBlock() {
			std::unique_lock guard(m_mutex);
			m_pBlockPromise = std::make_shared<std::promise<bool>>();
			return m_pBlockPromise->get_future();
		}

		void setBlock(std::shared_ptr<model::Block> pBlock) {
			std::atomic_store(&m_pBlock, std::move(pBlock));
			{
				std::unique_lock guard(m_mutex);
				if (m_pBlockPromise) {
					m_pBlockPromise->set_value(!!m_pBlock);
					m_pBlockPromise = nullptr;
				}
			}
		}

		auto block() const {
			return std::atomic_load(&m_pBlock);
		}

		void setCurrentBlockHeight(const Height& value) {
			m_currentBlockHeight = value;
		}

		void incrementCurrentBlockHeight() {
			m_currentBlockHeight = m_currentBlockHeight + Height(1);
		}

		auto currentBlockHeight() const {
			return m_currentBlockHeight;
		}

		void setUnexpectedBlockHeight(bool value) {
			std::unique_lock guard(m_mutex);
			m_unexpectedBlockHeight = value;
		}

		bool unexpectedBlockHeight() const {
			std::shared_lock guard(m_mutex);
			return m_unexpectedBlockHeight;
		}

		void setIsBlockBroadcastEnabled(bool value) {
			std::unique_lock guard(m_mutex);
			m_isBlockBroadcastEnabled = value;
		}

		bool isBlockBroadcastEnabled() const {
			std::shared_lock guard(m_mutex);
			return m_isBlockBroadcastEnabled;
		}

		void resetFailedNodeStateRetrievalCount() {
			m_failedNodeStateRetrievalCount = 0;
		}

		void incrementFailedNodeStateRetrievalCount() {
			++m_failedNodeStateRetrievalCount;
		}

		auto failedNodeStateRetrievalCount() const {
			return m_failedNodeStateRetrievalCount.load();
		}

	private:
		const plugins::PluginManager& m_pluginManager;
		std::atomic<FastFinalityRound> m_round;
		Key m_beneficiary;
		std::shared_ptr<harvesting::UnlockedAccounts> m_pUnlockedAccounts;
		Key m_primaryHarvestKey;
		boost::filesystem::path m_delegatedKeysDirectory;
		const crypto::KeyPair* m_pBlockProducer;
		Hash256 m_proposedBlockHash;
		std::shared_ptr<std::promise<bool>> m_pBlockPromise;
		std::shared_ptr<model::Block> m_pBlock;
		mutable std::shared_mutex m_mutex;
		Height m_currentBlockHeight;
		bool m_unexpectedBlockHeight;
		bool m_isBlockBroadcastEnabled;
		std::atomic_uint m_failedNodeStateRetrievalCount;
	};
}}