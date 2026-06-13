#include "APU.hpp"

#include <algorithm>

#include "Bus.hpp"
#include "Log.hpp"

namespace {
constexpr uint32_t IO_BASE = 0x04000000;
constexpr uint32_t REG_SOUNDCNT_L = 0x80;
constexpr uint32_t REG_SOUNDCNT_H = 0x82;
constexpr uint32_t REG_SOUNDCNT_X = 0x84;
constexpr uint32_t WAVE_RAM = 0x90;

// Duty patterns, one bit per 1/8 of the square waveform (GB convention):
// 12.5%, 25%, 50%, 75%.
constexpr uint8_t DUTY[4] = {0x01, 0x81, 0x87, 0x7E};

// The frame sequencer runs at 512 Hz and clocks length (256 Hz),
// sweep (128 Hz), and envelope (64 Hz).
constexpr int FRAME_SEQ_CYCLES = 16777216 / 512;

int16_t clampSample(int value) {
    return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}
}  // namespace

APU::APU(Bus& bus) : bus(bus) {}

bool APU::masterEnabled() const {
    return bus.read8(IO_BASE + REG_SOUNDCNT_X) & 0x80;
}

void APU::onRegisterWrite(uint32_t offset) {
    // While the master enable (SOUNDCNT_X bit 7) is off, the four PSG channel
    // control registers (0x60-0x7F) are locked on the GBA: writes are ignored
    // until the master is switched back on. Wave RAM and the SOUNDCNT_* mixer
    // registers stay accessible regardless.
    if (offset >= 0x60 && offset < 0x80 && !masterEnabled()) {
        return;
    }
    switch (offset & ~1u) {
        case 0x60: {  // SOUND1CNT_L: sweep
            const uint16_t v = bus.read16(IO_BASE + 0x60);
            square[0].sweepShift = v & 7;
            square[0].sweepDecrease = v & 8;
            square[0].sweepPeriod = (v >> 4) & 7;
            break;
        }
        case 0x62:    // SOUND1CNT_H / SOUND2CNT_L: length, duty, envelope
        case 0x68: {
            const int i = offset >= 0x68 ? 1 : 0;
            const uint16_t v = bus.read16(IO_BASE + (i ? 0x68u : 0x62u));
            Square& sq = square[i];
            sq.length = 64 - (v & 63);
            sq.duty = (v >> 6) & 3;
            sq.env.period = (v >> 8) & 7;
            sq.env.increase = v & (1u << 11);
            sq.env.initial = (v >> 12) & 15;
            if (sq.env.initial == 0 && !sq.env.increase) {
                sq.active = false;  // DAC off silences the channel
            }
            break;
        }
        case 0x64:    // SOUND1CNT_X / SOUND2CNT_H: frequency, trigger
        case 0x6C: {
            const int i = offset >= 0x6C ? 1 : 0;
            const uint16_t v = bus.read16(IO_BASE + (i ? 0x6Cu : 0x64u));
            Square& sq = square[i];
            sq.freq = v & 0x7FF;
            sq.lengthEnabled = v & (1u << 14);
            if ((offset & 1) && (v & (1u << 15))) {
                trigger(i);
            }
            break;
        }
        case 0x70: {  // SOUND3CNT_L: wave DAC, RAM dimension + bank select
            const uint16_t v = bus.read16(IO_BASE + 0x70);
            wave.dacOn = v & 0x80;
            if (!wave.dacOn) {
                wave.active = false;
            }
            waveDimension = v & 0x20;
            const int newBank = (v >> 6) & 1;
            if (newBank != wavePlayBank) {
                wavePlayBank = newBank;
                swapWaveBank();  // bring the newly selected bank into the window
            }
            break;
        }
        case 0x72: {  // SOUND3CNT_H: length, volume
            const uint16_t v = bus.read16(IO_BASE + 0x72);
            wave.length = 256 - (v & 0xFF);
            wave.volumeCode = (v >> 13) & 3;
            wave.force75 = v & (1u << 15);
            break;
        }
        case 0x74: {  // SOUND3CNT_X: frequency, trigger
            const uint16_t v = bus.read16(IO_BASE + 0x74);
            wave.freq = v & 0x7FF;
            wave.lengthEnabled = v & (1u << 14);
            if ((offset & 1) && (v & (1u << 15))) {
                trigger(2);
            }
            break;
        }
        case 0x78: {  // SOUND4CNT_L: length, envelope
            const uint16_t v = bus.read16(IO_BASE + 0x78);
            noise.length = 64 - (v & 63);
            noise.env.period = (v >> 8) & 7;
            noise.env.increase = v & (1u << 11);
            noise.env.initial = (v >> 12) & 15;
            if (noise.env.initial == 0 && !noise.env.increase) {
                noise.active = false;
            }
            break;
        }
        case 0x7C: {  // SOUND4CNT_H: polynomial counter, trigger
            const uint16_t v = bus.read16(IO_BASE + 0x7C);
            const int r = v & 7;
            noise.divisor = r == 0 ? 8 : 16 * r;
            noise.width7 = v & 8;
            noise.shift = (v >> 4) & 15;
            noise.lengthEnabled = v & (1u << 14);
            if ((offset & 1) && (v & (1u << 15))) {
                trigger(3);
            }
            break;
        }
        case REG_SOUNDCNT_H: {
            // FIFO reset bits flush the queues.
            const uint16_t v = bus.read16(IO_BASE + REG_SOUNDCNT_H);
            if (offset & 1) {
                if (v & (1u << 11)) {
                    fifos[0] = Fifo{};
                }
                if (v & (1u << 15)) {
                    fifos[1] = Fifo{};
                }
            }
            break;
        }
        case REG_SOUNDCNT_X: {
            if (!masterEnabled()) {
                square[0].active = false;
                square[1].active = false;
                wave.active = false;
                noise.active = false;
                publishStatus();
            }
            break;
        }
        default:
            break;  // unused registers need no side effects
    }
}

// SOUND3CNT_L bit 6 toggled: exchange the inactive bank held in waveAltBank
// with the 16-byte window at 0x90-0x9F so the window always shows the bank
// now selected for playback (the previous one is preserved in waveAltBank).
void APU::swapWaveBank() {
    uint8_t window[16];
    for (int i = 0; i < 16; ++i) {
        window[i] = bus.read8(IO_BASE + WAVE_RAM + static_cast<uint32_t>(i));
    }
    for (int i = 0; i < 16; i += 2) {
        bus.writeIODirect16(
            IO_BASE + WAVE_RAM + static_cast<uint32_t>(i),
            static_cast<uint16_t>(waveAltBank[i] | (waveAltBank[i + 1] << 8)));
    }
    std::copy(window, window + 16, waveAltBank);
}

void APU::trigger(int channel) {
    if (!masterEnabled()) {
        return;
    }
    switch (channel) {
        case 0:
        case 1: {
            Square& sq = square[channel];
            sq.active = sq.env.initial > 0 || sq.env.increase;
            if (sq.length == 0) {
                sq.length = 64;
            }
            sq.cycleAcc = 0;
            sq.env.volume = sq.env.initial;
            sq.env.timer = sq.env.period;
            if (channel == 0) {
                sq.sweepTimer = sq.sweepPeriod;
            }
            break;
        }
        case 2:
            wave.active = wave.dacOn;
            if (wave.length == 0) {
                wave.length = 256;
            }
            wave.pos = 0;
            wave.cycleAcc = 0;
            break;
        case 3:
            noise.active = noise.env.initial > 0 || noise.env.increase;
            if (noise.length == 0) {
                noise.length = 64;
            }
            noise.lfsr = 0x7FFF;
            noise.cycleAcc = 0;
            noise.env.volume = noise.env.initial;
            noise.env.timer = noise.env.period;
            break;
    }
    publishStatus();
}

void APU::onFifoWrite(int fifo, uint8_t value) {
    Fifo& f = fifos[fifo];
    if (f.count < 32) {
        f.data[(f.head + f.count) % 32] = value;
        ++f.count;
    }
}

void APU::popFifo(int fifo) {
    Fifo& f = fifos[fifo];
    if (f.count > 0) {
        f.current = static_cast<int8_t>(f.data[f.head]);
        f.head = (f.head + 1) % 32;
        --f.count;
    }
    if (f.count <= 16) {
        bus.requestFifoDMA(fifo);
    }
}

void APU::onTimerOverflow(int timer) {
    if (timer > 1 || !masterEnabled()) {
        return;
    }
    const uint16_t cntH = bus.read16(IO_BASE + REG_SOUNDCNT_H);
    if (((cntH >> 10) & 1) == static_cast<unsigned>(timer)) {
        popFifo(0);
    }
    if (((cntH >> 14) & 1) == static_cast<unsigned>(timer)) {
        popFifo(1);
    }
}

void APU::step(int cycles) {
    while (cycles > 0) {
        const int chunk = std::min(cycles, CYCLES_PER_SAMPLE - sampleAcc);
        advanceClocks(chunk);
        sampleAcc += chunk;
        cycles -= chunk;
        if (sampleAcc >= CYCLES_PER_SAMPLE) {
            sampleAcc = 0;
            mixSample();
        }
    }
}

void APU::advanceClocks(int cycles) {
    for (Square& sq : square) {
        const int period = (2048 - sq.freq) * 16;
        sq.cycleAcc += cycles;
        while (sq.cycleAcc >= period) {
            sq.cycleAcc -= period;
            sq.dutyStep = (sq.dutyStep + 1) & 7;
        }
    }

    const int wavePeriod = (2048 - wave.freq) * 8;
    const int waveMask = waveDimension ? 63 : 31;  // 64 vs 32 samples
    wave.cycleAcc += cycles;
    while (wave.cycleAcc >= wavePeriod) {
        wave.cycleAcc -= wavePeriod;
        wave.pos = (wave.pos + 1) & waveMask;
    }

    // Noise periods are GB timer values; the GBA clock is 4x the GB clock.
    const int noisePeriod = (noise.divisor << noise.shift) * 4;
    noise.cycleAcc += cycles;
    while (noise.cycleAcc >= noisePeriod) {
        noise.cycleAcc -= noisePeriod;
        const uint16_t bit = (noise.lfsr ^ (noise.lfsr >> 1)) & 1;
        noise.lfsr >>= 1;
        noise.lfsr |= bit << 14;
        if (noise.width7) {
            noise.lfsr = static_cast<uint16_t>(
                (noise.lfsr & ~(1u << 6)) | (bit << 6));
        }
    }

    frameAcc += cycles;
    while (frameAcc >= FRAME_SEQ_CYCLES) {
        frameAcc -= FRAME_SEQ_CYCLES;
        clockFrameSequencer();
    }
}

void APU::clockFrameSequencer() {
    frameStep = (frameStep + 1) & 7;
    if ((frameStep & 1) == 0) {
        clockLengths();
    }
    if (frameStep == 2 || frameStep == 6) {
        clockSweep();
    }
    if (frameStep == 7) {
        clockEnvelopes();
    }
}

void APU::clockLengths() {
    for (Square& sq : square) {
        if (sq.lengthEnabled && sq.length > 0 && --sq.length == 0) {
            sq.active = false;
        }
    }
    if (wave.lengthEnabled && wave.length > 0 && --wave.length == 0) {
        wave.active = false;
    }
    if (noise.lengthEnabled && noise.length > 0 && --noise.length == 0) {
        noise.active = false;
    }
    publishStatus();
}

void APU::clockSweep() {
    Square& sq = square[0];
    if (sq.sweepPeriod == 0 || !sq.active) {
        return;
    }
    if (--sq.sweepTimer > 0) {
        return;
    }
    sq.sweepTimer = sq.sweepPeriod;
    const int delta = sq.freq >> sq.sweepShift;
    const int next = sq.sweepDecrease ? sq.freq - delta : sq.freq + delta;
    if (next > 2047) {
        sq.active = false;  // sweep overflow silences the channel
        publishStatus();
    } else if (sq.sweepShift > 0 && next >= 0) {
        sq.freq = next;
    }
}

void APU::clockEnvelopes() {
    Envelope* envs[3] = {&square[0].env, &square[1].env, &noise.env};
    for (Envelope* env : envs) {
        if (env->period == 0) {
            continue;
        }
        if (--env->timer <= 0) {
            env->timer = env->period;
            if (env->increase && env->volume < 15) {
                ++env->volume;
            } else if (!env->increase && env->volume > 0) {
                --env->volume;
            }
        }
    }
}

int APU::squareOutput(const Square& sq) const {
    if (!sq.active) {
        return 0;
    }
    return (DUTY[sq.duty] >> sq.dutyStep) & 1 ? sq.env.volume : 0;
}

int APU::waveOutput() const {
    if (!wave.active || !wave.dacOn) {
        return 0;
    }
    // pos spans 0-31 (one bank) or 0-63 (both banks when dimension is set);
    // the second half reads from the inactive bank held in waveAltBank.
    const int idx = wave.pos & 31;
    const uint8_t byte =
        (waveDimension && (wave.pos & 32))
            ? waveAltBank[idx / 2]
            : bus.read8(IO_BASE + WAVE_RAM + static_cast<uint32_t>(idx / 2));
    const int s = idx & 1 ? byte & 0xF : byte >> 4;
    if (wave.force75) {
        return s * 3 / 4;
    }
    switch (wave.volumeCode) {
        case 0:  return 0;
        case 1:  return s;
        case 2:  return s >> 1;
        default: return s >> 2;
    }
}

int APU::noiseOutput() const {
    if (!noise.active) {
        return 0;
    }
    return ~noise.lfsr & 1 ? noise.env.volume : 0;
}

void APU::mixSample() {
    // Drop the oldest second if the frontend stops draining (e.g. tests).
    if (sampleBuffer.size() > static_cast<size_t>(SAMPLE_RATE) * 4) {
        sampleBuffer.erase(sampleBuffer.begin(),
                           sampleBuffer.begin() + SAMPLE_RATE * 2);
    }
    if (!masterEnabled()) {
        sampleBuffer.push_back(0);
        sampleBuffer.push_back(0);
        return;
    }

    const uint16_t cntL = bus.read16(IO_BASE + REG_SOUNDCNT_L);
    const uint16_t cntH = bus.read16(IO_BASE + REG_SOUNDCNT_H);
    const int amp[4] = {squareOutput(square[0]), squareOutput(square[1]),
                        waveOutput(), noiseOutput()};

    int left = 0;
    int right = 0;
    for (int ch = 0; ch < 4; ++ch) {
        if (cntL & (1u << (12 + ch))) {
            left += amp[ch];
        }
        if (cntL & (1u << (8 + ch))) {
            right += amp[ch];
        }
    }
    left *= ((cntL >> 4) & 7) + 1;  // PSG master volume, max 60 * 8 = 480
    right *= (cntL & 7) + 1;
    // SOUNDCNT_H bits 0-1: PSG ratio 25/50/100% (3 is prohibited -> 100%).
    const int ratioShift[4] = {2, 1, 0, 0};
    left >>= ratioShift[cntH & 3];
    right >>= ratioShift[cntH & 3];

    // Direct Sound: signed 8-bit samples at 50% or 100% volume (~9 bits).
    for (int f = 0; f < 2; ++f) {
        const int s = fifos[f].current * ((cntH & (1u << (2 + f))) ? 2 : 1);
        if (cntH & (1u << (9 + f * 4))) {
            left += s;
        }
        if (cntH & (1u << (8 + f * 4))) {
            right += s;
        }
    }

    // PSG + both FIFOs peak near +/-1000; x32 fills most of the s16 range.
    sampleBuffer.push_back(clampSample(left * 32));
    sampleBuffer.push_back(clampSample(right * 32));
}

void APU::publishStatus() {
    // SOUNDCNT_X bits 0-3 are the read-only PSG active flags; bit 7 is the
    // game-owned master enable and must survive this hardware write.
    uint16_t value = bus.read8(IO_BASE + REG_SOUNDCNT_X) & 0x80;
    value |= square[0].active ? 1u : 0u;
    value |= square[1].active ? 2u : 0u;
    value |= wave.active ? 4u : 0u;
    value |= noise.active ? 8u : 0u;
    bus.writeIODirect16(IO_BASE + REG_SOUNDCNT_X, value);
}

size_t APU::drainSamples(int16_t* out, size_t maxFrames) {
    const size_t frames = std::min(maxFrames, sampleBuffer.size() / 2);
    std::copy_n(sampleBuffer.begin(), frames * 2, out);
    sampleBuffer.erase(sampleBuffer.begin(),
                       sampleBuffer.begin() +
                           static_cast<std::ptrdiff_t>(frames * 2));
    return frames;
}
