#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "settings.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <vector>

namespace {

std::wstring WidenAscii(const char* value) {
    return std::wstring(value, value + std::char_traits<char>::length(value));
}

// Keep Win32 profile-file compatibility while allowing Unicode file paths and
// avoiding silent truncation of passwords or language names at 511 bytes.
std::string ReadString(const std::filesystem::path& path, const char* section,
                       const char* key, const char* fallback) {
    const auto wideSection = WidenAscii(section);
    const auto wideKey = WidenAscii(key);
    const auto wideFallback = WidenAscii(fallback);
    std::vector<wchar_t> value(256);
    for (;;) {
        const DWORD length = GetPrivateProfileStringW(wideSection.c_str(), wideKey.c_str(),
            wideFallback.c_str(), value.data(), static_cast<DWORD>(value.size()), path.c_str());
        if (length < value.size() - 1) {
            if (!length) return {};
            const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                value.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
            if (!bytes) throw std::runtime_error("Invalid Unicode value in INI");
            std::string result(static_cast<size_t>(bytes), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(length), result.data(), bytes, nullptr, nullptr)) {
                throw std::runtime_error("Could not convert INI value to UTF-8");
            }
            return result;
        }
        if (value.size() >= 32768) throw std::runtime_error("INI value is too long");
        value.resize(value.size() * 2);
    }
}

int ReadInt(const std::filesystem::path& path, const char* section, const char* key,
            int fallback, int minimum, int maximum) {
    const auto text = ReadString(path, section, key, "");
    if (text.empty()) return fallback;
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return fallback;
    return std::clamp(value, minimum, maximum);
}

bool ReadBool(const std::filesystem::path& path, const char* section, const char* key,
              bool fallback) {
    return ReadInt(path, section, key, fallback ? 1 : 0, 0, 1) != 0;
}

}  // namespace

Settings LoadSettings(const std::filesystem::path& requestedPath) {
    const auto iniPath = std::filesystem::absolute(requestedPath);
    Settings result;
    result.allowInvaders = ReadBool(iniPath, "GAMEPLAY", "allow_invaders", true);
    result.deathDebuffs = ReadBool(iniPath, "GAMEPLAY", "death_debuffs", true);
    result.overheadPlayerDisplay =
        ReadInt(iniPath, "GAMEPLAY", "overhead_player_display", 2, 0, 5);
    result.skipIntros = ReadBool(iniPath, "GAMEPLAY", "skip_intros", true);
    result.syncProgressAsGuest =
        ReadBool(iniPath, "GAMEPLAY", "sync_progress_as_guest", true);
    result.gameBootVolume = ReadInt(iniPath, "GAMEPLAY", "game_boot_volume", 5, 0, 10);

    result.enemyHealthScaling =
        ReadInt(iniPath, "SCALING", "enemy_health_scaling", 35, 0, 1000);
    result.enemyDamageScaling =
        ReadInt(iniPath, "SCALING", "enemy_damage_scaling", 0, 0, 1000);
    result.enemyPostureScaling =
        ReadInt(iniPath, "SCALING", "enemy_posture_scaling", 15, 0, 1000);
    result.bossHealthScaling =
        ReadInt(iniPath, "SCALING", "boss_health_scaling", 100, 0, 1000);
    result.bossDamageScaling =
        ReadInt(iniPath, "SCALING", "boss_damage_scaling", 0, 0, 1000);
    result.bossPostureScaling =
        ReadInt(iniPath, "SCALING", "boss_posture_scaling", 20, 0, 1000);

    result.coopPassword = ReadString(iniPath, "PASSWORD", "cooppassword", "");
    result.saveFileExtension =
        ReadString(iniPath, "SAVE", "save_file_extension", "co2");
    result.languageOverride =
        ReadString(iniPath, "LANGUAGE", "mod_language_override", "");

    if (result.saveFileExtension.empty() || result.saveFileExtension.size() > 120 ||
        result.saveFileExtension.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") !=
            std::string::npos) {
        result.saveFileExtension = "co2";
    }
    return result;
}
