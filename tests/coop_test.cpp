#include "../src/coop/runtime.h"
#include "../src/network/protocol.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace ds3sc;

namespace {
struct SentPacket {
    std::uint64_t peer;
    std::vector<std::uint8_t> bytes;
    int flags;
    int channel;
};

class Transport final : public network::MessageTransport {
public:
    int Send(std::uint64_t peer, std::span<const std::uint8_t> packet,
             int flags, int channel) override {
        packets.push_back({peer, {packet.begin(), packet.end()}, flags, channel});
        return sendSucceeds ? 1 : 0;
    }

    bool Reconnect(std::uint64_t peer) override {
        reconnectPeers.push_back(peer);
        return reconnectSucceeds;
    }

    bool sendSucceeds = true;
    bool reconnectSucceeds = false;
    std::vector<SentPacket> packets;
    std::vector<std::uint64_t> reconnectPeers;
};

coop::SessionOptions TestOptions() {
    coop::SessionOptions options;
    options.password = "secret";
    options.maximumPeers = 4;
    options.protocolVersion = 1;
    options.heartbeatIntervalMs = 1'000;
    options.peerTimeoutMs = 7'000;
    options.reconnectBaseDelayMs = 500;
    options.reconnectMaximumDelayMs = 8'000;
    options.sendFailureThreshold = 3;
    options.maximumReconnectAttempts = 8;
    return options;
}

std::vector<std::uint8_t> Frame(network::MessageId id,
                                std::span<const std::uint8_t> payload) {
    return network::Encode(network::SendType::type3, network::ToByte(id), payload);
}
} // namespace

int main() {
    Transport hostTransport;
    coop::Runtime host(hostTransport, TestOptions(), 7, 9);
    assert(host.StartHost(100));
    assert(host.AddPeer(200));
    assert(host.MarkConnected(200, 100));

    assert(host.BroadcastEventFlag({1234, true}));
    assert(hostTransport.packets.back().peer == 200);
    assert(hostTransport.packets.back().flags == 9);
    assert(hostTransport.packets.back().channel == 9);
    const auto eventPacket = hostTransport.packets.back().bytes;
    const auto decodedEvent = network::Decode(eventPacket, 64);
    assert(decodedEvent && decodedEvent->id == network::ToByte(network::MessageId::eventFlag));

    Transport guestTransport;
    coop::Runtime guest(guestTransport, TestOptions(), 7, 9);
    assert(guest.StartGuest(100));
    assert(guest.MarkConnected(100, 100));
    int eventCallbacks = 0;
    guest.SetEventCallback([&eventCallbacks](std::uint64_t peer, coop::EventFlagUpdate update) {
        assert(peer == 100 && update.eventId == 1234 && update.enabled);
        ++eventCallbacks;
    });
    assert(guest.Receive(100, eventPacket, 200) == network::DispatchResult::delivered);
    assert(guest.WorldState().IsSet(1234));
    assert(eventCallbacks == 1);
    assert(guest.Receive(999, eventPacket, 200) == network::DispatchResult::rejectedPayload);
    assert(!guest.BroadcastEventFlag({4321, true}));

    // Repeated send failures disconnect a dead peer and bounded retries reopen it.
    hostTransport.sendSucceeds = false;
    host.Tick(1'100);
    host.Tick(2'100);
    host.Tick(3'100);
    assert(!host.SessionState().IsConnected(200));
    assert(!host.BroadcastEventFlag({1235, true}));
    hostTransport.reconnectSucceeds = true;
    host.Tick(3'599);
    assert(hostTransport.reconnectPeers.empty());
    host.Tick(3'600);
    assert(hostTransport.reconnectPeers.size() == 1 && hostTransport.reconnectPeers[0] == 200);
    assert(host.SessionState().IsConnected(200));
    hostTransport.sendSucceeds = true;
    host.Tick(10'600);
    assert(!host.SessionState().IsConnected(200));
    const std::array<std::uint8_t, 3> malformed{1, 2, 3};
    assert(host.Receive(200, malformed, 10'601) == network::DispatchResult::rejectedFrame);
    assert(!host.SessionState().IsConnected(200));
    constexpr std::array<std::uint8_t, 1> pulse{1};
    const auto heartbeat = Frame(network::MessageId::sessionState, pulse);
    assert(host.Receive(200, heartbeat, 10'602) == network::DispatchResult::delivered);
    assert(host.SessionState().IsConnected(200));

    // Boss state only moves forward under host authority.
    const coop::BossState phaseOne{10, 1, 1, coop::BossPhase::active, 900, 1'000};
    assert(host.BroadcastBossState(phaseOne));
    const auto bossPacket = hostTransport.packets.back().bytes;
    int bossCallbacks = 0;
    guest.SetBossCallback([&bossCallbacks](std::uint64_t peer, coop::BossState state) {
        assert(peer == 100 && state.encounterId == 10);
        ++bossCallbacks;
    });
    assert(guest.Receive(100, bossPacket, 300) == network::DispatchResult::delivered);
    assert(bossCallbacks == 1);
    assert(guest.Receive(100, bossPacket, 301) == network::DispatchResult::delivered);
    assert(bossCallbacks == 1);

    const coop::BossState phaseTwo{10, 1, 2, coop::BossPhase::phaseTwo, 1'200, 1'500};
    const auto phaseTwoPacket = Frame(network::MessageId::worldData, coop::BossSync::Encode(phaseTwo));
    assert(guest.Receive(100, phaseTwoPacket, 302) == network::DispatchResult::delivered);
    assert(bossCallbacks == 2);
    const coop::BossState regression{10, 1, 3, coop::BossPhase::active, 500, 1'000};
    const auto regressionPacket = Frame(network::MessageId::worldData, coop::BossSync::Encode(regression));
    assert(guest.Receive(100, regressionPacket, 303) == network::DispatchResult::delivered);
    assert(bossCallbacks == 2);
    const coop::BossState defeated{10, 1, 4, coop::BossPhase::defeated, 0, 1'500};
    const auto defeatedPacket = Frame(network::MessageId::worldData, coop::BossSync::Encode(defeated));
    assert(guest.Receive(100, defeatedPacket, 304) == network::DispatchResult::delivered);
    assert(bossCallbacks == 3);
    const coop::BossState resurrection{10, 1, 5, coop::BossPhase::defeated, 1, 1'500};
    assert(coop::BossSync::Encode(resurrection).empty());

    const coop::BossState spoofed{99, 1, 1, coop::BossPhase::active, 100, 100};
    const auto spoofedPacket = Frame(network::MessageId::worldData, coop::BossSync::Encode(spoofed));
    assert(host.Receive(200, spoofedPacket, 10'603) == network::DispatchResult::delivered);
    assert(!host.BossStateData().Get(99));

    // A player entering mid-fight gets the current snapshot immediately.
    assert(host.AddPeer(300));
    const auto packetCount = hostTransport.packets.size();
    assert(host.MarkConnected(300, 10'604));
    assert(hostTransport.packets.size() == packetCount + 1);
    assert(hostTransport.packets.back().peer == 300);
    const auto lateBoss = network::Decode(hostTransport.packets.back().bytes, 64);
    assert(lateBoss && lateBoss->id == network::ToByte(network::MessageId::worldData));

    constexpr std::array<std::uint8_t, 8> nonce{1, 2, 3, 4, 5, 6, 7, 8};
    const auto helloBytes = coop::EncodeHello(coop::SessionRole::host, 1, 100, nonce, "secret");
    coop::Hello hello{};
    assert(coop::DecodeHello(helloBytes, hello));
    assert(hello.steamId == 100 && hello.nonce == 0x0807060504030201ull);
    assert(coop::VerifyHello(hello, 1, "secret"));
    assert(!coop::VerifyHello(hello, 1, "wrong"));
    std::cout << "Co-op reliability and boss synchronization tests passed\n";
}
