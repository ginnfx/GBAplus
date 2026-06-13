#include "Frontend.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <unistd.h>
#else
#  include <unistd.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "stb_image.h"
#include "tinyfiledialogs.h"

namespace {
constexpr double GBA_FPS = 16777216.0 / (PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
constexpr double TARGET_FRAME_MS = 1000.0 / GBA_FPS;
constexpr int FFWD_FRAMES = 8;
constexpr int NUM_SLOTS = 10;

enum ShaderKind { SHADER_NONE = 0, SHADER_SCANLINES, SHADER_CRT, SHADER_LCD };
struct ShaderInfo {
    const char* name;
    const char* desc;
};
constexpr std::array<ShaderInfo, 4> kShaders{{
    {"None", "Raw pixels, no overlay."},
    {"Scanlines", "Darkened gaps between rows, like a CRT raster."},
    {"CRT", "Scanlines plus a faint aperture-grille column mask."},
    {"LCD grid", "Soft per-pixel grid, like a handheld LCD panel."},
}};

#ifndef GBA_EMU_VERSION
#define GBA_EMU_VERSION "0.0.0-dev"
#endif
constexpr const char* CURRENT_VERSION = GBA_EMU_VERSION;
constexpr const char* RELEASE_URL = "https://github.com/ginnfx/GBAplus/releases";

bool versionNewer(const std::string& a, const std::string& b) {
    size_t ia = 0, ib = 0;
    while (ia < a.size() || ib < b.size()) {
        int na = 0, nb = 0;
        while (ia < a.size() && a[ia] != '.') na = na * 10 + (a[ia++] - '0');
        while (ib < b.size() && b[ib] != '.') nb = nb * 10 + (b[ib++] - '0');
        if (nb != na) return nb > na;
        if (ia < a.size()) ++ia;
        if (ib < b.size()) ++ib;
    }
    return false;
}

std::string jsonField(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return "";
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    p = json.find('"', p);
    if (p == std::string::npos) return "";
    std::string out;
    for (++p; p < json.size() && json[p] != '"'; ++p) {
        if (json[p] == '\\' && p + 1 < json.size()) {
            switch (json[++p]) {
                case 'n': out += '\n'; break;
                case 'r': break;
                case 't': out += '\t'; break;
                default:  out += json[p]; break;
            }
        } else {
            out += json[p];
        }
    }
    return out;
}

bool queryLatestRelease(std::string& version, std::string& notes,
                        std::string& url, std::string& assetUrl) {
    url = RELEASE_URL;
    const char* cmd =
        "curl -fsSL --max-time 6 "
        "-H \"Accept: application/vnd.github+json\" "
        "https://api.github.com/repos/ginnfx/GBAplus/releases/latest";
#if defined(_WIN32)
    std::FILE* pipe = _popen(cmd, "r");
#else
    std::FILE* pipe = popen(cmd, "r");
#endif
    if (pipe == nullptr) {
        return false;
    }
    std::string json;
    std::array<char, 4096> buf{};
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        json.append(buf.data(), n);
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif

    std::string tag = jsonField(json, "tag_name");
    if (tag.empty()) {
        return false;
    }
    if (tag[0] == 'v' || tag[0] == 'V') {
        tag.erase(0, 1);
    }
    version = tag;
    notes = jsonField(json, "body");
    const std::string page = jsonField(json, "html_url");
    if (!page.empty()) {
        url = page;
    }

#if defined(_WIN32)
    const char* suffix = ".exe\"";
#elif defined(__APPLE__)
    const char* suffix = ".dmg\"";
#else
    const char* suffix = ".AppImage\"";
#endif
    const size_t ap = json.find(suffix);
    if (ap != std::string::npos) {
        const size_t bp = json.find("\"browser_download_url\"", ap);
        if (bp != std::string::npos) {
            const size_t q = json.find('"', json.find(':', bp));
            const size_t e = q == std::string::npos
                                 ? std::string::npos
                                 : json.find('"', q + 1);
            if (e != std::string::npos) {
                assetUrl = json.substr(q + 1, e - q - 1);
            }
        }
    }
    return true;
}

std::string deriveSavPath(const std::string& romPath) {
    const size_t slash = romPath.find_last_of('/');
    const size_t dot = romPath.find_last_of('.');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash)) {
        return romPath.substr(0, dot) + ".sav";
    }
    return romPath + ".sav";
}

std::string currentExePath() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    return n > 0 ? std::string(buf, n) : std::string();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) != 0) return "";
    return buf;
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return buf;
#endif
}

bool launchUpdater(const std::string& assetUrl) {
    if (assetUrl.empty()) return false;
    namespace fs = std::filesystem;

#if defined(__APPLE__)
    const std::string exe = currentExePath();
    const size_t appPos = exe.rfind(".app/");
    if (appPos == std::string::npos) return false;
    const std::string bundle = exe.substr(0, appPos + 4);
    const std::string script = (fs::temp_directory_path() / "gba_emu_update.sh").string();
    std::ofstream f(script);
    if (!f) return false;
    f << "#!/bin/sh\n"
      << "while kill -0 " << getpid() << " 2>/dev/null; do sleep 0.3; done\n"
      << "DL=\"$(mktemp -t gbaupd)\"\n"
      << "if curl -fsSL -o \"$DL\" '" << assetUrl << "'; then\n"
      << "  MNT=\"$(mktemp -d)\"\n"
      << "  if hdiutil attach -nobrowse -quiet -mountpoint \"$MNT\" \"$DL\"; then\n"
      << "    APP=\"$(ls -d \"$MNT\"/*.app 2>/dev/null | head -1)\"\n"
      << "    if [ -n \"$APP\" ] && cp -R \"$APP\" \"" << bundle << ".new\"; then\n"
      << "      xattr -dr com.apple.quarantine \"" << bundle << ".new\" 2>/dev/null\n"
      << "      rm -rf \"" << bundle << ".old\"\n"
      << "      mv \"" << bundle << "\" \"" << bundle << ".old\" &&"
         " mv \"" << bundle << ".new\" \"" << bundle << "\" &&"
         " rm -rf \"" << bundle << ".old\" ||"
         " mv \"" << bundle << ".old\" \"" << bundle << "\"\n"
      << "    fi\n"
      << "    hdiutil detach \"$MNT\" -quiet 2>/dev/null\n"
      << "  fi\n"
      << "  rm -f \"$DL\"\n"
      << "fi\n"
      << "open \"" << bundle << "\"\n";
    f.close();
    return std::system(("nohup sh '" + script + "' >/dev/null 2>&1 &").c_str()) == 0;

#elif defined(__linux__)
    const char* appimg = std::getenv("APPIMAGE");
    if (appimg == nullptr) return false;
    const std::string script = (fs::temp_directory_path() / "gba_emu_update.sh").string();
    std::ofstream f(script);
    if (!f) return false;
    f << "#!/bin/sh\n"
      << "while kill -0 " << getpid() << " 2>/dev/null; do sleep 0.3; done\n"
      << "DL=\"$(mktemp)\"\n"
      << "if curl -fsSL -o \"$DL\" '" << assetUrl << "'; then\n"
      << "  chmod +x \"$DL\"\n"
      << "  mv -f \"$DL\" \"" << appimg << "\"\n"
      << "fi\n"
      << "chmod +x \"" << appimg << "\"\n"
      << "\"" << appimg << "\" >/dev/null 2>&1 &\n";
    f.close();
    return std::system(("nohup sh '" + script + "' >/dev/null 2>&1 &").c_str()) == 0;

#elif defined(_WIN32)
    const std::string exe = currentExePath();
    if (exe.empty()) return false;
    const std::string bat = (fs::temp_directory_path() / "gba_emu_update.bat").string();
    std::ofstream f(bat);
    if (!f) return false;
    const unsigned long pid = GetCurrentProcessId();
    f << "@echo off\r\n"
      << ":wait\r\n"
      << "tasklist /FI \"PID eq " << pid << "\" 2>nul | find \"" << pid
      << "\" >nul && (ping -n 2 127.0.0.1 >nul & goto wait)\r\n"
      << "curl -fsSL -o \"" << exe << ".new\" \"" << assetUrl << "\"\r\n"
      << "if exist \"" << exe << ".new\" move /Y \"" << exe << ".new\" \""
      << exe << "\" >nul\r\n"
      << "start \"\" \"" << exe << "\"\r\n"
      << "del \"%~f0\"\r\n";
    f.close();
    return std::system(("start \"\" /b cmd /c \"" + bat + "\"").c_str()) == 0;

#else
    (void)assetUrl;
    return false;
#endif
}
}

Frontend::Frontend(FrontendOptions o) : opts(std::move(o)) {}

Frontend::~Frontend() {
    if (emu && !sramPath().empty() && emu->sramDirty()) {
        emu->saveSRAM(sramPath());
    }
    if (!configPath.empty()) {
        config.save(configPath);
    }
    if (traceFile != nullptr) {
        if (emu) emu->setTraceFile(nullptr);
        std::fclose(traceFile);
    }
    if (backendsReady) {
        shutdownImGuiBackends();
    }
    if (imguiReady) {
        ImGui::DestroyContext();
    }
    clearCoverCache();
    if (shaderTex) SDL_DestroyTexture(shaderTex);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (audioDev != 0) SDL_CloseAudioDevice(audioDev);
    SDL_Quit();
}

bool Frontend::initWindowAndRenderer() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    if (char* pref = SDL_GetPrefPath("gbaplus", "gba_emu")) {
        configPath = std::string(pref) + "config.ini";
        SDL_free(pref);
        config.load(configPath);
    }

    Uint32 winFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (config.fullscreen) {
        winFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    window = SDL_CreateWindow("gba_emu", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, config.windowWidth,
                              config.windowHeight, winFlags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec audioWant{};
    audioWant.freq = APU::SAMPLE_RATE;
    audioWant.format = AUDIO_S16SYS;
    audioWant.channels = 2;
    audioWant.samples = 1024;
    audioDev = SDL_OpenAudioDevice(nullptr, 0, &audioWant, nullptr, 0);
    if (audioDev != 0) {
        SDL_PauseAudioDevice(audioDev, 0);
    } else {
        std::fprintf(stderr, "No audio device: %s\n", SDL_GetError());
    }

    initImGui();
    return createRenderer();
}

void Frontend::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.PopupRounding = 6.0f;
    loadFonts();
    imguiReady = true;
}

void Frontend::loadFonts() {
    namespace fs = std::filesystem;
    ImGuiIO& io = ImGui::GetIO();

    std::vector<std::string> bodyPaths;
    std::vector<std::string> headingPaths;
#if defined(__APPLE__)
    bodyPaths = {"/System/Library/Fonts/SFNS.ttf",
                 "/System/Library/Fonts/SFNSText.ttf",
                 "/Library/Fonts/SF-Pro.ttf"};
    headingPaths = {"/System/Library/Fonts/SFNSRounded.ttf",
                    "/System/Library/Fonts/SFNS.ttf"};
#elif defined(_WIN32)
    bodyPaths = {"C:/Windows/Fonts/Roboto-Regular.ttf",
                 "C:/Windows/Fonts/segoeui.ttf"};
    headingPaths = {"C:/Windows/Fonts/Roboto-Medium.ttf",
                    "C:/Windows/Fonts/segoeuib.ttf"};
#else
    bodyPaths = {"/usr/share/fonts/truetype/roboto/unhinted/Roboto-Regular.ttf",
                 "/usr/share/fonts/truetype/roboto/Roboto-Regular.ttf",
                 "/usr/share/fonts/google-roboto/Roboto-Regular.ttf",
                 "/usr/share/fonts/TTF/Roboto-Regular.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    headingPaths = {"/usr/share/fonts/truetype/roboto/unhinted/Roboto-Medium.ttf",
                    "/usr/share/fonts/truetype/roboto/Roboto-Medium.ttf",
                    "/usr/share/fonts/google-roboto/Roboto-Medium.ttf",
                    "/usr/share/fonts/TTF/Roboto-Medium.ttf"};
#endif
    if (char* base = SDL_GetBasePath()) {
        const std::string dir = std::string(base) + "assets/fonts/";
        SDL_free(base);
        bodyPaths.insert(bodyPaths.begin(), dir + "Roboto-Regular.ttf");
        headingPaths.insert(headingPaths.begin(), dir + "Roboto-Medium.ttf");
    }

    auto firstExisting = [](const std::vector<std::string>& paths) {
        std::error_code ec;
        for (const std::string& p : paths) {
            if (fs::exists(p, ec)) return p;
        }
        return std::string();
    };

    const std::string body = firstExisting(bodyPaths);
    if (!body.empty()) {
        io.Fonts->AddFontFromFileTTF(body.c_str(), 17.0f);
    } else {
        io.Fonts->AddFontDefault();
    }
    const std::string heading = firstExisting(headingPaths);
    if (!heading.empty()) {
        headingFont = io.Fonts->AddFontFromFileTTF(heading.c_str(), 24.0f);
    }
}

void Frontend::initImGuiBackends() {
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    backendsReady = true;
}

void Frontend::shutdownImGuiBackends() {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    backendsReady = false;
}

bool Frontend::createRenderer() {
    if (renderer) {
        if (backendsReady) shutdownImGuiBackends();
        clearCoverCache();
        if (texture) { SDL_DestroyTexture(texture); texture = nullptr; }
        if (shaderTex) { SDL_DestroyTexture(shaderTex); shaderTex = nullptr; }
        shaderTexId = -1;
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    Uint32 flags = SDL_RENDERER_ACCELERATED;
    if (config.vsync) flags |= SDL_RENDERER_PRESENTVSYNC;
    renderer = SDL_CreateRenderer(window, -1, flags);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_STREAMING, PPU::SCREEN_WIDTH,
                                PPU::SCREEN_HEIGHT);
    if (!texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }
    applyFilter();
    if (imguiReady) initImGuiBackends();
    return true;
}

void Frontend::applyFilter() {
    if (texture) {
        SDL_SetTextureScaleMode(
            texture, config.linearFilter ? SDL_ScaleModeLinear
                                         : SDL_ScaleModeNearest);
    }
}

void Frontend::toggleFullscreen() {
    config.fullscreen = !config.fullscreen;
    SDL_SetWindowFullscreen(
        window, config.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

std::string Frontend::sramPath() const {
    return romPath.empty() ? std::string() : deriveSavPath(romPath);
}

void Frontend::openROM(const std::string& path, bool userSelected) {
    if (emu && !sramPath().empty() && emu->sramDirty()) {
        emu->saveSRAM(sramPath());
    }
    auto next = std::make_unique<Emulator>();
    if (!opts.biosPath.empty()) {
        next->loadBIOS(opts.biosPath);
    }
    if (!next->loadROM(path)) {
        std::fprintf(stderr, "Failed to load ROM: %s\n", path.c_str());
        return;
    }
    romPath = path;
    next->loadSRAM(sramPath());
    if (traceFile != nullptr) {
        next->setTraceFile(traceFile);
    }
    emu = std::move(next);
    config.addRecent(path);
    keyState = 0x03FF;
    paused = false;
    showLibrary = false;

    showSaveStatePrompt = false;
    pendingStateSlot = -1;
    if (userSelected) {
        const int slot = mostRecentStateSlot();
        if (slot >= 0) {
            pendingStateSlot = slot;
            showSaveStatePrompt = true;
        }
    }
}

void Frontend::openRomDialog() {
    static const char* const filters[] = {"*.gba", "*.agb", "*.bin"};
    const char* picked = tinyfd_openFileDialog(
        "Open GBA ROM", "", 3, filters, "GBA ROMs (*.gba, *.agb, *.bin)", 0);
    if (picked != nullptr) {
        openROM(picked);
    }
}

void Frontend::scanGames() {
    namespace fs = std::filesystem;
    games.clear();
    gamesScanned = true;
    std::error_code ec;
    for (const std::string& gamesDir : config.gamesDirs) {
        if (gamesDir.empty()) {
            continue;
        }
        const fs::path coversDir = fs::path(gamesDir) / "covers";
        for (fs::directory_iterator it(gamesDir, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) {
                continue;
            }
            const fs::path p = it->path();
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (ext != ".gba" && ext != ".agb") {
                continue;
            }
            GameEntry g;
            g.path = p.string();
            g.title = p.stem().string();
            const fs::path png = coversDir / (g.title + ".png");
            const fs::path jpg = coversDir / (g.title + ".jpg");
            if (fs::exists(png, ec)) {
                g.coverPath = png.string();
            } else if (fs::exists(jpg, ec)) {
                g.coverPath = jpg.string();
            }
            games.push_back(std::move(g));
        }
    }
    std::sort(games.begin(), games.end(),
              [](const GameEntry& a, const GameEntry& b) {
                  return a.title < b.title;
              });
}

void Frontend::addGamesFolder() {
    const char* start =
        config.gamesDirs.empty() ? "" : config.gamesDirs.back().c_str();
    const char* dir =
        tinyfd_selectFolderDialog("Add Games Folder to Library", start);
    if (dir != nullptr) {
        if (config.addGamesDir(dir)) {
            scanGames();
        }
        showLibrary = true;
    }
}

void Frontend::clearCoverCache() {
    for (auto& [path, tex] : coverCache) {
        if (tex != nullptr) {
            SDL_DestroyTexture(tex);
        }
    }
    coverCache.clear();
}

SDL_Texture* Frontend::coverTexture(const std::string& coverPath) {
    if (coverPath.empty()) {
        return nullptr;
    }
    const auto it = coverCache.find(coverPath);
    if (it != coverCache.end()) {
        return it->second;
    }
    SDL_Texture* tex = nullptr;
    std::ifstream in(coverPath, std::ios::binary | std::ios::ate);
    if (in) {
        const std::streamsize n = in.tellg();
        in.seekg(0);
        std::vector<unsigned char> bytes(static_cast<size_t>(n));
        if (in.read(reinterpret_cast<char*>(bytes.data()), n)) {
            int w = 0, h = 0, channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(
                bytes.data(), static_cast<int>(n), &w, &h, &channels, 4);
            if (pixels != nullptr) {
                tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                        SDL_TEXTUREACCESS_STATIC, w, h);
                if (tex != nullptr) {
                    SDL_UpdateTexture(tex, nullptr, pixels, w * 4);
                    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
                    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                }
                stbi_image_free(pixels);
            }
        }
    }
    coverCache.emplace(coverPath, tex);
    return tex;
}

void Frontend::drawLibrary() {
    if (!gamesScanned) {
        scanGames();
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, menuBarHeight));
    ImGui::SetNextWindowSize(
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - menuBarHeight));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("##library", nullptr, flags)) {
        ImGui::End();
        return;
    }

    if (headingFont != nullptr) {
        ImGui::PushFont(headingFont);
        ImGui::TextUnformatted("Game Library");
        ImGui::PopFont();
    } else {
        ImGui::TextUnformatted("Game Library");
    }
    ImGui::Spacing();

    if (config.gamesDirs.empty()) {
        ImGui::TextWrapped(
            "No games folders yet.\n\nFile > Add Games to Library... to pick a "
            "folder of .gba ROMs. You can add more than one folder.");
        ImGui::End();
        return;
    }
    if (games.empty()) {
        ImGui::TextWrapped("No .gba/.agb ROMs found in your library folders:");
        for (const std::string& dir : config.gamesDirs) {
            ImGui::BulletText("%s", dir.c_str());
        }
        ImGui::End();
        return;
    }

    constexpr float COVER_W = 150.0f;
    constexpr float COVER_H = 150.0f;
    constexpr float COVER_GAP = 28.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(COVER_GAP, COVER_GAP));
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    int columns = static_cast<int>((avail + spacing) / (COVER_W + spacing));
    if (columns < 1) columns = 1;
    const ImVec2 cellSize(
        COVER_W, COVER_H + ImGui::GetTextLineHeightWithSpacing() * 2.0f + 4.0f);

    std::string launch;
    int col = 0;
    for (const GameEntry& g : games) {
        ImGui::PushID(g.path.c_str());
        ImGui::BeginChild("cell", cellSize, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        SDL_Texture* cover = coverTexture(g.coverPath);
        bool clicked = false;
        if (cover != nullptr) {
            clicked = ImGui::ImageButton(
                "##cover", reinterpret_cast<ImTextureID>(cover),
                ImVec2(COVER_W, COVER_H));
        } else {
            clicked = ImGui::Button(g.title.c_str(), ImVec2(COVER_W, COVER_H));
        }
        ImGui::PushTextWrapPos(COVER_W);
        ImGui::TextWrapped("%s", g.title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopID();
        if (clicked) {
            launch = g.path;
        }
        if (++col < columns) {
            ImGui::SameLine();
        } else {
            col = 0;
        }
    }
    ImGui::PopStyleVar();
    ImGui::End();

    if (!launch.empty()) {
        openROM(launch);
    }
}

void Frontend::loadDemo() {
    emu = std::make_unique<Emulator>();
    emu->loadDemo();
    romPath.clear();
    showLibrary = false;
}

void Frontend::resetEmu() {
    if (!romPath.empty()) {
        openROM(romPath, false);
    } else if (emu) {
        loadDemo();
    }
}

std::string Frontend::statePath(int slot) const {
    if (romPath.empty()) {
        return std::string();
    }
    const size_t slash = romPath.find_last_of('/');
    const size_t dot = romPath.find_last_of('.');
    const std::string base =
        (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            ? romPath.substr(0, dot)
            : romPath;
    return base + ".ss" + std::to_string(slot);
}

void Frontend::setStatus(const std::string& message) {
    statusMessage = message;
    statusExpiryMs = SDL_GetTicks() + 2500;
}

void Frontend::saveState(int slot) {
    if (!emu || romPath.empty()) {
        setStatus("No ROM loaded");
        return;
    }
    std::vector<uint8_t> buf;
    emu->saveState(buf);
    std::ofstream out(statePath(slot), std::ios::binary | std::ios::trunc);
    if (out && out.write(reinterpret_cast<const char*>(buf.data()),
                         static_cast<std::streamsize>(buf.size()))) {
        setStatus("Saved state " + std::to_string(slot));
    } else {
        setStatus("Save failed");
    }
}

void Frontend::loadState(int slot) {
    if (!emu || romPath.empty()) {
        setStatus("No ROM loaded");
        return;
    }
    std::ifstream in(statePath(slot), std::ios::binary | std::ios::ate);
    if (!in) {
        setStatus("No state in slot " + std::to_string(slot));
        return;
    }
    const std::streamsize n = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (in.read(reinterpret_cast<char*>(buf.data()), n) &&
        emu->loadState(buf)) {
        setStatus("Loaded state " + std::to_string(slot));
    } else {
        setStatus("Load failed (wrong ROM or corrupt state)");
    }
}

int Frontend::mostRecentStateSlot() const {
    namespace fs = std::filesystem;
    if (romPath.empty()) {
        return -1;
    }
    int best = -1;
    fs::file_time_type newest{};
    std::error_code ec;
    for (int s = 0; s < NUM_SLOTS; ++s) {
        const std::string path = statePath(s);
        if (!fs::exists(path, ec)) {
            continue;
        }
        const fs::file_time_type t = fs::last_write_time(path, ec);
        if (ec) {
            continue;
        }
        if (best < 0 || t > newest) {
            newest = t;
            best = s;
        }
    }
    return best;
}

void Frontend::handleEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL2_ProcessEvent(&e);
        switch (e.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED &&
                    !config.fullscreen) {
                    config.windowWidth = e.window.data1;
                    config.windowHeight = e.window.data2;
                }
                break;
            case SDL_KEYDOWN:
                handleKey(e, true);
                break;
            case SDL_KEYUP:
                handleKey(e, false);
                break;
        }
    }
}

void Frontend::handleKey(const SDL_Event& e, bool down) {
    if (ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }
    const SDL_Scancode sc = e.key.keysym.scancode;
    if (down) {
        switch (sc) {
            case SDL_SCANCODE_ESCAPE: running = false; return;
            case SDL_SCANCODE_F11:    toggleFullscreen(); return;
            case SDL_SCANCODE_P:      paused = !paused; return;
            case SDL_SCANCODE_F5:     saveState(saveSlot); return;
            case SDL_SCANCODE_F8:     loadState(saveSlot); return;
            case SDL_SCANCODE_O:
                if (e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) {
                    openRomDialog();
                    return;
                }
                break;
            default: break;
        }
    }
    if (sc == SDL_SCANCODE_TAB || sc == SDL_SCANCODE_SPACE) {
        fastForward = down;
        return;
    }
    for (int k = 0; k < GK_COUNT; ++k) {
        if (config.keyMap[k] == sc) {
            const uint16_t bit = static_cast<uint16_t>(1u << k);
            if (down) {
                keyState &= static_cast<uint16_t>(~bit);
            } else {
                keyState |= bit;
            }
            break;
        }
    }
}

void Frontend::pumpAudio() {
    if (audioDev == 0 || !emu) {
        return;
    }
    const size_t pending = emu->pendingAudioFrames();
    if (pending == 0) {
        return;
    }
    audioChunk.resize(pending * 2);
    const size_t frames = emu->drainAudio(audioChunk.data(), pending);
    const size_t samples = frames * 2;
    if (config.volume != 100) {
        for (size_t i = 0; i < samples; ++i) {
            audioChunk[i] = static_cast<int16_t>(
                audioChunk[i] * config.volume / 100);
        }
    }
    if (SDL_GetQueuedAudioSize(audioDev) < 16384) {
        SDL_QueueAudio(audioDev, audioChunk.data(),
                       static_cast<Uint32>(samples * sizeof(int16_t)));
    }
}

void Frontend::uploadFrame() {
    if (emu) {
        SDL_UpdateTexture(
            texture, nullptr, emu->framebuffer().data(),
            PPU::SCREEN_WIDTH * static_cast<int>(sizeof(uint32_t)));
    }
}

SDL_Rect Frontend::computeGameRect() const {
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    const int top = static_cast<int>(menuBarHeight);
    int availH = oh - top;
    if (availH < 1) availH = 1;
    double scale = std::min(static_cast<double>(ow) / PPU::SCREEN_WIDTH,
                            static_cast<double>(availH) / PPU::SCREEN_HEIGHT);
    if (config.integerScale) {
        scale = scale < 1.0 ? 1.0 : std::floor(scale);
    }
    const int w = static_cast<int>(PPU::SCREEN_WIDTH * scale);
    const int h = static_cast<int>(PPU::SCREEN_HEIGHT * scale);
    SDL_Rect r;
    r.w = w;
    r.h = h;
    r.x = (ow - w) / 2;
    r.y = top + (availH - h) / 2;
    return r;
}

void Frontend::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    menuBarHeight = ImGui::GetWindowSize().y;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open ROM...", "Ctrl+O")) {
            openRomDialog();
        }
        if (ImGui::MenuItem("Game Library", nullptr, showLibrary)) {
            showLibrary = !showLibrary;
        }
        if (ImGui::BeginMenu("Recent", !config.recentRoms.empty())) {
            std::string chosen;
            for (const std::string& entry : config.recentRoms) {
                if (ImGui::MenuItem(entry.c_str())) {
                    chosen = entry;
                    break;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Recent")) {
                config.recentRoms.clear();
            }
            ImGui::EndMenu();
            if (!chosen.empty()) {
                openROM(chosen);
            }
        }
        if (ImGui::MenuItem("Add Games to Library...")) {
            addGamesFolder();
        }
        if (ImGui::BeginMenu("Library Folders", !config.gamesDirs.empty())) {
            for (const std::string& dir : config.gamesDirs) {
                ImGui::MenuItem(dir.c_str(), nullptr, false, false);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove All Folders")) {
                config.gamesDirs.clear();
                scanGames();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset", nullptr, false, emu != nullptr)) {
            resetEmu();
        }
        if (ImGui::MenuItem("Quit")) {
            running = false;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Emulation")) {
        ImGui::MenuItem("Pause", "P", &paused);
        ImGui::MenuItem("Fast-forward", "Tab", &fastForward);
        ImGui::Separator();
        const bool stateable = emu != nullptr && !romPath.empty();
        if (ImGui::MenuItem("Quick Save", "F5", false, stateable)) {
            saveState(saveSlot);
        }
        if (ImGui::MenuItem("Quick Load", "F8", false, stateable)) {
            loadState(saveSlot);
        }
        if (ImGui::BeginMenu("Save State", stateable)) {
            for (int s = 0; s < NUM_SLOTS; ++s) {
                const std::string label = "Slot " + std::to_string(s);
                if (ImGui::MenuItem(label.c_str(), nullptr, s == saveSlot)) {
                    saveSlot = s;
                    saveState(s);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Load State", stateable)) {
            for (int s = 0; s < NUM_SLOTS; ++s) {
                const std::string label = "Slot " + std::to_string(s);
                if (ImGui::MenuItem(label.c_str(), nullptr, s == saveSlot)) {
                    saveSlot = s;
                    loadState(s);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Video")) {
        if (ImGui::MenuItem("Fullscreen", "F11", config.fullscreen)) {
            toggleFullscreen();
        }
        if (ImGui::MenuItem("VSync", nullptr, config.vsync)) {
            config.vsync = !config.vsync;
            createRenderer();
        }
        ImGui::MenuItem("Integer scale", nullptr, &config.integerScale);
        if (ImGui::MenuItem("Linear filter", nullptr, config.linearFilter)) {
            config.linearFilter = !config.linearFilter;
            applyFilter();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Shader: %s",
                            kShaders[config.shader % kShaders.size()].name);
        if (ImGui::MenuItem("Graphics Settings...")) {
            showGraphicsSettings = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Input")) {
        ImGui::MenuItem("Remap...", nullptr, false, false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Check for Updates...")) {
            checkForUpdates();
        }
        ImGui::MenuItem("Version", CURRENT_VERSION, false, false);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Frontend::drawOverlay() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 8.0f, menuBarHeight + 8.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##overlay", nullptr, flags)) {
        ImGui::Text("FPS: %.1f", displayFps);
        ImGui::Text("Speed: %.0f%%", emuSpeed * 100.0);
        if (paused) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "PAUSED");
        } else if (fastForward) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), ">> FF");
        }
        if (!statusMessage.empty() && SDL_GetTicks() < statusExpiryMs) {
            ImGui::Separator();
            ImGui::TextUnformatted(statusMessage.c_str());
        }
    }
    ImGui::End();
}

void Frontend::drawGraphicsSettings() {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 300.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics Settings", &showGraphicsSettings)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##graphics_tabs")) {
        if (ImGui::BeginTabItem("Display")) {
            if (ImGui::Checkbox("Fullscreen", &config.fullscreen)) {
                config.fullscreen = !config.fullscreen;
                toggleFullscreen();
            }
            if (ImGui::Checkbox("VSync", &config.vsync)) {
                createRenderer();
            }
            ImGui::Checkbox("Integer scale", &config.integerScale);
            if (ImGui::Checkbox("Linear filter (smooth)", &config.linearFilter)) {
                applyFilter();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shaders")) {
            ImGui::TextWrapped(
                "Pick a display style. Applied live over the game image.");
            ImGui::Spacing();
            for (int i = 0; i < static_cast<int>(kShaders.size()); ++i) {
                if (ImGui::RadioButton(kShaders[i].name, config.shader == i)) {
                    config.shader = i;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("- %s", kShaders[i].desc);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void Frontend::drawSaveStatePrompt() {
    constexpr const char* kTitle = "Resume Game?";
    if (showSaveStatePrompt && !ImGui::IsPopupOpen(kTitle)) {
        ImGui::OpenPopup(kTitle);
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kTitle, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "This game has saved states. Do you want to continue from the most "
            "recent save?");
        ImGui::Spacing();
        if (ImGui::Button("Yes - Continue", ImVec2(140.0f, 0.0f))) {
            if (pendingStateSlot >= 0) {
                loadState(pendingStateSlot);
            }
            showSaveStatePrompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No - Start fresh", ImVec2(140.0f, 0.0f))) {
            showSaveStatePrompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Frontend::checkForUpdates() {
    std::string version, notes, url, assetUrl;
    if (queryLatestRelease(version, notes, url, assetUrl) &&
        versionNewer(CURRENT_VERSION, version)) {
        updateVersion = version;
        updateNotes = notes;
        updateUrl = url;
        updateAssetUrl = assetUrl;
        showUpdateWindow = true;
    } else {
        setStatus("You're up to date (v" + std::string(CURRENT_VERSION) + ")");
    }
}

void Frontend::drawUpdateWindow() {
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Update Available", &showUpdateWindow,
                      ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (headingFont != nullptr) {
        ImGui::PushFont(headingFont);
        ImGui::Text("Version %s is available", updateVersion.c_str());
        ImGui::PopFont();
    } else {
        ImGui::Text("Version %s is available", updateVersion.c_str());
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", updateNotes.c_str());
    ImGui::Separator();
    if (ImGui::Button("Update now", ImVec2(130.0f, 0.0f))) {
        if (launchUpdater(updateAssetUrl)) {
            setStatus("Downloading update; the app will relaunch...");
            showUpdateWindow = false;
            running = false;
        } else {
            SDL_OpenURL(updateUrl.c_str());
            showUpdateWindow = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("View page", ImVec2(110.0f, 0.0f))) {
        SDL_OpenURL(updateUrl.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Remind me later", ImVec2(130.0f, 0.0f))) {
        showUpdateWindow = false;
    }
    ImGui::End();
}

void Frontend::drawUI() {
    drawMenuBar();
    if (showLibrary) {
        drawLibrary();
    } else if (emu) {
        drawOverlay();
    }
    if (showGraphicsSettings) {
        drawGraphicsSettings();
    }
    drawSaveStatePrompt();
    if (showUpdateWindow) {
        drawUpdateWindow();
    }
}

void Frontend::drawShaderOverlay(const SDL_Rect& dst) {
    if (config.shader <= SHADER_NONE ||
        config.shader >= static_cast<int>(kShaders.size())) {
        return;
    }
    if (shaderTex == nullptr || shaderTexId != config.shader) {
        if (shaderTex) { SDL_DestroyTexture(shaderTex); shaderTex = nullptr; }
        constexpr int SCALE = 3;
        const int w = PPU::SCREEN_WIDTH * SCALE;
        const int h = PPU::SCREEN_HEIGHT * SCALE;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
        for (int y = 0; y < h; ++y) {
            const int r = y % SCALE;
            for (int x = 0; x < w; ++x) {
                const int c = x % SCALE;
                int a = 0;
                switch (config.shader) {
                    case SHADER_SCANLINES:
                        a = (r == SCALE - 1) ? 120 : 0;
                        break;
                    case SHADER_CRT:
                        a = (r == SCALE - 1) ? 110 : 0;
                        if (c == SCALE - 1) a += 50;
                        break;
                    case SHADER_LCD:
                        a = (r == SCALE - 1 || c == SCALE - 1) ? 70 : 0;
                        break;
                    default:
                        break;
                }
                if (a > 200) a = 200;
                uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = p[1] = p[2] = 0;
                p[3] = static_cast<uint8_t>(a);
            }
        }
        shaderTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STATIC, w, h);
        if (shaderTex != nullptr) {
            SDL_UpdateTexture(shaderTex, nullptr, px.data(), w * 4);
            SDL_SetTextureBlendMode(shaderTex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(shaderTex, SDL_ScaleModeLinear);
        }
        shaderTexId = config.shader;
    }
    if (shaderTex != nullptr) {
        SDL_RenderCopy(renderer, shaderTex, nullptr, &dst);
    }
}

void Frontend::render() {
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    drawUI();
    ImGui::Render();

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (emu && !showLibrary) {
        const SDL_Rect dst = computeGameRect();
        SDL_RenderCopy(renderer, texture, nullptr, &dst);
        drawShaderOverlay(dst);
    }
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
}

int Frontend::run() {
    if (!initWindowAndRenderer()) {
        return 1;
    }

    if (opts.demo) {
        loadDemo();
    } else if (!opts.romPath.empty()) {
        openROM(opts.romPath);
        if (!emu) {
            return 1;
        }
    }
    if (emu && opts.trace) {
        traceFile = std::fopen("emu_trace.log", "w");
        if (traceFile) {
            emu->setTraceFile(traceFile);
            std::printf("Tracing CPU state to emu_trace.log\n");
        }
    }

    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    counterAnchor = SDL_GetPerformanceCounter();

    while (running) {
        const Uint64 frameStart = SDL_GetPerformanceCounter();
        handleEvents();

        if (emu && !paused && !showLibrary && !showSaveStatePrompt) {
            const int frames = fastForward ? FFWD_FRAMES : 1;
            for (int i = 0; i < frames; ++i) {
                emu->setKeys(keyState);
                emu->runFrame();
                ++emuFrameCount;
                pumpAudio();
            }
        }

        uploadFrame();
        render();
        ++displayFrameCount;

        const Uint64 now = SDL_GetPerformanceCounter();
        const double sinceCounter =
            static_cast<double>(now - counterAnchor) / perfFreq;
        if (sinceCounter >= 0.5) {
            displayFps = displayFrameCount / sinceCounter;
            emuSpeed = (emuFrameCount / sinceCounter) / GBA_FPS;
            displayFrameCount = 0;
            emuFrameCount = 0;
            counterAnchor = now;
        }

        const double elapsedMs =
            static_cast<double>(now - frameStart) * 1000.0 / perfFreq;
        if (!fastForward && elapsedMs < TARGET_FRAME_MS) {
            SDL_Delay(static_cast<Uint32>(TARGET_FRAME_MS - elapsedMs));
        }
    }
    return 0;
}
