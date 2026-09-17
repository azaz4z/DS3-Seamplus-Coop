#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "game_hooks.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace ds3sc::hooks {
namespace {

constexpr DWORD kDs3Timestamp = 0x639c4dddu;
constexpr std::size_t kDs3ImageSize = 0x56f4000u;

using SteamApiInit = bool (*)();
using SteamApiRunCallbacks = void (*)();

SteamApiInit gSteamApiInit = nullptr;
SteamApiRunCallbacks gSteamApiRunCallbacks = nullptr;
std::atomic_bool gSteamReady = false;

bool EqualAsciiInsensitive(const char* left, std::string_view right) noexcept {
    if (!left) return false;
    const auto length = std::strlen(left);
    if (length != right.size()) return false;
    for (std::size_t i = 0; i < length; ++i) {
        const auto a = static_cast<unsigned char>(left[i]);
        const auto b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool IsReadable(const void* address, std::size_t bytes) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) ||
        info.Protect == PAGE_NOACCESS) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + bytes;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return end >= begin && end <= regionEnd;
}

bool ReadImageIdentity(HMODULE module, GameIdentity& identity, std::string& error) noexcept {
    if (!module) {
        error = "DarkSoulsIII.exe is not loaded";
        return false;
    }
    const auto* base = reinterpret_cast<const std::byte*>(module);
    if (!IsReadable(base, sizeof(IMAGE_DOS_HEADER))) {
        error = "DOS header unreadable";
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        !IsReadable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) {
        error = "invalid PE header";
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "unsupported PE format";
        return false;
    }
    identity.imageSize = nt->OptionalHeader.SizeOfImage;
    identity.timestamp = nt->FileHeader.TimeDateStamp;
    return true;
}

bool SteamApiInitHook() noexcept {
    if (gSteamApiInit) gSteamReady = gSteamApiInit();
    return gSteamReady.load();
}

void SteamApiRunCallbacksHook() noexcept {
    if (gSteamApiRunCallbacks) gSteamApiRunCallbacks();
}

}  // namespace

bool GameHooks::InstallImportHook(std::string_view moduleName, std::string_view functionName,
                                  void* replacement, void** original, void*** slotOut,
                                  std::string& error) noexcept {
    const auto game = GetModuleHandleW(L"DarkSoulsIII.exe");
    if (!game) {
        error = "game module not found";
        return false;
    }
    const auto* base = reinterpret_cast<const std::byte*>(game);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) {
        error = "game import table is empty";
        return false;
    }

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        const_cast<std::byte*>(base) + directory.VirtualAddress);
    for (; imports->Name != 0; ++imports) {
        const auto* imported = reinterpret_cast<const char*>(base + imports->Name);
        if (!EqualAsciiInsensitive(imported, moduleName)) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            const_cast<std::byte*>(base) + (imports->OriginalFirstThunk
                ? imports->OriginalFirstThunk : imports->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            const_cast<std::byte*>(base) + imports->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name),
                            std::string(functionName).c_str()) != 0) continue;
            auto* slot = reinterpret_cast<void**>(&slots->u1.Function);
            DWORD protection = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) {
                error = "could not make Steam import writable";
                return false;
            }
            *original = *slot;
            *slot = replacement;
            if (slotOut) *slotOut = slot;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return true;
        }
    }
    error = "requested import not found in steam_api64.dll";
    return false;
}

bool GameHooks::RestoreImportHook(void** slot, void* original) noexcept {
    if (!slot || !original) return true;
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    *slot = original;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

bool GameHooks::Install(std::string& error) noexcept {
    if (status_.load() == Status::installed) return true;
    const auto game = GetModuleHandleW(L"DarkSoulsIII.exe");
    if (!ReadImageIdentity(game, identity_, error)) {
        status_ = Status::failed;
        return false;
    }
    if (identity_.imageSize != kDs3ImageSize || identity_.timestamp != kDs3Timestamp) {
        error = "unsupported version: expected DS3 1.15.2.0";
        status_ = Status::unsupportedGame;
        return false;
    }

    if (!InstallImportHook("steam_api64.dll", "SteamAPI_Init",
                          reinterpret_cast<void*>(&SteamApiInitHook), &initOriginal_, &initSlot_, error)) {
        status_ = Status::failed;
        return false;
    }
    if (!InstallImportHook("steam_api64.dll", "SteamAPI_RunCallbacks",
                          reinterpret_cast<void*>(&SteamApiRunCallbacksHook),
                          &callbacksOriginal_, &callbacksSlot_, error)) {
        RestoreImportHook(initSlot_, initOriginal_);
        initOriginal_ = nullptr;
        status_ = Status::failed;
        return false;
    }
    gSteamApiInit = reinterpret_cast<SteamApiInit>(initOriginal_);
    gSteamApiRunCallbacks = reinterpret_cast<SteamApiRunCallbacks>(callbacksOriginal_);
    status_ = Status::installed;
    return true;
}

void GameHooks::Uninstall() noexcept {
    RestoreImportHook(callbacksSlot_, callbacksOriginal_);
    RestoreImportHook(initSlot_, initOriginal_);
    callbacksSlot_ = nullptr;
    initSlot_ = nullptr;
    callbacksOriginal_ = nullptr;
    initOriginal_ = nullptr;
    gSteamApiRunCallbacks = nullptr;
    gSteamApiInit = nullptr;
    gSteamReady = false;
    status_ = Status::idle;
}

}  // namespace ds3sc::hooks
