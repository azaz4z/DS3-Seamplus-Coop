#pragma once

#include "sender.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ds3sc::network {

constexpr uint32_t kDs3scLanMagic = 0x43533344; // "DS3C"
constexpr uint16_t kDefaultLanPort = 27015;

#pragma pack(push, 1)
struct LanBeaconPacket {
    uint32_t magic = kDs3scLanMagic;
    uint16_t version = 1;
    uint16_t port = kDefaultLanPort;
    char sessionName[32] = {};
    uint32_t passwordHash = 0;
};
#pragma pack(pop)

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

    void RegisterPeer(std::uint64_t peerId, const sockaddr_in& address) noexcept;
    void UnregisterPeer(std::uint64_t peerId) noexcept;

    int ReceivePacket(std::uint64_t& outPeer, std::vector<uint8_t>& outBuffer, sockaddr_in& outFromAddr) noexcept;

    void StartBeaconBroadcaster(const std::string& sessionName, uint32_t passwordHash, uint16_t port) noexcept;
    void StopBeaconBroadcaster() noexcept;

    bool DiscoverHost(std::string& outHostIp, uint16_t& outHostPort, uint32_t expectedPasswordHash, uint32_t timeoutMs = 3000) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return isInitialized_.load(); }
    [[nodiscard]] uint16_t BoundPort() const noexcept { return boundPort_; }

private:
    std::atomic<bool> isInitialized_{false};
    SOCKET socket_ = INVALID_SOCKET;
    uint16_t boundPort_ = 0;
    bool isHost_ = false;

    mutable std::mutex peersMutex_;
    std::unordered_map<std::uint64_t, sockaddr_in> peerToAddress_;
    std::unordered_map<std::string, std::uint64_t> addressKeyToPeer_;

    std::atomic<bool> beaconRunning_{false};
    std::thread beaconThread_;
};

} // namespace ds3sc::network
