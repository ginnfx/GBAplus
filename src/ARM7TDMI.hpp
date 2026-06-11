#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

class Bus;

// ARM7TDMI core. Talks to the rest of the system exclusively through the Bus.
class ARM7TDMI {
public:
    enum class Mode : uint32_t {
        User       = 0x10,
        FIQ        = 0x11,
        IRQ        = 0x12,
        Supervisor = 0x13,
        Abort      = 0x17,
        Undefined  = 0x1B,
        System     = 0x1F,
    };

    // CPSR flag bits.
    static constexpr uint32_t FLAG_N = 1u << 31;  // Negative
    static constexpr uint32_t FLAG_Z = 1u << 30;  // Zero
    static constexpr uint32_t FLAG_C = 1u << 29;  // Carry
    static constexpr uint32_t FLAG_V = 1u << 28;  // Overflow
    static constexpr uint32_t BIT_I  = 1u << 7;   // IRQ disable
    static constexpr uint32_t BIT_F  = 1u << 6;   // FIQ disable
    static constexpr uint32_t BIT_T  = 1u << 5;   // Thumb state
    static constexpr uint32_t MODE_MASK = 0x1F;

    explicit ARM7TDMI(Bus& bus);

    // Puts the CPU in the post-BIOS boot state and fills the pipeline.
    void reset();

    // Fetches, decodes and executes a single instruction.
    void step();

    // Register access. Reading r15 mid-execution yields the pipelined value
    // (instruction address + 8 in ARM state, + 4 in Thumb state).
    uint32_t reg(int index) const { return regs[index]; }
    void setReg(int index, uint32_t value);

    uint32_t getCPSR() const { return cpsr; }
    void setCPSR(uint32_t value);

    uint32_t getSPSR() const;
    void setSPSR(uint32_t value);

    Mode currentMode() const { return static_cast<Mode>(cpsr & MODE_MASK); }
    bool inThumbState() const { return cpsr & BIT_T; }

    // True while the CPU sleeps after Halt/IntrWait. step() still consumes
    // time but executes nothing until an enabled interrupt arrives.
    bool isHalted() const { return halted; }

    // Switches the CPSR mode bits and swaps the banked registers in/out.
    void switchMode(Mode newMode);

    // Invalidates and refills the pipeline. Must be called after any write
    // to r15 (branches, mode-changing returns, ...).
    void flushPipeline();

    // When set, each step() writes "PC=... CPSR=... R0=... R15=..." to the
    // file before executing, for diffing against reference emulators.
    void setTraceFile(std::FILE* file) { traceFile = file; }

private:
    // Banked register storage indices. User and System share a bank.
    enum BankIndex : int {
        BANK_USER = 0,  // also System
        BANK_FIQ,
        BANK_IRQ,
        BANK_SVC,
        BANK_ABT,
        BANK_UND,
        BANK_COUNT,
    };

    using ArmHandler = void (ARM7TDMI::*)(uint32_t);
    using ThumbHandler = void (ARM7TDMI::*)(uint16_t);

    static int bankIndex(Mode mode);

    // 4096-entry dispatch table indexed by instruction bits 27-20 and 7-4,
    // which together uniquely identify every ARM instruction class.
    static const std::array<ArmHandler, 4096>& armTable();

    // 256-entry Thumb dispatch table indexed by instruction bits 15-8.
    static const std::array<ThumbHandler, 256>& thumbTable();

    bool checkCondition(uint32_t cond) const;

    // Takes the IRQ exception if IME/IE/IF line one up and I is clear.
    bool irqPending() const;
    void enterIRQ();

    // High-level emulation of the BIOS IRQ stub when no bios.bin is loaded:
    // dispatch reads the user handler pointer from 0x03007FFC; execution
    // returning to HLE_IRQ_RETURN is intercepted and unwound in step().
    static constexpr uint32_t HLE_IRQ_RETURN = 0x00000138;
    void hleIrqDispatch();
    void hleIrqReturn();

    void executeARM(uint32_t opcode);
    void executeThumb(uint16_t opcode);

    void armDataProcessing(uint32_t opcode);
    void armBranch(uint32_t opcode);
    void armBranchExchange(uint32_t opcode);
    void armSingleDataTransfer(uint32_t opcode);  // LDR/STR/LDRB/STRB
    void armBlockDataTransfer(uint32_t opcode);   // LDM/STM
    void armHalfwordTransfer(uint32_t opcode);    // LDRH/STRH/LDRSB/LDRSH
    void armMultiply(uint32_t opcode);            // MUL/MLA
    void armMultiplyLong(uint32_t opcode);        // UMULL/UMLAL/SMULL/SMLAL
    void armPSRTransfer(uint32_t opcode);         // MRS/MSR
    void armSwap(uint32_t opcode);                // SWP/SWPB
    void armSoftwareInterrupt(uint32_t opcode);
    void armUnimplemented(uint32_t opcode);

    void thumbShifted(uint16_t opcode);        // F1:  LSL/LSR/ASR imm
    void thumbAddSub(uint16_t opcode);         // F2:  ADD/SUB reg or imm3
    void thumbImmediate(uint16_t opcode);      // F3:  MOV/CMP/ADD/SUB imm8
    void thumbALU(uint16_t opcode);            // F4:  register ALU ops
    void thumbHiRegBX(uint16_t opcode);        // F5:  hi-reg ADD/CMP/MOV/BX
    void thumbLoadPC(uint16_t opcode);         // F6:  LDR rd, [PC, #imm]
    void thumbLoadStoreReg(uint16_t opcode);   // F7+8: [rb, ro] transfers
    void thumbLoadStoreImm(uint16_t opcode);   // F9:  word/byte imm offset
    void thumbLoadStoreHalf(uint16_t opcode);  // F10: halfword imm offset
    void thumbLoadStoreSP(uint16_t opcode);    // F11: [SP, #imm]
    void thumbLoadAddress(uint16_t opcode);    // F12: ADD rd, PC/SP, #imm
    void thumbAdjustSP(uint16_t opcode);       // F13: ADD SP, #±imm
    void thumbPushPop(uint16_t opcode);        // F14: PUSH/POP
    void thumbMultiple(uint16_t opcode);       // F15: LDMIA/STMIA
    void thumbCondBranch(uint16_t opcode);     // F16: Bcc
    void thumbBranch(uint16_t opcode);         // F18: B
    void thumbLongBranch(uint16_t opcode);     // F19: BL (two halves)
    void thumbSoftwareInterrupt(uint16_t opcode);  // F17
    void thumbUnimplemented(uint16_t opcode);

    // SWI entry: takes the SVC exception when a real BIOS is loaded,
    // otherwise high-level-emulates the call (ARM7TDMI_swi.cpp).
    void softwareInterrupt(uint32_t number);
    void hleSWI(uint32_t number);
    void swiSoftReset();
    void swiRegisterRamReset(uint32_t flags);
    void swiIntrWait(uint32_t discardOld, uint32_t mask);
    void swiDiv(int32_t numerator, int32_t denominator);
    void swiArcTan();
    void swiArcTan2();
    void swiCpuSet();
    void swiCpuFastSet();
    void swiBgAffineSet();
    void swiObjAffineSet();
    void swiBitUnPack();
    void swiLZ77UnComp();
    void swiHuffUnComp();
    void swiRLUnComp();

    // ALU helpers shared by ARM and Thumb paths. Subtraction is expressed
    // as a + ~b + carryIn, which yields ARM-convention C and V for free.
    uint32_t aluAdd(uint32_t a, uint32_t b, uint32_t carryIn, bool setFlags);
    void setNZ(uint32_t result);
    void setNZC(uint32_t result, bool carry);

    // Barrel shifter. `immediateForm` selects the encoding-dependent
    // interpretation of amount 0 (LSR/ASR #32, RRX vs. no-op).
    static uint32_t shiftOperand(uint32_t value, uint32_t type,
                                 uint32_t amount, bool immediateForm,
                                 bool& carry);

    Bus& bus;

    // Active register file. regs[15] is the PC.
    std::array<uint32_t, 16> regs{};
    uint32_t cpsr = 0;

    // Inactive copies of the banked registers. FIQ banks r8-r14; the other
    // exception modes bank only r13-r14. The active mode's slots are stale
    // until the next switchMode() saves into them.
    std::array<std::array<uint32_t, 5>, 2>          bankedR8_12{};   // [isFiq]
    std::array<std::array<uint32_t, 2>, BANK_COUNT> bankedR13_14{};
    std::array<uint32_t, BANK_COUNT>                spsrBank{};      // USER slot unused

    // 3-stage pipeline model: [0] = decode slot (next to execute),
    // [1] = fetch slot. regs[15] is the fetch address, i.e. during execution
    // of the instruction at A it reads A+8 (ARM) / A+4 (Thumb).
    std::array<uint32_t, 2> pipeline{};
    bool pipelineFlushed = false;

    // Halt/IntrWait state. intrWaiting models the BIOS IntrWait loop: the
    // CPU re-halts after each IRQ until the user handler has set one of the
    // requested bits in the IF-shadow halfword at 0x03007FF8.
    bool halted = false;
    bool intrWaiting = false;
    uint32_t intrWaitMask = 0;

    std::FILE* traceFile = nullptr;
};
