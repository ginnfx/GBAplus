#include "ARM7TDMI.hpp"

#include <bit>

#include "Bus.hpp"
#include "Log.hpp"

ARM7TDMI::ARM7TDMI(Bus& bus) : bus(bus) {}

int ARM7TDMI::bankIndex(Mode mode) {
    switch (mode) {
        case Mode::User:
        case Mode::System:     return BANK_USER;
        case Mode::FIQ:        return BANK_FIQ;
        case Mode::IRQ:        return BANK_IRQ;
        case Mode::Supervisor: return BANK_SVC;
        case Mode::Abort:      return BANK_ABT;
        case Mode::Undefined:  return BANK_UND;
    }
    TRACE_LOG("bankIndex: invalid mode 0x%02X", static_cast<uint32_t>(mode));
    return BANK_USER;
}

void ARM7TDMI::reset() {
    regs.fill(0);
    bankedR8_12 = {};
    bankedR13_14 = {};
    spsrBank = {};

    // Post-BIOS boot state: System mode, ARM state, IRQs enabled, and the
    // stack pointers the BIOS would have set up for each mode.
    cpsr = static_cast<uint32_t>(Mode::System);
    bankedR13_14[BANK_IRQ][0] = 0x03007FA0;
    bankedR13_14[BANK_SVC][0] = 0x03007FE0;
    regs[13] = 0x03007F00;

    regs[15] = 0x08000000;
    flushPipeline();
}

bool ARM7TDMI::irqPending() const {
    if (cpsr & BIT_I) {
        return false;
    }
    if (!(bus.peek16(0x04000208) & 1)) {  // REG_IME
        return false;
    }
    return (bus.peek16(0x04000200) & bus.peek16(0x04000202)) != 0;  // IE & IF
}

void ARM7TDMI::enterIRQ() {
    const uint32_t oldCpsr = cpsr;
    // The instruction about to execute sits at r15-8 (ARM) / r15-4 (Thumb);
    // the IRQ return convention is LR = that address + 4, so the handler's
    // SUBS PC, LR, #4 resumes correctly.
    const uint32_t returnAddr = regs[15] - (inThumbState() ? 0 : 4);
    switchMode(Mode::IRQ);
    spsrBank[BANK_IRQ] = oldCpsr;
    regs[14] = returnAddr;
    cpsr = (cpsr & ~BIT_T) | BIT_I;  // enter ARM state, mask further IRQs
    if (bus.hasBIOS()) {
        regs[15] = 0x00000018;  // IRQ exception vector
        flushPipeline();
    } else {
        hleIrqDispatch();
    }
}

// Mirrors the real BIOS stub at the IRQ vector:
//   stmfd sp!, {r0-r3, r12, lr}
//   ldr   pc, [0x03007FFC]   (with lr pointing back into the stub)
void ARM7TDMI::hleIrqDispatch() {
    uint32_t sp = regs[13] - 24;  // IRQ-mode stack
    regs[13] = sp;
    for (int i = 0; i < 4; ++i) {
        bus.write32(sp, regs[i]);
        sp += 4;
    }
    bus.write32(sp, regs[12]);
    bus.write32(sp + 4, regs[14]);

    regs[14] = HLE_IRQ_RETURN;
    regs[15] = bus.read32(0x03007FFC);  // user handler pointer
    flushPipeline();
}

// Mirrors the back half of the BIOS stub once the user handler returns:
//   ldmfd sp!, {r0-r3, r12, lr}
//   subs  pc, lr, #4         (restores CPSR from SPSR_irq)
void ARM7TDMI::hleIrqReturn() {
    uint32_t sp = regs[13];
    for (int i = 0; i < 4; ++i) {
        regs[i] = bus.read32(sp);
        sp += 4;
    }
    regs[12] = bus.read32(sp);
    regs[14] = bus.read32(sp + 4);
    regs[13] = sp + 8;

    const uint32_t resumeAddr = regs[14] - 4;
    setCPSR(getSPSR());  // leave IRQ mode, restore T and I
    regs[15] = resumeAddr;
    flushPipeline();

    // The BIOS IntrWait loop: stay asleep until the user handler has set
    // one of the requested bits in the IF-shadow at 0x03007FF8.
    if (intrWaiting) {
        const uint16_t shadow = bus.read16(0x03007FF8);
        if (shadow & intrWaitMask) {
            bus.write16(0x03007FF8,
                        shadow & static_cast<uint16_t>(~intrWaitMask));
            intrWaiting = false;
        } else {
            halted = true;
        }
    }
}

void ARM7TDMI::step() {
    if (halted) {
        // Asleep after Halt/IntrWait: wake when any enabled interrupt is
        // requested (IE & IF), independent of IME. The system clock keeps
        // advancing around the sleeping core.
        if ((bus.peek16(0x04000200) & bus.peek16(0x04000202)) == 0) {
            return;
        }
        halted = false;
    }
    if (irqPending()) {
        enterIRQ();
    }
    // The HLE IRQ dispatch leaves LR pointing at a fake BIOS address;
    // catch execution arriving there and unwind instead of running zeros.
    if (!bus.hasBIOS() && !inThumbState() &&
        regs[15] - 8 == HLE_IRQ_RETURN) {
        hleIrqReturn();
    }
    if (traceFile != nullptr) {
        std::fprintf(traceFile, "PC=%08X CPSR=%08X",
                     regs[15] - (inThumbState() ? 4 : 8), cpsr);
        for (int i = 0; i < 16; ++i) {
            std::fprintf(traceFile, " R%d=%08X", i, regs[i]);
        }
        std::fputc('\n', traceFile);
    }
    pipelineFlushed = false;
    if (inThumbState()) {
        const uint16_t opcode = static_cast<uint16_t>(pipeline[0]);
        pipeline[0] = pipeline[1];
        pipeline[1] = bus.read16(regs[15]);
        executeThumb(opcode);
        if (!pipelineFlushed) {
            regs[15] += 2;
        }
    } else {
        const uint32_t opcode = pipeline[0];
        pipeline[0] = pipeline[1];
        pipeline[1] = bus.read32(regs[15]);
        executeARM(opcode);
        if (!pipelineFlushed) {
            regs[15] += 4;
        }
    }
}

void ARM7TDMI::flushPipeline() {
    if (inThumbState()) {
        regs[15] &= ~1u;
        pipeline[0] = bus.read16(regs[15]);
        pipeline[1] = bus.read16(regs[15] + 2);
        regs[15] += 4;
    } else {
        regs[15] &= ~3u;
        pipeline[0] = bus.read32(regs[15]);
        pipeline[1] = bus.read32(regs[15] + 4);
        regs[15] += 8;
    }
    pipelineFlushed = true;
}

void ARM7TDMI::setReg(int index, uint32_t value) {
    regs[index] = value;
    if (index == 15) {
        flushPipeline();
    }
}

void ARM7TDMI::setCPSR(uint32_t value) {
    switchMode(static_cast<Mode>(value & MODE_MASK));
    cpsr = value;
}

uint32_t ARM7TDMI::getSPSR() const {
    const int idx = bankIndex(currentMode());
    if (idx == BANK_USER) {
        // User/System have no SPSR; reading it is unpredictable on hardware.
        TRACE_LOG("getSPSR in User/System mode");
        return cpsr;
    }
    return spsrBank[idx];
}

void ARM7TDMI::setSPSR(uint32_t value) {
    const int idx = bankIndex(currentMode());
    if (idx == BANK_USER) {
        TRACE_LOG("setSPSR in User/System mode ignored");
        return;
    }
    spsrBank[idx] = value;
}

void ARM7TDMI::switchMode(Mode newMode) {
    const int oldIdx = bankIndex(currentMode());
    const int newIdx = bankIndex(newMode);
    cpsr = (cpsr & ~MODE_MASK) | static_cast<uint32_t>(newMode);
    if (oldIdx == newIdx) {
        return;
    }

    bankedR13_14[oldIdx][0] = regs[13];
    bankedR13_14[oldIdx][1] = regs[14];
    regs[13] = bankedR13_14[newIdx][0];
    regs[14] = bankedR13_14[newIdx][1];

    // r8-r12 only differ between FIQ and everyone else.
    const bool oldFiq = oldIdx == BANK_FIQ;
    const bool newFiq = newIdx == BANK_FIQ;
    if (oldFiq != newFiq) {
        for (int i = 0; i < 5; ++i) {
            bankedR8_12[oldFiq][i] = regs[8 + i];
            regs[8 + i] = bankedR8_12[newFiq][i];
        }
    }
}

bool ARM7TDMI::checkCondition(uint32_t cond) const {
    const bool n = cpsr & FLAG_N;
    const bool z = cpsr & FLAG_Z;
    const bool c = cpsr & FLAG_C;
    const bool v = cpsr & FLAG_V;
    switch (cond) {
        case 0x0: return z;             // EQ
        case 0x1: return !z;            // NE
        case 0x2: return c;             // CS
        case 0x3: return !c;            // CC
        case 0x4: return n;             // MI
        case 0x5: return !n;            // PL
        case 0x6: return v;             // VS
        case 0x7: return !v;            // VC
        case 0x8: return c && !z;       // HI
        case 0x9: return !c || z;       // LS
        case 0xA: return n == v;        // GE
        case 0xB: return n != v;        // LT
        case 0xC: return !z && n == v;  // GT
        case 0xD: return z || n != v;   // LE
        case 0xE: return true;          // AL
        default:  return false;         // NV (reserved)
    }
}

const std::array<ARM7TDMI::ArmHandler, 4096>& ARM7TDMI::armTable() {
    static const auto table = [] {
        std::array<ArmHandler, 4096> t{};
        for (uint32_t idx = 0; idx < 4096; ++idx) {
            const uint32_t top8 = idx >> 4;   // instruction bits 27-20
            const uint32_t low4 = idx & 0xF;  // instruction bits 7-4
            ArmHandler handler = &ARM7TDMI::armUnimplemented;

            if ((top8 & 0xF0) == 0xF0) {
                handler = &ARM7TDMI::armSoftwareInterrupt;  // bits 27-24
            } else if ((top8 & 0xE0) == 0xA0) {
                handler = &ARM7TDMI::armBranch;  // bits 27-25 == 101
            } else if (top8 == 0x12 && (low4 == 0x1 || low4 == 0x3)) {
                handler = &ARM7TDMI::armBranchExchange;  // BX / BLX-reg
            } else if ((top8 & 0xC0) == 0x40) {
                handler = &ARM7TDMI::armSingleDataTransfer;  // bits 27-26 == 01
            } else if ((top8 & 0xE0) == 0x80) {
                handler = &ARM7TDMI::armBlockDataTransfer;  // bits 27-25 == 100
            } else if ((top8 & 0xC0) == 0x00) {  // bits 27-26 == 00
                if (!(top8 & 0x20) && low4 == 0x9) {
                    // Register form with bits 7-4 == 1001: multiplies and
                    // SWP live here.
                    if ((top8 & 0xFC) == 0x00) {
                        handler = &ARM7TDMI::armMultiply;
                    } else if ((top8 & 0xF8) == 0x08) {
                        handler = &ARM7TDMI::armMultiplyLong;
                    } else if ((top8 & 0xFB) == 0x10) {
                        handler = &ARM7TDMI::armSwap;  // SWP/SWPB
                    }
                } else if (!(top8 & 0x20) && (low4 & 0x9) == 0x9) {
                    // bits 7,4 set with bits 6-5 != 00: halfword/signed
                    handler = &ARM7TDMI::armHalfwordTransfer;
                } else if ((top8 & 0x19) == 0x10) {
                    // TST/TEQ/CMP/CMN with S=0: the PSR-transfer space
                    // (BX was carved out above).
                    if (top8 & 0x20) {
                        if (top8 & 0x02) {
                            handler = &ARM7TDMI::armPSRTransfer;  // MSR imm
                        }
                    } else if (low4 == 0x0) {
                        handler = &ARM7TDMI::armPSRTransfer;  // MRS/MSR reg
                    }
                } else {
                    handler = &ARM7TDMI::armDataProcessing;
                }
            }
            t[idx] = handler;
        }
        return t;
    }();
    return table;
}

void ARM7TDMI::executeARM(uint32_t opcode) {
    if (!checkCondition(opcode >> 28)) {
        return;
    }
    const uint32_t idx = ((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF);
    (this->*armTable()[idx])(opcode);
}

uint32_t ARM7TDMI::shiftOperand(uint32_t value, uint32_t type, uint32_t amount,
                                bool immediateForm, bool& carry) {
    switch (type) {
        case 0:  // LSL
            if (amount == 0) {
                return value;  // carry unchanged
            }
            if (amount < 32) {
                carry = (value >> (32 - amount)) & 1;
                return value << amount;
            }
            carry = (amount == 32) && (value & 1);
            return 0;
        case 1:  // LSR
            if (amount == 0) {
                if (!immediateForm) {
                    return value;
                }
                amount = 32;  // LSR #0 encodes LSR #32
            }
            if (amount < 32) {
                carry = (value >> (amount - 1)) & 1;
                return value >> amount;
            }
            carry = (amount == 32) && (value >> 31);
            return 0;
        case 2:  // ASR
            if (amount == 0) {
                if (!immediateForm) {
                    return value;
                }
                amount = 32;  // ASR #0 encodes ASR #32
            }
            if (amount >= 32) {
                carry = value >> 31;
                return (value >> 31) ? 0xFFFFFFFF : 0;
            }
            carry = (value >> (amount - 1)) & 1;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >>
                                         amount);
        case 3:  // ROR
            if (amount == 0) {
                if (!immediateForm) {
                    return value;
                }
                // ROR #0 encodes RRX: rotate right by one through carry.
                const bool oldCarry = carry;
                carry = value & 1;
                return (static_cast<uint32_t>(oldCarry) << 31) | (value >> 1);
            }
            if ((amount & 31) == 0) {
                carry = value >> 31;
                return value;
            }
            amount &= 31;
            carry = (value >> (amount - 1)) & 1;
            return std::rotr(value, static_cast<int>(amount));
    }
    return value;
}

void ARM7TDMI::armDataProcessing(uint32_t opcode) {
    const uint32_t op = (opcode >> 21) & 0xF;
    const bool setFlags = opcode & (1u << 20);
    const uint32_t rn = (opcode >> 16) & 0xF;
    const uint32_t rd = (opcode >> 12) & 0xF;

    bool shifterCarry = cpsr & FLAG_C;
    uint32_t op1 = regs[rn];
    uint32_t op2;

    if (opcode & (1u << 25)) {
        // Immediate: 8-bit value rotated right by twice the 4-bit field.
        const uint32_t imm = opcode & 0xFF;
        const uint32_t rot = ((opcode >> 8) & 0xF) * 2;
        op2 = std::rotr(imm, static_cast<int>(rot));
        if (rot != 0) {
            shifterCarry = op2 >> 31;
        }
    } else {
        const uint32_t rm = opcode & 0xF;
        const uint32_t type = (opcode >> 5) & 3;
        const bool regShift = opcode & (1u << 4);
        uint32_t value = regs[rm];
        uint32_t amount;
        if (regShift) {
            amount = regs[(opcode >> 8) & 0xF] & 0xFF;
            // The extra internal cycle makes r15 read 12 bytes ahead here.
            if (rm == 15) value += 4;
            if (rn == 15) op1 += 4;
        } else {
            amount = (opcode >> 7) & 0x1F;
        }
        op2 = shiftOperand(value, type, amount, !regShift, shifterCarry);
    }

    bool carry = shifterCarry;
    bool overflow = cpsr & FLAG_V;
    uint32_t result = 0;
    bool writeback = true;

    // Subtraction is a + ~b + carryIn, which yields the ARM-convention
    // carry (no-borrow) and overflow flags for free.
    auto add = [&](uint32_t a, uint32_t b, uint32_t carryIn) {
        const uint64_t sum = static_cast<uint64_t>(a) + b + carryIn;
        result = static_cast<uint32_t>(sum);
        carry = (sum >> 32) != 0;
        overflow = ((~(a ^ b) & (a ^ result)) >> 31) != 0;
    };
    const uint32_t c = (cpsr & FLAG_C) ? 1 : 0;

    switch (op) {
        case 0x0: result = op1 & op2; break;             // AND
        case 0x1: result = op1 ^ op2; break;             // EOR
        case 0x2: add(op1, ~op2, 1); break;              // SUB
        case 0x3: add(op2, ~op1, 1); break;              // RSB
        case 0x4: add(op1, op2, 0); break;               // ADD
        case 0x5: add(op1, op2, c); break;               // ADC
        case 0x6: add(op1, ~op2, c); break;              // SBC
        case 0x7: add(op2, ~op1, c); break;              // RSC
        case 0x8: result = op1 & op2; writeback = false; break;  // TST
        case 0x9: result = op1 ^ op2; writeback = false; break;  // TEQ
        case 0xA: add(op1, ~op2, 1); writeback = false; break;   // CMP
        case 0xB: add(op1, op2, 0); writeback = false; break;    // CMN
        case 0xC: result = op1 | op2; break;             // ORR
        case 0xD: result = op2; break;                   // MOV
        case 0xE: result = op1 & ~op2; break;            // BIC
        case 0xF: result = ~op2; break;                  // MVN
    }

    if (setFlags) {
        if (rd == 15 && writeback) {
            // Exception return: restore CPSR from SPSR before the flush
            // below so the pipeline refills in the right state.
            setCPSR(getSPSR());
        } else {
            uint32_t flags = 0;
            if (result >> 31)  flags |= FLAG_N;
            if (result == 0)   flags |= FLAG_Z;
            if (carry)         flags |= FLAG_C;
            if (overflow)      flags |= FLAG_V;
            cpsr = (cpsr & 0x0FFFFFFF) | flags;
        }
    }

    if (writeback) {
        regs[rd] = result;
        if (rd == 15) {
            flushPipeline();
        }
    }
}

void ARM7TDMI::armBranch(uint32_t opcode) {
    // 24-bit signed word offset, relative to the pipelined PC (instr + 8).
    const int32_t offset = static_cast<int32_t>(opcode << 8) >> 6;
    if (opcode & (1u << 24)) {
        regs[14] = regs[15] - 4;  // BL: link points at the next instruction
    }
    regs[15] += static_cast<uint32_t>(offset);
    flushPipeline();
}

void ARM7TDMI::armBranchExchange(uint32_t opcode) {
    const uint32_t target = regs[opcode & 0xF];
    // Bits 7-4 == 0011 is the ARMv5 BLX-register encoding. The ARM7TDMI
    // doesn't implement it, but linking costs nothing if a ROM uses it.
    if (((opcode >> 4) & 0xF) == 0x3) {
        regs[14] = regs[15] - 4;
    }
    // Bit 0 of the target selects the new state.
    if (target & 1) {
        cpsr |= BIT_T;
        regs[15] = target & ~1u;
    } else {
        cpsr &= ~BIT_T;
        regs[15] = target & ~3u;
    }
    flushPipeline();
}

void ARM7TDMI::armSingleDataTransfer(uint32_t opcode) {
    const bool regOffset = opcode & (1u << 25);
    const bool preIndex = opcode & (1u << 24);
    const bool up = opcode & (1u << 23);
    const bool byte = opcode & (1u << 22);
    const bool writeback = opcode & (1u << 21);
    const bool load = opcode & (1u << 20);
    const uint32_t rn = (opcode >> 16) & 0xF;
    const uint32_t rd = (opcode >> 12) & 0xF;

    uint32_t offset;
    if (regOffset) {
        // Register offset with an immediate-form barrel shift.
        bool carry = cpsr & FLAG_C;  // discarded; SDT never updates C
        offset = shiftOperand(regs[opcode & 0xF], (opcode >> 5) & 3,
                              (opcode >> 7) & 0x1F, true, carry);
    } else {
        offset = opcode & 0xFFF;
    }

    const uint32_t base = regs[rn];
    const uint32_t offsetBase = up ? base + offset : base - offset;
    const uint32_t addr = preIndex ? offsetBase : base;

    if (load) {
        uint32_t value;
        if (byte) {
            value = bus.read8(addr);
        } else {
            // Unaligned LDR rotates the aligned word right by the byte
            // offset, matching hardware.
            value = std::rotr(bus.read32(addr),
                              static_cast<int>((addr & 3) * 8));
        }
        // Base writeback happens first so a load into rn wins.
        if (!preIndex || writeback) {
            regs[rn] = offsetBase;
        }
        regs[rd] = value;
        if (rd == 15) {
            flushPipeline();
        }
    } else {
        // STR of r15 stores the instruction address + 12.
        const uint32_t value = regs[rd] + (rd == 15 ? 4 : 0);
        if (byte) {
            bus.write8(addr, static_cast<uint8_t>(value));
        } else {
            bus.write32(addr, value);
        }
        if (!preIndex || writeback) {
            regs[rn] = offsetBase;
        }
    }
}

void ARM7TDMI::armBlockDataTransfer(uint32_t opcode) {
    const bool preIndex = opcode & (1u << 24);
    const bool up = opcode & (1u << 23);
    const bool sBit = opcode & (1u << 22);
    const bool writeback = opcode & (1u << 21);
    const bool load = opcode & (1u << 20);
    const uint32_t rn = (opcode >> 16) & 0xF;
    const uint32_t rlist = opcode & 0xFFFF;

    int count = 0;
    for (int i = 0; i < 16; ++i) {
        count += (rlist >> i) & 1;
    }

    // Transfers always walk memory upward from the lowest touched address,
    // regardless of the addressing mode (IA/IB/DA/DB).
    const uint32_t base = regs[rn];
    const uint32_t span = static_cast<uint32_t>(count) * 4;
    uint32_t addr;
    if (up) {
        addr = preIndex ? base + 4 : base;
    } else {
        addr = preIndex ? base - span : base - span + 4;
    }
    const uint32_t newBase = up ? base + span : base - span;

    if (sBit && !(load && (rlist & (1u << 15)))) {
        // User-bank transfer variant; not needed by anything we run yet.
        TRACE_LOG("LDM/STM S-bit user-bank transfer unimplemented @ 0x%08X",
                  regs[15] - 8);
    }

    for (int i = 0; i < 16; ++i) {
        if (!(rlist & (1u << i))) {
            continue;
        }
        if (load) {
            regs[i] = bus.read32(addr);
            if (i == 15) {
                if (sBit) {
                    setCPSR(getSPSR());  // exception return variant
                }
                flushPipeline();
            }
        } else {
            bus.write32(addr, regs[i] + (i == 15 ? 4 : 0));
        }
        addr += 4;
    }

    // A load that includes the base register wins over writeback.
    if (writeback && !(load && (rlist & (1u << rn)))) {
        regs[rn] = newBase;
    }
}

void ARM7TDMI::armHalfwordTransfer(uint32_t opcode) {
    const bool preIndex = opcode & (1u << 24);
    const bool up = opcode & (1u << 23);
    const bool immForm = opcode & (1u << 22);
    const bool writeback = opcode & (1u << 21);
    const bool load = opcode & (1u << 20);
    const uint32_t rn = (opcode >> 16) & 0xF;
    const uint32_t rd = (opcode >> 12) & 0xF;
    const uint32_t sh = (opcode >> 5) & 3;  // 1=H, 2=SB, 3=SH

    // Immediate form splits the 8-bit offset across bits 11-8 and 3-0.
    const uint32_t offset =
        immForm ? (((opcode >> 4) & 0xF0) | (opcode & 0xF))
                : regs[opcode & 0xF];

    const uint32_t base = regs[rn];
    const uint32_t offsetBase = up ? base + offset : base - offset;
    const uint32_t addr = preIndex ? offsetBase : base;

    if (load) {
        uint32_t value;
        switch (sh) {
            case 1:  // LDRH: a misaligned read rotates the aligned halfword
                value = std::rotr(static_cast<uint32_t>(bus.read16(addr)),
                                  static_cast<int>((addr & 1) * 8));
                break;
            case 2:  // LDRSB
                value = static_cast<uint32_t>(
                    static_cast<int8_t>(bus.read8(addr)));
                break;
            default:  // LDRSH: misaligned degrades to LDRSB on this core
                if (addr & 1) {
                    value = static_cast<uint32_t>(
                        static_cast<int8_t>(bus.read8(addr)));
                } else {
                    value = static_cast<uint32_t>(
                        static_cast<int16_t>(bus.read16(addr)));
                }
                break;
        }
        if (!preIndex || writeback) {
            regs[rn] = offsetBase;
        }
        regs[rd] = value;
        if (rd == 15) {
            flushPipeline();
        }
    } else {
        if (sh == 1) {  // STRH
            bus.write16(addr,
                        static_cast<uint16_t>(regs[rd] + (rd == 15 ? 4 : 0)));
        } else {
            // LDRD/STRD encodings are ARMv5; nothing valid lands here.
            TRACE_LOG("invalid halfword-space store 0x%08X @ 0x%08X", opcode,
                      regs[15] - 8);
        }
        if (!preIndex || writeback) {
            regs[rn] = offsetBase;
        }
    }
}

// Multiplies on ARMv4T set N/Z when S is given; C and V are unpredictable
// on hardware, so we leave them untouched.
void ARM7TDMI::armMultiply(uint32_t opcode) {
    const bool accumulate = opcode & (1u << 21);
    const bool setFlags = opcode & (1u << 20);
    const uint32_t rd = (opcode >> 16) & 0xF;  // note: rd/rn swapped vs SDT
    const uint32_t rn = (opcode >> 12) & 0xF;
    const uint32_t rs = (opcode >> 8) & 0xF;
    const uint32_t rm = opcode & 0xF;

    uint32_t result = regs[rm] * regs[rs];
    if (accumulate) {
        result += regs[rn];
    }
    regs[rd] = result;
    if (setFlags) {
        setNZ(result);
    }
}

void ARM7TDMI::armMultiplyLong(uint32_t opcode) {
    const bool isSigned = opcode & (1u << 22);
    const bool accumulate = opcode & (1u << 21);
    const bool setFlags = opcode & (1u << 20);
    const uint32_t rdHi = (opcode >> 16) & 0xF;
    const uint32_t rdLo = (opcode >> 12) & 0xF;
    const uint32_t rs = (opcode >> 8) & 0xF;
    const uint32_t rm = opcode & 0xF;

    uint64_t result;
    if (isSigned) {
        result = static_cast<uint64_t>(
            static_cast<int64_t>(static_cast<int32_t>(regs[rm])) *
            static_cast<int32_t>(regs[rs]));
    } else {
        result = static_cast<uint64_t>(regs[rm]) * regs[rs];
    }
    if (accumulate) {
        result += (static_cast<uint64_t>(regs[rdHi]) << 32) | regs[rdLo];
    }
    regs[rdLo] = static_cast<uint32_t>(result);
    regs[rdHi] = static_cast<uint32_t>(result >> 32);
    if (setFlags) {
        cpsr = (cpsr & ~(FLAG_N | FLAG_Z)) |
               ((result >> 63) ? FLAG_N : 0) | ((result == 0) ? FLAG_Z : 0);
    }
}

void ARM7TDMI::armPSRTransfer(uint32_t opcode) {
    const bool useSPSR = opcode & (1u << 22);

    if (!(opcode & (1u << 21))) {
        // MRS rd, CPSR/SPSR
        regs[(opcode >> 12) & 0xF] = useSPSR ? getSPSR() : cpsr;
        return;
    }

    // MSR CPSR/SPSR_fields, op2
    uint32_t value;
    if (opcode & (1u << 25)) {
        const uint32_t imm = opcode & 0xFF;
        const uint32_t rot = ((opcode >> 8) & 0xF) * 2;
        value = std::rotr(imm, static_cast<int>(rot));
    } else {
        value = regs[opcode & 0xF];
    }

    uint32_t mask = 0;
    if (opcode & (1u << 16)) mask |= 0x000000FF;  // c: control
    if (opcode & (1u << 17)) mask |= 0x0000FF00;  // x: extension
    if (opcode & (1u << 18)) mask |= 0x00FF0000;  // s: status
    if (opcode & (1u << 19)) mask |= 0xFF000000;  // f: flags

    if (useSPSR) {
        setSPSR((getSPSR() & ~mask) | (value & mask));
        return;
    }
    // User mode may only alter the flags field; and MSR can never switch
    // ARM/Thumb state (writing T here is unpredictable on hardware).
    if (currentMode() == Mode::User) {
        mask &= 0xFF000000;
    }
    mask &= ~BIT_T;
    setCPSR((cpsr & ~mask) | (value & mask));
}

// SWP/SWPB: atomic swap between a register and memory. A word SWP from a
// misaligned address rotates the loaded value like LDR does.
void ARM7TDMI::armSwap(uint32_t opcode) {
    const bool byte = opcode & (1u << 22);
    const uint32_t addr = regs[(opcode >> 16) & 0xF];
    const uint32_t rd = (opcode >> 12) & 0xF;
    const uint32_t rm = opcode & 0xF;

    if (byte) {
        const uint8_t old = bus.read8(addr);
        bus.write8(addr, static_cast<uint8_t>(regs[rm]));
        regs[rd] = old;
    } else {
        uint32_t old = bus.read32(addr);
        const uint32_t rot = (addr & 3) * 8;
        old = std::rotr(old, static_cast<int>(rot));
        bus.write32(addr, regs[rm]);
        regs[rd] = old;
    }
}

void ARM7TDMI::armSoftwareInterrupt(uint32_t opcode) {
    // The BIOS dispatches on bits 23-16 of the comment field.
    softwareInterrupt((opcode >> 16) & 0xFF);
}

void ARM7TDMI::thumbSoftwareInterrupt(uint16_t opcode) {
    softwareInterrupt(opcode & 0xFF);
}

void ARM7TDMI::softwareInterrupt(uint32_t number) {
    if (bus.hasBIOS()) {
        // Take the real SVC exception and let the loaded BIOS handle it.
        const uint32_t oldCpsr = cpsr;
        const uint32_t returnAddr = regs[15] - (inThumbState() ? 2 : 4);
        switchMode(Mode::Supervisor);
        spsrBank[BANK_SVC] = oldCpsr;
        regs[14] = returnAddr;
        cpsr = (cpsr & ~BIT_T) | BIT_I;
        regs[15] = 0x00000008;  // SWI exception vector
        flushPipeline();
        return;
    }
    hleSWI(number);
}

void ARM7TDMI::armUnimplemented(uint32_t opcode) {
    TRACE_LOG("unimplemented ARM opcode 0x%08X @ 0x%08X", opcode,
              regs[15] - 8);
    (void)opcode;
}

// ---------------------------------------------------------------------------
// Shared ALU helpers
// ---------------------------------------------------------------------------

uint32_t ARM7TDMI::aluAdd(uint32_t a, uint32_t b, uint32_t carryIn,
                          bool setFlags) {
    const uint64_t sum = static_cast<uint64_t>(a) + b + carryIn;
    const uint32_t result = static_cast<uint32_t>(sum);
    if (setFlags) {
        uint32_t flags = 0;
        if (result >> 31)                          flags |= FLAG_N;
        if (result == 0)                           flags |= FLAG_Z;
        if (sum >> 32)                             flags |= FLAG_C;
        if ((~(a ^ b) & (a ^ result)) >> 31)       flags |= FLAG_V;
        cpsr = (cpsr & 0x0FFFFFFF) | flags;
    }
    return result;
}

void ARM7TDMI::setNZ(uint32_t result) {
    cpsr = (cpsr & ~(FLAG_N | FLAG_Z)) | ((result >> 31) ? FLAG_N : 0) |
           ((result == 0) ? FLAG_Z : 0);
}

void ARM7TDMI::setNZC(uint32_t result, bool carry) {
    setNZ(result);
    cpsr = (cpsr & ~FLAG_C) | (carry ? FLAG_C : 0);
}

// ---------------------------------------------------------------------------
// Thumb decoder and handlers
// ---------------------------------------------------------------------------

const std::array<ARM7TDMI::ThumbHandler, 256>& ARM7TDMI::thumbTable() {
    static const auto table = [] {
        std::array<ThumbHandler, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {  // instruction bits 15-8
            ThumbHandler h = &ARM7TDMI::thumbUnimplemented;
            if ((i & 0xF8) == 0x18) {
                h = &ARM7TDMI::thumbAddSub;         // 00011
            } else if ((i & 0xE0) == 0x00) {
                h = &ARM7TDMI::thumbShifted;        // 000
            } else if ((i & 0xE0) == 0x20) {
                h = &ARM7TDMI::thumbImmediate;      // 001
            } else if ((i & 0xFC) == 0x40) {
                h = &ARM7TDMI::thumbALU;            // 010000
            } else if ((i & 0xFC) == 0x44) {
                h = &ARM7TDMI::thumbHiRegBX;        // 010001
            } else if ((i & 0xF8) == 0x48) {
                h = &ARM7TDMI::thumbLoadPC;         // 01001
            } else if ((i & 0xF0) == 0x50) {
                h = &ARM7TDMI::thumbLoadStoreReg;   // 0101
            } else if ((i & 0xE0) == 0x60) {
                h = &ARM7TDMI::thumbLoadStoreImm;   // 011
            } else if ((i & 0xF0) == 0x80) {
                h = &ARM7TDMI::thumbLoadStoreHalf;  // 1000
            } else if ((i & 0xF0) == 0x90) {
                h = &ARM7TDMI::thumbLoadStoreSP;    // 1001
            } else if ((i & 0xF0) == 0xA0) {
                h = &ARM7TDMI::thumbLoadAddress;    // 1010
            } else if (i == 0xB0) {
                h = &ARM7TDMI::thumbAdjustSP;       // 10110000
            } else if ((i & 0xF6) == 0xB4) {
                h = &ARM7TDMI::thumbPushPop;        // 1011x10x
            } else if ((i & 0xF0) == 0xC0) {
                h = &ARM7TDMI::thumbMultiple;       // 1100
            } else if (i == 0xDF) {
                h = &ARM7TDMI::thumbSoftwareInterrupt;  // F17
            } else if ((i & 0xF0) == 0xD0) {
                h = &ARM7TDMI::thumbCondBranch;     // 1101
            } else if ((i & 0xF8) == 0xE0) {
                h = &ARM7TDMI::thumbBranch;         // 11100
            } else if ((i & 0xF0) == 0xF0) {
                h = &ARM7TDMI::thumbLongBranch;     // 1111
            }
            t[i] = h;
        }
        return t;
    }();
    return table;
}

void ARM7TDMI::executeThumb(uint16_t opcode) {
    (this->*thumbTable()[opcode >> 8])(opcode);
}

void ARM7TDMI::thumbShifted(uint16_t opcode) {
    const uint32_t op = (opcode >> 11) & 3;  // 0 LSL, 1 LSR, 2 ASR
    const uint32_t amount = (opcode >> 6) & 0x1F;
    const uint32_t rs = (opcode >> 3) & 7;
    const uint32_t rd = opcode & 7;
    bool carry = cpsr & FLAG_C;
    const uint32_t result = shiftOperand(regs[rs], op, amount, true, carry);
    regs[rd] = result;
    setNZC(result, carry);
}

void ARM7TDMI::thumbAddSub(uint16_t opcode) {
    const bool immediate = opcode & (1u << 10);
    const bool subtract = opcode & (1u << 9);
    const uint32_t field = (opcode >> 6) & 7;  // rn or imm3
    const uint32_t rs = (opcode >> 3) & 7;
    const uint32_t rd = opcode & 7;
    const uint32_t operand = immediate ? field : regs[field];
    regs[rd] = subtract ? aluAdd(regs[rs], ~operand, 1, true)
                        : aluAdd(regs[rs], operand, 0, true);
}

void ARM7TDMI::thumbImmediate(uint16_t opcode) {
    const uint32_t op = (opcode >> 11) & 3;
    const uint32_t rd = (opcode >> 8) & 7;
    const uint32_t imm = opcode & 0xFF;
    switch (op) {
        case 0:  // MOV
            regs[rd] = imm;
            setNZ(imm);
            break;
        case 1:  // CMP
            aluAdd(regs[rd], ~imm, 1, true);
            break;
        case 2:  // ADD
            regs[rd] = aluAdd(regs[rd], imm, 0, true);
            break;
        case 3:  // SUB
            regs[rd] = aluAdd(regs[rd], ~imm, 1, true);
            break;
    }
}

void ARM7TDMI::thumbALU(uint16_t opcode) {
    const uint32_t op = (opcode >> 6) & 0xF;
    const uint32_t rs = (opcode >> 3) & 7;
    const uint32_t rd = opcode & 7;
    const uint32_t a = regs[rd];
    const uint32_t b = regs[rs];
    const uint32_t c = (cpsr & FLAG_C) ? 1 : 0;
    bool carry = cpsr & FLAG_C;
    uint32_t result;

    switch (op) {
        case 0x0: result = a & b; regs[rd] = result; setNZ(result); break;
        case 0x1: result = a ^ b; regs[rd] = result; setNZ(result); break;
        case 0x2:  // LSL
            result = shiftOperand(a, 0, b & 0xFF, false, carry);
            regs[rd] = result;
            setNZC(result, carry);
            break;
        case 0x3:  // LSR
            result = shiftOperand(a, 1, b & 0xFF, false, carry);
            regs[rd] = result;
            setNZC(result, carry);
            break;
        case 0x4:  // ASR
            result = shiftOperand(a, 2, b & 0xFF, false, carry);
            regs[rd] = result;
            setNZC(result, carry);
            break;
        case 0x5: regs[rd] = aluAdd(a, b, c, true); break;   // ADC
        case 0x6: regs[rd] = aluAdd(a, ~b, c, true); break;  // SBC
        case 0x7:  // ROR
            result = shiftOperand(a, 3, b & 0xFF, false, carry);
            regs[rd] = result;
            setNZC(result, carry);
            break;
        case 0x8: setNZ(a & b); break;                       // TST
        case 0x9: regs[rd] = aluAdd(0, ~b, 1, true); break;  // NEG
        case 0xA: aluAdd(a, ~b, 1, true); break;             // CMP
        case 0xB: aluAdd(a, b, 0, true); break;              // CMN
        case 0xC: result = a | b; regs[rd] = result; setNZ(result); break;
        case 0xD: result = a * b; regs[rd] = result; setNZ(result); break;
        case 0xE: result = a & ~b; regs[rd] = result; setNZ(result); break;
        default:  result = ~b; regs[rd] = result; setNZ(result); break;
    }
}

void ARM7TDMI::thumbHiRegBX(uint16_t opcode) {
    const uint32_t op = (opcode >> 8) & 3;
    const uint32_t rs = ((opcode >> 3) & 7) | ((opcode & 0x40) ? 8 : 0);
    const uint32_t rd = (opcode & 7) | ((opcode & 0x80) ? 8 : 0);
    const uint32_t value = regs[rs];

    switch (op) {
        case 0:  // ADD (no flags)
            regs[rd] += value;
            if (rd == 15) {
                regs[15] &= ~1u;
                flushPipeline();
            }
            break;
        case 1:  // CMP
            aluAdd(regs[rd], ~value, 1, true);
            break;
        case 2:  // MOV (no flags)
            regs[rd] = value;
            if (rd == 15) {
                regs[15] &= ~1u;
                flushPipeline();
            }
            break;
        case 3:  // BX: bit 0 of the target selects ARM/Thumb state
            if (value & 1) {
                cpsr |= BIT_T;
                regs[15] = value & ~1u;
            } else {
                cpsr &= ~BIT_T;
                regs[15] = value & ~3u;
            }
            flushPipeline();
            break;
    }
}

void ARM7TDMI::thumbLoadPC(uint16_t opcode) {
    const uint32_t rd = (opcode >> 8) & 7;
    const uint32_t offset = (opcode & 0xFF) * 4;
    // PC reads as instruction + 4, force word-aligned for the literal pool.
    regs[rd] = bus.read32((regs[15] & ~2u) + offset);
}

void ARM7TDMI::thumbLoadStoreReg(uint16_t opcode) {
    const uint32_t ro = (opcode >> 6) & 7;
    const uint32_t rb = (opcode >> 3) & 7;
    const uint32_t rd = opcode & 7;
    const uint32_t addr = regs[rb] + regs[ro];
    const uint32_t op = (opcode >> 10) & 3;

    if (opcode & (1u << 9)) {  // F8: halfword / sign-extended
        switch (op) {
            case 0: bus.write16(addr, static_cast<uint16_t>(regs[rd])); break;
            case 1:  // LDSB
                regs[rd] = static_cast<uint32_t>(
                    static_cast<int8_t>(bus.read8(addr)));
                break;
            case 2: regs[rd] = bus.read16(addr); break;  // LDRH
            case 3:  // LDSH
                regs[rd] = static_cast<uint32_t>(
                    static_cast<int16_t>(bus.read16(addr)));
                break;
        }
    } else {  // F7: word / byte
        switch (op) {
            case 0: bus.write32(addr, regs[rd]); break;                 // STR
            case 1: bus.write8(addr, static_cast<uint8_t>(regs[rd])); break;
            case 2:  // LDR: an unaligned word load rotates like ARM (hardware)
                regs[rd] = std::rotr(bus.read32(addr),
                                     static_cast<int>((addr & 3) * 8));
                break;
            case 3: regs[rd] = bus.read8(addr); break;                  // LDRB
        }
    }
}

void ARM7TDMI::thumbLoadStoreImm(uint16_t opcode) {
    const bool byte = opcode & (1u << 12);
    const bool load = opcode & (1u << 11);
    const uint32_t offset = (opcode >> 6) & 0x1F;
    const uint32_t rb = (opcode >> 3) & 7;
    const uint32_t rd = opcode & 7;

    if (byte) {
        const uint32_t addr = regs[rb] + offset;
        if (load) {
            regs[rd] = bus.read8(addr);
        } else {
            bus.write8(addr, static_cast<uint8_t>(regs[rd]));
        }
    } else {
        const uint32_t addr = regs[rb] + offset * 4;
        if (load) {
            regs[rd] = std::rotr(bus.read32(addr),
                                 static_cast<int>((addr & 3) * 8));
        } else {
            bus.write32(addr, regs[rd]);
        }
    }
}

void ARM7TDMI::thumbLoadStoreHalf(uint16_t opcode) {
    const bool load = opcode & (1u << 11);
    const uint32_t offset = ((opcode >> 6) & 0x1F) * 2;
    const uint32_t rb = (opcode >> 3) & 7;
    const uint32_t rd = opcode & 7;
    const uint32_t addr = regs[rb] + offset;
    if (load) {
        regs[rd] = bus.read16(addr);
    } else {
        bus.write16(addr, static_cast<uint16_t>(regs[rd]));
    }
}

void ARM7TDMI::thumbLoadStoreSP(uint16_t opcode) {
    const bool load = opcode & (1u << 11);
    const uint32_t rd = (opcode >> 8) & 7;
    const uint32_t addr = regs[13] + (opcode & 0xFF) * 4;
    if (load) {
        regs[rd] = std::rotr(bus.read32(addr),
                             static_cast<int>((addr & 3) * 8));
    } else {
        bus.write32(addr, regs[rd]);
    }
}

void ARM7TDMI::thumbLoadAddress(uint16_t opcode) {
    const bool useSP = opcode & (1u << 11);
    const uint32_t rd = (opcode >> 8) & 7;
    const uint32_t offset = (opcode & 0xFF) * 4;
    regs[rd] = (useSP ? regs[13] : (regs[15] & ~2u)) + offset;
}

void ARM7TDMI::thumbAdjustSP(uint16_t opcode) {
    const uint32_t offset = (opcode & 0x7F) * 4;
    if (opcode & (1u << 7)) {
        regs[13] -= offset;
    } else {
        regs[13] += offset;
    }
}

void ARM7TDMI::thumbPushPop(uint16_t opcode) {
    const bool load = opcode & (1u << 11);  // 0 = PUSH, 1 = POP
    const bool extra = opcode & (1u << 8);  // PUSH: LR, POP: PC
    const uint32_t rlist = opcode & 0xFF;

    if (!load) {
        int count = extra ? 1 : 0;
        for (int i = 0; i < 8; ++i) {
            count += (rlist >> i) & 1;
        }
        uint32_t addr = regs[13] - static_cast<uint32_t>(count) * 4;
        regs[13] = addr;
        for (int i = 0; i < 8; ++i) {
            if (rlist & (1u << i)) {
                bus.write32(addr, regs[i]);
                addr += 4;
            }
        }
        if (extra) {
            bus.write32(addr, regs[14]);
        }
    } else {
        uint32_t addr = regs[13];
        for (int i = 0; i < 8; ++i) {
            if (rlist & (1u << i)) {
                regs[i] = bus.read32(addr);
                addr += 4;
            }
        }
        if (extra) {
            // ARMv4T: POP PC ignores bit 0 and stays in Thumb state.
            regs[15] = bus.read32(addr) & ~1u;
            addr += 4;
            regs[13] = addr;
            flushPipeline();
            return;
        }
        regs[13] = addr;
    }
}

void ARM7TDMI::thumbMultiple(uint16_t opcode) {
    const bool load = opcode & (1u << 11);
    const uint32_t rb = (opcode >> 8) & 7;
    const uint32_t rlist = opcode & 0xFF;
    uint32_t addr = regs[rb];
    for (int i = 0; i < 8; ++i) {
        if (rlist & (1u << i)) {
            if (load) {
                regs[i] = bus.read32(addr);
            } else {
                bus.write32(addr, regs[i]);
            }
            addr += 4;
        }
    }
    regs[rb] = addr;  // writeback
}

void ARM7TDMI::thumbCondBranch(uint16_t opcode) {
    const uint32_t cond = (opcode >> 8) & 0xF;
    if (!checkCondition(cond)) {
        return;
    }
    const int32_t offset = static_cast<int8_t>(opcode & 0xFF) * 2;
    regs[15] += static_cast<uint32_t>(offset);
    flushPipeline();
}

void ARM7TDMI::thumbBranch(uint16_t opcode) {
    // 11-bit signed halfword offset.
    const int32_t offset = (static_cast<int32_t>(opcode << 21) >> 21) * 2;
    regs[15] += static_cast<uint32_t>(offset);
    flushPipeline();
}

void ARM7TDMI::thumbLongBranch(uint16_t opcode) {
    const uint32_t offset = opcode & 0x7FF;
    if (!(opcode & (1u << 11))) {
        // First half: LR = PC + (signed offset << 12).
        const int32_t high = static_cast<int32_t>(offset << 21) >> 9;
        regs[14] = regs[15] + static_cast<uint32_t>(high);
    } else {
        // Second half: branch to LR + (offset << 1), LR = return | 1.
        const uint32_t returnAddr = regs[15] - 2;
        regs[15] = regs[14] + (offset << 1);
        regs[14] = returnAddr | 1;
        flushPipeline();
    }
}

void ARM7TDMI::thumbUnimplemented(uint16_t opcode) {
    TRACE_LOG("unimplemented Thumb opcode 0x%04X @ 0x%08X", opcode,
              regs[15] - 4);
    (void)opcode;
}
