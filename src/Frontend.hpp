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

// The SDL2 + Dear ImGui application shell. Owns the window/renderer/audio, the
// ImGui context, the persistent Config, and an optional Emulator, and drives
// the main loop. The emulator core stays oblivious to all of this.
class Frontend {
public:
    explicit Frontend(FrontendOptions opts);
    ~Frontend();

    int run();

private:
    // Setup / teardown.
    bool initWindowAndRenderer();
    void initImGui();
    void initImGuiBackends();
    void shutdownImGuiBackends();
    // (Re)creates renderer + game texture (+ ImGui render backend) with the
    // current vsync setting. Used at startup and when vsync is toggled.
    bool createRenderer();
    void destroyRenderer();
    void applyFilter();
    void loadFonts();  // platform UI font (SF Pro on macOS, Roboto elsewhere)

    // Main-loop stages.
    void handleEvents();
    void handleKey(const SDL_Event& e, bool down);
    void emulateAndPace(const Uint64 frameStart);
    void pumpAudio();
    void uploadFrame();
    void render();
    void drawUI();
    void drawMenuBar();
    void drawOverlay();
    void drawGraphicsSettings();  // Display/Shaders tabbed settings window
    void drawSaveStatePrompt();   // "continue from latest save?" modal
    void drawUpdateWindow();      // release-notes + update/remind window

    // ROM / state lifecycle. userSelected gates the save-state continue prompt
    // so it fires when a player picks a game, not on an internal reset.
    void openROM(const std::string& path, bool userSelected = true);
    void openRomDialog();  // native file picker -> openROM
    void loadDemo();

    // Game library (PCSX2-style cover grid).
    void scanGames();        // (re)build games from config.gamesDirs
    void addGamesFolder();   // native folder picker -> append + rescan
    void drawLibrary();      // the cover grid
    void drawShaderOverlay(const SDL_Rect& dst);  // CRT/scanline/LCD post-fx
    SDL_Texture* coverTexture(const std::string& coverPath);  // lazy, cached
    void clearCoverCache();  // invalidate cover textures (renderer teardown)
    void resetEmu();
    std::string sramPath() const;

    // Save states.
    std::string statePath(int slot) const;
    void saveState(int slot);
    void loadState(int slot);
    int  mostRecentStateSlot() const;  // newest slot on disk, or -1 if none
    void setStatus(const std::string& message);

    // Manual update check: queries for a newer release and, if found, opens
    // the release-notes window. Synchronous (the user clicked; a brief hang ok).
    void checkForUpdates();

    // In-app updater (RPCS3/PCSX2 style): the download stays in-app with a
    // progress bar, then the new bundle is swapped in and the app relaunches.
    // The download runs as a detached curl process writing a temp file; the UI
    // polls that file for progress so the app stays usable meanwhile.
    void beginUpdateDownload();
    void pollUpdateDownload();
    void applyUpdate();

    // Non-blocking update check fired once at launch (config-gated) so it never
    // freezes the window coming up; its result is polled from the main loop.
    void startStartupUpdateCheck();
    void pollStartupUpdateCheck();

    // Display helpers.
    SDL_Rect computeGameRect() const;
    void toggleFullscreen();

    FrontendOptions opts;
    Config config;
    std::string configPath;
    std::string romPath;

    // Game library (cover grid).
    struct GameEntry {
        std::string path;
        std::string title;
        std::string coverPath;  // empty if no cached cover on disk
    };
    std::vector<GameEntry> games;
    // Cover textures keyed by coverPath; a null value means "load failed,
    // don't retry". Invalidated whenever the renderer is recreated.
    std::unordered_map<std::string, SDL_Texture*> coverCache;
    bool showLibrary = true;   // grid visible (the default landing screen)
    bool gamesScanned = false;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    // Tiled post-process overlay (scanlines/CRT/LCD). Rebuilt lazily when the
    // selected shader changes; shaderTexId tracks which shader it holds.
    SDL_Texture* shaderTex = nullptr;
    int shaderTexId = -1;
    SDL_AudioDeviceID audioDev = 0;
    std::vector<int16_t> audioChunk;

    // Modal/secondary windows.
    bool showGraphicsSettings = false;
    bool showSaveStatePrompt = false;  // emulation is gated while this is up
    int pendingStateSlot = -1;         // slot offered by the continue prompt
    bool showUpdateWindow = false;
    std::string updateVersion;         // newer release version (when available)
    std::string updateNotes;           // its release notes
    std::string updateUrl;             // release page (browser fallback)
    std::string updateAssetUrl;        // direct download for in-app auto-update

    // In-app download progress. The download writes <path>.part then renames to
    // <path> on success (or touches <path>.fail); the UI polls these each frame.
    enum UpdatePhase { UP_NONE, UP_DOWNLOADING, UP_READY, UP_FAILED };
    int updatePhase = UP_NONE;
    long long updateTotalBytes = 0;    // asset size from the API (0 = unknown)
    std::string updateDownloadPath;    // final downloaded file once complete

    // Async launch-time update check (see startStartupUpdateCheck).
    bool startupCheckPending = false;
    std::string startupCheckPath;      // temp JSON the detached curl writes

    std::unique_ptr<Emulator> emu;
    std::FILE* traceFile = nullptr;
    // Larger display face for headings (SF Pro Rounded / Roboto Medium); the
    // default atlas font is the regular UI face. Null falls back to default.
    struct ImFont* headingFont = nullptr;
    bool imguiReady = false;     // ImGui context created
    bool backendsReady = false;  // ImGui SDL2/SDLRenderer2 backends inited

    uint16_t keyState = 0x03FF;  // active low
    bool running = true;
    bool paused = false;
    bool fastForward = false;
    int saveSlot = 0;            // slot used by the F5/F8 quick save/load
    float menuBarHeight = 0.0f;

    // Transient on-screen status line (e.g. "Saved state 0").
    std::string statusMessage;
    uint32_t statusExpiryMs = 0;

    // Perf counters (refreshed roughly twice a second).
    double displayFps = 0.0;
    double emuSpeed = 0.0;  // 1.0 == full GBA speed
    int displayFrameCount = 0;
    int emuFrameCount = 0;
    Uint64 counterAnchor = 0;
};
