/**
*** Copyright (c) 2016-2019, Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp.
*** Copyright (c) 2020-present, Jaguar0625, gimre, BloodyRookie.
*** All rights reserved.
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

#include "SslPacketSocket.h"
#include "BufferedPacketIo.h"
#include "Node.h"
#include "WorkingBuffer.h"
#include "catapult/thread/StrandOwnerLifetimeExtender.h"
#include "catapult/thread/TimedCallback.h"
#include "catapult/utils/StackTimer.h"
#include <boost/asio/ssl.hpp>

namespace catapult { namespace ionet {

	namespace {
		// region error mapping

		SocketOperationCode mapReadErrorCodeToSslSocketOperationCode(const boost::system::error_code& ec) {
			if (!ec)
				return SocketOperationCode::Success;

			auto isEof = boost::asio::error::eof == ec;
			if (isEof)
				CATAPULT_LOG(info) << "eof reading from socket: " << ec.message();
			else
				CATAPULT_LOG(error) << "failed when reading from socket: " << ec.message();

			return isEof ? SocketOperationCode::Closed : SocketOperationCode::Read_Error;
		}

		SocketOperationCode mapWriteErrorCodeToSslSocketOperationCode(const boost::system::error_code& ec) {
			if (!ec)
				return SocketOperationCode::Success;

			CATAPULT_LOG(error) << "failed when writing to socket: " << ec.message();
			return SocketOperationCode::Write_Error;
		}

		// endregion

		// region SocketGuard

		class SocketGuard final : public std::enable_shared_from_this<SocketGuard> {
		public:
			SocketGuard(boost::asio::io_context& ioContext, boost::asio::ssl::context& sslContext)
					: m_strand(ioContext)
					, m_strandWrapper(m_strand)
					, m_socket(ioContext, sslContext)
					, m_sentinelByte(0) {
				m_isClosed.clear();
				m_isClosed.test_and_set();
			}

			SocketGuard(NetworkSocket&& socket, boost::asio::io_context& ioContext, boost::asio::ssl::context& sslContext)
					: m_strand(ioContext)
					, m_strandWrapper(m_strand)
					, m_socket(std::move(socket), sslContext)
					, m_sentinelByte(0) {
				m_isClosed.clear();
				m_isClosed.test_and_set();
			}

		public:
			SslSocket& socket() {
				return m_socket;
			}

			boost::asio::io_context::strand& strand() {
				return m_strand;
			}

			void markOpen() {
				m_isClosed.clear();
			}

		private:
			template<typename THandler>
			auto wrap(THandler handler) {
				return m_strandWrapper.wrap(shared_from_this(), handler);
			}

		public:
			void close() {
				if (m_isClosed.test_and_set()) {
					abort();
					return;
				}

				m_socket.async_shutdown(wrap([](const auto& ec) {
					if (ec && boost::asio::error::operation_aborted != ec && boost::asio::error::bad_descriptor != ec)
						CATAPULT_LOG(warning) << "async_shutdown returned an error: " << ec.message();
				}));

				// write must be of non-zero length to avoid asio optimization to no-op
				auto asioBuffer = boost::asio::buffer(&m_sentinelByte, 1);
				boost::asio::async_write(m_socket, asioBuffer, wrap([this](const auto& ec, auto) {
					if (ec && !IsProtocolShutdown(ec))
						CATAPULT_LOG(warning) << "async_write returned an error: " << ec.message();

					abort();
				}));
			}

			void abort() {
				boost::system::error_code ignoredEc;
				m_socket.lowest_layer().close(ignoredEc);
			}

		private:
			static int OpensslErrorGetReason(unsigned long errcode) {
				if (ERR_SYSTEM_ERROR(errcode))
					return errcode & ERR_SYSTEM_MASK;

				return errcode & ERR_REASON_MASK;
			}

			static bool IsProtocolShutdown(const boost::system::error_code& ec) {
				return boost::asio::error::get_ssl_category() == ec.category()
						&& SSL_R_PROTOCOL_IS_SHUTDOWN == OpensslErrorGetReason(static_cast<unsigned long>(ec.value()));
			}

		private:
			boost::asio::io_context::strand m_strand;
			thread::StrandOwnerLifetimeExtender<SocketGuard> m_strandWrapper;
			SslSocket m_socket;
			uint8_t m_sentinelByte;
			std::atomic_flag m_isClosed;
		};

		// endregion

		// region BasicPacketSocket(Writer)

		template<typename TSocketCallbackWrapper>
		class BasicPacketSocketWriter {
		public:
			BasicPacketSocketWriter(SslSocket& socket, TSocketCallbackWrapper& wrapper, size_t maxPacketDataSize)
					: m_socket(socket)
					, m_wrapper(wrapper)
					, m_maxPacketDataSize(maxPacketDataSize)
			{}

		public:
			void write(const PacketPayload& payload, const SslPacketSocket::WriteCallback& callback) {
				if (!IsPacketDataSizeValid(payload.header(), m_maxPacketDataSize)) {
					CATAPULT_LOG(warning) << "bypassing write of malformed " << payload.header();
					callback(SocketOperationCode::Malformed_Data);
					return;
				}

				auto pContext = std::make_shared<WriteContext>(payload, callback);
				boost::asio::async_write(m_socket, pContext->headerBuffer(), m_wrapper.wrap([this, pContext](const auto& ec, auto) {
					this->writeNext(ec, pContext);
				}));
			}

		private:
			struct WriteContext {
			public:
				WriteContext(const PacketPayload& payload, const SslPacketSocket::WriteCallback& callback)
						: m_payload(payload)
						, m_callback(callback)
						, m_nextBufferIndex(0)
				{}

			public:
				auto headerBuffer() const {
					const auto& header = m_payload.header();
					return boost::asio::buffer(static_cast<const void*>(&header), sizeof(header));
				}

				auto nextDataBuffer() {
					const auto& rawBuffer = m_payload.buffers()[m_nextBufferIndex++];
					return boost::asio::buffer(static_cast<const void*>(rawBuffer.pData), rawBuffer.Size);
				}

				bool tryComplete(const boost::system::error_code& ec) {
					auto lastCode = mapWriteErrorCodeToSslSocketOperationCode(ec);
					if (SocketOperationCode::Success != lastCode || m_nextBufferIndex >= m_payload.buffers().size()) {
						m_callback(lastCode);
						return true;
					}

					return false;
				}

			private:
				const PacketPayload m_payload;
				const SslPacketSocket::WriteCallback m_callback;
				size_t m_nextBufferIndex;
			};

			void writeNext(const boost::system::error_code& lastEc, const std::shared_ptr<WriteContext>& pContext) {
				if (pContext->tryComplete(lastEc))
					return;

				boost::asio::async_write(m_socket, pContext->nextDataBuffer(), m_wrapper.wrap([this, pContext](const auto& ec, auto) {
					this->writeNext(ec, pContext);
				}));
			}

		private:
			SslSocket& m_socket;
			TSocketCallbackWrapper& m_wrapper;
			size_t m_maxPacketDataSize;
		};

		// endregion

		// region BasicPacketSocket(Reader)

		template<typename TSocketCallbackWrapper>
		class BasicPacketSocketReader {
		public:
			BasicPacketSocketReader(SslSocket& socket, TSocketCallbackWrapper& wrapper, WorkingBuffer& buffer)
					: m_socket(socket)
					, m_wrapper(wrapper)
					, m_buffer(buffer)
			{}

		public:
			void read(const SslPacketSocket::ReadCallback& callback, bool allowMultiple) {
				readInternal(callback, allowMultiple);
			}

		private:
			struct SharedAppendContext {
			public:
				explicit SharedAppendContext(AppendContext&& context) : Context(std::move(context))
				{}

			public:
				AppendContext Context;
			};

			class AutoConsume {
			public:
				explicit AutoConsume(PacketExtractor& packetExtractor) : m_packetExtractor(packetExtractor)
				{}

				~AutoConsume() {
					m_packetExtractor.consume();
				}

			private:
				PacketExtractor& m_packetExtractor;
			};

		private:
			void readInternal(const SslPacketSocket::ReadCallback& callback, bool allowMultiple) {
				// try to extract a packet from the working buffer
				const Packet* pExtractedPacket = nullptr;
				auto packetExtractor = m_buffer.preparePacketExtractor();

				AutoConsume autoConsume(packetExtractor);
				auto extractResult = packetExtractor.tryExtractNextPacket(pExtractedPacket);

				switch (extractResult) {
				case PacketExtractResult::Success:
					do {
						callback(SocketOperationCode::Success, pExtractedPacket);
						if (!allowMultiple)
							return;

						extractResult = packetExtractor.tryExtractNextPacket(pExtractedPacket);
					} while (PacketExtractResult::Success == extractResult);
					return checkAndHandleError(extractResult, callback, allowMultiple);

				case PacketExtractResult::Insufficient_Data:
					break;

				default:
					return checkAndHandleError(extractResult, callback, allowMultiple);
				}

				// Read additional data from the socket and append it to the working buffer.
				// Note that readSome is only called when extractor returns Insufficient_Data, which also means no data was consumed
				// thus, the in-place read will have exclusive access to the working buffer and autoConsume's destruction will be a no-op.
				readSome(callback, allowMultiple);
			}

			void readSome(const SslPacketSocket::ReadCallback& callback, bool allowMultiple) {
				auto pAppendContext = std::make_shared<SharedAppendContext>(m_buffer.prepareAppend());
				auto readHandler = [this, callback, allowMultiple, pAppendContext](const auto& ec, auto bytesReceived) {
					auto code = mapReadErrorCodeToSslSocketOperationCode(ec);
					if (SocketOperationCode::Success != code)
						return callback(code, nullptr);

					pAppendContext->Context.commit(bytesReceived);
					this->readInternal(callback, allowMultiple);
				};

				m_socket.async_read_some(pAppendContext->Context.buffer(), m_wrapper.wrap(readHandler));
			}

			void checkAndHandleError(PacketExtractResult extractResult, const SslPacketSocket::ReadCallback& callback, bool allowMultiple) {
				// ignore non errors
				switch (extractResult) {
				case PacketExtractResult::Success:
					return;

				case PacketExtractResult::Insufficient_Data:
					// signal the completion of a multi-read operation
					if (allowMultiple)
						callback(SocketOperationCode::Insufficient_Data, nullptr);

					// this is not a termination condition for a single-read operation
					return;

				default:
					break;
				}

				// invoke the callback for errors
				CATAPULT_LOG(error) << "failed processing malformed packet: " << extractResult;
				callback(SocketOperationCode::Malformed_Data, nullptr);
			}

		private:
			SslSocket& m_socket;
			TSocketCallbackWrapper& m_wrapper;
			WorkingBuffer& m_buffer;
		};

		// endregion

		// region BasicPacketSocket

		namespace {
			void ConfigureSslVerify(SslSocket& socket, Key& publicKey, const predicate<PacketSocketSslVerifyContext&>& verifyCallback) {
				socket.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
				socket.set_verify_depth(1);
				socket.set_verify_callback([&publicKey, verifyCallback](auto preverified, auto& asioVerifyContext) {
					PacketSocketSslVerifyContext verifyContext(preverified, asioVerifyContext, publicKey);
					return verifyCallback(verifyContext);
				});
			}
		}

		// implements packet based socket conventions with an implicit strand
		// \note user callbacks are executed in the context of the strand, so they are effectively serialized.
		template<typename TSocketCallbackWrapper>
		class BasicPacketSocket final
				: public BasicPacketSocketWriter<TSocketCallbackWrapper>
				, public BasicPacketSocketReader<TSocketCallbackWrapper> {
		public:
			BasicPacketSocket(
					const std::shared_ptr<SocketGuard>& pSocketGuard,
					const PacketSocketOptions& options,
					TSocketCallbackWrapper& wrapper)
					: BasicPacketSocketWriter<TSocketCallbackWrapper>(pSocketGuard->socket(), wrapper, options.MaxPacketDataSize)
					, BasicPacketSocketReader<TSocketCallbackWrapper>(pSocketGuard->socket(), wrapper, m_buffer)
					, m_pSocketGuard(pSocketGuard)
					, m_socket(m_pSocketGuard->socket())
					, m_buffer(options)
					, m_wrapper(wrapper) {
				ConfigureSslVerify(m_socket, m_publicKey, options.SslOptions.VerifyCallbackSupplier());
			}

		public:
			void stats(const SslPacketSocket::StatsCallback& callback) {
				SslPacketSocket::Stats stats{};
				stats.IsOpen = m_socket.lowest_layer().is_open();
				stats.NumUnprocessedBytes = m_buffer.size();
				callback(stats);
			}

			void close() {
				m_pSocketGuard->close();
			}

			void abort() {
				m_pSocketGuard->abort();
			}

		public:
			SslSocket& impl() {
				return m_socket;
			}

			const Key& publicKey() const {
				return m_publicKey;
			}

			boost::asio::io_context::strand& strand() {
				return m_pSocketGuard->strand();
			}

			void setOptions() {
				m_socket.lowest_layer().non_blocking(true);
			}

			void markOpen() {
				m_pSocketGuard->markOpen();
			}

		private:
			std::shared_ptr<SocketGuard> m_pSocketGuard;
			SslSocket& m_socket;
			Key m_publicKey;
			WorkingBuffer m_buffer;
			TSocketCallbackWrapper& m_wrapper;
		};

		// endregion

		// region StrandedSslPacketSocket

		class SocketIdentifier {
		public:
			explicit SocketIdentifier(uint64_t id)
					: m_id(id)
					, m_isClosed(false)
			{}

		public:
			bool fetchClose() {
				if (m_isClosed)
					return true;

				m_isClosed = true;
				return false;
			}

		public:
			friend std::ostream& operator<<(std::ostream& out, const SocketIdentifier& id) {
				out << "(" << utils::HexFormat(id.m_id) << ")";
				if (id.m_isClosed)
					out << ", " << id.m_timer.millis() << "ms elapsed";

				return out;
			}

		private:
			uint64_t m_id;
			bool m_isClosed;
			utils::StackTimer m_timer;
		};

		// implements SslPacketSocket using an explicit strand and ensures deterministic shutdown by using enable_shared_from_this
		class StrandedSslPacketSocket final
				: public SslPacketSocket
				, public std::enable_shared_from_this<StrandedSslPacketSocket> {
		private:
			using SocketType = BasicPacketSocket<StrandedSslPacketSocket>;

		public:
			StrandedSslPacketSocket(const std::shared_ptr<SocketGuard>& pSocketGuard, const PacketSocketOptions& options)
					: m_strandWrapper(pSocketGuard->strand())
					, m_socket(pSocketGuard, options, *this)
					, m_id(s_idCounter.fetch_add(1))
			{}

			~StrandedSslPacketSocket() override {
				// all async operations posted on the strand must be completed by now because all operations
				// posted on the strand have been initiated by this object and captured this as a shared_ptr
				// (executing the destructor means they all must have been destroyed)

				// closing the socket is safe (this is the only thread left) and the strand can be destroyed
				// because it has been emptied
				if (m_id.fetchClose())
					return;

				CATAPULT_LOG(debug) << "socket close triggered by destruction " << m_id;
				m_socket.close();
			}

		public:
			void write(const PacketPayload& payload, const WriteCallback& callback) override {
				post([payload, callback](auto& socket) { socket.write(payload, callback); });
			}

			void read(const ReadCallback& callback) override {
				post([callback](auto& socket) { socket.read(callback, false); });
			}

			void readMultiple(const ReadCallback& callback) override {
				post([callback](auto& socket) { socket.read(callback, true); });
			}

			void stats(const StatsCallback& callback) override {
				post([callback](auto& socket) { socket.stats(callback); });
			}

			void close() override {
				postCloseOnce("close", [](auto& socket) {
					socket.close();
				});
			}

			void abort() override {
				postCloseOnce("abort", [](auto& socket) {
					socket.abort();
				});
			}

			std::shared_ptr<PacketIo> buffered() override {
				return CreateBufferedPacketIo(weak_from_this(), strand());
			}

		public:
			SslSocket& impl() {
				return m_socket.impl();
			}

			const Key& publicKey() const {
				return m_socket.publicKey();
			}

			boost::asio::io_context::strand& strand() {
				return m_socket.strand();
			}

			const SocketIdentifier& id() const {
				return m_id;
			}

			void setOptions() {
				// post is not required because call is made immediately after opening socket
				// so there is no opportunity for multithreaded access
				m_socket.setOptions();
			}

			void markOpen() {
				m_socket.markOpen();
			}

		public:
			template<typename THandler>
			auto wrap(THandler handler) {
				// when BasicPacketSocket calls wrap, the returned callback needs to extend the lifetime of this object
				return m_strandWrapper.wrap(shared_from_this(), handler);
			}

		private:
			template<typename THandler>
			void post(THandler handler) {
				// ensure all handlers extend the lifetime of this object and post to a strand
				return m_strandWrapper.post(shared_from_this(), [handler](const auto& pThis) {
					handler(pThis->m_socket);
				});
			}

			template<typename THandler>
			void postCloseOnce(const char* operationName, THandler handler) {
				post([&id = m_id, operationName, handler](auto& socket) {
					if (id.fetchClose())
						return;

					CATAPULT_LOG(debug) << "socket " << operationName << " triggered by owner " << id;
					handler(socket);
				});
			}

		private:
			static std::atomic<uint64_t> s_idCounter;

		private:
			thread::StrandOwnerLifetimeExtender<StrandedSslPacketSocket> m_strandWrapper;
			SocketType m_socket;
			SocketIdentifier m_id;
		};

		std::atomic<uint64_t> StrandedSslPacketSocket::s_idCounter(1);

		// endregion
	}

	// region SslPacketSocketInfo

	SslPacketSocketInfo::SslPacketSocketInfo() = default;

	SslPacketSocketInfo::SslPacketSocketInfo(const std::string& host, const Key& publicKey, const std::shared_ptr<SslPacketSocket>& pPacketSocket)
			: m_host(host)
			, m_publicKey(publicKey)
			, m_pPacketSocket(pPacketSocket)
	{}

	const std::string& SslPacketSocketInfo::host() const {
		return m_host;
	}

	const Key& SslPacketSocketInfo::publicKey() const {
		return m_publicKey;
	}

	const std::shared_ptr<SslPacketSocket>& SslPacketSocketInfo::socket() const {
		return m_pPacketSocket;
	}

	SslPacketSocketInfo::operator bool() const {
		return !!m_pPacketSocket;
	}

	// endregion

	// region Accept

	namespace {
		class AcceptHandler : public std::enable_shared_from_this<AcceptHandler> {
		public:
			AcceptHandler(
					boost::asio::io_context& ioContext,
					boost::asio::ip::tcp::acceptor& acceptor,
					const PacketSocketOptions& options,
					const SslAcceptCallback& accept)
					: m_ioContext(ioContext)
					, m_acceptor(acceptor)
					, m_accept(accept)
					, m_options(options)
			{}

		public:
			void start() {
				m_acceptor.async_accept([pThis = shared_from_this()](const auto& ec, auto&& socket) {
					pThis->handleAccept(ec, std::move(socket));
				});
			}

		private:
			void handleAccept(const boost::system::error_code& ec, NetworkSocket&& socket) {
				if (ec) {
					CATAPULT_LOG(warning) << "async_accept returned an error: " << ec.message();
					return m_accept(SslPacketSocketInfo());
				}

				// try to determine the remote endpoint (ignore errors if socket was immediately closed after accept)
				boost::system::error_code remoteEndpointEc;
				const auto& asioEndpoint = socket.remote_endpoint(remoteEndpointEc);
				if (remoteEndpointEc) {
					CATAPULT_LOG(warning) << "unable to determine remote endpoint: " << remoteEndpointEc;
					return m_accept(SslPacketSocketInfo());
				}

				m_host = asioEndpoint.address().to_string();
				m_pSocket = std::make_shared<StrandedSslPacketSocket>(
						std::make_shared<SocketGuard>(std::move(socket), m_ioContext, m_options.SslOptions.ContextSupplier()),
						m_options);
				m_pSocket->setOptions();

				auto pTimedCallback = thread::MakeTimedCallback(m_ioContext, m_accept, SslPacketSocketInfo());
				pTimedCallback->setTimeout(m_options.AcceptHandshakeTimeout);
				pTimedCallback->setTimeoutHandler([pSocket = m_pSocket]() {
					pSocket->close();

					// need to post log on strand because SocketIdentifier fields are non-atomic
					boost::asio::post(pSocket->strand(), [pSocket]() {
						CATAPULT_LOG(debug) << "accept handshake failed due to timeout " << pSocket->id();
					});
				});

				m_accept = [pTimedCallback](const auto& socketInfo) {
					pTimedCallback->callback(socketInfo);
				};

				m_pSocket->impl().async_handshake(SslSocket::server, [pThis = shared_from_this()](const auto& handshakeEc) {
					pThis->handleHandshake(handshakeEc);
				});
			}

			void handleHandshake(const boost::system::error_code& ec) {
				if (ec) {
					CATAPULT_LOG(warning) << "socket " << m_pSocket->id() << ": async_handshake returned an error: " << ec.message();
					return m_accept(SslPacketSocketInfo());
				}

				CATAPULT_LOG(debug) << "invoking user callback after successful async_accept " << m_pSocket->id();
				m_pSocket->markOpen();
				return m_accept(SslPacketSocketInfo(m_host, m_pSocket->publicKey(), m_pSocket));
			}

		private:
			boost::asio::io_context& m_ioContext;
			boost::asio::ip::tcp::acceptor& m_acceptor;
			SslAcceptCallback m_accept;
			PacketSocketOptions m_options;
			std::string m_host;
			std::shared_ptr<StrandedSslPacketSocket> m_pSocket;
		};
	}

	void Accept(
			boost::asio::io_context& ioContext,
			boost::asio::ip::tcp::acceptor& acceptor,
			const PacketSocketOptions& options,
			const SslAcceptCallback& accept) {
		auto pHandler = std::make_shared<AcceptHandler>(ioContext, acceptor, options, accept);
		pHandler->start();
	}

	// endregion

	// region Connect

	namespace {
		// basic connect handler implementation using an implicit strand
		template<typename TCallbackWrapper>
		class BasicConnectHandler final {
		private:
			using Resolver = boost::asio::ip::tcp::resolver;

		public:
			BasicConnectHandler(
					boost::asio::io_context& ioContext,
					const PacketSocketOptions& options,
					const NodeEndpoint& endpoint,
					const SslConnectCallback& callback,
					TCallbackWrapper& wrapper)
					: m_callback(callback)
					, m_wrapper(wrapper)
					, m_pSocket(std::make_shared<StrandedSslPacketSocket>(
							std::make_shared<SocketGuard>(ioContext, options.SslOptions.ContextSupplier()),
							options))
					, m_resolver(ioContext)
					, m_host(endpoint.Host)
					, m_port(std::to_string(endpoint.Port))
					, m_protocols(options.OutgoingProtocols)
					, m_isCancelled(false)
			{}

		public:
			void start() {
				m_resolver.async_resolve(m_host, m_port, m_wrapper.wrap([this](const auto& ec, const auto& results) {
					this->handleResolve(ec, results);
				}));
			}

			void cancel() {
				m_isCancelled = true;
				m_resolver.cancel();
				m_pSocket->close();
			}

		public:
			StrandedSslPacketSocket& impl() {
				return *m_pSocket;
			}

		private:
			void handleResolve(const boost::system::error_code& ec, const Resolver::results_type& results) {
				if (shouldAbort(ec, "resolving address"))
					return invokeCallback(ConnectResult::Resolve_Error);

				auto foundMatchingProtocol = false;
				for (const auto& entry : results) {
					auto endpoint = entry.endpoint();
					if (HasFlag(IpProtocol::IPv4, m_protocols) && boost::asio::ip::tcp::v4() == endpoint.protocol()) {
						m_endpoint = endpoint;
						foundMatchingProtocol = true;
						break;
					}

					if (HasFlag(IpProtocol::IPv6, m_protocols) && boost::asio::ip::tcp::v6() == endpoint.protocol()) {
						// select first IPv6 endpoint
						if (!foundMatchingProtocol) {
							m_endpoint = endpoint;
							foundMatchingProtocol = true;
						}

						// prefer IPv4, so only break if it's not configured
						if (!HasFlag(IpProtocol::IPv4, m_protocols))
							break;
					}
				}

				if (!foundMatchingProtocol)
					return invokeCallback(ConnectResult::Resolve_Error);

				m_pSocket->impl().lowest_layer().async_connect(m_endpoint, m_wrapper.wrap([this](const auto& connectEc) {
					this->handleConnect(connectEc);
				}));
			}

			void handleConnect(const boost::system::error_code& ec) {
				if (shouldAbort(ec, "connecting to"))
					return invokeCallback(ConnectResult::Connect_Error);

				m_pSocket->impl().async_handshake(SslSocket::client, m_wrapper.wrap([this](const auto& handshakeEc) {
					this->handleHandshake(handshakeEc);
				}));
			}

			void handleHandshake(const boost::system::error_code& ec) {
				if (shouldAbort(ec, "handshaking with"))
					return invokeCallback(ConnectResult::Handshake_Error);

				CATAPULT_LOG(info) << "connected to " << m_host << " [" << m_endpoint << "] " << m_pSocket->id();
				return invokeCallback(ConnectResult::Connected);
			}

			bool shouldAbort(const boost::system::error_code& ec, const char* operation) {
				if (!ec && !m_isCancelled)
					return false;

				std::ostringstream socketId;
				if (m_pSocket)
					socketId << "socket " << m_pSocket->id() << ": ";

				CATAPULT_LOG(error)
						<< socketId.str() << "failed when " << operation << " '" << m_host << "': " << ec.message()
						<< " (cancelled? " << m_isCancelled << ")";
				return true;
			}

			void invokeCallback(ConnectResult result) {
				// if the cancelled flag is set, override the result
				auto callbackResult = m_isCancelled ? ConnectResult::Connect_Cancelled : result;
				if (ConnectResult::Connected == callbackResult) {
					m_pSocket->setOptions();
					m_pSocket->markOpen();
					m_callback(callbackResult, SslPacketSocketInfo(m_endpoint.address().to_string(), m_pSocket->publicKey(), m_pSocket));
				} else {
					m_callback(callbackResult, SslPacketSocketInfo());
				}
			}

		private:
			SslConnectCallback m_callback;
			TCallbackWrapper& m_wrapper;

			std::shared_ptr<StrandedSslPacketSocket> m_pSocket;
			Resolver m_resolver;
			std::string m_host;
			std::string m_port;
			IpProtocol m_protocols;
			bool m_isCancelled;
			boost::asio::ip::tcp::endpoint m_endpoint;
		};

		// implements connect handler using an explicit strand and ensures deterministic shutdown by using enable_shared_from_this
		class StrandedConnectHandler : public std::enable_shared_from_this<StrandedConnectHandler> {
		public:
			StrandedConnectHandler(
					boost::asio::io_context& ioContext,
					const PacketSocketOptions& options,
					const NodeEndpoint& endpoint,
					const SslConnectCallback& callback)
					: m_handler(ioContext, options, endpoint, callback, *this)
					, m_strandWrapper(m_handler.impl().strand()) // use the socket's strand
			{}

		public:
			void start() {
				post([](auto& handler) { handler.start(); });
			}

			void cancel() {
				post([](auto& handler) { handler.cancel(); });
			}

		public:
			template<typename THandler>
			auto wrap(THandler handler) {
				return m_strandWrapper.wrap(shared_from_this(), handler);
			}

		private:
			template<typename THandler>
			void post(THandler handler) {
				return m_strandWrapper.post(shared_from_this(), [handler](const auto& pThis) {
					handler(pThis->m_handler);
				});
			}

		private:
			BasicConnectHandler<StrandedConnectHandler> m_handler;
			thread::StrandOwnerLifetimeExtender<StrandedConnectHandler> m_strandWrapper;
		};
	}

	action Connect(
			boost::asio::io_context& ioContext,
			const PacketSocketOptions& options,
			const NodeEndpoint& endpoint,
			const SslConnectCallback& callback) {
		auto pHandler = std::make_shared<StrandedConnectHandler>(ioContext, options, endpoint, callback);
		pHandler->start();
		return [pHandler] { pHandler->cancel(); };
	}

	// endregion
}}
