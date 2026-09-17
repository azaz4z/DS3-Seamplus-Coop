#include "runtime.h"

#include "../network/protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace ds3sc::coop {

Runtime::Runtime(network::MessageTransport& transport, SessionOptions options,
                 int primaryChannel, int alternateChannel)
    : sender_(transport, primaryChannel, alternateChannel), session_(std::move(options)) {
    RegisterHandlers();
}

void Runtime::RegisterHandlers() {
    router_.Register(network::SendType::type3,
                     network::ToByte(network::MessageId::sessionState),
                     [](std::uint64_t, const network::PacketView&) {});
    router_.Register(network::SendType::type3,
                     network::ToByte(network::MessageId::eventFlag),
                     [this](std::uint64_t peer, const network::PacketView& packet) {
                         const auto update = WorldSync::DecodeEventFlag(packet.payload);
                         const auto snapshot = session_.Snapshot();
                         if (!update || snapshot.role != SessionRole::guest ||
                             peer != snapshot.host) return;
                         if (world_.Apply(*update) && eventCallback_) eventCallback_(peer, *update);
                     });
    router_.Register(network::SendType::type3,
                     network::ToByte(network::MessageId::worldData),
                     [this](std::uint64_t peer, const network::PacketView& packet) {
                         const auto state = BossSync::Decode(packet.payload);
                         const auto snapshot = session_.Snapshot();
                         if (!state || snapshot.role != SessionRole::guest ||
                             peer != snapshot.host) return;
                         if (bosses_.ApplyAuthoritative(*state) && bossCallback_)
                             bossCallback_(peer, *state);
                     });
}

bool Runtime::StartHost(std::uint64_t localSteamId) {
    localSteamId_ = localSteamId;
    return session_.StartHost(localSteamId);
}

bool Runtime::StartGuest(std::uint64_t hostSteamId) {
    localSteamId_ = 0;
    return session_.StartGuest(hostSteamId);
}

bool Runtime::AddPeer(std::uint64_t steamId) {
    return session_.AddPeer(steamId);
}

bool Runtime::MarkConnected(std::uint64_t steamId) {
    return MarkConnected(steamId, NowMs());
}

bool Runtime::MarkConnected(std::uint64_t steamId, std::uint64_t nowMs) {
    if (!session_.MarkConnected(steamId, nowMs)) return false;
    const auto snapshot = session_.Snapshot();
    if (snapshot.role == SessionRole::host && steamId != localSteamId_)
        SendBossSnapshot(steamId, nowMs);
    return true;
}

void Runtime::Stop() noexcept {
    session_.Stop();
    bosses_.Clear();
    localSteamId_ = 0;
}

network::DispatchResult Runtime::Receive(std::uint64_t peer,
                                         std::span<const std::uint8_t> bytes) noexcept {
    return Receive(peer, bytes, NowMs());
}

network::DispatchResult Runtime::Receive(std::uint64_t peer,
                                         std::span<const std::uint8_t> bytes,
                                         std::uint64_t nowMs) noexcept {
    if (!session_.HasPeer(peer)) return network::DispatchResult::rejectedPayload;
    const auto result = router_.Dispatch(peer, bytes);
    if (result == network::DispatchResult::delivered ||
        result == network::DispatchResult::ignoredUnknown)
        session_.NotePacket(peer, nowMs);
    return result;
}

void Runtime::Tick() {
    Tick(NowMs());
}

void Runtime::Tick(std::uint64_t nowMs) {
    constexpr std::array<std::uint8_t, 1> heartbeat{1};
    for (const auto action : session_.Tick(nowMs)) {
        if (action.type == PeerActionType::heartbeat) {
            SendToPeer(action.steamId, network::MessageId::sessionState, heartbeat, nowMs);
            continue;
        }
        if (sender_.Reconnect(action.steamId)) MarkConnected(action.steamId, nowMs);
    }
}

bool Runtime::BroadcastEventFlag(EventFlagUpdate update) {
    if (session_.Snapshot().role != SessionRole::host) return false;
    world_.Apply(update);
    const auto payload = WorldSync::EncodeEventFlag(update);
    const auto peers = session_.TransportPeers();
    bool sent = false;
    for (const auto& peer : peers) {
        if (peer.steamId == localSteamId_) continue;
        sent = SendToPeer(peer.steamId, network::MessageId::eventFlag,
                          payload, NowMs()) || sent;
    }
    return sent;
}

bool Runtime::BroadcastBossState(BossState state) {
    if (session_.Snapshot().role != SessionRole::host ||
        !bosses_.ApplyAuthoritative(state)) return false;
    const auto payload = BossSync::Encode(state);
    if (payload.empty()) return false;
    const auto peers = session_.TransportPeers();
    bool sent = false;
    const auto nowMs = NowMs();
    for (const auto& peer : peers) {
        if (peer.steamId == localSteamId_) continue;
        sent = SendToPeer(peer.steamId, network::MessageId::worldData,
                          payload, nowMs) || sent;
    }
    return sent;
}

void Runtime::SetEventCallback(EventCallback callback) {
    eventCallback_ = std::move(callback);
}

void Runtime::SetBossCallback(BossCallback callback) {
    bossCallback_ = std::move(callback);
}

bool Runtime::SendToPeer(std::uint64_t peer, network::MessageId id,
                         std::span<const std::uint8_t> payload, std::uint64_t nowMs) {
    const auto peers = session_.TransportPeers();
    constexpr std::array<std::uint32_t, 0> noType0Ids{};
    const bool sent = sender_.Send(peers, noType0Ids, peer, network::ToByte(id),
                                   network::SendType::type3, payload);
    session_.NoteSendResult(peer, sent, nowMs);
    return sent;
}

void Runtime::SendBossSnapshot(std::uint64_t peer, std::uint64_t nowMs) {
    for (const auto state : bosses_.Snapshot()) {
        const auto payload = BossSync::Encode(state);
        if (!payload.empty())
            SendToPeer(peer, network::MessageId::worldData, payload, nowMs);
    }
}

std::uint64_t Runtime::NowMs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace ds3sc::coop
