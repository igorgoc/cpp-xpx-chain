/**
*** Copyright 2020 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#pragma once
#include "FastFinalityData.h"
#include "FastFinalityRound.h"
#include "FastFinalityTransitionTable.h"
#include "fastfinality/src/dbrb/DbrbProcessContainer.h"
#include "catapult/thread/IoThreadPool.h"

namespace catapult { namespace ionet { class NodePacketIoPair; } }

namespace catapult { namespace fastfinality {

#define TRY_GET_FSM() \
		auto pFsmShared = pFsmWeak.lock(); \
		if (!pFsmShared || pFsmShared->stopped()) \
            return;

	struct ChainSyncData {
		Height NetworkHeight = Height(0);
		Height LocalHeight = Height(0);
		std::vector<Key> NodeIdentityKeys;
	};

	class FastFinalityFsm : public std::enable_shared_from_this<FastFinalityFsm> {
	public:
		FastFinalityFsm(
				std::shared_ptr<thread::IoThreadPool> pPool,
				const config::BlockchainConfiguration& config,
				std::shared_ptr<dbrb::DbrbProcess> pDbrbProcess,
				const plugins::PluginManager& pluginManager)
			: m_pPool(std::move(pPool))
			, m_timer(m_pPool->ioContext())
			, m_sm(boost::sml::sm<FastFinalityTransitionTable>(m_actions))
			, m_fastFinalityData(pluginManager)
			, m_strand(m_pPool->ioContext())
			, m_nodeWorkState(NodeWorkState::None)
			, m_stopped(false)
			, m_dbrbProcess(std::move(pDbrbProcess))
			, m_packetHandlers(config.Node.MaxPacketDataSize.bytes32(), true)
		{}

		FastFinalityFsm(
				std::shared_ptr<thread::IoThreadPool> pPool,
				const config::BlockchainConfiguration& config,
				std::shared_ptr<dbrb::ShardedDbrbProcess> pDbrbProcess,
				const plugins::PluginManager& pluginManager)
			: m_pPool(std::move(pPool))
			, m_timer(m_pPool->ioContext())
			, m_sm(boost::sml::sm<FastFinalityTransitionTable>(m_actions))
			, m_fastFinalityData(pluginManager)
			, m_strand(m_pPool->ioContext())
			, m_nodeWorkState(NodeWorkState::None)
			, m_stopped(false)
			, m_dbrbProcess(std::move(pDbrbProcess))
			, m_packetHandlers(config.Node.MaxPacketDataSize.bytes32())
		{}

	public:
		void start() {
			m_timer.expires_after(std::chrono::seconds(5));
			m_timer.async_wait([pFsmWeak = weak_from_this()](const boost::system::error_code& ec) {
				if (ec) {
					if (ec == boost::asio::error::operation_aborted)
						return;

					CATAPULT_THROW_EXCEPTION(boost::system::system_error(ec));
				}

				TRY_GET_FSM()

				pFsmShared->processEvent(StartLocalChainCheck{});
			});
		}

		template<typename TEvent>
		void processEvent(const TEvent& event) {
			auto func = __PRETTY_FUNCTION__;
			boost::asio::post(m_strand, [pFsmWeak = weak_from_this(), event, func] {
				TRY_GET_FSM()

				CATAPULT_LOG(debug) << func;
				pFsmShared->m_sm.process_event(event);
			});
		}

		void shutdown() {
			std::lock_guard<std::mutex> guard(m_mutex);
			m_stopped = true;
			resetFastFinalityData();
			m_timer.cancel();
			processEvent(Stop{});
			m_dbrbProcess.shutdown();
		}

		auto& timer() {
			return m_timer;
		}

		auto& actions() {
			return m_actions;
		}

		auto& chainSyncData() {
			if (!m_pChainSyncData)
				m_pChainSyncData = std::make_unique<ChainSyncData>();

			return *m_pChainSyncData;
		}

		void resetChainSyncData() {
			m_pChainSyncData.reset();
		}

		auto& fastFinalityData() {
			return m_fastFinalityData;
		}

		const auto& fastFinalityData() const {
			return m_fastFinalityData;
		}

		void resetFastFinalityData() {
			m_fastFinalityData.setRound(FastFinalityRound{});
			m_fastFinalityData.setBlockProducer(nullptr);
			m_fastFinalityData.setProposedBlockHash(Hash256());
			m_fastFinalityData.setBlock(nullptr);
			m_fastFinalityData.setUnexpectedBlockHeight(false);
		}

		bool stopped() const {
			return m_stopped;
		}

		auto nodeWorkState() {
			return m_nodeWorkState;
		}

		void setNodeWorkState(NodeWorkState state) {
			m_nodeWorkState = state;
		}

		auto& mutex() {
			return m_mutex;
		}

		auto& dbrbProcess() {
			return m_dbrbProcess;
		}

		auto& packetHandlers() {
			return m_packetHandlers;
		}

	private:
		std::shared_ptr<thread::IoThreadPool> m_pPool;
		boost::asio::system_timer m_timer;
		FastFinalityActions m_actions;
		boost::sml::sm<FastFinalityTransitionTable> m_sm;
		std::unique_ptr<ChainSyncData> m_pChainSyncData;
		FastFinalityData m_fastFinalityData;
		boost::asio::io_context::strand m_strand;
		NodeWorkState m_nodeWorkState;
		bool m_stopped;
		mutable std::mutex m_mutex;
		dbrb::DbrbProcessContainer m_dbrbProcess;
		ionet::ServerPacketHandlers m_packetHandlers;
	};
}}