#pragma once

#include <array>
#include <string>
#include <vector>

// GBA keypad, ordered to match the REG_KEYINPUT bit layout so keyMap[k] is the
// scancode bound to bit k.
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

// Human-readable names for the input-remap UI, indexed by GbaKey.
const char* gbaKeyName(int key);

// Persistent user settings: video, audio, recent ROMs, and the keyboard->keypad
// map. Serialized as a flat "key=value" text file (no external dependency).
// Stored as plain ints (SDL_Scancode values) so this stays SDL-free.
struct Config {
    // Video.
    int windowWidth  = 480;
    int windowHeight = 320;
    bool fullscreen    = false;
    bool vsync         = true;
    bool integerScale  = false;
    bool linearFilter  = false;  // false = nearest-neighbour (sharp pixels)
    int  shader        = 0;      // post-process overlay; see Frontend ShaderKind

    // Audio.
    int volume = 100;  // 0..100

    // Input: one SDL_Scancode per GbaKey.
    std::array<int, GK_COUNT> keyMap{};

    // Folders scanned for the game-library grid. Each folder's cover art lives
    // in <folder>/covers/<rom-stem>.png|jpg. Multiple folders are supported so
    // the library can be assembled from several locations.
    std::vector<std::string> gamesDirs;

    // Appends a games folder, de-duplicating. Returns false if already present.
    bool addGamesDir(const std::string& dir);

    // Recent ROM paths, most-recent first (capped at MAX_RECENT).
    std::vector<std::string> recentRoms;

    static constexpr size_t MAX_RECENT = 10;

    Config();  // fills keyMap and fields with defaults

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    // Inserts romPath at the front, de-duplicating and capping the list.
    void addRecent(const std::string& romPath);
};
