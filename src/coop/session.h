#pragma once

#include "../network/sender.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ds3sc::coop {

enum class SessionRole : std::uint8_t { host = 0, guest = 1 };
enum class SessionPhase : std::uint8_t { offline, lobby, connecting, active, closing };

struct SessionOptions {
    std::string password;
    std::size_t maximumPeers = 5;
    std::uint16_t protocolVersion = 1;
    std::uint64_t heartbeatIntervalMs = 1'000;
    std::uint64_t peerTimeoutMs = 7'000;
    std::uint64_t reconnectBaseDelayMs = 500;
    std::uint64_t reconnectMaximumDelayMs = 8'000;
    std::uint32_t sendFailureThreshold = 3;
    std::uint32_t maximumReconnectAttempts = 8;
};

struct Peer {
    std::uint64_t steamId = 0;
    bool active = false;
    bool connected = false;
    std::uint64_t lastReceiveMs = 0;
    std::uint64_t nextHeartbeatMs = 0;
    std::uint64_t nextReconnectMs = 0;
    std::uint32_t consecutiveSendFailures = 0;
    std::uint32_t reconnectAttempts = 0;
};

enum class PeerActionType : std::uint8_t { heartbeat, reconnect };

struct PeerAction {
    PeerActionType type;
    std::uint64_t steamId;
};

struct SessionSnapshot {
    SessionRole role = SessionRole::guest;
    SessionPhase phase = SessionPhase::offline;
    std::uint64_t host = 0;
    std::uint64_t generation = 0;
    std::vector<Peer> peers;
};

// The original DLL uses Steam lobby callbacks, but its private callback
// object layout is not a stable public ABI. This state machine keeps that
// boundary explicit and makes the recovered session behaviour testable.
class Session final {
public:
    explicit Session(SessionOptions options = {});

    bool StartHost(std::uint64_t localSteamId);
    bool StartGuest(std::uint64_t hostSteamId);
    bool AddPeer(std::uint64_t steamId);
    bool MarkConnected(std::uint64_t steamId, std::uint64_t nowMs);
    bool NotePacket(std::uint64_t steamId, std::uint64_t nowMs);
    bool NoteSendResult(std::uint64_t steamId, bool success, std::uint64_t nowMs);
    [[nodiscard]] std::vector<PeerAction> Tick(std::uint64_t nowMs);
    bool RemovePeer(std::uint64_t steamId);
    void Stop() noexcept;

    [[nodiscard]] SessionSnapshot Snapshot() const;
    [[nodiscard]] std::vector<network::PeerState> TransportPeers() const;
    [[nodiscard]] bool HasPeer(std::uint64_t steamId) const;
    [[nodiscard]] bool IsConnected(std::uint64_t steamId) const;
    [[nodiscard]] const SessionOptions& Options() const noexcept { return options_; }

private:
    void UpdatePhaseLocked() noexcept;

    SessionOptions options_;
    mutable std::mutex mutex_;
    SessionSnapshot state_;
};

std::vector<std::uint8_t> EncodeHello(SessionRole role, std::uint16_t protocolVersion,
                                      std::uint64_t steamId, std::span<const std::uint8_t> nonce,
                                      std::string_view password);

struct Hello {
    SessionRole role;
    std::uint16_t protocolVersion;
    std::uint64_t steamId;
    std::uint64_t nonce;
    std::uint32_t passwordTag;
};

bool DecodeHello(std::span<const std::uint8_t> bytes, Hello& result) noexcept;
bool VerifyHello(const Hello& hello, std::uint16_t expectedProtocolVersion,
                 std::string_view password) noexcept;

} // namespace ds3sc::coop
