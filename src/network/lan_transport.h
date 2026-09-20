#pragma once

#include "sender.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ds3sc::network {

constexpr uint32_t kDs3scLanMagic = 0x43533344;       // "DS3C" (Beacons)
constexpr uint32_t kDs3scLanPacketMagic = 0x4453334C; // "DS3L" (Game data packets)
constexpr uint16_t kDefaultLanPort = 27015;

#pragma pack(push, 1)
struct LanBeaconPacket {
    uint32_t magic = kDs3scLanMagic;
    uint16_t version = 1;
    uint16_t port = kDefaultLanPort;
    char sessionName[32] = {};
    uint32_t passwordHash = 0;
    uint64_t hostSteamId = 0;
};

struct LanPacketHeader {
    uint32_t magic = kDs3scLanPacketMagic;
    uint64_t senderSteamId = 0;
    uint64_t targetSteamId = 0;
    uint16_t channel = 0;
    uint32_t payloadLength = 0;
};
#pragma pack(pop)

struct QueuedLanPacket {
    uint64_t senderSteamId = 0;
    int channel = 0;
    std::vector<uint8_t> payload;
};

class LanTransport final : public MessageTransport {
public:
    static LanTransport& Instance() noexcept;

    LanTransport();
    ~LanTransport() override;

    bool Initialize(uint16_t localPort = kDefaultLanPort, bool isHost = true) noexcept;
    void Shutdown() noexcept;

    int Send(std::uint64_t peer, std::span<const std::uint8_t> packet,
             int flags, int channel) override;
    bool Reconnect(std::uint64_t peer) override;

    int SendFramed(uint64_t targetPeer, uint64_t senderSteamId, int channel,
                   const void* data, size_t size) noexcept;

    void RegisterPeer(std::uint64_t peerId, const sockaddr_in& address) noexcept;
    void UnregisterPeer(std::uint64_t peerId) noexcept;

    int ReceivePacket(std::uint64_t& outPeer, std::vector<uint8_t>& outBuffer, sockaddr_in& outFromAddr) noexcept;

    void PollIncomingPackets() noexcept;
    bool IsPacketAvailable(int channel, uint32_t* outSize) noexcept;
    bool ReadPacket(void* pubDest, uint32_t cubDest, uint32_t* pcubMsgSize,
                    uint64_t* psteamIDRemote, int channel) noexcept;

    void StartBeaconBroadcaster(const std::string& sessionName, uint32_t passwordHash,
                                uint16_t port, uint64_t hostSteamId = 0) noexcept;
    void StopBeaconBroadcaster() noexcept;

    bool DiscoverHost(std::string& outHostIp, uint16_t& outHostPort, uint64_t& outHostSteamId,
                      uint32_t expectedPasswordHash, uint32_t timeoutMs = 3000) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return isInitialized_.load(); }
    [[nodiscard]] uint16_t BoundPort() const noexcept { return boundPort_; }
    [[nodiscard]] bool IsHost() const noexcept { return isHost_; }

    void SetLocalSteamId(uint64_t steamId) noexcept { localSteamId_ = steamId; }
    [[nodiscard]] uint64_t LocalSteamId() const noexcept { return localSteamId_; }

private:
    std::atomic<bool> isInitialized_{false};
    SOCKET socket_ = INVALID_SOCKET;
    uint16_t boundPort_ = 0;
    bool isHost_ = false;
    uint64_t localSteamId_ = 0;

    mutable std::mutex peersMutex_;
    std::unordered_map<std::uint64_t, sockaddr_in> peerToAddress_;
    std::unordered_map<std::string, std::uint64_t> addressKeyToPeer_;

    mutable std::mutex queueMutex_;
    std::deque<QueuedLanPacket> incomingQueue_;

    std::atomic<bool> beaconRunning_{false};
    std::thread beaconThread_;
};

} // namespace ds3sc::network

