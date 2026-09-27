#include <winsock2.h>
#include <array>
#include <chrono>
#include <vector>

#include "net/server.h"

#include "logger.h"
#include "adb.h"
#include "net/serializer.h"

Server::Server(int port, const ConnectionListener& connectionListener) :
	port(port),
	connectionListener(connectionListener),
	acceptor(context)
{
	try {
		// The legacy Windows path is supported only through the authorized ADB
		// reverse tunnel. Do not expose its unauthenticated protocol to the LAN.
		tcp::endpoint endpoint(asio::ip::address_v4::loopback(), port);
		acceptor.open(endpoint.protocol());

		acceptor.set_option(tcp::acceptor::reuse_address(true));

		acceptor.bind(endpoint);
		acceptor.listen();

		logger << "[SERVER] Initialized on port " << port << std::endl;
		
		adb::reverse(port);
		adb::forward(8554);
	}
	catch(std::exception& e)
	{
		logger << "[SERVER] CRITICAL INIT ERROR: " << e.what() << std::endl;
		throw;
	}
}

Server::HostInfo Server::GetHostInfo()
{
	std::string name = asio::ip::host_name();
	return { name, "127.0.0.1", std::to_string(port) };
}

void Server::Send(int id, const unsigned char* bytes, size_t size) const
{
	if (id >= 0 && id < connections.size())
	{
		connections[id]->Send(bytes, size);
	}
}

void Server::Start()
{
	if (!acceptor.is_open()) 
	{
		logger << "[SERVER] Cannot start: Acceptor is not open.\n";
		return;
	}

	try
	{
		TCPDoAccept();

		thread = std::thread([this]() {
			while (true)
			{
				try {
					context.run();
					break;
				}
				catch (std::exception& e) {
					logger << "[SERVER] CRITICAL EXCEPTION in IO Thread: " << e.what() << std::endl;
				}
				catch (...) {
					logger << "[SERVER] Unknown exception in IO Thread.\n";
				}
			}
		});
		
		logger << "[SERVER] Started" << std::endl;
	}
	catch (std::exception e)
	{
		logger << "[SERVER] Start failed: " << e.what() << "\n";
	}
}

void Server::Close()
{
	logger << "[SERVER] Closing...\n";

	asio::error_code ec;
	acceptor.close(ec);
	if (ec) logger << "[SERVER] Error closing acceptor: " << ec.message() << "\n";
	
	for (std::shared_ptr<Connection> conn : connections)
	{
		conn->Close(true);
	}

	context.stop();

	if (thread.joinable())
	{
		thread.join();
	}

	logger << "[SERVER] Closed.\n";

	adb::kill(port);
}

void Server::TCPDoAccept()
{
	acceptor.async_accept([&, this](asio::error_code ec, tcp::socket socket) {
		if (!ec)
		{
			try
			{
				logger << "[SERVER] Device connected over loopback/ADB.\n";
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
				auto readExact = [&](uint8_t* destination, size_t size) {
					size_t offset = 0;
					while (offset < size)
					{
						const auto now = std::chrono::steady_clock::now();
						if (now >= deadline) throw std::runtime_error("Descriptor timeout");
						const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
						timeval timeout{};
						timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
						timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
						fd_set readable;
						FD_ZERO(&readable);
						FD_SET(socket.native_handle(), &readable);
						if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
							throw std::runtime_error("Descriptor timeout");
						const size_t count = socket.read_some(asio::buffer(destination + offset, size - offset));
						if (count == 0) throw std::runtime_error("Descriptor EOF");
						offset += count;
					}
				};
				std::array<uint8_t, 4> lengthBytes{};
				readExact(lengthBytes.data(), lengthBytes.size());
				const uint32_t descriptorLength =
					(static_cast<uint32_t>(lengthBytes[0]) << 24) |
					(static_cast<uint32_t>(lengthBytes[1]) << 16) |
					(static_cast<uint32_t>(lengthBytes[2]) << 8) |
					static_cast<uint32_t>(lengthBytes[3]);
				if (descriptorLength == 0 || descriptorLength > 64 * 1024)
					throw std::runtime_error("Invalid descriptor frame length");
				std::vector<uint8_t> received(descriptorLength);
				readExact(received.data(), received.size());
				DeviceDescriptor descriptor = Serializer::DeserializeDeviceDescriptor(
					received.data(), received.size());

				auto conn = std::make_shared<Connection>(
					std::move(socket), descriptor,
					std::bind(&Server::OnConnectionDisconnected, this, std::placeholders::_1),
					std::bind(&Server::OnConnectionReportingError, this, std::placeholders::_1, std::placeholders::_2));
				connections.push_back(conn);
				conn->Read();
				connectionListener.OnDeviceConnected(descriptor);
			}
			catch (const std::exception&)
			{
				logger << "[SERVER] Rejected invalid or incomplete descriptor.\n";
				asio::error_code ignored;
				socket.close(ignored);
			}
		}
		else if (ec != asio::error::operation_aborted)
		{
			logger << "[SERVER] Accept Error: " << ec.message() << std::endl;
		}

		if (acceptor.is_open()) {
			TCPDoAccept();
		}
	});
}

void Server::OnConnectionDisconnected(std::shared_ptr<Connection> connection)
{
	logger << "[SERVER] Device disconnected." << std::endl;

	connections.erase(std::remove(connections.begin(), connections.end(), connection), connections.end());
	connectionListener.OnDeviceDisconnected(connection->descriptor);
}

void Server::OnConnectionReportingError(std::shared_ptr<Connection> connection, const Connection::ErrorReport& report)
{
	connectionListener.OnDeviceErrorReported(connection->descriptor, report);
}
