#include "Config.hpp"

#include <SDL_scancode.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

const char* gbaKeyName(int key) {
    switch (key) {
        case GK_A:      return "A";
        case GK_B:      return "B";
        case GK_SELECT: return "Select";
        case GK_START:  return "Start";
        case GK_RIGHT:  return "Right";
        case GK_LEFT:   return "Left";
        case GK_UP:     return "Up";
        case GK_DOWN:   return "Down";
        case GK_R:      return "R";
        case GK_L:      return "L";
        default:        return "?";
    }
}

Config::Config() {
    keyMap[GK_A]      = SDL_SCANCODE_K;
    keyMap[GK_B]      = SDL_SCANCODE_L;
    keyMap[GK_SELECT] = SDL_SCANCODE_BACKSPACE;
    keyMap[GK_START]  = SDL_SCANCODE_RETURN;
    keyMap[GK_RIGHT]  = SDL_SCANCODE_D;
    keyMap[GK_LEFT]   = SDL_SCANCODE_A;
    keyMap[GK_UP]     = SDL_SCANCODE_W;
    keyMap[GK_DOWN]   = SDL_SCANCODE_S;
    keyMap[GK_R]      = SDL_SCANCODE_E;
    keyMap[GK_L]      = SDL_SCANCODE_Q;
}

namespace {
std::string keyField(int k) {
    std::string name = gbaKeyName(k);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return "key_" + name;
}
}

bool Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    recentRoms.clear();
    gamesDirs.clear();
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        auto toInt = [&](int fallback) {
            try {
                return std::stoi(val);
            } catch (...) {
                return fallback;
            }
        };
        if (key == "window_width")        windowWidth = toInt(windowWidth);
        else if (key == "window_height")  windowHeight = toInt(windowHeight);
        else if (key == "fullscreen")     fullscreen = toInt(0) != 0;
        else if (key == "vsync")          vsync = toInt(1) != 0;
        else if (key == "integer_scale")  integerScale = toInt(0) != 0;
        else if (key == "linear_filter")  linearFilter = toInt(0) != 0;
        else if (key == "shader")         shader = toInt(shader);
        else if (key == "color_correct")  colorCorrect = toInt(0) != 0;
        else if (key == "volume")         volume = toInt(volume);
        else if (key == "pause_on_focus_loss")
            pauseOnFocusLoss = toInt(1) != 0;
        else if (key == "rewind_enabled") rewindEnabled = toInt(1) != 0;
        else if (key == "prefetch")       prefetch = toInt(1) != 0;
        else if (key == "speed_index")    speedIndex = toInt(speedIndex);
        else if (key == "check_updates_on_startup")
            checkUpdatesOnStartup = toInt(1) != 0;
        else if (key == "games_dir") {
            if (!val.empty()) addGamesDir(val);
        } else if (key == "recent") {
            if (!val.empty()) recentRoms.push_back(val);
        } else if (key.rfind("key_", 0) == 0) {
            for (int k = 0; k < GK_COUNT; ++k) {
                if (key == keyField(k)) {
                    keyMap[k] = toInt(keyMap[k]);
                    break;
                }
            }
        }
    }
    if (recentRoms.size() > MAX_RECENT) {
        recentRoms.resize(MAX_RECENT);
    }
    return true;
}

bool Config::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "# gba_emu settings\n";
    out << "window_width=" << windowWidth << "\n";
    out << "window_height=" << windowHeight << "\n";
    out << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
    out << "vsync=" << (vsync ? 1 : 0) << "\n";
    out << "integer_scale=" << (integerScale ? 1 : 0) << "\n";
    out << "linear_filter=" << (linearFilter ? 1 : 0) << "\n";
    out << "shader=" << shader << "\n";
    out << "color_correct=" << (colorCorrect ? 1 : 0) << "\n";
    out << "volume=" << volume << "\n";
    out << "pause_on_focus_loss=" << (pauseOnFocusLoss ? 1 : 0) << "\n";
    out << "rewind_enabled=" << (rewindEnabled ? 1 : 0) << "\n";
    out << "prefetch=" << (prefetch ? 1 : 0) << "\n";
    out << "speed_index=" << speedIndex << "\n";
    out << "check_updates_on_startup=" << (checkUpdatesOnStartup ? 1 : 0) << "\n";
    for (const std::string& dir : gamesDirs) {
        out << "games_dir=" << dir << "\n";
    }
    for (int k = 0; k < GK_COUNT; ++k) {
        out << keyField(k) << "=" << keyMap[k] << "\n";
    }
    for (const std::string& rom : recentRoms) {
        out << "recent=" << rom << "\n";
    }
    return static_cast<bool>(out);
}

bool Config::addGamesDir(const std::string& dir) {
    if (dir.empty() ||
        std::find(gamesDirs.begin(), gamesDirs.end(), dir) != gamesDirs.end()) {
        return false;
    }
    gamesDirs.push_back(dir);
    return true;
}

void Config::addRecent(const std::string& romPath) {
    if (romPath.empty()) {
        return;
    }
    auto it = std::find(recentRoms.begin(), recentRoms.end(), romPath);
    if (it != recentRoms.end()) {
        recentRoms.erase(it);
    }
    recentRoms.insert(recentRoms.begin(), romPath);
    if (recentRoms.size() > MAX_RECENT) {
        recentRoms.resize(MAX_RECENT);
    }
}
