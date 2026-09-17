#pragma once

#include <filesystem>
#include <string>

struct Settings {
    bool allowInvaders = true;
    bool deathDebuffs = true;
    int overheadPlayerDisplay = 2;
    bool skipIntros = true;
    bool syncProgressAsGuest = true;
    int gameBootVolume = 5;

    int enemyHealthScaling = 35;
    int enemyDamageScaling = 0;
    int enemyPostureScaling = 15;
    int bossHealthScaling = 100;
    int bossDamageScaling = 0;
    int bossPostureScaling = 20;

    std::string coopPassword;
    std::string saveFileExtension = "co2";
    std::string languageOverride;
};

Settings LoadSettings(const std::filesystem::path& iniPath);
