#include "net/connection.h"
#include "net/serializer.h"
#include "logger.h"

#include <algorithm>

Connection::Connection(tcp::socket socket, DeviceDescriptor& descriptor,
    OnDisconnectedListener onDisconnectedListener, OnBytesReceived onBytesReceived)
    : socket(std::move(socket)), descriptor(descriptor),
      onDisconnectedListener(std::move(onDisconnectedListener)),
      onBytesReceived(std::move(onBytesReceived))
{
    active = false;
}

void Connection::Read()
{
    auto self = shared_from_this();
    socket.async_read_some(asio::buffer(byteBuffer), [self](asio::error_code ec, size_t bytes) {
        if (ec)
        {
            self->Close();
            return;
        }
        if (self->pendingBytes.size() + bytes > 4096)
        {
            logger << "[CONNECTION] Rejected oversized error report.\n";
            self->Close();
            return;
        }
        self->pendingBytes.insert(self->pendingBytes.end(),
            self->byteBuffer.begin(), self->byteBuffer.begin() + bytes);

        while (!self->pendingBytes.empty())
        {
            size_t consumed = 0;
            try
            {
                auto report = Serializer::DeserializeErrorReport(
                    self->pendingBytes.data(), self->pendingBytes.size(), consumed);
                if (consumed == 0 || consumed > self->pendingBytes.size())
                    throw std::runtime_error("Invalid error report size");
                self->onBytesReceived(self, report);
                self->pendingBytes.erase(self->pendingBytes.begin(),
                    self->pendingBytes.begin() + consumed);
            }
            catch (const Serializer::IncompleteMessage&)
            {
                break;
            }
            catch (const std::exception&)
            {
                logger << "[CONNECTION] Rejected malformed error report.\n";
                self->Close();
                return;
            }
        }
        self->Read();
    });
}

void Connection::Send(std::string message)
{
    socket.send(asio::buffer(message));
}

void Connection::Send(const unsigned char* bytes, size_t size)
{
    if (size > 64 * 1024) throw std::length_error("Control message too large");
    socket.send(asio::buffer(bytes, size));
}

void Connection::Close(bool stoppedByServer)
{
    if (closed.exchange(true)) return;
    asio::error_code ec;
    socket.close(ec);
    if (!stoppedByServer && onDisconnectedListener)
        onDisconnectedListener(shared_from_this());
}
