#include <cmath>
#include <numbers>

#include "ARM7TDMI.hpp"
#include "Bus.hpp"
#include "Log.hpp"

namespace {
constexpr uint32_t BIOS_IF_SHADOW = 0x03007FF8;
constexpr uint32_t SOFTRESET_FLAG = 0x03007FFA;

uint32_t isqrt32(uint32_t value) {
    uint32_t result = 0;
    uint32_t bit = 1u << 30;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}
}

void ARM7TDMI::hleSWI(uint32_t number) {
    switch (number) {
        case 0x00: swiSoftReset(); break;
        case 0x01: swiRegisterRamReset(regs[0]); break;
        case 0x02:
            halted = true;
            intrWaiting = false;
            break;
        case 0x03:
            halted = true;
            intrWaiting = false;
            break;
        case 0x04: swiIntrWait(regs[0], regs[1]); break;
        case 0x05:
            swiIntrWait(1, 1);
            break;
        case 0x06:
            swiDiv(static_cast<int32_t>(regs[0]),
                   static_cast<int32_t>(regs[1]));
            break;
        case 0x07:
            swiDiv(static_cast<int32_t>(regs[1]),
                   static_cast<int32_t>(regs[0]));
            break;
        case 0x08: regs[0] = isqrt32(regs[0]); break;
        case 0x09: swiArcTan(); break;
        case 0x0A: swiArcTan2(); break;
        case 0x0B: swiCpuSet(); break;
        case 0x0C: swiCpuFastSet(); break;
        case 0x0D: regs[0] = 0xBAAE187F; break;
        case 0x0E: swiBgAffineSet(); break;
        case 0x0F: swiObjAffineSet(); break;
        case 0x10: swiBitUnPack(); break;
        case 0x11:
        case 0x12:
            swiLZ77UnComp();
            break;
        case 0x13: swiHuffUnComp(); break;
        case 0x14:
        case 0x15:
            swiRLUnComp();
            break;
        default:
            TRACE_LOG("unimplemented SWI 0x%02X @ 0x%08X", number,
                      regs[15] - (inThumbState() ? 4 : 8));
            break;
    }
}

void ARM7TDMI::swiSoftReset() {
    const uint8_t flag = bus.read8(SOFTRESET_FLAG);
    for (uint32_t addr = 0x03007E00; addr < 0x03008000; addr += 4) {
        bus.write32(addr, 0);
    }

    bankedR13_14[BANK_SVC][0] = 0x03007FE0;
    bankedR13_14[BANK_SVC][1] = 0;
    bankedR13_14[BANK_IRQ][0] = 0x03007FA0;
    bankedR13_14[BANK_IRQ][1] = 0;
    for (int i = 0; i < 13; ++i) {
        regs[i] = 0;
    }
    switchMode(Mode::System);
    cpsr &= ~(BIT_T | BIT_I | BIT_F);
    regs[13] = 0x03007F00;
    regs[14] = 0;
    halted = false;
    intrWaiting = false;
    regs[15] = flag ? 0x02000000 : 0x08000000;
    flushPipeline();
}

void ARM7TDMI::swiRegisterRamReset(uint32_t flags) {
    if (flags & (1u << 0)) {
        for (uint32_t a = 0x02000000; a < 0x02040000; a += 4) {
            bus.write32(a, 0);
        }
    }
    if (flags & (1u << 1)) {
        for (uint32_t a = 0x03000000; a < 0x03007E00; a += 4) {
            bus.write32(a, 0);
        }
    }
    if (flags & (1u << 2)) {
        for (uint32_t a = 0x05000000; a < 0x05000400; a += 4) {
            bus.write32(a, 0);
        }
    }
    if (flags & (1u << 3)) {
        for (uint32_t a = 0x06000000; a < 0x06018000; a += 4) {
            bus.write32(a, 0);
        }
    }
    if (flags & (1u << 4)) {
        for (uint32_t a = 0x07000000; a < 0x07000400; a += 4) {
            bus.write32(a, 0);
        }
    }
    if (flags & 0xE0) {
        TRACE_LOG("RegisterRamReset IO flags 0x%02X ignored", flags >> 5);
    }
}

void ARM7TDMI::swiIntrWait(uint32_t discardOld, uint32_t mask) {
    bus.write16(0x04000208, 1);
    if (discardOld != 0) {
        const uint16_t shadow = bus.read16(BIOS_IF_SHADOW);
        bus.write16(BIOS_IF_SHADOW,
                    shadow & static_cast<uint16_t>(~mask));
    }
    intrWaitMask = mask;
    intrWaiting = true;
    halted = true;
}

void ARM7TDMI::swiDiv(int32_t numerator, int32_t denominator) {
    if (denominator == 0) {
        TRACE_LOG("SWI Div by zero");
        regs[0] = numerator < 0 ? static_cast<uint32_t>(-1) : 1;
        regs[1] = static_cast<uint32_t>(numerator);
        regs[3] = 1;
        return;
    }
    const int64_t q = static_cast<int64_t>(numerator) / denominator;
    const int64_t r = static_cast<int64_t>(numerator) % denominator;
    regs[0] = static_cast<uint32_t>(q);
    regs[1] = static_cast<uint32_t>(r);
    regs[3] = static_cast<uint32_t>(q < 0 ? -q : q);
}

void ARM7TDMI::swiArcTan() {
    const double tan = static_cast<int16_t>(regs[0]) / 16384.0;
    const double angle = std::atan(tan) / std::numbers::pi * 0x8000;
    regs[0] = static_cast<uint32_t>(
        static_cast<int32_t>(std::lround(angle)));
}

void ARM7TDMI::swiArcTan2() {
    const double x = static_cast<int16_t>(regs[0]);
    const double y = static_cast<int16_t>(regs[1]);
    const double angle = std::atan2(y, x) / std::numbers::pi * 0x8000;
    regs[0] = static_cast<uint16_t>(std::lround(angle));
}

void ARM7TDMI::swiCpuSet() {
    uint32_t src = regs[0];
    uint32_t dst = regs[1];
    const uint32_t count = regs[2] & 0x1FFFFF;
    const bool fill = regs[2] & (1u << 24);
    const bool word = regs[2] & (1u << 26);

    if (word) {
        const uint32_t fillValue = fill ? bus.read32(src) : 0;
        for (uint32_t i = 0; i < count; ++i) {
            bus.write32(dst, fill ? fillValue : bus.read32(src));
            src += 4;
            dst += 4;
        }
    } else {
        const uint16_t fillValue = fill ? bus.read16(src) : 0;
        for (uint32_t i = 0; i < count; ++i) {
            bus.write16(dst, fill ? fillValue : bus.read16(src));
            src += 2;
            dst += 2;
        }
    }
}

void ARM7TDMI::swiCpuFastSet() {
    uint32_t src = regs[0];
    uint32_t dst = regs[1];
    const uint32_t count = regs[2] & 0x1FFFFF;
    const uint32_t rounded = (count + 7) & ~7u;
    const bool fill = regs[2] & (1u << 24);

    const uint32_t fillValue = fill ? bus.read32(src) : 0;
    for (uint32_t i = 0; i < rounded; ++i) {
        bus.write32(dst, fill ? fillValue : bus.read32(src));
        src += 4;
        dst += 4;
    }
}

void ARM7TDMI::swiBgAffineSet() {
    uint32_t src = regs[0];
    uint32_t dst = regs[1];
    TRACE_LOG("BgAffineSet src=%08X dst=%08X n=%u", src, dst, regs[2]);
    for (uint32_t n = regs[2]; n != 0; --n) {
        const double ox = static_cast<int32_t>(bus.read32(src)) / 256.0;
        const double oy = static_cast<int32_t>(bus.read32(src + 4)) / 256.0;
        const double cx = static_cast<int16_t>(bus.read16(src + 8));
        const double cy = static_cast<int16_t>(bus.read16(src + 10));
        const double sx = static_cast<int16_t>(bus.read16(src + 12)) / 256.0;
        const double sy = static_cast<int16_t>(bus.read16(src + 14)) / 256.0;
        const double theta =
            (bus.read16(src + 16) >> 8) / 128.0 * std::numbers::pi;
        src += 20;

        const double a = sx * std::cos(theta);
        const double b = -sx * std::sin(theta);
        const double c = sy * std::sin(theta);
        const double d = sy * std::cos(theta);
        const double rx = ox - (a * cx + b * cy);
        const double ry = oy - (c * cx + d * cy);

        bus.write16(dst, static_cast<uint16_t>(
                             static_cast<int16_t>(a * 256)));
        bus.write16(dst + 2, static_cast<uint16_t>(
                                 static_cast<int16_t>(b * 256)));
        bus.write16(dst + 4, static_cast<uint16_t>(
                                 static_cast<int16_t>(c * 256)));
        bus.write16(dst + 6, static_cast<uint16_t>(
                                 static_cast<int16_t>(d * 256)));
        bus.write32(dst + 8, static_cast<uint32_t>(
                                 static_cast<int32_t>(rx * 256)));
        bus.write32(dst + 12, static_cast<uint32_t>(
                                  static_cast<int32_t>(ry * 256)));
        dst += 16;
    }
}

void ARM7TDMI::swiObjAffineSet() {
    uint32_t src = regs[0];
    uint32_t dst = regs[1];
    const uint32_t offset = regs[3];
    for (uint32_t n = regs[2]; n != 0; --n) {
        const double sx = static_cast<int16_t>(bus.read16(src)) / 256.0;
        const double sy = static_cast<int16_t>(bus.read16(src + 2)) / 256.0;
        const double theta =
            (bus.read16(src + 4) >> 8) / 128.0 * std::numbers::pi;
        src += 6;

        bus.write16(dst, static_cast<uint16_t>(static_cast<int16_t>(
                             sx * std::cos(theta) * 256)));
        bus.write16(dst + offset,
                    static_cast<uint16_t>(static_cast<int16_t>(
                        -sx * std::sin(theta) * 256)));
        bus.write16(dst + offset * 2,
                    static_cast<uint16_t>(static_cast<int16_t>(
                        sy * std::sin(theta) * 256)));
        bus.write16(dst + offset * 3,
                    static_cast<uint16_t>(static_cast<int16_t>(
                        sy * std::cos(theta) * 256)));
        dst += offset * 4;
    }
}

void ARM7TDMI::swiBitUnPack() {
    uint32_t src = regs[0];
    uint32_t dst = regs[1];
    const uint32_t info = regs[2];
    const uint32_t srcLen = bus.read16(info);
    const uint32_t srcWidth = bus.read8(info + 2);
    const uint32_t dstWidth = bus.read8(info + 3);
    const uint32_t offsetWord = bus.read32(info + 4);
    const uint32_t dataOffset = offsetWord & 0x7FFFFFFF;
    const bool zeroFlag = offsetWord >> 31;

    if (srcWidth == 0 || dstWidth == 0 || srcWidth > 8 || dstWidth > 32) {
        TRACE_LOG("BitUnPack bad widths %u -> %u", srcWidth, dstWidth);
        return;
    }

    uint32_t outWord = 0;
    uint32_t outBits = 0;
    for (uint32_t i = 0; i < srcLen; ++i) {
        const uint8_t byte = bus.read8(src + i);
        for (uint32_t bit = 0; bit < 8; bit += srcWidth) {
            uint32_t unit = (byte >> bit) & ((1u << srcWidth) - 1);
            if (unit != 0 || zeroFlag) {
                unit += dataOffset;
            }
            outWord |= unit << outBits;
            outBits += dstWidth;
            if (outBits >= 32) {
                bus.write32(dst, outWord);
                dst += 4;
                outWord = 0;
                outBits = 0;
            }
        }
    }
    if (outBits > 0) {
        bus.write32(dst, outWord);
    }
}

void ARM7TDMI::swiLZ77UnComp() {
    uint32_t src = regs[0];
    const uint32_t dst = regs[1];
    const uint32_t header = bus.read32(src);
    const uint32_t size = header >> 8;
    src += 4;

    uint32_t written = 0;
    while (written < size) {
        const uint8_t flags = bus.read8(src++);
        for (int i = 0; i < 8 && written < size; ++i) {
            if (flags & (0x80u >> i)) {
                const uint8_t b1 = bus.read8(src++);
                const uint8_t b2 = bus.read8(src++);
                const uint32_t disp =
                    (static_cast<uint32_t>(b1 & 0xF) << 8) | b2;
                const uint32_t len = (b1 >> 4) + 3;
                for (uint32_t j = 0; j < len && written < size; ++j) {
                    const uint8_t byte =
                        bus.read8(dst + written - disp - 1);
                    bus.write8(dst + written, byte);
                    ++written;
                }
            } else {
                bus.write8(dst + written, bus.read8(src++));
                ++written;
            }
        }
    }
}

void ARM7TDMI::swiHuffUnComp() {
    const uint32_t src = regs[0];
    const uint32_t dst = regs[1];
    const uint32_t header = bus.read32(src);
    const uint32_t dataBits = header & 0xF;
    const uint32_t size = header >> 8;
    if (dataBits != 4 && dataBits != 8) {
        TRACE_LOG("HuffUnComp bad data size %u bits", dataBits);
        return;
    }

    const uint32_t treeSize = bus.read8(src + 4);
    const uint32_t rootAddr = src + 5;
    uint32_t bitstream = src + 4 + (treeSize + 1) * 2;

    uint32_t nodeAddr = rootAddr;
    uint8_t node = bus.read8(nodeAddr);
    uint32_t outWord = 0;
    uint32_t outBits = 0;
    uint32_t written = 0;

    while (written < size) {
        const uint32_t word = bus.read32(bitstream);
        bitstream += 4;
        for (int bit = 31; bit >= 0 && written < size; --bit) {
            const uint32_t b = (word >> bit) & 1;
            const bool isData = node & (b == 0 ? 0x80 : 0x40);
            const uint32_t childBase =
                (nodeAddr & ~1u) + (node & 0x3Fu) * 2 + 2;
            nodeAddr = childBase + b;
            node = bus.read8(nodeAddr);
            if (isData) {
                outWord |= static_cast<uint32_t>(node) << outBits;
                outBits += dataBits;
                if (outBits >= 32) {
                    bus.write32(dst + written, outWord);
                    written += 4;
                    outWord = 0;
                    outBits = 0;
                }
                nodeAddr = rootAddr;
                node = bus.read8(nodeAddr);
            }
        }
    }
}

void ARM7TDMI::swiRLUnComp() {
    uint32_t src = regs[0];
    const uint32_t dst = regs[1];
    const uint32_t header = bus.read32(src);
    const uint32_t size = header >> 8;
    src += 4;

    uint32_t written = 0;
    while (written < size) {
        const uint8_t flag = bus.read8(src++);
        if (flag & 0x80) {
            const uint32_t len = (flag & 0x7Fu) + 3;
            const uint8_t byte = bus.read8(src++);
            for (uint32_t i = 0; i < len && written < size; ++i) {
                bus.write8(dst + written, byte);
                ++written;
            }
        } else {
            const uint32_t len = (flag & 0x7Fu) + 1;
            for (uint32_t i = 0; i < len && written < size; ++i) {
                bus.write8(dst + written, bus.read8(src++));
                ++written;
            }
        }
    }
}
