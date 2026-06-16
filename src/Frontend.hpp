#pragma once

#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config.hpp"
#include "Emulator.hpp"

struct FrontendOptions {
    std::string romPath;
    std::string biosPath;
    bool demo = false;
    bool trace = false;
};

class Frontend {
public:
    explicit Frontend(FrontendOptions opts);
    ~Frontend();

    int run();

private:
    struct CheatOp {
        uint32_t addr;
        uint32_t value;
        int width;
    };
    struct Cheat {
        std::string name;
        std::string codes;
        bool enabled = true;
        std::vector<CheatOp> ops;
    };

    bool initWindowAndRenderer();
    void initImGui();
    void initImGuiBackends();
    void shutdownImGuiBackends();
    bool createRenderer();
    void applyFilter();
    void loadFonts();

    void handleEvents();
    void handleKey(const SDL_Event& e, bool down);
    void emulateAndPace(const Uint64 frameStart);
    void stepEmuFrame(bool audible);
    void pumpAudio(bool audible = true);
    void uploadFrame();
    void applyColorCorrection();
    void render();
    void drawUI();
    void drawMenuBar();
    void drawOverlay();
    void drawGraphicsSettings();
    void drawSaveStatePrompt();
    void drawSaveStateBrowser();
    void drawCheatsWindow();
    void drawUpdateWindow();

    void openGamepad(int joystickIndex);
    void closeGamepad();
    void pollGamepad();

    void takeScreenshot();

    void captureRewindSnapshot();
    void doRewindStep();
    void clearRewind();

    std::string cheatPath() const;
    void loadCheats();
    void saveCheats() const;
    void applyCheats();
    std::vector<CheatOp> parseCheatCodes(const std::string& text) const;

    void openROM(const std::string& path, bool userSelected = true);
    void openRomDialog();
    void loadDemo();

    void scanGames();
    void addGamesFolder();
    void drawLibrary();
    void drawShaderOverlay(const SDL_Rect& dst);
    SDL_Texture* coverTexture(const std::string& coverPath);
    void clearCoverCache();
    void resetEmu();
    std::string sramPath() const;

    std::string statePath(int slot) const;
    void saveState(int slot);
    void loadState(int slot);
    int  mostRecentStateSlot() const;
    void setStatus(const std::string& message);

    void checkForUpdates();

    void beginUpdateDownload();
    void pollUpdateDownload();
    void applyUpdate();

    void startStartupUpdateCheck();
    void pollStartupUpdateCheck();

    SDL_Rect computeGameRect() const;
    void toggleFullscreen();

    FrontendOptions opts;
    Config config;
    std::string configPath;
    std::string romPath;

    struct GameEntry {
        std::string path;
        std::string title;
        std::string coverPath;
    };
    std::vector<GameEntry> games;
    std::unordered_map<std::string, SDL_Texture*> coverCache;
    bool showLibrary = true;
    bool gamesScanned = false;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_Texture* shaderTex = nullptr;
    int shaderTexId = -1;
    SDL_AudioDeviceID audioDev = 0;
    std::vector<int16_t> audioChunk;

    bool showGraphicsSettings = false;
    bool showSaveStatePrompt = false;
    int pendingStateSlot = -1;
    bool showUpdateWindow = false;
    std::string updateVersion;
    std::string updateNotes;
    std::string updateUrl;
    std::string updateAssetUrl;

    enum UpdatePhase { UP_NONE, UP_DOWNLOADING, UP_READY, UP_FAILED };
    int updatePhase = UP_NONE;
    long long updateTotalBytes = 0;
    std::string updateDownloadPath;

    bool startupCheckPending = false;
    std::string startupCheckPath;

    std::unique_ptr<Emulator> emu;
    std::FILE* traceFile = nullptr;
    struct ImFont* headingFont = nullptr;
    bool imguiReady = false;
    bool backendsReady = false;

    uint16_t keyState = 0x03FF;
    uint16_t padState = 0x03FF;
    bool running = true;
    bool paused = false;
    bool fastForward = false;
    bool rewinding = false;
    bool windowFocused = true;
    int saveSlot = 0;
    float menuBarHeight = 0.0f;
    double frameAccum = 0.0;

    SDL_GameController* gamepad = nullptr;
    SDL_JoystickID gamepadId = -1;

    std::vector<uint32_t> ccFrame;

    std::deque<std::vector<uint8_t>> rewindStates;
    int rewindCounter = 0;

    std::vector<Cheat> cheats;
    bool showCheats = false;
    bool showStateBrowser = false;
    std::unordered_map<int, SDL_Texture*> slotThumbCache;

    std::string statusMessage;
    uint32_t statusExpiryMs = 0;

    double displayFps = 0.0;
    double emuSpeed = 0.0;
    int displayFrameCount = 0;
    int emuFrameCount = 0;
    Uint64 counterAnchor = 0;
};
