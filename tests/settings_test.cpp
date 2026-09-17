#include "../src/mod/settings.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int wmain(int argc, wchar_t** argv) try {
    if (argc != 2) return 2;
    const std::filesystem::path directory = argv[1];
    std::filesystem::create_directories(directory);
    const auto missing = LoadSettings(directory / "missing.ini");
    Check(missing.allowInvaders && missing.enemyHealthScaling == 35 &&
          missing.saveFileExtension == "co2", "Missing file defaults");
    const auto path = directory / L"configuración.ini";
    auto write = [&](const std::wstring& contents) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const unsigned char bom[] = {0xff, 0xfe};
        file.write(reinterpret_cast<const char*>(bom), sizeof(bom));
        file.write(reinterpret_cast<const char*>(contents.data()),
                   static_cast<std::streamsize>(contents.size() * sizeof(wchar_t)));
        if (!file) throw std::runtime_error("Writing test INI failed");
    };
    write(L"[GAMEPLAY]\r\nallow_invaders=0\r\ngame_boot_volume=99\r\n"
          L"overhead_player_display=nonsense\r\n[SCALING]\r\nenemy_health_scaling=-9\r\n"
          L"boss_health_scaling=99999999999999999\r\n[SAVE]\r\nsave_file_extension=../sl2\r\n"
          L"[PASSWORD]\r\ncooppassword=" + std::wstring(900, L'x') +
          L"\r\n[LANGUAGE]\r\nmod_language_override=español\r\n");
    const auto values = LoadSettings(path);
    Check(!values.allowInvaders && values.gameBootVolume == 10, "Gameplay values");
    Check(values.overheadPlayerDisplay == 2 && values.bossHealthScaling == 100,
          "Malformed/overflowing numbers use defaults");
    Check(values.enemyHealthScaling == 0 && values.saveFileExtension == "co2",
          "Bounds and extension validation");
    Check(values.coopPassword == std::string(900, 'x'), "Password truncation");
    Check(values.languageOverride == "español", "Unicode INI and file path");
    write(L"[SAVE]\r\nsave_file_extension=" + std::wstring(120, L'a') + L"\r\n");
    Check(LoadSettings(path).saveFileExtension.size() == 120, "120-character extension");
    write(L"[SAVE]\r\nsave_file_extension=" + std::wstring(121, L'a') + L"\r\n");
    Check(LoadSettings(path).saveFileExtension == "co2", "121-character extension rejected");
    std::cout << "Settings tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
