#include "extension_manager.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace ds3sc::extensions {

ExtensionManager& ExtensionManager::Instance() noexcept {
    static ExtensionManager s_instance;
    return s_instance;
}

void ExtensionManager::Register(std::shared_ptr<IExtension> extension) noexcept {
    if (!extension) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& existing : extensions_) {
        if (std::strcmp(existing->GetId(), extension->GetId()) == 0) {
            return; // Already registered
        }
    }
    extensions_.push_back(std::move(extension));
}

bool ExtensionManager::InitializeAll() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    bool allOk = true;
    for (auto& ext : extensions_) {
        if (std::find(active_.begin(), active_.end(), ext) != active_.end()) continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[DS3SC-Extensions] Initializing module: %s (%s)...\n",
                      ext->GetName(), ext->GetId());
        OutputDebugStringA(buf);

        if (!ext->Initialize()) {
            std::snprintf(buf, sizeof(buf), "[DS3SC-Extensions] Error initializing module: %s\n",
                          ext->GetId());
            OutputDebugStringA(buf);
            allOk = false;
        } else {
            active_.push_back(ext);
            std::snprintf(buf, sizeof(buf), "[DS3SC-Extensions] Module ready: %s\n", ext->GetId());
            OutputDebugStringA(buf);
        }
    }

    initialized_ = allOk;
    return allOk;
}

void ExtensionManager::ShutdownAll() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = active_.rbegin(); it != active_.rend(); ++it) {
        if (*it) {
            (*it)->Shutdown();
        }
    }
    active_.clear();
    extensions_.clear();
    initialized_ = false;
}

void ExtensionManager::OnTick() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& ext : active_) {
        if (ext) {
            ext->OnTick();
        }
    }
}

bool ExtensionManager::IsActive(const char* id) const noexcept {
    if (!id) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& ext : active_) {
        if (std::strcmp(ext->GetId(), id) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace ds3sc::extensions
