#include "SaveState.hpp"

#include "APU.hpp"
#include "ARM7TDMI.hpp"
#include "Bus.hpp"
#include "DMA.hpp"
#include "PPU.hpp"
#include "Timers.hpp"

void Bus::serialize(Serializer& s) const {
    s.bytes(ewram.data(), ewram.size());
    s.bytes(iwram.data(), iwram.size());
    s.bytes(io.data(), io.size());
    s.bytes(palette.data(), palette.size());
    s.bytes(vram.data(), vram.size());
    s.bytes(oam.data(), oam.size());

    s.pod(backup);
    s.pod(flashState);
    s.pod(flashIdMode);
    s.pod(flashErasePending);
    s.pod(flashBank);
    const uint64_t backupSize = backupMem.size();
    s.pod(backupSize);
    s.bytes(backupMem.data(), backupMem.size());
    s.pod(sramWritten);

    s.pod(eepromAddrBits);
    const uint64_t eepromSize = eepromBits.size();
    s.pod(eepromSize);
    s.bytes(eepromBits.data(), eepromBits.size());
    s.pod(eepromReadActive);
    s.pod(eepromReadPos);
    s.pod(eepromReadValue);

    s.pod(hasRtc);
    s.pod(gpioData);
    s.pod(gpioDir);
    s.pod(gpioReadable);
    s.pod(rtc);
}

void Bus::deserialize(Deserializer& d) {
    d.bytes(ewram.data(), ewram.size());
    d.bytes(iwram.data(), iwram.size());
    d.bytes(io.data(), io.size());
    d.bytes(palette.data(), palette.size());
    d.bytes(vram.data(), vram.size());
    d.bytes(oam.data(), oam.size());

    d.pod(backup);
    d.pod(flashState);
    d.pod(flashIdMode);
    d.pod(flashErasePending);
    d.pod(flashBank);
    uint64_t backupSize = 0;
    d.pod(backupSize);
    backupMem.resize(backupSize);
    d.bytes(backupMem.data(), backupMem.size());
    d.pod(sramWritten);

    d.pod(eepromAddrBits);
    uint64_t eepromSize = 0;
    d.pod(eepromSize);
    eepromBits.resize(eepromSize);
    d.bytes(eepromBits.data(), eepromBits.size());
    d.pod(eepromReadActive);
    d.pod(eepromReadPos);
    d.pod(eepromReadValue);

    d.pod(hasRtc);
    d.pod(gpioData);
    d.pod(gpioDir);
    d.pod(gpioReadable);
    d.pod(rtc);

    updateWaitstate();
}

uint32_t Bus::romHash() const {
    uint32_t hash = 2166136261u;
    const size_t n = rom.size() < 0x100 ? rom.size() : 0x100;
    for (size_t i = 0; i < n; ++i) {
        hash = (hash ^ rom[i]) * 16777619u;
    }
    return hash;
}

void ARM7TDMI::serialize(Serializer& s) const {
    s.pod(regs);
    s.pod(cpsr);
    s.pod(bankedR8_12);
    s.pod(bankedR13_14);
    s.pod(spsrBank);
    s.pod(pipeline);
    s.pod(pipelineFlushed);
    s.pod(halted);
    s.pod(intrWaiting);
    s.pod(intrWaitMask);
}

void ARM7TDMI::deserialize(Deserializer& d) {
    d.pod(regs);
    d.pod(cpsr);
    d.pod(bankedR8_12);
    d.pod(bankedR13_14);
    d.pod(spsrBank);
    d.pod(pipeline);
    d.pod(pipelineFlushed);
    d.pod(halted);
    d.pod(intrWaiting);
    d.pod(intrWaitMask);
}

void PPU::serialize(Serializer& s) const {
    s.pod(cycleCounter);
    s.pod(vcount);
    s.pod(affineX);
    s.pod(affineY);
    s.pod(inHBlank);
    s.pod(frameDone);
}

void PPU::deserialize(Deserializer& d) {
    d.pod(cycleCounter);
    d.pod(vcount);
    d.pod(affineX);
    d.pod(affineY);
    d.pod(inHBlank);
    d.pod(frameDone);
}

void DMA::serialize(Serializer& s) const {
    s.pod(channels);
}

void DMA::deserialize(Deserializer& d) {
    d.pod(channels);
}

void Timers::serialize(Serializer& s) const {
    s.pod(timers);
}

void Timers::deserialize(Deserializer& d) {
    d.pod(timers);
}

void APU::serialize(Serializer& s) const {
    s.pod(square);
    s.pod(wave);
    s.pod(noise);
    s.pod(fifos);
    s.pod(waveAltBank);
    s.pod(wavePlayBank);
    s.pod(waveDimension);
    s.pod(sampleAcc);
    s.pod(frameAcc);
    s.pod(frameStep);
}

void APU::deserialize(Deserializer& d) {
    d.pod(square);
    d.pod(wave);
    d.pod(noise);
    d.pod(fifos);
    d.pod(waveAltBank);
    d.pod(wavePlayBank);
    d.pod(waveDimension);
    d.pod(sampleAcc);
    d.pod(frameAcc);
    d.pod(frameStep);
    sampleBuffer.clear();
}
