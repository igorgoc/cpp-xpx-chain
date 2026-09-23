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

#include "PacketHandlers.h"
#include "catapult/utils/Casting.h"

namespace catapult { namespace ionet {

	// region ServerPacketHandlerContext

	ServerPacketHandlerContext::ServerPacketHandlerContext(const Key& key, const std::string& host)
			: m_key(key)
			, m_host(host)
			, m_hasResponse(false)
	{}

	const Key& ServerPacketHandlerContext::key() const {
		return m_key;
	}

	const std::string& ServerPacketHandlerContext::host() const {
		return m_host;
	}

	bool ServerPacketHandlerContext::hasResponse() const {
		return m_hasResponse;
	}

	const PacketPayload& ServerPacketHandlerContext::response() const {
		if (!hasResponse())
			CATAPULT_THROW_RUNTIME_ERROR("response is not set");

		return m_payload;
	}

	void ServerPacketHandlerContext::response(PacketPayload&& payload) {
		if (hasResponse())
			CATAPULT_THROW_RUNTIME_ERROR("response is already set");

		m_payload = std::move(payload);
		m_hasResponse = true;
	}

	// endregion

	// region ServerPacketHandlers

	ServerPacketHandlers::ServerPacketHandlers(uint32_t maxPacketDataSize, bool ignoreUnknownPackets)
		: m_maxPacketDataSize(maxPacketDataSize)
		, m_ignoreUnknownPackets(ignoreUnknownPackets)
	{}

	ServerPacketHandlers::ServerPacketHandlers(const ServerPacketHandlers& rhs)
		: m_maxPacketDataSize(rhs.m_maxPacketDataSize)
		, m_handlers(rhs.m_handlers)
		, m_removableHandlers(rhs.m_removableHandlers)
		, m_ignoreUnknownPackets(rhs.m_ignoreUnknownPackets)
	{}

	ServerPacketHandlers::ServerPacketHandlers(ServerPacketHandlers&& rhs)
		: m_maxPacketDataSize(rhs.m_maxPacketDataSize)
		, m_handlers(std::move(rhs.m_handlers))
		, m_removableHandlers(std::move(rhs.m_removableHandlers))
		, m_ignoreUnknownPackets(rhs.m_ignoreUnknownPackets)
	{}

	ServerPacketHandlers& ServerPacketHandlers::operator=(const ServerPacketHandlers& rhs) {
		m_maxPacketDataSize = rhs.m_maxPacketDataSize;
		m_handlers = rhs.m_handlers;
		m_removableHandlers = rhs.m_removableHandlers;
		m_ignoreUnknownPackets = rhs.m_ignoreUnknownPackets;
		return *this;
	}

	ServerPacketHandlers& ServerPacketHandlers::operator=(ServerPacketHandlers&& rhs) {
		m_maxPacketDataSize = rhs.m_maxPacketDataSize;
		m_handlers = std::move(rhs.m_handlers);
		m_removableHandlers = std::move(rhs.m_removableHandlers);
		m_ignoreUnknownPackets = rhs.m_ignoreUnknownPackets;
		return *this;
	}

	size_t ServerPacketHandlers::size() const {
		size_t numHandlers = 0;
		for (const auto& handler : m_handlers)
			numHandlers += handler ? 1 : 0;

		{
			std::lock_guard<std::mutex> guard(m_mutex);
			for (const auto& handler : m_removableHandlers)
				numHandlers += handler ? 1 : 0;
		}

		return numHandlers;
	}

	uint32_t ServerPacketHandlers::maxPacketDataSize() const {
		return m_maxPacketDataSize;
	}

	bool ServerPacketHandlers::canProcess(PacketType type) const {
		Packet packet;
		packet.Type = type;
		return canProcess(packet);
	}

	bool ServerPacketHandlers::canProcess(const Packet& packet) const {
		auto rawType = utils::to_underlying_type(packet.Type);
		if (rawType < m_handlers.size() && m_handlers[rawType])
			return true;

		std::lock_guard<std::mutex> guard(m_mutex);
		return rawType < m_removableHandlers.size() && !!m_removableHandlers[rawType];
	}

	bool ServerPacketHandlers::process(const Packet& packet, ContextType& context) const {
		auto rawType = utils::to_underlying_type(packet.Type);

		// handlers registered via registerHandler are never removed, so they can be invoked in place
		if (rawType < m_handlers.size() && m_handlers[rawType]) {
			CATAPULT_LOG(trace) << "processing " << packet;
			m_handlers[rawType](packet, context);
			return true;
		}

		// removable handlers can be unregistered (or the backing vector reallocated) by another thread at any time,
		// so take a copy under the lock and invoke the copy, which stays valid for the duration of the call
		auto handler = tryCopyRemovableHandler(rawType);
		if (!handler) {
			CATAPULT_LOG(warning) << "requested unknown handler: " << packet;
			return m_ignoreUnknownPackets;
		}

		CATAPULT_LOG(trace) << "processing " << packet;

		handler(packet, context);
		return true;
	}

	void ServerPacketHandlers::registerHandler(PacketType type, const PacketHandler& handler) {
		auto rawType = utils::to_underlying_type(type);
		if (rawType >= m_handlers.size())
			m_handlers.resize(rawType + 1);

		if (m_handlers[rawType])
			CATAPULT_THROW_RUNTIME_ERROR_1("handler for type is already registered", rawType);

		m_handlers[rawType] = handler;
	}

	void ServerPacketHandlers::registerRemovableHandler(PacketType type, const PacketHandler& handler) {
		std::lock_guard<std::mutex> guard(m_mutex);
		auto rawType = utils::to_underlying_type(type);
		if (rawType >= m_removableHandlers.size())
			m_removableHandlers.resize(rawType + 1);

		if (m_removableHandlers[rawType])
			CATAPULT_THROW_RUNTIME_ERROR_1("removable handler for type is already registered", rawType);

		m_removableHandlers[rawType] = handler;
	}

	void ServerPacketHandlers::removeHandler(PacketType type) {
		std::lock_guard<std::mutex> guard(m_mutex);
		auto rawType = utils::to_underlying_type(type);
		if (rawType < m_removableHandlers.size())
			m_removableHandlers[rawType] = nullptr;
	}

	ServerPacketHandlers::PacketHandler ServerPacketHandlers::tryCopyRemovableHandler(size_t rawType) const {
		std::lock_guard<std::mutex> guard(m_mutex);
		if (rawType >= m_removableHandlers.size())
			return PacketHandler();

		return m_removableHandlers[rawType];
	}

	// endregion
}}
