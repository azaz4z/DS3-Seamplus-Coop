#pragma once

#include "../extension.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace ds3sc::extensions {

class LanCoopExtension final : public IExtension {
public:
    LanCoopExtension() noexcept;
    ~LanCoopExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "lan_coop"; }
    [[nodiscard]] const char* GetName() const noexcept override { return "LAN Co-op Transport"; }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override;

    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] bool IsHost() const noexcept;
    [[nodiscard]] uint16_t GetPort() const noexcept;
    [[nodiscard]] uint32_t GetPasswordHash() const noexcept;
    [[nodiscard]] uint64_t GetVirtualLobbyId() const noexcept;

    void TriggerHostSession() noexcept;
    void TriggerGuestSearch() noexcept;
    void DissolveSession() noexcept;

private:
    void LoadSettings() noexcept;
    bool InstallSteamHooks() noexcept;
    void RemoveSteamHooks() noexcept;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> isHost_{false};
    std::atomic<uint16_t> port_{27015};
    std::atomic<uint32_t> passwordHash_{0};
    std::string sessionPassword_;
    char iniPath_[MAX_PATH]{};
};

std::shared_ptr<IExtension> CreateLanCoopExtension() noexcept;
LanCoopExtension* GetLanCoopInstance() noexcept;

} // namespace ds3sc::extensions

extern "C" {
__declspec(dllexport) int ds3sc_is_lan_coop_active();
__declspec(dllexport) int ds3sc_is_lan_host();
__declspec(dllexport) uint16_t ds3sc_get_lan_port();
__declspec(dllexport) void ds3sc_set_lan_mode(int mode);
}
