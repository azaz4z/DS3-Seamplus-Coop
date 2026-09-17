#include "session.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace ds3sc::coop {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'D', 'S', '3', 'S'};
constexpr std::size_t kHelloSize = 28;

void Put16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Put32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i != 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void Put64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i != 8; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint16_t Get16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
}

std::uint32_t Get32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i != 4; ++i) value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8);
    return value;
}

std::uint64_t Get64(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i != 8; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
    return value;
}

std::uint32_t PasswordTag(std::string_view password) noexcept {
    std::uint32_t hash = 2166136261u;
    for (const auto ch : password) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= 16777619u;
    }
    return hash;
}

} // namespace

Session::Session(SessionOptions options) : options_(std::move(options)) {
    if (options_.maximumPeers == 0) options_.maximumPeers = 1;
    if (options_.heartbeatIntervalMs == 0) options_.heartbeatIntervalMs = 1'000;
    if (options_.peerTimeoutMs <= options_.heartbeatIntervalMs)
        options_.peerTimeoutMs = options_.heartbeatIntervalMs * 3;
    if (options_.reconnectBaseDelayMs == 0) options_.reconnectBaseDelayMs = 500;
    if (options_.reconnectMaximumDelayMs < options_.reconnectBaseDelayMs)
        options_.reconnectMaximumDelayMs = options_.reconnectBaseDelayMs;
    if (options_.sendFailureThreshold == 0) options_.sendFailureThreshold = 1;
}

bool Session::StartHost(std::uint64_t localSteamId) {
    std::scoped_lock lock(mutex_);
    if (state_.phase != SessionPhase::offline || localSteamId == 0) return false;
    state_ = {SessionRole::host, SessionPhase::lobby, localSteamId, state_.generation + 1, {}};
    state_.peers.push_back({localSteamId, true, true});
    return true;
}

bool Session::StartGuest(std::uint64_t hostSteamId) {
    std::scoped_lock lock(mutex_);
    if (state_.phase != SessionPhase::offline || hostSteamId == 0) return false;
    state_ = {SessionRole::guest, SessionPhase::connecting, hostSteamId, state_.generation + 1, {}};
    state_.peers.push_back({hostSteamId, false, false});
    return true;
}

bool Session::AddPeer(std::uint64_t steamId) {
    std::scoped_lock lock(mutex_);
    if (state_.phase == SessionPhase::offline || steamId == 0 || state_.peers.size() >= options_.maximumPeers) return false;
    if (std::ranges::any_of(state_.peers, [steamId](const Peer& peer) { return peer.steamId == steamId; })) return false;
    state_.peers.push_back({steamId, false, false});
    return true;
}

bool Session::MarkConnected(std::uint64_t steamId, std::uint64_t nowMs) {
    std::scoped_lock lock(mutex_);
    const auto found = std::ranges::find(state_.peers, steamId, &Peer::steamId);
    if (found == state_.peers.end()) return false;
    found->connected = true;
    found->active = true;
    found->lastReceiveMs = nowMs;
    found->nextHeartbeatMs = nowMs + options_.heartbeatIntervalMs;
    found->nextReconnectMs = 0;
    found->consecutiveSendFailures = 0;
    found->reconnectAttempts = 0;
    if (state_.phase == SessionPhase::connecting || state_.phase == SessionPhase::lobby) state_.phase = SessionPhase::active;
    return true;
}

bool Session::NotePacket(std::uint64_t steamId, std::uint64_t nowMs) {
    std::scoped_lock lock(mutex_);
    const auto found = std::ranges::find(state_.peers, steamId, &Peer::steamId);
    if (found == state_.peers.end()) return false;
    found->active = true;
    found->connected = true;
    found->lastReceiveMs = nowMs;
    found->nextHeartbeatMs = nowMs + options_.heartbeatIntervalMs;
    found->nextReconnectMs = 0;
    found->consecutiveSendFailures = 0;
    found->reconnectAttempts = 0;
    if (state_.phase == SessionPhase::connecting || state_.phase == SessionPhase::lobby)
        state_.phase = SessionPhase::active;
    return true;
}

bool Session::NoteSendResult(std::uint64_t steamId, bool success, std::uint64_t nowMs) {
    std::scoped_lock lock(mutex_);
    const auto found = std::ranges::find(state_.peers, steamId, &Peer::steamId);
    if (found == state_.peers.end()) return false;
    // Broadcasts can encounter a peer already waiting for reconnect. Do not
    // move its retry deadline forward every time another world update occurs.
    if (!found->connected) return true;
    if (success) {
        found->consecutiveSendFailures = 0;
        return true;
    }
    if (found->consecutiveSendFailures != std::numeric_limits<std::uint32_t>::max())
        ++found->consecutiveSendFailures;
    if (found->consecutiveSendFailures >= options_.sendFailureThreshold) {
        found->active = false;
        found->connected = false;
        found->nextReconnectMs = nowMs + options_.reconnectBaseDelayMs;
        UpdatePhaseLocked();
    }
    return true;
}

std::vector<PeerAction> Session::Tick(std::uint64_t nowMs) {
    std::scoped_lock lock(mutex_);
    std::vector<PeerAction> actions;
    for (auto& peer : state_.peers) {
        const bool localHost = state_.role == SessionRole::host && peer.steamId == state_.host;
        if (localHost) continue;
        if (peer.connected) {
            const bool timedOut = nowMs >= peer.lastReceiveMs &&
                                  nowMs - peer.lastReceiveMs >= options_.peerTimeoutMs;
            if (timedOut) {
                peer.active = false;
                peer.connected = false;
                peer.nextReconnectMs = nowMs + options_.reconnectBaseDelayMs;
                continue;
            }
            if (nowMs >= peer.nextHeartbeatMs) {
                actions.push_back({PeerActionType::heartbeat, peer.steamId});
                peer.nextHeartbeatMs = nowMs + options_.heartbeatIntervalMs;
            }
            continue;
        }
        if (nowMs < peer.nextReconnectMs) continue;
        if (options_.maximumReconnectAttempts != 0 &&
            peer.reconnectAttempts >= options_.maximumReconnectAttempts) continue;
        actions.push_back({PeerActionType::reconnect, peer.steamId});
        const auto shift = std::min<std::uint32_t>(peer.reconnectAttempts, 20);
        const auto limit = options_.reconnectMaximumDelayMs;
        const auto delay = options_.reconnectBaseDelayMs > (limit >> shift)
            ? limit : std::min(limit, options_.reconnectBaseDelayMs << shift);
        peer.nextReconnectMs = nowMs + delay;
        if (peer.reconnectAttempts != std::numeric_limits<std::uint32_t>::max())
            ++peer.reconnectAttempts;
    }
    UpdatePhaseLocked();
    return actions;
}

void Session::UpdatePhaseLocked() noexcept {
    if (state_.phase == SessionPhase::offline || state_.phase == SessionPhase::closing) return;
    if (state_.role == SessionRole::guest) {
        const auto host = std::ranges::find(state_.peers, state_.host, &Peer::steamId);
        state_.phase = host != state_.peers.end() && host->active && host->connected
            ? SessionPhase::active : SessionPhase::connecting;
        return;
    }
    const bool hasConnectedGuest = std::ranges::any_of(state_.peers, [this](const Peer& peer) {
        return peer.steamId != state_.host && peer.active && peer.connected;
    });
    state_.phase = hasConnectedGuest ? SessionPhase::active : SessionPhase::lobby;
}

bool Session::RemovePeer(std::uint64_t steamId) {
    std::scoped_lock lock(mutex_);
    const auto found = std::ranges::find(state_.peers, steamId, &Peer::steamId);
    if (found == state_.peers.end()) return false;
    if (steamId == state_.host) {
        state_.phase = SessionPhase::closing;
        return true;
    }
    state_.peers.erase(found);
    if (state_.peers.size() <= 1 && state_.role == SessionRole::host) state_.phase = SessionPhase::lobby;
    return true;
}

void Session::Stop() noexcept {
    std::scoped_lock lock(mutex_);
    state_.phase = SessionPhase::offline;
    state_.host = 0;
    state_.peers.clear();
}

SessionSnapshot Session::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::vector<network::PeerState> Session::TransportPeers() const {
    std::scoped_lock lock(mutex_);
    std::vector<network::PeerState> peers;
    peers.reserve(state_.peers.size());
    for (const auto& peer : state_.peers) {
        std::uint64_t flags = peer.active ? 1ull : 0ull;
        if (peer.connected) flags |= 2ull;
        peers.push_back({peer.steamId, flags});
    }
    return peers;
}

bool Session::HasPeer(std::uint64_t steamId) const {
    std::scoped_lock lock(mutex_);
    return std::ranges::any_of(state_.peers,
        [steamId](const Peer& peer) { return peer.steamId == steamId; });
}

bool Session::IsConnected(std::uint64_t steamId) const {
    std::scoped_lock lock(mutex_);
    const auto found = std::ranges::find(state_.peers, steamId, &Peer::steamId);
    return found != state_.peers.end() && found->active && found->connected;
}

std::vector<std::uint8_t> EncodeHello(SessionRole role, std::uint16_t protocolVersion,
                                      std::uint64_t steamId, std::span<const std::uint8_t> nonce,
                                      std::string_view password) {
    std::vector<std::uint8_t> bytes(kHelloSize, 0);
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    bytes[4] = static_cast<std::uint8_t>(role);
    Put16(bytes, 5, protocolVersion);
    Put64(bytes, 7, steamId);
    std::uint64_t nonceValue = 0;
    for (std::size_t i = 0; i != std::min(nonce.size(), sizeof(nonceValue)); ++i) nonceValue |= static_cast<std::uint64_t>(nonce[i]) << (i * 8);
    Put64(bytes, 15, nonceValue);
    Put32(bytes, 23, PasswordTag(password));
    return bytes;
}

bool DecodeHello(std::span<const std::uint8_t> bytes, Hello& result) noexcept {
    if (bytes.size() != kHelloSize || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) || bytes[4] > 1) return false;
    result.role = static_cast<SessionRole>(bytes[4]);
    result.protocolVersion = Get16(bytes, 5);
    result.steamId = Get64(bytes, 7);
    result.nonce = Get64(bytes, 15);
    result.passwordTag = Get32(bytes, 23);
    return result.steamId != 0 && result.protocolVersion != 0;
}

bool VerifyHello(const Hello& hello, std::uint16_t expectedProtocolVersion,
                 std::string_view password) noexcept {
    return hello.protocolVersion == expectedProtocolVersion &&
           hello.passwordTag == PasswordTag(password);
}

} // namespace ds3sc::coop
