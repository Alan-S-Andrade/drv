#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <sstream>
#include <type_traits>
#include "SSTRISCVSimulator.hpp"
#include "SSTRISCVCore.hpp"
#include "riscv64-unknown-elf/include/machine/syscall.h"
#include "DrvAPIReadModifyWrite.hpp"
#include "DrvCustomStdMem.hpp"

namespace SST {
namespace Drv {
using namespace SST::Interfaces;

bool RISCVSimulator::isMMIO(SST::Interfaces::StandardMem::Addr addr) {
    return (addr >= MMIO_BASE) && (addr < MMIO_BASE + MMIO_SIZE);
}

template <typename T>
void RISCVSimulator::visitStoreMMIO(RISCVHart &shart, RISCVInstruction &i) {
    StandardMem::Addr addr = shart.x(i.rs1()) + i.Simm();
    std::stringstream ss;
    switch (addr) {
    case MMIO_PRINT_INT:
        std::cout << static_cast<std::make_signed_t<T>>(shart.sx(i.rs2())) << std::endl;;
        break;
    case MMIO_PRINT_HEX:
        ss << "0x" << std::hex << std::setfill('0') << std::setw(sizeof(T)*2);
        ss << static_cast<std::make_unsigned_t<T>>(shart.x(i.rs2()));
        std::cout << ss.str() << std::endl;
        break;
    case MMIO_PRINT_CHAR:
        std::cout << static_cast<char>(shart.x(i.rs2()));
        break;
    default:
        core_->output_.fatal(CALL_INFO, -1, "Unknown MMIO address: 0x%lx\n", addr);
    }
    shart.pc() += 4;
}

template <typename R, typename T>
void RISCVSimulator::visitLoad(RISCVHart &hart, RISCVInstruction &i) {
   RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
   StandardMem::Addr addr = shart.x(i.rs1()) + i.SIimm();
   // create the read request
   StandardMem::Read *rd = new StandardMem::Read(addr, sizeof(T));
   rd->tid = core_->getHartId(shart);
   shart.ready() = false;
   auto ird = i.rd();
   RISCVCore::ICompletionHandler ch([&shart, ird](StandardMem::Request *req) {
       // handle the read response
       auto*rsp = static_cast<StandardMem::ReadResp *>(req);
       T *ptr = (T*)&rsp->data[0];
       shart.x(ird) = static_cast<R>(*ptr);
       shart.pc() += 4;
       shart.ready() = true;
       delete req;
   });
   core_->issueMemoryRequest(rd, rd->tid, ch);
}

template <typename T>
void RISCVSimulator::visitStore(RISCVHart &hart, RISCVInstruction &i) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    StandardMem::Addr addr = shart.x(i.rs1()) + i.Simm();
    if (isMMIO(addr)) {
        visitStoreMMIO<T>(shart, i);
        return;
    }
    // create the write request
    std::vector<uint8_t> data(sizeof(T));
    T *ptr = (T*)&data[0];
    *ptr = static_cast<T>(hart.x(i.rs2()));
    StandardMem::Write *wr = new StandardMem::Write(addr, sizeof(T), data);
    wr->tid = core_->getHartId(shart);    
    shart.ready() = false; // stores are blocking
    RISCVCore::ICompletionHandler ch([&shart](StandardMem::Request *req) {
        // handle the write response
        shart.pc() += 4;
        shart.ready() = true;
        delete req;
    });
    core_->issueMemoryRequest(wr, wr->tid, ch);
}

template <typename T>
void RISCVSimulator::visitAMO(RISCVHart &hart, RISCVInstruction &i, DrvAPI::DrvAPIMemAtomicType op) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    StandardMem::Addr addr = shart.x(i.rs1());
    AtomicReqData *data = new AtomicReqData();
    data->pAddr = addr;
    data->size = sizeof(T);
    data->wdata.resize(sizeof(T));
    data->opcode = op;
    *(T*)&data->wdata[0] = shart.x(i.rs2());
    StandardMem::CustomReq *req = new StandardMem::CustomReq(data);
    req->tid = core_->getHartId(shart);
    shart.ready() = false;
    int ird = i.rd();
    RISCVCore::ICompletionHandler ch([&shart, ird](StandardMem::Request *req) {
        // handle the atomic response
        auto *rsp = static_cast<StandardMem::CustomResp *>(req);
        auto *data = static_cast<AtomicReqData*>(rsp->data);        
        shart.x(ird) = *(T*)&data->rdata[0];
        shart.pc() += 4;
        shart.ready() = true;
        delete req;
    });
    core_->issueMemoryRequest(req, req->tid, ch);
}

void RISCVSimulator::visitLB(RISCVHart &hart, RISCVInstruction &i) {
    visitLoad<int64_t, int8_t>(hart, i);
}

void RISCVSimulator::visitLH(RISCVHart &hart, RISCVInstruction &i) {
    visitLoad<int64_t, int16_t>(hart, i);
}

void RISCVSimulator::visitLW(RISCVHart &hart, RISCVInstruction &i) {
    visitLoad<int64_t, int32_t>(hart, i);
}

void RISCVSimulator::visitLBU(RISCVHart &hart, RISCVInstruction &i) {
    visitLoad<uint64_t, uint8_t>(hart, i);
}

void RISCVSimulator::visitLHU(RISCVHart &hart, RISCVInstruction &i) {
    visitLoad<uint64_t, uint16_t>(hart, i);
}

void RISCVSimulator::visitLWU(RISCVHart &hart, RISCVInstruction &i) {
    visitLoad<uint64_t, uint32_t>(hart, i);
}

void RISCVSimulator::visitLD(RISCVHart &hart, RISCVInstruction &i) {
    visitLoad<uint64_t, uint64_t>(hart, i);
}

void RISCVSimulator::visitSB(RISCVHart &hart, RISCVInstruction &i) {
    visitStore<uint8_t>(hart, i);
}

void RISCVSimulator::visitSH(RISCVHart &hart, RISCVInstruction &i) {
    visitStore<uint16_t>(hart, i);
}

void RISCVSimulator::visitSW(RISCVHart &hart, RISCVInstruction &i) {
    visitStore<uint32_t>(hart, i);
}

void RISCVSimulator::visitSD(RISCVHart &hart, RISCVInstruction &i) {
    visitStore<uint64_t>(hart, i);
}

void RISCVSimulator::visitAMOSWAPW(RISCVHart &hart, RISCVInstruction &i) {    
    visitAMO<uint32_t>(hart, i, DrvAPI::DrvAPIMemAtomicSWAP);
}

void RISCVSimulator::visitAMOSWAPW_RL(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<uint32_t>(hart, i, DrvAPI::DrvAPIMemAtomicSWAP);
}

void RISCVSimulator::visitAMOSWAPW_AQ(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<uint32_t>(hart, i, DrvAPI::DrvAPIMemAtomicSWAP);
}

void RISCVSimulator::visitAMOSWAPW_RL_AQ(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<uint32_t>(hart, i, DrvAPI::DrvAPIMemAtomicSWAP);
}

void RISCVSimulator::visitAMOADDW(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<int32_t>(hart, i, DrvAPI::DrvAPIMemAtomicADD);
}

void RISCVSimulator::visitAMOADDW_RL(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<int32_t>(hart, i, DrvAPI::DrvAPIMemAtomicADD);
}

void RISCVSimulator::visitAMOADDW_AQ(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<int32_t>(hart, i, DrvAPI::DrvAPIMemAtomicADD);
}

void RISCVSimulator::visitAMOADDW_RL_AQ(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<int32_t>(hart, i, DrvAPI::DrvAPIMemAtomicADD);
}

void RISCVSimulator::visitAMOSWAPD(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<uint64_t>(hart, i, DrvAPI::DrvAPIMemAtomicSWAP);
}

void RISCVSimulator::visitAMOSWAPD_RL(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<uint64_t>(hart, i, DrvAPI::DrvAPIMemAtomicSWAP);
}

void RISCVSimulator::visitAMOSWAPD_AQ(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<uint64_t>(hart, i, DrvAPI::DrvAPIMemAtomicSWAP);
}

void RISCVSimulator::visitAMOSWAPD_RL_AQ(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<uint64_t>(hart, i, DrvAPI::DrvAPIMemAtomicSWAP);
}

void RISCVSimulator::visitAMOADDD(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<int64_t>(hart, i, DrvAPI::DrvAPIMemAtomicADD);
}

void RISCVSimulator::visitAMOADDD_RL(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<int64_t>(hart, i, DrvAPI::DrvAPIMemAtomicADD);
}

void RISCVSimulator::visitAMOADDD_AQ(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<int64_t>(hart, i, DrvAPI::DrvAPIMemAtomicADD);
}

void RISCVSimulator::visitAMOADDD_RL_AQ(RISCVHart &hart, RISCVInstruction &i) {
    visitAMO<int64_t>(hart, i, DrvAPI::DrvAPIMemAtomicADD);
}

/////////
// CSR //
/////////
uint64_t RISCVSimulator::visitCSRRWUnderMask(RISCVHart &hart, uint64_t csr, uint64_t wval, uint64_t mask) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    uint64_t rval = 0;
    switch (csr) {
    case CSR_MHARTID: // read-only
        rval = core_->getHartId(shart);
        break;
    }
    return rval;
}

void RISCVSimulator::visitCSRRW(RISCVHart &hart, RISCVInstruction &i) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    uint64_t csr = i.Iimm();
    uint64_t wval = shart.x(i.rs1());
    uint64_t rval = visitCSRRWUnderMask(shart, csr, wval, 0xFFFFFFFFFFFFFFFF);
    shart.x(i.rd()) = rval;
    shart.pc() += 4;
}

void RISCVSimulator::visitCSRRS(RISCVHart &hart, RISCVInstruction &i) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    uint64_t csr = i.Iimm();
    uint64_t wval = shart.x(i.rs1());
    uint64_t rval = visitCSRRWUnderMask(shart, csr, 0xFFFFFFFFFFFFFFFF, wval);
    shart.x(i.rd()) = rval;
    shart.pc() += 4;
}

void RISCVSimulator::visitCSRRC(RISCVHart &shart, RISCVInstruction &i) {
    uint64_t csr = i.Iimm();
    uint64_t wval = shart.x(i.rs1());
    uint64_t rval = visitCSRRWUnderMask(shart, csr, 0x0000000000000000, wval);
    shart.x(i.rd()) = rval;
    shart.pc() += 4;
}

void RISCVSimulator::visitCSRRWI(RISCVHart &hart, RISCVInstruction &i) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    uint64_t csr = i.Iimm();
    uint64_t wval = i.rs1();
    uint64_t rval = visitCSRRWUnderMask(shart, csr, wval, 0xFFFFFFFFFFFFFFFF);
    shart.x(i.rd()) = rval;
    shart.pc() += 4;
}

void RISCVSimulator::visitCSRRSI(RISCVHart &hart, RISCVInstruction &i) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    uint64_t csr = i.Iimm();
    uint64_t wval = i.rs1();
    uint64_t rval = visitCSRRWUnderMask(shart, csr, 0xFFFFFFFFFFFFFFFF, wval);
    shart.x(i.rd()) = rval;
    shart.pc() += 4;
}

void RISCVSimulator::visitCSRRCI(RISCVHart &hart, RISCVInstruction &i) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    uint64_t csr = i.Iimm();
    uint64_t wval = i.rs1();
    uint64_t rval = visitCSRRWUnderMask(shart, csr, 0x0000000000000000, wval);
    shart.x(i.rd()) = rval;
    shart.pc() += 4;
}

//////////////////
// system calls //
//////////////////

void RISCVSimulator::sysWRITE(RISCVSimHart &hart, RISCVInstruction &i) {
    int fd = hart.sa(0);
    uint64_t buf = hart.a(1);
    uint64_t len = hart.a(2);
    core_->output_.verbose(CALL_INFO, 2, 0, "WRITE: fd=%d, buf=%#lx, len=%lu\n", fd, buf, len);
    // issue a request for the buffer
    // then call write when buffer returns
    hart.ready() = false;
    RISCVCore::ICompletionHandler ch([&hart, fd, len](StandardMem::Request *req) {
        // handle the write response
        auto *rsp = static_cast<StandardMem::ReadResp *>(req);
        hart.ready() = true;
        hart.a(0) = write(fd, &rsp->data[0], len);
        delete rsp;
    });
    auto rd = new StandardMem::Read(buf, len);
    rd->tid = core_->getHartId(hart);
    core_->issueMemoryRequest(rd, rd->tid, ch);
}

void RISCVSimulator::sysREAD(RISCVSimHart &shart, RISCVInstruction &i) {
    int fd = shart.sa(0);
    uint64_t buf = shart.a(1);
    uint64_t len = shart.a(2);
    core_->output_.verbose(CALL_INFO, 2, 0, "READ: fd=%d, buf=%#lx, len=%lu\n", fd, buf, len);
    // call read on a simulation space buffer
    std::vector<uint8_t> data(len);
    shart.a(0) = read(fd, &data[0], len);
    // issue a write request to the userspace buffer
    shart.ready() = false;
    RISCVCore::ICompletionHandler ch([&shart](StandardMem::Request *req) {
        // handle the write response
        shart.ready() = true;
        delete req;
    });
    auto wr = new StandardMem::Write(buf, len, data);
    wr->tid = core_->getHartId(shart);
    core_->issueMemoryRequest(wr, wr->tid, ch);
}

void RISCVSimulator::sysBRK(RISCVSimHart &shart, RISCVInstruction &i) {
    uint64_t addr = shart.a(0);
    core_->output_.verbose(CALL_INFO, 2, 0, "BRK: addr=%#lx\n", addr);
    shart.a(0) = -1;
}

void RISCVSimulator::sysEXIT(RISCVSimHart &shart, RISCVInstruction &i) {
    shart.ready() = false;
    shart.exit() = true;
}

void RISCVSimulator::sysFSTAT(RISCVSimHart &shart, RISCVInstruction &i) {
    int fd = shart.sa(0);
    uint64_t stat_buf = shart.a(1);
    core_->output_.verbose(CALL_INFO, 2, 0, "FSTAT: fd=%d, stat_buf=%#lx\n", fd, stat_buf);    
    struct stat stat_s;
    int r = fstat(fd, &stat_s);
    std::vector<unsigned char> sim_stat_s = _type_translator.nativeToSimulator_stat(&stat_s);
    // set the return value
    shart.a(0) = r;
    // issue a write request
    shart.ready() = false;
    RISCVCore::ICompletionHandler ch([&shart, sim_stat_s](StandardMem::Request *req) {
        // handle the write response
        shart.ready() = true;
        delete req;
    });
    auto wr = new StandardMem::Write(stat_buf, sim_stat_s.size(), sim_stat_s);
    wr->tid = core_->getHartId(shart);
    core_->issueMemoryRequest(wr, wr->tid, ch);
}

void RISCVSimulator::sysOPEN(RISCVSimHart &shart, RISCVInstruction &i) {
    uint64_t path = shart.a(0);

    // TODO: these flags need to be translated
    // to native flags for the running host
    int32_t flags = shart.a(1);
    mode_t mode = static_cast<mode_t>(shart.a(2));

    // issue a read request for the filename
    auto rd = new StandardMem::Read(path, 1024);
    rd->tid = core_->getHartId(shart);
    // make a handler
    shart.ready() = false;
    RISCVCore::ICompletionHandler ch([&shart, this, flags, mode](StandardMem::Request *req) {
        auto *rsp = static_cast<StandardMem::ReadResp *>(req);
        char *path = (char *)&rsp->data[0];
        int32_t my_flags = _type_translator.simulatorToNative_openflags(flags);
        mode_t my_mode = 0644;
        core_->output_.verbose(CALL_INFO, 2, 0
                               , "OPEN: path=%s, flags=%lx (my_flags=%lx), mode=%ld (my_mode=%lx)\n"
                               , path, flags, my_flags, mode, my_mode);
        // handle the read response
        shart.a(0) = open((const char *)&rsp->data[0], my_flags, my_mode);
        delete rsp;
        shart.ready() = true;
    });
    core_->issueMemoryRequest(rd, rd->tid, ch);
}

void RISCVSimulator::sysCLOSE(RISCVSimHart &shart, RISCVInstruction &i) {
    int fd = shart.sa(0);
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO && fd != STDIN_FILENO) {
        core_->output_.verbose(CALL_INFO, 2, 0, "CLOSE: fd=%d\n", fd);    
        shart.a(0) = close(fd);
    } else {
        shart.a(0) = 0;
    }
}

void RISCVSimulator::visitECALL(RISCVHart &hart, RISCVInstruction &i) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    switch(shart.a(7)) {
    case SYS_exit:
        sysEXIT(shart, i);
        break;
    case SYS_brk:
        sysBRK(shart, i);
        break;
    case SYS_write:
        sysWRITE(shart, i);
        break;
    case SYS_read:
        sysREAD(shart, i);
        break;
    case SYS_fstat:
        sysFSTAT(shart, i);
        break;
    case SYS_close:
        sysCLOSE(shart, i);
        break;
    case SYS_open:
        sysOPEN(shart, i);
        break;
    default:
        core_->output_.fatal(CALL_INFO, -1, "Unknown ECALL %lu\n", (unsigned long)shart.a(7));
    }
    hart.pc() += 4;
}

}
}
