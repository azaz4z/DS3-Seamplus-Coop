#include "../src/render/marker_visibility.h"
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
struct Memory {
    std::map<std::uintptr_t, std::vector<unsigned char>> values;
    template<class T> void Put(std::uintptr_t address, T value) {
        auto& data = values[address];
        data.resize(sizeof(T));
        std::memcpy(data.data(), &value, sizeof(T));
    }
    template<class T> bool operator()(std::uintptr_t address, T& value) const noexcept {
        const auto it = values.find(address);
        if (it == values.end() || it->second.size() != sizeof(T)) return false;
        std::memcpy(&value, it->second.data(), sizeof(T));
        return true;
    }
};
void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}

int main() try {
    constexpr std::uintptr_t game = 0x140000000, menu = 0x10000, frontend = 0x20000;
    constexpr std::uintptr_t player = 0x30000, modules = 0x40000, animation = 0x50000;
    Memory memory;
    memory.Put(game + 0x4763258, menu);
    memory.Put(menu + 0x1d50, std::uintptr_t{0});
    memory.Put(game + 0x478da40, frontend);
    memory.Put(frontend + 0x140, std::uint8_t{1});
    memory.Put(frontend + 0x143, std::uint8_t{3});
    memory.Put(frontend + 0x2fa1, std::uint8_t{3});
    memory.Put(player + 0x1f90, modules);
    memory.Put(modules + 0x80, animation);
    memory.Put(animation + 0xc8, std::int32_t{12000000});
    const auto hidden = [&] { return ds3sc::render::NativeSceneSuppressesMarkers(game, player, memory); };
    Check(!hidden(), "Idle gameplay must retain markers");

    // Native menu request changes before its frontend presentation catches up.
    memory.Put(frontend + 0x143, std::uint8_t{0});
    Check(hidden(), "Inventory must hide markers even with MenuMan+1d50 null");
    memory.Put(frontend + 0x2fa1, std::uint8_t{0});
    memory.Put(frontend + 0x143, std::uint8_t{3});
    Check(hidden(), "Closing transition must not expose markers over fading menu");
    memory.Put(frontend + 0x2fa1, std::uint8_t{3});
    Check(!hidden(), "Markers must return when menu finishes closing");

    memory.Put(frontend + 0x2fa1, std::uint8_t{1});
    Check(hidden(), "Hidden frontend presentation must suppress markers");
    memory.Put(frontend + 0x2fa1, std::uint8_t{3});
    memory.Put(frontend + 0x140, std::uint8_t{0});
    Check(hidden(), "Disabled frontend must suppress markers");
    memory.Put(frontend + 0x140, std::uint8_t{1});

    // Bonfire entry/exit and fog remain hidden even without a native menu.
    for (const std::int32_t id : {60060, 68000, 68001, 68002, 68010, 68011, 68012, 68100, 68101, 12060060, 12068011}) {
        memory.Put(animation + 0xc8, id);
        Check(hidden(), "Event action leaked markers");
    }
    for (const std::int32_t id : {-1, 0, 12000000, 1000, 3000, 60000, 60061, 68009, 68013, 80000}) {
        memory.Put(animation + 0xc8, id);
        Check(!hidden(), "Unrelated action hid markers");
    }
    memory.Put(frontend + 0x3084, std::uint8_t{1});
    Check(!hidden(), "Debug help option must not be treated as a cutscene");
    memory.Put(menu + 0x1d50, std::uintptr_t{0x60000});
    Check(hidden(), "Legacy native modal must still hide markers");
    memory.Put(menu + 0x1d50, std::uintptr_t{0});
    memory.values.erase(animation + 0xc8);
    Check(!hidden(), "Unavailable animation must not latch suppression");
    memory.values.erase(game + 0x478da40);
    Check(!hidden(), "Unavailable frontend must not latch suppression");
    Check(!ds3sc::render::NativeSceneSuppressesMarkers(game, 0, Memory{}), "Missing reads must be safe");
    std::cout << "PASS: native menu transitions, hidden frontend, bonfire/fog actions, ordinary gameplay and missing reads.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
