#pragma once
#include "../extension.h"
#include <atomic>
#include <memory>

namespace ds3sc::extensions {
enum class ConnectionType { Unknown = 0, Steam = 1, LAN = 2 };

class VerboseConnectionsExtension final : public IExtension {
public:
    VerboseConnectionsExtension() noexcept;
    ~VerboseConnectionsExtension() override;
    const char* GetId() const noexcept override { return "verbose_connections"; }
    const char* GetName() const noexcept override { return "Verbose Connection Notifications"; }
    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override;
    void TriggerHostingNotification() noexcept;
    void TriggerLeavingNotification() noexcept;
    void GetStats(std::uint32_t& hostNotifs, std::uint32_t& joinNotifs) const noexcept;
    bool IsSpanish() const noexcept;
    ConnectionType GetCurrentConnectionType() const noexcept;
private:
    void LoadSettings() noexcept;
    bool InstallHooks() noexcept;
    void RemoveHooks() noexcept;
    std::atomic<bool> initialized_{false};
    std::atomic<std::uint32_t> hostNotifications_{0};
    char iniPath_[MAX_PATH]{};
};
std::shared_ptr<VerboseConnectionsExtension> CreateVerboseConnectionsExtension() noexcept;
VerboseConnectionsExtension* GetVerboseConnectionsInstance() noexcept;
} // namespace ds3sc::extensions

extern "C" {
__declspec(dllexport) void ds3sc_trigger_verbose_host_notification();
__declspec(dllexport) void ds3sc_trigger_verbose_leave_notification();
__declspec(dllexport) void ds3sc_get_verbose_connection_stats(uint32_t* hostNotifs, uint32_t* joinNotifs);
__declspec(dllexport) void ds3sc_get_verbose_delivery_stats(uint32_t* displayed, uint32_t* failed);
}
