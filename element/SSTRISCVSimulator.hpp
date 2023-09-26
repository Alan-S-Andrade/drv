#pragma once
#include <RV64IMInterpreter.hpp>
#include <sst/core/interfaces/stdMem.h>
#include <map>
#include "DrvAPIReadModifyWrite.hpp"
namespace SST {
namespace Drv {

class RISCVCore;
class RISCVSimHart;

/**
 * @brief a riscv simulator
 */
class RISCVSimulator : public RV64IMInterpreter {
public:
    /**
     * constructor
     */
    RISCVSimulator(RISCVCore *core): core_(core) {}

    /**
     * destructor
     */
    virtual ~RISCVSimulator() {}

    // load/stores
    void visitLB(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitLH(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitLW(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitLBU(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitLHU(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitLWU(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitLD(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitSB(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitSH(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitSW(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitSD(RISCVHart &hart, RISCVInstruction &instruction) override;

    // csr instructions
private:
    uint64_t visitCSRRWUnderMask(RISCVHart &hart, uint64_t csr, uint64_t wval, uint64_t mask);

public:
    void visitCSRRW(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitCSRRS(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitCSRRC(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitCSRRWI(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitCSRRSI(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitCSRRCI(RISCVHart &hart, RISCVInstruction &instruction) override;

    // atomics
private:
    template <typename T>
    void visitAMO(RISCVHart &hart, RISCVInstruction &i, DrvAPI::DrvAPIMemAtomicType op);

    void visitAMOSWAPW(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOSWAPW_RL(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOSWAPW_AQ(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOSWAPW_RL_AQ(RISCVHart &hart, RISCVInstruction &instruction) override;

    void visitAMOADDW(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOADDW_RL(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOADDW_AQ(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOADDW_RL_AQ(RISCVHart &hart, RISCVInstruction &instruction) override;
    
    void visitAMOSWAPD(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOSWAPD_RL(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOSWAPD_AQ(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOSWAPD_RL_AQ(RISCVHart &hart, RISCVInstruction &instruction) override;

    void visitAMOADDD(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOADDD_RL(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOADDD_AQ(RISCVHart &hart, RISCVInstruction &instruction) override;
    void visitAMOADDD_RL_AQ(RISCVHart &hart, RISCVInstruction &instruction) override;

    // environment calls
    void visitECALL(RISCVHart &hart, RISCVInstruction &instruction) override;
    
    RISCVCore *core_; //!< the riscv core component
    static constexpr uint64_t MMIO_SIZE       = 0xFFFF;
    static constexpr uint64_t MMIO_BASE       = 0xFFFFFFFFFFFF0000;
    static constexpr uint64_t MMIO_PRINT_INT  = MMIO_BASE + 0x0000;
    static constexpr uint64_t MMIO_PRINT_HEX  = MMIO_BASE + 0x0008;
    static constexpr uint64_t MMIO_PRINT_CHAR = MMIO_BASE + 0x0010;

    // CSRs
    static constexpr uint64_t CSR_MHARTID = 0xF14;
    static constexpr uint64_t CSR_MSTATUS = 0x300;
    
    
private:    
    template <typename R, typename T>
    void visitLoad(RISCVHart &hart, RISCVInstruction &instruction);

    template <typename T>
    void visitStore(RISCVHart &hart, RISCVInstruction &instruction);

    template <typename T>
    void visitStoreMMIO(RISCVHart &shart, RISCVInstruction &i);

    // TODO: implement this for malloc/free
    // or provide a different malloc/free implementation...
    void sysBRK(RISCVSimHart &shart, RISCVInstruction &i);

    void sysOPEN(RISCVSimHart &shart, RISCVInstruction &i);
    void sysWRITE(RISCVSimHart &shart, RISCVInstruction &i);
    void sysREAD(RISCVSimHart &shart, RISCVInstruction &i);
    void sysEXIT(RISCVSimHart &shart, RISCVInstruction &i);

    // TODO: implement these for stdio
    
    // void sysREADV(RISCVSimHart &shart, RISCVInstruction &i);
    // void sysWRITEV(RISCVSimHart &shart, RISCVInstruction &i);
    // void sysCLOSE(RISCVSimHart &shart, RISCVInstruction &i);
    // void sysFSTAT(RISCVSimHart &shart, RISCVInstruction &i);
    
    bool isMMIO(SST::Interfaces::StandardMem::Addr addr);

    std::map<uint64_t, int64_t> _pchist;
};

}
}
