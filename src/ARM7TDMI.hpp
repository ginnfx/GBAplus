#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

class Bus;

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

    static constexpr uint32_t FLAG_N = 1u << 31;
    static constexpr uint32_t FLAG_Z = 1u << 30;
    static constexpr uint32_t FLAG_C = 1u << 29;
    static constexpr uint32_t FLAG_V = 1u << 28;
    static constexpr uint32_t BIT_I  = 1u << 7;
    static constexpr uint32_t BIT_F  = 1u << 6;
    static constexpr uint32_t BIT_T  = 1u << 5;
    static constexpr uint32_t MODE_MASK = 0x1F;

    explicit ARM7TDMI(Bus& bus);

    void reset();

    void step();

    uint32_t reg(int index) const { return regs[index]; }
    void setReg(int index, uint32_t value);

    uint32_t getCPSR() const { return cpsr; }
    void setCPSR(uint32_t value);

    uint32_t getSPSR() const;
    void setSPSR(uint32_t value);

    Mode currentMode() const { return static_cast<Mode>(cpsr & MODE_MASK); }
    bool inThumbState() const { return cpsr & BIT_T; }

    void switchMode(Mode newMode);

    void flushPipeline();

    void setTraceFile(std::FILE* file) { traceFile = file; }

private:
    enum BankIndex : int {
        BANK_USER = 0,
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

    static const std::array<ArmHandler, 4096>& armTable();

    static const std::array<ThumbHandler, 256>& thumbTable();

    bool checkCondition(uint32_t cond) const;

    bool irqPending() const;
    void enterIRQ();

    static constexpr uint32_t HLE_IRQ_RETURN = 0x00000138;
    void hleIrqDispatch();
    void hleIrqReturn();

    void executeARM(uint32_t opcode);
    void executeThumb(uint16_t opcode);

    void armDataProcessing(uint32_t opcode);
    void armBranch(uint32_t opcode);
    void armBranchExchange(uint32_t opcode);
    void armSingleDataTransfer(uint32_t opcode);
    void armBlockDataTransfer(uint32_t opcode);
    void armHalfwordTransfer(uint32_t opcode);
    void armMultiply(uint32_t opcode);
    void armMultiplyLong(uint32_t opcode);
    void armPSRTransfer(uint32_t opcode);
    void armUnimplemented(uint32_t opcode);

    void thumbShifted(uint16_t opcode);
    void thumbAddSub(uint16_t opcode);
    void thumbImmediate(uint16_t opcode);
    void thumbALU(uint16_t opcode);
    void thumbHiRegBX(uint16_t opcode);
    void thumbLoadPC(uint16_t opcode);
    void thumbLoadStoreReg(uint16_t opcode);
    void thumbLoadStoreImm(uint16_t opcode);
    void thumbLoadStoreHalf(uint16_t opcode);
    void thumbLoadStoreSP(uint16_t opcode);
    void thumbLoadAddress(uint16_t opcode);
    void thumbAdjustSP(uint16_t opcode);
    void thumbPushPop(uint16_t opcode);
    void thumbMultiple(uint16_t opcode);
    void thumbCondBranch(uint16_t opcode);
    void thumbBranch(uint16_t opcode);
    void thumbLongBranch(uint16_t opcode);
    void thumbUnimplemented(uint16_t opcode);

    uint32_t aluAdd(uint32_t a, uint32_t b, uint32_t carryIn, bool setFlags);
    void setNZ(uint32_t result);
    void setNZC(uint32_t result, bool carry);

    static uint32_t shiftOperand(uint32_t value, uint32_t type,
                                 uint32_t amount, bool immediateForm,
                                 bool& carry);

    Bus& bus;

    std::array<uint32_t, 16> regs{};
    uint32_t cpsr = 0;

    std::array<std::array<uint32_t, 5>, 2>          bankedR8_12{};
    std::array<std::array<uint32_t, 2>, BANK_COUNT> bankedR13_14{};
    std::array<uint32_t, BANK_COUNT>                spsrBank{};

    std::array<uint32_t, 2> pipeline{};
    bool pipelineFlushed = false;

    std::FILE* traceFile = nullptr;
};
