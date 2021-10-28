#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace nioev {

class TcpClientConnection final {
public:
    TcpClientConnection(int sockFd, std::string remoteIp, uint16_t remotePort);
    ~TcpClientConnection();

    TcpClientConnection(const TcpClientConnection&) = delete;
    void operator=(const TcpClientConnection&) = delete;

    TcpClientConnection(TcpClientConnection&&) noexcept;
    TcpClientConnection& operator=(TcpClientConnection&&) = delete;

    [[nodiscard]] const std::string& getRemoteIp() const {
        return mRemoteIp;
    }
    [[nodiscard]] uint16_t getRemotePort() const {
        return mRemotePort;
    }
    [[nodiscard]] int getFd() const {
        return mSockFd;
    }
    uint send(const uint8_t* data, uint len);
    uint recv(std::vector<uint8_t>& buffer);

private:
    int mSockFd;
    std::string mRemoteIp;
    uint16_t mRemotePort;
};

}
