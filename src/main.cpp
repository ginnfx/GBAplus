#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "APU.hpp"
#include "ARM7TDMI.hpp"
#include "Bus.hpp"
#include "DMA.hpp"
#include "PPU.hpp"
#include "Timers.hpp"

namespace {

constexpr int WINDOW_WIDTH  = 480;
constexpr int WINDOW_HEIGHT = 320;

constexpr int CYCLES_PER_FRAME = PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL;
constexpr double TARGET_FRAME_MS = 1000.0 * CYCLES_PER_FRAME / 16777216.0;

constexpr int APPROX_CYCLES_PER_INSTR = 4;

constexpr uint32_t REG_KEYINPUT = 0x04000130;

enum KeypadBit : uint16_t {
    KEY_A      = 1u << 0,
    KEY_B      = 1u << 1,
    KEY_SELECT = 1u << 2,
    KEY_START  = 1u << 3,
    KEY_RIGHT  = 1u << 4,
    KEY_LEFT   = 1u << 5,
    KEY_UP     = 1u << 6,
    KEY_DOWN   = 1u << 7,
    KEY_R      = 1u << 8,
    KEY_L      = 1u << 9,
    KEY_ALL_RELEASED = 0x03FF,
};

uint16_t keypadBitForScancode(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_W:         return KEY_UP;
        case SDL_SCANCODE_A:         return KEY_LEFT;
        case SDL_SCANCODE_S:         return KEY_DOWN;
        case SDL_SCANCODE_D:         return KEY_RIGHT;
        case SDL_SCANCODE_K:         return KEY_A;
        case SDL_SCANCODE_L:         return KEY_B;
        case SDL_SCANCODE_RETURN:    return KEY_START;
        case SDL_SCANCODE_BACKSPACE: return KEY_SELECT;
        case SDL_SCANCODE_Q:         return KEY_L;
        case SDL_SCANCODE_E:         return KEY_R;
        default:                     return 0;
    }
}

std::string savePathForROM(const std::string& romPath) {
    const size_t slash = romPath.find_last_of('/');
    const size_t dot = romPath.find_last_of('.');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash)) {
        return romPath.substr(0, dot) + ".sav";
    }
    return romPath + ".sav";
}

struct Options {
    std::string romPath;
    std::string biosPath;
    bool demo = false;
    bool trace = false;
};

int runEmulator(const Options& opts) {
    Bus bus;
    ARM7TDMI cpu(bus);
    PPU ppu(bus);
    DMA dma(bus);
    bus.attachDMA(&dma);
    Timers timers(bus);
    bus.attachTimers(&timers);
    APU apu(bus);
    bus.attachAPU(&apu);

    if (!opts.biosPath.empty()) {
        if (bus.loadBIOS(opts.biosPath)) {
            std::printf("Loaded BIOS from %s\n", opts.biosPath.c_str());
        } else {
            std::fprintf(stderr,
                         "Failed to load BIOS %s; using HLE IRQ vector\n",
                         opts.biosPath.c_str());
        }
    }

    std::string savPath;
    if (opts.demo) {
        bus.write16(0x04000000, 0x0403);
        for (int y = 0; y < PPU::SCREEN_HEIGHT; ++y) {
            for (int x = 0; x < PPU::SCREEN_WIDTH; ++x) {
                const uint16_t r = static_cast<uint16_t>(x * 31 / 239);
                const uint16_t g = static_cast<uint16_t>(y * 31 / 159);
                const uint16_t b =
                    static_cast<uint16_t>(31 - x * 31 / 239);
                bus.write16(0x06000000 + (y * 240 + x) * 2,
                            static_cast<uint16_t>(r | (g << 5) | (b << 10)));
            }
        }
        bus.write32(0x02000000, 0xEAFFFFFE);
        cpu.reset();
        cpu.setReg(15, 0x02000000);
    } else {
        if (!bus.loadROM(opts.romPath)) {
            std::fprintf(stderr, "Failed to load ROM: %s\n",
                         opts.romPath.c_str());
            return 1;
        }
        std::printf("Loaded %s (%zu bytes)\n", opts.romPath.c_str(),
                    bus.romSize());
        const char* backupName = "SRAM";
        switch (bus.backupType()) {
            case Bus::BackupType::Flash64:  backupName = "Flash 64K"; break;
            case Bus::BackupType::Flash128: backupName = "Flash 128K"; break;
            case Bus::BackupType::EEPROM: backupName = "EEPROM"; break;
            default: break;
        }
        std::printf("Backup type: %s\n", backupName);
        savPath = savePathForROM(opts.romPath);
        if (bus.loadCartridgeData(savPath)) {
            std::printf("Loaded save data from %s\n", savPath.c_str());
        }
        cpu.reset();
    }

    std::FILE* traceFile = nullptr;
    if (opts.trace) {
        traceFile = std::fopen("emu_trace.log", "w");
        if (traceFile != nullptr) {
            cpu.setTraceFile(traceFile);
            std::printf("Tracing CPU state to emu_trace.log\n");
        } else {
            std::fprintf(stderr, "Could not open emu_trace.log\n");
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(
        "gba_emu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING,
        PPU::SCREEN_WIDTH, PPU::SCREEN_HEIGHT);
    if (!window || !renderer || !texture) {
        std::fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_AudioSpec audioWant{};
    audioWant.freq = APU::SAMPLE_RATE;
    audioWant.format = AUDIO_S16SYS;
    audioWant.channels = 2;
    audioWant.samples = 1024;
    const SDL_AudioDeviceID audioDev =
        SDL_OpenAudioDevice(nullptr, 0, &audioWant, nullptr, 0);
    if (audioDev != 0) {
        SDL_PauseAudioDevice(audioDev, 0);
    } else {
        std::fprintf(stderr, "No audio device: %s\n", SDL_GetError());
    }
    std::vector<int16_t> audioChunk;

    uint16_t keyState = KEY_ALL_RELEASED;
    bus.writeIODirect16(REG_KEYINPUT, keyState);

    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    bool quit = false;
    while (!quit) {
        const Uint64 frameStart = SDL_GetPerformanceCounter();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    quit = true;
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        quit = true;
                        break;
                    }
                    const uint16_t bit =
                        keypadBitForScancode(event.key.keysym.scancode);
                    if (event.type == SDL_KEYDOWN) {
                        keyState &= static_cast<uint16_t>(~bit);
                    } else {
                        keyState |= bit;
                    }
                    break;
                }
            }
        }
        bus.writeIODirect16(REG_KEYINPUT, keyState);

        for (int c = 0; c < CYCLES_PER_FRAME; c += APPROX_CYCLES_PER_INSTR) {
            cpu.step();
            ppu.step(APPROX_CYCLES_PER_INSTR);
            timers.step(APPROX_CYCLES_PER_INSTR);
            apu.step(APPROX_CYCLES_PER_INSTR);
        }

        if (audioDev != 0 && apu.pendingFrames() > 0) {
            audioChunk.resize(apu.pendingFrames() * 2);
            const size_t frames =
                apu.drainSamples(audioChunk.data(), apu.pendingFrames());
            if (SDL_GetQueuedAudioSize(audioDev) < 16384) {
                SDL_QueueAudio(audioDev, audioChunk.data(),
                               static_cast<Uint32>(frames * 2 *
                                                   sizeof(int16_t)));
            }
        }

        if (ppu.frameReady()) {
            SDL_UpdateTexture(texture, nullptr, ppu.framebuffer().data(),
                              PPU::SCREEN_WIDTH * static_cast<int>(
                                  sizeof(uint32_t)));
        }
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        const double elapsedMs =
            static_cast<double>(SDL_GetPerformanceCounter() - frameStart) *
            1000.0 / static_cast<double>(perfFreq);
        if (elapsedMs < TARGET_FRAME_MS) {
            SDL_Delay(static_cast<Uint32>(TARGET_FRAME_MS - elapsedMs));
        }
    }

    if (!savPath.empty() && bus.sramDirty()) {
        if (bus.saveCartridgeData(savPath)) {
            std::printf("Saved cartridge data to %s\n", savPath.c_str());
        }
    }
    if (traceFile != nullptr) {
        cpu.setTraceFile(nullptr);
        std::fclose(traceFile);
    }

    if (audioDev != 0) {
        SDL_CloseAudioDevice(audioDev);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

}

int main(int argc, char* argv[]) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--demo") == 0) {
            opts.demo = true;
        } else if (std::strcmp(argv[i], "--trace") == 0) {
            opts.trace = true;
        } else if ((std::strcmp(argv[i], "-b") == 0 ||
                    std::strcmp(argv[i], "--bios") == 0) &&
                   i + 1 < argc) {
            opts.biosPath = argv[++i];
        } else {
            opts.romPath = argv[i];
        }
    }

    if (opts.romPath.empty() && !opts.demo) {
        std::fprintf(
            stderr,
            "Usage: gba_emu <rom.gba> [-b|--bios bios.bin] [--trace] | --demo\n"
            "  --trace writes per-instruction CPU state to emu_trace.log\n"
            "  (compare with mGBA via compare_logs.py)\n"
            "Keys: WASD = D-Pad, K = A, L = B, Q/E = L/R,\n"
            "      Enter = Start, Backspace = Select, Esc = quit\n");
        return 1;
    }
    return runEmulator(opts);
}
