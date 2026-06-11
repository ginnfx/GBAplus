#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class Bus;

class APU {
public:
    static constexpr int SAMPLE_RATE = 32768;
    static constexpr int CYCLES_PER_SAMPLE = 16777216 / SAMPLE_RATE;

    explicit APU(Bus& bus);

    void onRegisterWrite(uint32_t offset);

    void onFifoWrite(int fifo, uint8_t value);

    void onTimerOverflow(int timer);

    void step(int cycles);

    size_t pendingFrames() const { return sampleBuffer.size() / 2; }
    size_t drainSamples(int16_t* out, size_t maxFrames);

    int fifoCount(int fifo) const { return fifos[fifo].count; }

private:
    struct Envelope {
        int volume = 0;
        int initial = 0;
        bool increase = false;
        int period = 0;
        int timer = 0;
    };
    struct Square {
        bool active = false;
        int duty = 0;
        int dutyStep = 0;
        int freq = 0;
        int cycleAcc = 0;
        int length = 0;
        bool lengthEnabled = false;
        Envelope env;
        int sweepShift = 0;
        int sweepPeriod = 0;
        int sweepTimer = 0;
        bool sweepDecrease = false;
    };
    struct WaveChannel {
        bool active = false;
        bool dacOn = false;
        int freq = 0;
        int pos = 0;
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
        int8_t current = 0;
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
