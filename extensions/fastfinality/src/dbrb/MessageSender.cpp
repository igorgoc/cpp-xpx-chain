/**
*** Copyright 2022 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#include "MessageSender.h"
#include "DbrbChainPackets.h"
#include "catapult/dbrb/Messages.h"
#include "catapult/ionet/NetworkNode.h"
#include "catapult/ionet/NodeContainer.h"
#include "catapult/net/PacketReadersWriters.h"
#include "catapult/thread/Future.h"
#include "catapult/thread/IoThreadPool.h"
#include "catapult/utils/NetworkTime.h"
#include <condition_variable>
#include <shared_mutex>
#include <thread>
#include <boost/asio/system_timer.hpp>

namespace catapult { namespace dbrb {

	namespace {
		constexpr utils::LogLevel MapToLogLevel(net::PeerConnectCode connectCode) {
			if (connectCode == net::PeerConnectCode::Accepted)
				return utils::LogLevel::Debug;
			else
				return utils::LogLevel::Warning;
		}

		struct Message {
			dbrb::Payload Payload;
			bool DropOnFailure;
			uint32_t RetryCount = 0;
		};

		class MessageGroup {
		public:
			bool add(const ProcessId& recipient, const Message& message) {
				if (!m_recipients.emplace(recipient).second)
					return false;

				m_messages.emplace_back(recipient, message);

				return true;
			}

			void remove_if(const predicate<const Message&>& pred) {
				std::vector<std::pair<ProcessId, Message>> messages;
				messages.reserve(m_messages.size());
				m_recipients.clear();
				for (const auto& pair : m_messages) {
					if (!pred(pair.second)) {
						m_recipients.emplace(pair.first);
						messages.emplace_back(pair);
					}
				}
				std::swap(messages, m_messages);
			}

			const std::vector<std::pair<ProcessId, Message>>& messages() const {
				return m_messages;
			}

		private:
			std::set<ProcessId> m_recipients;
			std::vector<std::pair<ProcessId, Message>> m_messages;
		};

		class MessageBuffer {
		public:
			MessageBuffer() : m_size(0) {}

		public:
			void enqueue(const Payload& payload, bool dropOnFailure, const std::set<ProcessId>& recipients, uint32_t retryCount = 0) {
				Message message{ payload, dropOnFailure, retryCount };
				for (const auto& recipient : recipients) {
					bool groupNotFound = true;
					for (auto& messageGroup : m_buffer) {
						if (messageGroup.add(recipient, message)) {
							groupNotFound = false;
							break;
						}
					}

					if (groupNotFound) {
						MessageGroup group;
						group.add(recipient, message);
						m_buffer.emplace_back(group);
					}

					m_size++;
				}
			}

			const std::vector<MessageGroup>& groups() const {
				return m_buffer;
			}

			size_t size() const {
				return m_size;
			}

			bool empty() const {
				return !m_size;
			}

			void clear() {
				m_buffer.clear();
				m_size = 0;
			}

			void remove_if(const predicate<const Message&>& pred) {
				for (auto& group : m_buffer) {
					auto originalSize = group.messages().size();
					group.remove_if(pred);
					m_size -= originalSize - group.messages().size();
				}
			}

		private:
			std::vector<MessageGroup> m_buffer;
			size_t m_size;
		};

		class DefaultMessageSender : public MessageSender, public std::enable_shared_from_this<DefaultMessageSender> {
		public:
			explicit DefaultMessageSender(
				ionet::Node thisNode,
				const ionet::NodeContainer& nodeContainer,
				bool broadcastThisNode,
				std::shared_ptr<thread::IoThreadPool> pPool,
				const utils::TimeSpan& resendMessagesInterval);
			~DefaultMessageSender() override;

			// Message sending
			void enqueue(const Payload& payload, bool dropOnFailure, const std::set<ProcessId>& recipients) override;
			void clearQueue() override;

		private:
			void sendMessages();
			void resendMessages();
			void removePersistentMessages();

		public:
			// Node discovery
			void connectNodes(const std::set<ProcessId>& requestedIds) override;
			void closeAllConnections() override;
			void closeConnections(const std::set<ProcessId>& requestedIds) override;
			void addNodes(const std::vector<ionet::Node>& nodes) override;
			void sendNodes(const std::vector<ionet::Node>& nodes, const ProcessId& recipient) override;
			ViewData getUnreachableNodes(ViewData& view) const override;
			size_t getUnreachableNodeCount(const dbrb::ViewData& view) const override;
			std::vector<ionet::Node> getKnownNodes(const ViewData& view) const override;

		private:
			void requestNodes(const std::set<ProcessId>& requestedIds);
			void broadcast(const Payload& payload);
			void startResendMessagesTimer();

		private:
			// Message sending
			MessageBuffer m_buffer;
			MessageBuffer m_failedMessageBuffer;
			std::mutex m_messageMutex;
			std::condition_variable m_condVar;
			volatile std::atomic_bool m_running;
			std::thread m_sendMessagesThread;
			std::shared_ptr<thread::IoThreadPool> m_pPool;
			boost::asio::system_timer m_timer;
			std::chrono::milliseconds m_resendMessagesInterval;
			std::atomic_bool m_timerRunning;

			// Node discovery
			ionet::Node m_thisNode;
			bool m_broadcastThisNode;
			const ionet::NodeContainer& m_nodeContainer;
			std::unordered_map<ProcessId, ionet::Node, utils::ArrayHasher<ProcessId>> m_nodes;
			mutable std::shared_mutex m_nodeMutex;
		};
	}

	std::shared_ptr<MessageSender> CreateMessageSender(
			ionet::Node thisNode,
			const ionet::NodeContainer& nodeContainer,
			bool broadcastThisNode,
			const std::shared_ptr<thread::IoThreadPool>& pPool,
			const utils::TimeSpan& resendMessagesInterval) {
		return std::make_shared<DefaultMessageSender>(std::move(thisNode), nodeContainer, broadcastThisNode, pPool, resendMessagesInterval);
	}

	DefaultMessageSender::DefaultMessageSender(
			ionet::Node thisNode,
			const ionet::NodeContainer& nodeContainer,
			bool broadcastThisNode,
			std::shared_ptr<thread::IoThreadPool> pPool,
			const utils::TimeSpan& resendMessagesInterval)
		: m_running(true)
		, m_sendMessagesThread(&DefaultMessageSender::sendMessages, this)
		, m_timerRunning(false)
		, m_thisNode(std::move(thisNode))
		, m_nodeContainer(nodeContainer)
		, m_broadcastThisNode(broadcastThisNode)
		, m_pPool(std::move(pPool))
		, m_timer(m_pPool->ioContext())
		, m_resendMessagesInterval(resendMessagesInterval.millis())
	{}

	DefaultMessageSender::~DefaultMessageSender() {
		m_running = false;
		m_timer.cancel();
		m_condVar.notify_one();
		m_sendMessagesThread.join();
	}

	void DefaultMessageSender::enqueue(const Payload& payload, bool dropOnFailure, const std::set<ProcessId>& recipients) {
		{
			std::unique_lock guard(m_messageMutex);
			m_buffer.enqueue(payload, dropOnFailure, recipients);
			CATAPULT_LOG(trace) << "[MESSAGE SENDER] enqueued " << *payload << ", " << recipients.size() << " recipient(s), buffer size " << m_buffer.size();
		}
		m_condVar.notify_one();
	}

	void DefaultMessageSender::removePersistentMessages() {
		auto originalSize = m_buffer.size();
		m_buffer.remove_if([](const Message& message) {
			return !message.DropOnFailure;
		});
		auto currentSize = m_buffer.size();
		if (originalSize > currentSize)
			CATAPULT_LOG(trace) << "[MESSAGE SENDER] removed " << (originalSize - currentSize) << " message(s)";

		if (!m_failedMessageBuffer.empty()) {
			CATAPULT_LOG(trace) << "[MESSAGE SENDER] clearing failed messages (" << m_failedMessageBuffer.size() << ")";
			m_failedMessageBuffer.clear();
		}
	}

	void DefaultMessageSender::clearQueue() {
		m_timer.cancel();
		std::unique_lock guard(m_messageMutex);
		removePersistentMessages();
	}

	void DefaultMessageSender::resendMessages() {
		{
			std::unique_lock guard(m_messageMutex);
			if (m_failedMessageBuffer.empty())
				return;

			CATAPULT_LOG(trace) << "[MESSAGE SENDER] resending " << m_failedMessageBuffer.size() << " message(s)";

			for (const auto& messageGroup : m_failedMessageBuffer.groups()) {
				for (const auto& [recipient, message] : messageGroup.messages())
					m_buffer.enqueue(message.Payload, message.DropOnFailure, { recipient }, message.RetryCount);
			}

			m_failedMessageBuffer.clear();
		}
		m_condVar.notify_one();
	}

	void DefaultMessageSender::startResendMessagesTimer() {
		if (m_timerRunning)
			return;

		m_timerRunning = true;
		m_timer.expires_after(m_resendMessagesInterval);
		m_timer.async_wait([pThisWeak = weak_from_this()](const boost::system::error_code& ec) {
			auto pThis = pThisWeak.lock();
			if (!pThis)
				return;

			pThis->m_timerRunning = false;

			if (ec) {
				if (ec == boost::asio::error::operation_aborted)
					return;

				CATAPULT_THROW_EXCEPTION(boost::system::system_error(ec));
			}

			pThis->resendMessages();
		});
	}

	void DefaultMessageSender::sendMessages() {
		while (m_running) {
			MessageBuffer buffer;
			{
				std::unique_lock lock(m_messageMutex);
				if (m_buffer.empty() && m_running)
					m_condVar.wait(lock, [this] { return !m_buffer.empty() || !m_running; });

				std::swap(buffer, m_buffer);
			}

			if (!m_running) {
				CATAPULT_LOG(trace) << "[MESSAGE SENDER] stopped, " << buffer.size() << " remaining message(s)";
				return;
			}

			auto pWriters = m_pWriters.lock();
			if (!pWriters) {
				CATAPULT_LOG(trace) << "[MESSAGE SENDER] no packet writers, " << buffer.size() << " remaining message(s)";
				return;
			}

			CATAPULT_LOG(trace) << "[MESSAGE SENDER] sending " << buffer.size() << " message(s)";
			for (const auto& messageGroup : buffer.groups()) {
				for (const auto& pair : messageGroup.messages()) {
					const auto& recipient = pair.first;
					const auto& message = pair.second;
					CATAPULT_LOG(trace) << "[MESSAGE SENDER] sending " << *message.Payload << " to " << recipient;
					pWriters->write(recipient, ionet::PacketPayload(message.Payload), [pThisWeak = weak_from_this(), message, recipient](ionet::SocketOperationCode code) {
						if (code != ionet::SocketOperationCode::Success) {
							CATAPULT_LOG(debug) << "[MESSAGE SENDER] sending " << *message.Payload << " to " << recipient << " completed with " << code;
							auto pThis = pThisWeak.lock();
							if (pThis && !message.DropOnFailure) {
								constexpr uint32_t Max_Message_Retries = 5;
								if (message.RetryCount < Max_Message_Retries) {
									auto retryMessage = message;
									retryMessage.RetryCount++;
									{
										std::unique_lock lock(pThis->m_messageMutex);
										pThis->m_failedMessageBuffer.enqueue(retryMessage.Payload, false, { recipient }, retryMessage.RetryCount);
									}
									pThis->startResendMessagesTimer();
								} else {
									CATAPULT_LOG(debug) << "[MESSAGE SENDER] dropping message to " << recipient << " after " << Max_Message_Retries << " retries";
								}
							}
						}
					});
				}
			}
		}
	}

	void DefaultMessageSender::connectNodes(const std::set<ProcessId>& requestedIds) {
		std::vector<ionet::Node> nodes;
		std::set<ProcessId> ids;

		{
			auto pWriters = m_pWriters.lock();
			if (!pWriters)
				return;

			auto peers = pWriters->peers();
			for (const auto& id : requestedIds) {
				if (peers.find(id) == peers.cend())
					ids.emplace(id);
			}
		}

		ids.erase(m_thisNode.identityKey());

		if (ids.empty())
			return;

		CATAPULT_LOG(trace) << "[MESSAGE SENDER] looking for " << ids.size() << " nodes";

		nodes = m_nodeContainer.view().getNodes(ids);
		nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [](const auto& node) { return node.endpoint().Host.empty(); }), nodes.end());
		CATAPULT_LOG(trace) << "[MESSAGE SENDER] got " << nodes.size() << " discovered nodes";
		if (!nodes.empty())
			addNodes(nodes);

		for (const auto& node : nodes)
			ids.erase(node.identityKey());

		if (ids.empty())
			return;

		CATAPULT_LOG(debug) << "[MESSAGE SENDER] requesting " << ids.size() << " nodes";

		requestNodes(ids);
	}

	void DefaultMessageSender::closeAllConnections() {
		auto pWriters = m_pWriters.lock();
		if (!pWriters)
			return;

		pWriters->closeActiveConnections();
	}

	void DefaultMessageSender::closeConnections(const std::set<ProcessId>& requestedIds) {
		if (requestedIds.empty())
			return;

		auto pWriters = m_pWriters.lock();
		if (!pWriters)
			return;

		for (const auto& id : requestedIds)
			pWriters->closeOne(id);
	}

	void DefaultMessageSender::requestNodes(const std::set<ProcessId>& requestedIds) {
		if (requestedIds.empty())
			return;

		auto pPacket = ionet::CreateSharedPacket<DbrbPullNodesPacket>(requestedIds.size() * ProcessId_Size);
		pPacket->NodeCount = utils::checked_cast<size_t, uint16_t>(requestedIds.size());
		auto* pBuffer = reinterpret_cast<uint8_t*>(pPacket.get() + 1);
		for (const auto& id : requestedIds) {
			memcpy(pBuffer, id.data(), ProcessId_Size);
			pBuffer += ProcessId_Size;
		}

		broadcast(pPacket);
	}

	void DefaultMessageSender::addNodes(const std::vector<ionet::Node>& nodes) {
		auto pWriters = m_pWriters.lock();
		if (!pWriters)
			return;

		for (const auto& node : nodes) {
			const auto& id = node.identityKey();
			if (id == m_thisNode.identityKey())
				continue;

			const auto& endpoint = node.endpoint();
			if (!endpoint.DbrbPort) {
				CATAPULT_LOG(trace) << "[MESSAGE SENDER] no DBRB port " << node << " " << id;
				continue;
			}

			{
				std::unique_lock guard(m_nodeMutex);
				m_nodes[id] = node;
			}

			auto dbrbNode = ionet::Node(node.identityKey(), ionet::NodeEndpoint{ endpoint.Host, endpoint.DbrbPort }, node.metadata());
			auto peers = pWriters->peers();
			if ((peers.find(id) != peers.cend())) {
				CATAPULT_LOG(trace) << "[MESSAGE SENDER] Already connected or connecting to " << dbrbNode << " " << id;
				continue;
			}

			CATAPULT_LOG(debug) << "[MESSAGE SENDER] Connecting to " << dbrbNode << " " << id;
			pWriters->connect(dbrbNode, [pThisWeak = weak_from_this(), dbrbNode](const auto& result) {
				const auto& identityKey = dbrbNode.identityKey();
				CATAPULT_LOG_LEVEL(MapToLogLevel(result.Code)) << "[MESSAGE SENDER] connection attempt to " << dbrbNode << " " << identityKey << " completed with " << result.Code;
				auto pThis = pThisWeak.lock();
				if (pThis && result.Code == net::PeerConnectCode::Accepted) {
					CATAPULT_LOG(debug) << "[MESSAGE SENDER] Connected to node " << dbrbNode << " " << identityKey;
					if (pThis->m_broadcastThisNode) {
						CATAPULT_LOG(debug) << "[MESSAGE SENDER] sharing this node " << pThis->m_thisNode << " [dbrb port " << pThis->m_thisNode.endpoint().DbrbPort << "] " << pThis->m_thisNode.identityKey();
						pThis->sendNodes({ pThis->m_thisNode }, identityKey);
					} else {
						pThis->m_condVar.notify_one();
					}
				}
			});
		}
	}

	void DefaultMessageSender::sendNodes(const std::vector<ionet::Node>& nodes, const ProcessId& recipient) {
		if (nodes.empty())
			return;

		auto nodeCount = utils::checked_cast<size_t, uint16_t>(nodes.size());
		std::vector<model::UniqueEntityPtr<ionet::NetworkNode>> networkNodes;
		networkNodes.reserve(nodeCount);
		auto payloadSize = 0u;
		for (const auto& node : nodes) {
			networkNodes.emplace_back(ionet::PackNode(node));
			payloadSize += networkNodes.back()->Size;
		}

		auto pPacket = ionet::CreateSharedPacket<DbrbPushNodesPacket>(payloadSize);
		pPacket->NodeCount = nodeCount;
		auto* pBuffer = reinterpret_cast<uint8_t*>(pPacket.get() + 1);
		for (const auto& pNetworkNode : networkNodes) {
			memcpy(pBuffer, pNetworkNode.get(), pNetworkNode->Size);
			pBuffer += pNetworkNode->Size;
		}

		enqueue(pPacket, true, { recipient });
	}

	void DefaultMessageSender::broadcast(const Payload& payload) {
		auto pWriters = m_pWriters.lock();
		if (!pWriters)
			return;

		auto connectedNodes = pWriters->identities();
		std::set<ProcessId> recipients(connectedNodes.cbegin(), connectedNodes.cend());
		if (!recipients.empty())
			enqueue(payload, true, recipients);
	}

	ViewData DefaultMessageSender::getUnreachableNodes(ViewData& view) const {
		auto pWriters = m_pWriters.lock();
		if (!pWriters)
			return {};

		ViewData unreachableNodes;
		auto connectedNodes = pWriters->identities();
		for (auto iter = view.cbegin(); iter != view.cend();) {
			if (connectedNodes.find(*iter) == connectedNodes.cend()) {
				unreachableNodes.emplace(*iter);
				iter = view.erase(iter);
			} else {
				++iter;
			}
		}

		return unreachableNodes;
	}

	size_t DefaultMessageSender::getUnreachableNodeCount(const dbrb::ViewData& view) const {
		auto pWriters = m_pWriters.lock();
		if (!pWriters)
			return {};

		size_t count = 0;
		auto connectedNodes = pWriters->identities();
		for (auto iter = view.cbegin(); iter != view.cend(); ++iter) {
			if (connectedNodes.find(*iter) == connectedNodes.cend())
				count++;
		}

		return count;
	}

	std::vector<ionet::Node> DefaultMessageSender::getKnownNodes(const ViewData& view) const {
		std::shared_lock guard(m_nodeMutex);
		std::vector<ionet::Node> nodes;
		for (const auto& id : view) {
			auto iter = m_nodes.find(id);
			if (iter != m_nodes.cend()) {
				nodes.push_back(iter->second);
			} if (id == m_thisNode.identityKey()) {
				nodes.push_back(m_thisNode);
			}
		}

		return nodes;
	}
}}