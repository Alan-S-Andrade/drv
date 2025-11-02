// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#ifndef RISCVINSTRUCTIONBASE_HPP
#define RISCVINSTRUCTIONBASE_HPP
#include <cstdint>
#include <string>
#include <RISCVInstructionId.hpp>
#include <RISCVRegisterIndices.h>
class RISCVInterpreter;

/**
 * @brief Base class for all RISCV instructions.
 */
class RISCVInstruction {
public:
    RISCVInstruction(uint32_t instruction) : instruction_(instruction) {}
    virtual ~RISCVInstruction() {}
    virtual void accept(RISCVHart &hart, RISCVInterpreter &interpreter) = 0;
    virtual const char* getMnemonic() const = 0;
    virtual RISCVInstructionId getInstructionId() const = 0;
    uint32_t rs1() const { return (instruction_ >> 15) & 0x1F; }
    uint32_t rs2() const { return (instruction_ >> 20) & 0x1F; }
    uint32_t rs3() const { return (instruction_ >> 27) & 0x1F; }
    uint32_t rd()  const { return (instruction_ >> 7) & 0x1F; }
    uint32_t Iimm() const { return (instruction_ >> 20); }
    int32_t SIimm() const {
        int32_t sinstruction = instruction_;
        return (sinstruction >> 20);
    }
    int32_t Simm() const {
        int32_t sinstruction = instruction_;
        return ((sinstruction >> 25) << 5)
            |  ((sinstruction >> 7) & 0x1F);
    }
    int32_t Bimm() const {
        int32_t sinstruction = instruction_;
        return ((sinstruction >> 31) << 12)
            | ((sinstruction >> 7) & 0x1)<<11
            | ((sinstruction >> 25) & 0x3F)<<5
            | ((sinstruction >> 8) & 0xF) << 1;
    }
    uint32_t Uimm() const { return instruction_ & 0xFFFFF000; }
    int32_t SUimm() const { return instruction_ & 0xFFFFF000; }
    int32_t  Jimm() const {
        int32_t sinstruction = instruction_;
        return  ((sinstruction >> 31) << 20)
            |   ((sinstruction >> 21) & 0x3FF) << 1
            |   ((sinstruction >> 20) & 0x1) << 11
            |   ((sinstruction >> 12) & 0xFF) << 12;
    }
    uint32_t shamt()  const { return (instruction_ >> 20) & 0x1F; }
    uint32_t shamt5() const { return shamt(); }
    uint32_t shamt6() const { return (instruction_ >> 20) & 0x3F; }
    uint32_t instruction() const { return instruction_; }
    bool uses_rs1()  const { return uses_ & _RS1_; }
    bool uses_rs2()  const { return uses_ & _RS2_; }
    bool uses_rs3()  const { return uses_ & _RS3_; }
    bool uses_rd()   const { return uses_ & _RD_; }
    bool uses_frs1() const { return uses_ & _FRS1_; }
    bool uses_frs2() const { return uses_ & _FRS2_; }
    bool uses_frs3() const { return uses_ & _FRS3_; }
    bool uses_frd()  const { return uses_ & _FRD_; }
    uint32_t instruction_;
    uint32_t uses_;
};

#endif
