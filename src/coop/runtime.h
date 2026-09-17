#pragma once

#include "boss_sync.h"
#include "session.h"
#include "world_sync.h"
#include "../network/router.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ds3sc::coop {

class Runtime final {
public:
    using EventCallback = std::function<void(std::uint64_t, EventFlagUpdate)>;
    using BossCallback = std::function<void(std::uint64_t, BossState)>;

    Runtime(network::MessageTransport& transport, SessionOptions options = {},
            int primaryChannel = 0, int alternateChannel = 1);

    bool StartHost(std::uint64_t localSteamId);
    bool StartGuest(std::uint64_t hostSteamId);
    bool AddPeer(std::uint64_t steamId);
    bool MarkConnected(std::uint64_t steamId);
    bool MarkConnected(std::uint64_t steamId, std::uint64_t nowMs);
    void Stop() noexcept;

    network::DispatchResult Receive(std::uint64_t peer,
                                    std::span<const std::uint8_t> bytes) noexcept;
    network::DispatchResult Receive(std::uint64_t peer,
                                    std::span<const std::uint8_t> bytes,
                                    std::uint64_t nowMs) noexcept;
    void Tick();
    void Tick(std::uint64_t nowMs);
    bool BroadcastEventFlag(EventFlagUpdate update);
    bool BroadcastBossState(BossState state);

    void SetEventCallback(EventCallback callback);
    void SetBossCallback(BossCallback callback);
    [[nodiscard]] const Session& SessionState() const noexcept { return session_; }
    [[nodiscard]] const WorldSync& WorldState() const noexcept { return world_; }
    [[nodiscard]] const BossSync& BossStateData() const noexcept { return bosses_; }

private:
    void RegisterHandlers();
    bool SendToPeer(std::uint64_t peer, network::MessageId id,
                    std::span<const std::uint8_t> payload, std::uint64_t nowMs);
    void SendBossSnapshot(std::uint64_t peer, std::uint64_t nowMs);
    static std::uint64_t NowMs() noexcept;

    network::Sender sender_;
    Session session_;
    WorldSync world_;
    BossSync bosses_;
    network::PacketRouter router_;
    EventCallback eventCallback_;
    BossCallback bossCallback_;
    std::uint64_t localSteamId_ = 0;
};

} // namespace ds3sc::coop
