#pragma once

#include <SDL.h>

#include <cstdint>
#include <cstdio>
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
    bool initWindowAndRenderer();
    void initImGui();
    void initImGuiBackends();
    void shutdownImGuiBackends();
    bool createRenderer();
    void destroyRenderer();
    void applyFilter();
    void loadFonts();

    void handleEvents();
    void handleKey(const SDL_Event& e, bool down);
    void emulateAndPace(const Uint64 frameStart);
    void pumpAudio();
    void uploadFrame();
    void render();
    void drawUI();
    void drawMenuBar();
    void drawOverlay();
    void drawGraphicsSettings();
    void drawSaveStatePrompt();
    void drawUpdateWindow();

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

    std::unique_ptr<Emulator> emu;
    std::FILE* traceFile = nullptr;
    struct ImFont* headingFont = nullptr;
    bool imguiReady = false;
    bool backendsReady = false;

    uint16_t keyState = 0x03FF;
    bool running = true;
    bool paused = false;
    bool fastForward = false;
    int saveSlot = 0;
    float menuBarHeight = 0.0f;

    std::string statusMessage;
    uint32_t statusExpiryMs = 0;

    double displayFps = 0.0;
    double emuSpeed = 0.0;
    int displayFrameCount = 0;
    int emuFrameCount = 0;
    Uint64 counterAnchor = 0;
};
