#pragma once

#include <memory>
#include <functional>
#include <array>
#include <atomic>
#include <vector>
#include <asio.hpp>

#include "devicedescriptor.h"

/// <summary>
/// Manages a TCP connection
/// </summary>
class Connection : public std::enable_shared_from_this<Connection>
{
	friend class Server;

public:
	using tcp = asio::ip::tcp;
	using udp = asio::ip::udp;
	using OnDisconnectedListener = std::function<void(std::shared_ptr<Connection>)>;

	struct ErrorReport
	{
		enum { SEVERITY_WARNING, SERVERITY_ERROR };
		int severity;
		std::string error;
		std::string description;
	};
	using OnBytesReceived = std::function<void(std::shared_ptr<Connection>, const ErrorReport& report)>;

	Connection(tcp::socket socket, DeviceDescriptor& descriptor, OnDisconnectedListener onDisconnectedListener, OnBytesReceived onBytesReceived);
private:

	OnDisconnectedListener onDisconnectedListener;
	OnBytesReceived onBytesReceived;

	tcp::socket socket;
	std::array<unsigned char, 512> byteBuffer{};
	std::vector<uint8_t> pendingBytes;

	bool active;
	std::atomic<bool> closed{false};
	DeviceDescriptor descriptor;

	/// <summary>
	/// Connected devices only send their name once after connecting.
	/// That is handled directly by the server when creating a new connection.
	/// This function is used only to detect when a device disconnects.
	/// </summary>
	void Read();

	void Send(std::string message);
	void Send(const unsigned char* bytes, size_t size);
	void Close(bool stoppedByServer = false);
};
