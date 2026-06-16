#pragma once

#include <array>
#include <string>
#include <vector>

enum GbaKey {
    GK_A = 0,
    GK_B,
    GK_SELECT,
    GK_START,
    GK_RIGHT,
    GK_LEFT,
    GK_UP,
    GK_DOWN,
    GK_R,
    GK_L,
    GK_COUNT,
};

const char* gbaKeyName(int key);

struct Config {
    int windowWidth  = 480;
    int windowHeight = 320;
    bool fullscreen    = false;
    bool vsync         = true;
    bool integerScale  = false;
    bool linearFilter  = false;
    int  shader        = 0;
    bool colorCorrect  = false;

    int volume = 100;

    bool pauseOnFocusLoss = true;
    bool rewindEnabled    = true;
    bool prefetch         = true;
    int  speedIndex       = 2;

    bool checkUpdatesOnStartup = true;

    std::array<int, GK_COUNT> keyMap{};

    std::vector<std::string> gamesDirs;

    bool addGamesDir(const std::string& dir);

    std::vector<std::string> recentRoms;

    static constexpr size_t MAX_RECENT = 10;

    Config();

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    void addRecent(const std::string& romPath);
};
