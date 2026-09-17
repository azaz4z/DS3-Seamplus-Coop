#include "../src/network/sender.h"
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace ds3sc::network;
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct RecordingTransport final : MessageTransport {
    int calls = 0, lastFlags = 0, lastChannel = 0, result = 1;
    std::vector<std::uint8_t> lastPacket;
    int Send(std::uint64_t peer, std::span<const std::uint8_t> packet, int flags, int channel) override {
        Check(peer == 42, "Destination Steam ID");
        ++calls; lastFlags = flags; lastChannel = channel;
        lastPacket.assign(packet.begin(), packet.end());
        return result;
    }
};
int main() try {
    // Golden packet from original RVA 0x69170, with input flag bit 2 set.
    const std::vector<std::uint8_t> golden{3, 1, 0, 1, 0, 0, 0, 1};
    const std::array<std::uint8_t, 1> payload{1};
    Check(Encode(SendType::type3, 1, payload) == golden, "Original fixed packet bytes");
    Check(Decode(golden, 1)->payload[0] == 1, "Decode known packet");
    for (std::size_t i = 0; i < golden.size(); ++i) {
        Check(!Decode(std::span(golden).first(i), 1024), "Truncated packet rejected");
    }
    auto malformed = golden;
    malformed.push_back(0);
    Check(!Decode(malformed, 1024), "Trailing bytes rejected");
    malformed = golden; malformed[3] = 255;
    Check(!Decode(malformed, 1024), "Forged length rejected");
    malformed = golden; malformed[0] = 4;
    Check(!Decode(malformed, 1024), "Invalid type rejected");
    Check(!Decode(golden, 0), "Payload cap enforced");
    const std::vector<std::uint8_t> longPayload(0x102, 0xaa);
    const auto encoded = Encode(SendType::type2, 29, longPayload);
    Check(encoded[3] == 2 && encoded[4] == 1 && encoded[5] == 0 && encoded.size() == 0x109,
          "Little-endian unaligned length");
    RecordingTransport transport;
    Sender sender(transport, 10, 11);
    std::array<PeerState, 1> peers{{{42, 3}}};
    const std::array<std::uint32_t, 1> list{0x101};
    Check(sender.Send(peers, list, 42, 0x101, SendType::type0, payload), "Type 0 send");
    Check(transport.lastFlags == 5 && transport.lastChannel == 10 && transport.lastPacket[1] == 1,
          "Full-width ID lookup and low-byte wire ID");
    Check(sender.Send(peers, list, 42, 1, SendType::type0, payload) && transport.lastFlags == 9,
          "Unlisted type 0 ID flags");
    Check(sender.Send(peers, list, 42, 1, SendType::type1, payload) && transport.lastFlags == 9,
          "Type 1 flags");
    Check(sender.Send(peers, list, 42, 1, SendType::type2, payload) && transport.lastFlags == 5,
          "Type 2 flags");
    Check(sender.Send(peers, list, 42, 1, SendType::type3, payload) && transport.lastChannel == 11 &&
          transport.lastFlags == 9 && transport.lastPacket == golden, "Alternate channel");
    const auto calls = transport.calls;
    for (const std::uint64_t flags : {0, 1, 2}) {
        peers[0].flags = flags;
        Check(!sender.Send(peers, list, 42, 1, SendType::type3, payload), "Peer flags");
    }
    Check(transport.calls == calls, "Rejected peers never reach transport");
    peers[0].flags = 3;
    Check(!sender.Send(peers, list, 99, 1, SendType::type3, payload), "Unknown peer");
    transport.result = 2;
    Check(!sender.Send(peers, list, 42, 1, SendType::type3, payload), "Transport failure propagated");
    std::mt19937 random(374320);
    for (int i = 0; i < 10000; ++i) {
        std::vector<std::uint8_t> bytes(random() % 128);
        for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
        if (auto packet = Decode(bytes, 64)) {
            Check(packet->payload.size() <= 64 && packet->payload.size() + headerSize == bytes.size(),
                  "Random malformed input bounds");
        }
    }
    std::cout << "Packet and sender tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
