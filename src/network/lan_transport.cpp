#include "lan_transport.h"

#include <chrono>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace ds3sc::network {

namespace {

std::string MakeAddressKey(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &(addr.sin_addr), ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

} // namespace

LanTransport& LanTransport::Instance() noexcept {
    static LanTransport instance;
    return instance;
}

LanTransport::LanTransport() = default;

LanTransport::~LanTransport() {
    Shutdown();
}

bool LanTransport::Initialize(uint16_t localPort, bool isHost) noexcept {
    if (isInitialized_.load()) {
        if (boundPort_ == localPort) return true;
        Shutdown();
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    // Enable Broadcast
    BOOL broadcastOpt = TRUE;
    setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcastOpt), sizeof(broadcastOpt));

    // Enable Address Reuse
    BOOL reuseOpt = TRUE;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseOpt), sizeof(reuseOpt));

    // Set Non-blocking
    u_long nonBlocking = 1;
    ioctlsocket(socket_, FIONBIO, &nonBlocking);

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(isHost ? localPort : 0);

    if (bind(socket_, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    sockaddr_in actualAddr{};
    int addrLen = sizeof(actualAddr);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&actualAddr), &addrLen) == 0) {
        boundPort_ = ntohs(actualAddr.sin_port);
    } else {
        boundPort_ = localPort;
    }

    isHost_ = isHost;
    isInitialized_.store(true);
    return true;
}

void LanTransport::Shutdown() noexcept {
    StopBeaconBroadcaster();

    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    if (isInitialized_.exchange(false)) {
        WSACleanup();
    }

    std::lock_guard lock(peersMutex_);
    peerToAddress_.clear();
    addressKeyToPeer_.clear();
}

int LanTransport::Send(std::uint64_t peer, std::span<const std::uint8_t> packet,
                       int /*flags*/, int /*channel*/) {
    if (!isInitialized_.load() || socket_ == INVALID_SOCKET) return -1;

    sockaddr_in targetAddr{};
    {
        std::lock_guard lock(peersMutex_);
        auto it = peerToAddress_.find(peer);
        if (it == peerToAddress_.end()) return -1;
        targetAddr = it->second;
    }

    const int sent = sendto(socket_, reinterpret_cast<const char*>(packet.data()),
                            static_cast<int>(packet.size()), 0,
                            reinterpret_cast<const sockaddr*>(&targetAddr), sizeof(targetAddr));
    return (sent >= 0) ? sent : -1;
}

bool LanTransport::Reconnect(std::uint64_t /*peer*/) {
    return isInitialized_.load();
}

void LanTransport::RegisterPeer(std::uint64_t peerId, const sockaddr_in& address) noexcept {
    std::lock_guard lock(peersMutex_);
    peerToAddress_[peerId] = address;
    addressKeyToPeer_[MakeAddressKey(address)] = peerId;
}

void LanTransport::UnregisterPeer(std::uint64_t peerId) noexcept {
    std::lock_guard lock(peersMutex_);
    auto it = peerToAddress_.find(peerId);
    if (it != peerToAddress_.end()) {
        addressKeyToPeer_.erase(MakeAddressKey(it->second));
        peerToAddress_.erase(it);
    }
}

int LanTransport::ReceivePacket(std::uint64_t& outPeer, std::vector<uint8_t>& outBuffer,
                                sockaddr_in& outFromAddr) noexcept {
    if (!isInitialized_.load() || socket_ == INVALID_SOCKET) return -1;

    uint8_t tempBuf[4096];
    sockaddr_in fromAddr{};
    int fromLen = sizeof(fromAddr);

    int bytesRead = recvfrom(socket_, reinterpret_cast<char*>(tempBuf), sizeof(tempBuf), 0,
                             reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

    if (bytesRead <= 0) return bytesRead;

    outFromAddr = fromAddr;
    outBuffer.assign(tempBuf, tempBuf + bytesRead);

    {
        std::lock_guard lock(peersMutex_);
        const auto key = MakeAddressKey(fromAddr);
        auto it = addressKeyToPeer_.find(key);
        if (it != addressKeyToPeer_.end()) {
            outPeer = it->second;
        } else {
            outPeer = 0;
        }
    }

    return bytesRead;
}

void LanTransport::StartBeaconBroadcaster(const std::string& sessionName,
                                          uint32_t passwordHash, uint16_t port) noexcept {
    StopBeaconBroadcaster();
    if (!isInitialized_.load() || socket_ == INVALID_SOCKET) return;

    beaconRunning_.store(true);
    beaconThread_ = std::thread([this, sessionName, passwordHash, port]() {
        LanBeaconPacket packet{};
        packet.magic = kDs3scLanMagic;
        packet.version = 1;
        packet.port = port;
        packet.passwordHash = passwordHash;
        strncpy_s(packet.sessionName, sessionName.c_str(), sizeof(packet.sessionName) - 1);

        sockaddr_in broadcastAddr{};
        broadcastAddr.sin_family = AF_INET;
        broadcastAddr.sin_port = htons(port);
        broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

        while (beaconRunning_.load()) {
            sendto(socket_, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
                   reinterpret_cast<const sockaddr*>(&broadcastAddr), sizeof(broadcastAddr));

            for (int i = 0; i < 10 && beaconRunning_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void LanTransport::StopBeaconBroadcaster() noexcept {
    if (beaconRunning_.exchange(false)) {
        if (beaconThread_.joinable()) {
            beaconThread_.join();
        }
    }
}

bool LanTransport::DiscoverHost(std::string& outHostIp, uint16_t& outHostPort,
                                uint32_t expectedPasswordHash, uint32_t timeoutMs) noexcept {
    if (!isInitialized_.load() || socket_ == INVALID_SOCKET) return false;

    const auto start = std::chrono::steady_clock::now();
    LanBeaconPacket beacon{};
    sockaddr_in fromAddr{};
    int fromLen = sizeof(fromAddr);

    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        int res = recvfrom(socket_, reinterpret_cast<char*>(&beacon), sizeof(beacon), 0,
                           reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (res == sizeof(beacon) && beacon.magic == kDs3scLanMagic) {
            if (expectedPasswordHash == 0 || beacon.passwordHash == expectedPasswordHash) {
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &(fromAddr.sin_addr), ipStr, sizeof(ipStr));
                outHostIp = ipStr;
                outHostPort = beacon.port;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace ds3sc::network
