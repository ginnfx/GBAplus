#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class Bus;

// GBA APU: the four GB-style PSG channels (square 1 with sweep, square 2,
// programmable wave, noise) plus the two Direct Sound FIFO channels that are
// fed by DMA 1/2 and clocked by Timer 0/1 overflows. Registers live at
// 0x04000060-0x040000A7; the Bus notifies this class of writes. Output is
// stereo int16 at SAMPLE_RATE; the frontend drains drainSamples() into the
// audio device.
class APU {
public:
    static constexpr int SAMPLE_RATE = 32768;
    static constexpr int CYCLES_PER_SAMPLE = 16777216 / SAMPLE_RATE;  // 512

    explicit APU(Bus& bus);

    // Called by the Bus after a game writes a byte in 0x060-0x09F;
    // `offset` is relative to 0x04000000.
    void onRegisterWrite(uint32_t offset);

    // Called by the Bus for every byte landing in FIFO_A/B (0x0A0-0x0A7).
    void onFifoWrite(int fifo, uint8_t value);

    // Timer 0/1 overflow: each FIFO clocked by that timer consumes one
    // sample and requests a DMA refill once half empty.
    void onTimerOverflow(int timer);

    // Advances the PSG channel clocks and produces output samples.
    void step(int cycles);

    size_t pendingFrames() const { return sampleBuffer.size() / 2; }
    size_t drainSamples(int16_t* out, size_t maxFrames);

    // Test hook: number of bytes queued in FIFO A (0) or B (1).
    int fifoCount(int fifo) const { return fifos[fifo].count; }

private:
    struct Envelope {
        int volume = 0;
        int initial = 0;
        bool increase = false;
        int period = 0;  // in 1/64 s envelope clocks; 0 = static
        int timer = 0;
    };
    struct Square {
        bool active = false;
        int duty = 0;
        int dutyStep = 0;
        int freq = 0;  // raw 11-bit register value
        int cycleAcc = 0;
        int length = 0;
        bool lengthEnabled = false;
        Envelope env;
        // Sweep state (square 1 only).
        int sweepShift = 0;
        int sweepPeriod = 0;
        int sweepTimer = 0;
        bool sweepDecrease = false;
    };
    struct WaveChannel {
        bool active = false;
        bool dacOn = false;
        int freq = 0;
        int pos = 0;  // 0-31 nibble index into wave RAM
        int cycleAcc = 0;
        int length = 0;
        bool lengthEnabled = false;
        int volumeCode = 0;
        bool force75 = false;
    };
    struct NoiseChannel {
        bool active = false;
        uint16_t lfsr = 0x7FFF;
        bool width7 = false;
        int shift = 0;
        int divisor = 8;
        int cycleAcc = 0;
        int length = 0;
        bool lengthEnabled = false;
        Envelope env;
    };
    struct Fifo {
        uint8_t data[32] = {};
        int head = 0;
        int count = 0;
        int8_t current = 0;  // sample being output until the next overflow
    };

    bool masterEnabled() const;
    void trigger(int channel);
    void popFifo(int fifo);
    void advanceClocks(int cycles);
    void clockFrameSequencer();
    void clockLengths();
    void clockEnvelopes();
    void clockSweep();
    void mixSample();
    void publishStatus();
    int squareOutput(const Square& sq) const;
    int waveOutput() const;
    int noiseOutput() const;

    Bus& bus;
    Square square[2];
    WaveChannel wave;
    NoiseChannel noise;
    Fifo fifos[2];
    int sampleAcc = 0;
    int frameAcc = 0;
    int frameStep = 0;
    std::vector<int16_t> sampleBuffer;
};
