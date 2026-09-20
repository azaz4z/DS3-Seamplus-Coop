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
    const uint16_t targetPort = (localPort != 0) ? localPort : kDefaultLanPort;
    bindAddr.sin_port = htons(targetPort);

    if (bind(socket_, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        // Fall back to dynamic port only if preferred port cannot be bound
        bindAddr.sin_port = 0;
        if (bind(socket_, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            WSACleanup();
            return false;
        }
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

    {
        std::lock_guard lock(peersMutex_);
        peerToAddress_.clear();
        addressKeyToPeer_.clear();
    }

    {
        std::lock_guard lock(queueMutex_);
        incomingQueue_.clear();
    }
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

int LanTransport::SendFramed(uint64_t targetPeer, uint64_t senderSteamId, int channel,
                             const void* data, size_t size) noexcept {
    if (!isInitialized_.load() || socket_ == INVALID_SOCKET) return -1;

    std::vector<uint8_t> buffer(sizeof(LanPacketHeader) + size);
    auto* header = reinterpret_cast<LanPacketHeader*>(buffer.data());
    header->magic = kDs3scLanPacketMagic;
    header->senderSteamId = senderSteamId;
    header->targetSteamId = targetPeer;
    header->channel = static_cast<uint16_t>(channel);
    header->payloadLength = static_cast<uint32_t>(size);

    if (size > 0 && data != nullptr) {
        std::memcpy(buffer.data() + sizeof(LanPacketHeader), data, size);
    }

    sockaddr_in targetAddr{};
    bool found = false;
    {
        std::lock_guard lock(peersMutex_);
        auto it = peerToAddress_.find(targetPeer);
        if (it != peerToAddress_.end()) {
            targetAddr = it->second;
            found = true;
        }
    }

    if (!found) {
        // Fallback: broadcast packet if peer address is unknown
        targetAddr.sin_family = AF_INET;
        targetAddr.sin_port = htons(boundPort_ != 0 ? boundPort_ : kDefaultLanPort);
        targetAddr.sin_addr.s_addr = INADDR_BROADCAST;
    }

    const int sent = sendto(socket_, reinterpret_cast<const char*>(buffer.data()),
                            static_cast<int>(buffer.size()), 0,
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

void LanTransport::PollIncomingPackets() noexcept {
    if (!isInitialized_.load() || socket_ == INVALID_SOCKET) return;

    uint8_t tempBuf[65536];
    sockaddr_in fromAddr{};
    int fromLen = sizeof(fromAddr);

    while (true) {
        int bytesRead = recvfrom(socket_, reinterpret_cast<char*>(tempBuf), sizeof(tempBuf), 0,
                                 reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (bytesRead <= 0) break;

        // Check if this is a framed data packet
        if (bytesRead >= static_cast<int>(sizeof(LanPacketHeader))) {
            const auto* header = reinterpret_cast<const LanPacketHeader*>(tempBuf);
            if (header->magic == kDs3scLanPacketMagic) {
                // Auto-register peer
                if (header->senderSteamId != 0) {
                    RegisterPeer(header->senderSteamId, fromAddr);
                }

                const size_t payloadSize = bytesRead - sizeof(LanPacketHeader);
                QueuedLanPacket queued{};
                queued.senderSteamId = header->senderSteamId;
                queued.channel = header->channel;
                if (payloadSize > 0) {
                    queued.payload.assign(tempBuf + sizeof(LanPacketHeader), tempBuf + bytesRead);
                }

                std::lock_guard lock(queueMutex_);
                incomingQueue_.push_back(std::move(queued));
                continue;
            }
        }

        // Check if beacon
        if (bytesRead == sizeof(LanBeaconPacket)) {
            const auto* beacon = reinterpret_cast<const LanBeaconPacket*>(tempBuf);
            if (beacon->magic == kDs3scLanMagic && beacon->hostSteamId != 0) {
                RegisterPeer(beacon->hostSteamId, fromAddr);
            }
        }
    }
}

bool LanTransport::IsPacketAvailable(int channel, uint32_t* outSize) noexcept {
    PollIncomingPackets();
    std::lock_guard lock(queueMutex_);
    for (const auto& pkt : incomingQueue_) {
        if (channel < 0 || pkt.channel == channel) {
            if (outSize) *outSize = static_cast<uint32_t>(pkt.payload.size());
            return true;
        }
    }
    return false;
}

bool LanTransport::ReadPacket(void* pubDest, uint32_t cubDest, uint32_t* pcubMsgSize,
                              uint64_t* psteamIDRemote, int channel) noexcept {
    PollIncomingPackets();
    std::lock_guard lock(queueMutex_);
    for (auto it = incomingQueue_.begin(); it != incomingQueue_.end(); ++it) {
        if (channel < 0 || it->channel == channel) {
            const uint32_t copyLen = (std::min)(cubDest, static_cast<uint32_t>(it->payload.size()));
            if (pubDest && copyLen > 0) {
                std::memcpy(pubDest, it->payload.data(), copyLen);
            }
            if (pcubMsgSize) *pcubMsgSize = static_cast<uint32_t>(it->payload.size());
            if (psteamIDRemote) *psteamIDRemote = it->senderSteamId;

            incomingQueue_.erase(it);
            return true;
        }
    }
    return false;
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
                                          uint32_t passwordHash, uint16_t port,
                                          uint64_t hostSteamId) noexcept {
    StopBeaconBroadcaster();
    if (!isInitialized_.load() || socket_ == INVALID_SOCKET) return;

    beaconRunning_.store(true);
    beaconThread_ = std::thread([this, sessionName, passwordHash, port, hostSteamId]() {
        LanBeaconPacket packet{};
        packet.magic = kDs3scLanMagic;
        packet.version = 1;
        packet.port = port;
        packet.passwordHash = passwordHash;
        packet.hostSteamId = (hostSteamId != 0) ? hostSteamId : localSteamId_;
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
                                uint64_t& outHostSteamId, uint32_t expectedPasswordHash,
                                uint32_t timeoutMs) noexcept {
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
                outHostSteamId = beacon.hostSteamId;
                RegisterPeer(beacon.hostSteamId, fromAddr);
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace ds3sc::network
