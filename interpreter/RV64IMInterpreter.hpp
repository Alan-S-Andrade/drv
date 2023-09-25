#ifndef RV64IMINTERPRETER_HPP
#define RV64IMINTERPRETER_HPP
#include "RISCVInstruction.hpp"
#include "RISCVHart.hpp"
#include "RV64IInterpreter.hpp"
#include <stdexcept>
#include <boost/multiprecision/cpp_int.hpp>
class RV64IMInterpreter : public RV64IInterpreter
{
public:
    RV64IMInterpreter(){}        
    using int128_t = boost::multiprecision::int128_t;
    using uint128_t = boost::multiprecision::uint128_t;
    void visitMUL(RISCVHart &hart, RISCVInstruction &i) override {
        int128_t rs1 = static_cast<int64_t>(hart.sx(i.rs1()));
        int128_t rs2 = static_cast<int64_t>(hart.sx(i.rs2()));
        int128_t rd = rs1 * rs2;
        hart.x(i.rd()) = static_cast<int64_t>(rd);
        hart.pc() += 4;
    }
    void visitMULH(RISCVHart &hart, RISCVInstruction &i) override {
        int128_t rs1 = static_cast<int64_t>(hart.sx(i.rs1()));
        int128_t rs2 = static_cast<int64_t>(hart.sx(i.rs2()));
        int128_t rd = rs1 * rs2;
        hart.x(i.rd()) = static_cast<int64_t>(rd >> 64);
        hart.pc() += 4;
    }
    void visitMULHU(RISCVHart &hart, RISCVInstruction &i) override {
        uint128_t rs1 = static_cast<uint64_t>(hart.x(i.rs1()));
        uint128_t rs2 = static_cast<uint64_t>(hart.x(i.rs2()));
        uint128_t rd = rs1 * rs2;
        hart.x(i.rd()) = static_cast<uint64_t>(rd >> 64);
        hart.pc() += 4;
    }
    void visitMULHSU(RISCVHart &hart, RISCVInstruction &i) override {
        int128_t rs1 = static_cast<int64_t>(hart.sx(i.rs1()));
        uint128_t rs2 = static_cast<uint64_t>(hart.x(i.rs2()));
        int128_t rd = rs1 * rs2;
        hart.x(i.rd()) = static_cast<int64_t>(rd >> 64);
        hart.pc() += 4;
    }
    void visitDIV(RISCVHart &hart, RISCVInstruction &i) override {
        int64_t rs1 = hart.sx(i.rs1());
        int64_t rs2 = hart.sx(i.rs2());
        if (rs2 == 0) {
            throw std::runtime_error("DIV: division by zero");
        } else {
            hart.x(i.rd()) = rs1 / rs2;
        }
        hart.pc() += 4;
    }
    void visitDIVU(RISCVHart &hart, RISCVInstruction &i) override {
        uint64_t rs1 = hart.x(i.rs1());
        uint64_t rs2 = hart.x(i.rs2());
        if (rs2 == 0) {
            throw std::runtime_error("DIVU: division by zero");
        } else {
            hart.x(i.rd()) = rs1 / rs2;
        }
        hart.pc() += 4;
    }
    void visitREM(RISCVHart &hart, RISCVInstruction &i) override {
        int64_t rs1 = hart.sx(i.rs1());
        int64_t rs2 = hart.sx(i.rs2());
        if (rs2 == 0) {
            throw std::runtime_error("REM: division by zero");
        } else {
            hart.x(i.rd()) = rs1 % rs2;
        }
        hart.pc() += 4;
    }
    void visitREMU(RISCVHart &hart, RISCVInstruction &i) override {
        uint64_t rs1 = hart.x(i.rs1());
        uint64_t rs2 = hart.x(i.rs2());
        if (rs2 == 0) {
            throw std::runtime_error("REMU: division by zero");
        } else {
            hart.x(i.rd()) = rs1 % rs2;
        }
        hart.pc() += 4;
    }
    void visitMULW(RISCVHart &hart, RISCVInstruction &i) override {
        int32_t rs1 = hart.sx(i.rs1());
        int32_t rs2 = hart.sx(i.rs2());
        int64_t rd = rs1 * rs2;
        hart.x(i.rd()) = rd;
        hart.pc() += 4;
    }
    void visitDIVW(RISCVHart &hart, RISCVInstruction &i) override {
        int32_t rs1 = hart.sx(i.rs1());
        int32_t rs2 = hart.sx(i.rs2());
        if (rs2 == 0) {
            throw std::runtime_error("DIVW: division by zero");
        } else {
            hart.x(i.rd()) = rs1 / rs2;
        }
        hart.pc() += 4;
    }
    void visitDIVUW(RISCVHart &hart, RISCVInstruction &i) override {
        uint32_t rs1 = hart.x(i.rs1());
        uint32_t rs2 = hart.x(i.rs2());
        if (rs2 == 0) {
            throw std::runtime_error("DIVUW: division by zero");
        } else {
            hart.x(i.rd()) = rs1 / rs2;
        }
        hart.pc() += 4;
    }
    void visitREMW(RISCVHart &hart, RISCVInstruction &i) override {
        int32_t rs1 = hart.sx(i.rs1());
        int32_t rs2 = hart.sx(i.rs2());
        if (rs2 == 0) {
            throw std::runtime_error("REMW: division by zero");
        } else {
            hart.x(i.rd()) = rs1 % rs2;
        }
        hart.pc() += 4;
    }
    void visitREMUW(RISCVHart &hart, RISCVInstruction &i) override {
        int32_t rs1 = hart.sx(i.rs1());
        int32_t rs2 = hart.sx(i.rs2());
        if (rs2 == 0) {
            throw std::runtime_error("REMW: division by zero");
        } else {
            hart.x(i.rd()) = rs1 % rs2;
        }
        hart.pc() += 4;        
    }
};
#endif
