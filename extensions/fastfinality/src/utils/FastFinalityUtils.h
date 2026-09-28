/**
*** Copyright 2022 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#pragma once
#include "fastfinality/src/dbrb/MessageSender.h"
#include "fastfinality/src/FastFinalityChainPackets.h"
#include "catapult/api/ApiTypes.h"
#include "catapult/dbrb/View.h"
#include "catapult/harvesting_core/HarvesterBlockGenerator.h"
#include "catapult/ionet/NodePacketIoPair.h"
#include "catapult/model/Elements.h"
#include "catapult/thread/FutureUtils.h"
#include <boost/asio/post.hpp>

namespace catapult {
	namespace harvesting {
		class UnlockedAccounts;
		struct HarvestingConfiguration;
	}
	namespace extensions {
		class ServiceState;
	}
	namespace handlers {
		struct PullBlocksHandlerConfiguration;
	}
}

namespace catapult { namespace fastfinality {

	std::shared_ptr<harvesting::UnlockedAccounts> CreateUnlockedAccounts(const harvesting::HarvestingConfiguration& config);
	harvesting::BlockGenerator CreateHarvesterBlockGenerator(extensions::ServiceState& state);
	handlers::PullBlocksHandlerConfiguration CreatePullBlocksHandlerConfiguration(const config::NodeConfiguration& nodeConfig);

	template<typename TFsm>
	RemoteNodeStateRetriever CreateRemoteNodeStateRetriever(
			const std::weak_ptr<TFsm>& pFsmWeak,
			const std::shared_ptr<config::BlockchainConfigurationHolder>& pConfigHolder,
			const model::BlockElementSupplier& lastBlockElementSupplier) {
		return [pFsmWeak, pConfigHolder, lastBlockElementSupplier]() {
			auto pFsmShared = pFsmWeak.lock();
			if (!pFsmShared || pFsmShared->stopped())
				return std::vector<RemoteNodeState>();

			auto pNodeStates = std::make_shared<std::vector<RemoteNodeState>>();
			const auto& dbrbProcess = pFsmShared->dbrbProcess();
			if (!pFsmShared || pFsmShared->stopped()) {
				CATAPULT_LOG(warning) << "aborting node states retrieval";
				return *pNodeStates;
			}

			auto chainHeight = lastBlockElementSupplier()->Block.Height;
			const auto& config = pConfigHolder->Config(chainHeight + Height(1));
			auto view = config.Network.DbrbBootstrapProcesses;
			view.erase(dbrbProcess.id());

			std::weak_ptr<std::vector<RemoteNodeState>> pNodeStatesWeak = pNodeStates;
			auto pMutex = std::make_shared<std::mutex>();
			std::weak_ptr<std::mutex> pMutexWeak = pMutex;
			auto pReadyPromise = std::make_shared<std::promise<bool>>();
			auto count = view.size();
			auto quorumCount = count > 3 ? std::min(dbrb::View::quorumSize(count), size_t(3)) : count;
			auto pIsPromiseFulfilled = std::make_shared<bool>(false);
			pFsmShared->packetHandlers().registerRemovableHandler(ionet::PacketType::Pull_Remote_Node_State_Response, [pNodeStatesWeak, pMutexWeak, pReadyPromise, pIsPromiseFulfilled, count, quorumCount](
					const auto& packet, auto& context) {
				auto pNodeStates = pNodeStatesWeak.lock();
				auto pMutex = pMutexWeak.lock();
				if (!pNodeStates || !pMutex)
					return;

				std::lock_guard<std::mutex> guard(*pMutex);
				const auto* pResponse = static_cast<const RemoteNodeStatePacket*>(&packet);
				RemoteNodeState state;
				state.NodeKey = context.key();
				state.Height = pResponse->Height;
				state.BlockHash = pResponse->BlockHash;
				state.NodeWorkState = pResponse->NodeWorkState;
				pNodeStates->push_back(state);
				CATAPULT_LOG(debug) << "retrieved node state from " << state.NodeKey << " (" << pNodeStates->size() << "/" << count << ")";
				if (!*pIsPromiseFulfilled && pNodeStates->size() >= quorumCount) {
					*pIsPromiseFulfilled = true;
					pReadyPromise->set_value(true);
				}
			});
			auto pPacket = ionet::CreateSharedPacket<RemoteNodeStatePacket>();
			pPacket->Height = chainHeight + Height(config.Node.MaxBlocksPerSyncAttempt);
			dbrbProcess.messageSender()->enqueue(pPacket, true, view);
			pReadyPromise->get_future().wait_for(std::chrono::seconds(15));

			pFsmShared->packetHandlers().removeHandler(ionet::PacketType::Pull_Remote_Node_State_Response);

			{
				std::lock_guard<std::mutex> guard(*pMutex);
				CATAPULT_LOG(debug) << "retrieved " << pNodeStates->size() << " node state(s) out of " << view.size();
				return std::move(*pNodeStates);
			}
		};
	}
}}