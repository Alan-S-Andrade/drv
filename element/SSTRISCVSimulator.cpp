#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <sstream>
#include <type_traits>
#include "SSTRISCVSimulator.hpp"
#include "SSTRISCVCore.hpp"
#include "riscv64-unknown-elfpandodrvsim/include/machine/syscall.h"
#include "DrvAPIReadModifyWrite.hpp"
#include "DrvCustomStdMem.hpp"

namespace SST {
namespace Drv {
using namespace SST::Interfaces;

bool RISCVSimulator::isMMIO(SST::Interfaces::StandardMem::Addr addr) {
    return (addr >= MMIO_BASE) && (addr < MMIO_BASE + MMIO_SIZE);
}

template <typename T>
void RISCVSimulator::visitStoreMMIO(RISCVHart &hart, RISCVInstruction &i) {
    RISCVSimHart &shart = static_cast<RISCVSimHart &>(hart);
    StandardMem::Addr addr = shart.x(i.rs1()) + i.Simm();
    std::stringstream ss;
    switch (addr) {
    case MMIO_PRINT_INT:
        ss << "PXN: " << std::setw(3) << core_->getPXNId() << " ";
        ss << "POD: " << std::setw(2) << core_->getPodId() << " ";
        ss << "CORE: " << std::setw(3) << core_->getCoreId() << " ";
        ss << "THREAD: " << std::setw(2) << core_->getHartId(shart) << " ";
        ss << ": " << static_cast<std::make_signed_t<T>>(shart.sx(i.rs2()));
        std::cout << ss.str() << std::endl;
        break;
    case MMIO_PRINT_HEX:
        ss << "PXN: " << std::setw(3) << core_->getPXNId() << " ";
        ss << "POD: " << std::setw(2) << core_->getPodId() << " ";
        ss << "CORE: " << std::setw(3) << core_->getCoreId() << " ";
        ss << "THREAD: " << std::setw(2) << core_->getHartId(shart) << " ";
        ss << ": 0x" << std::hex << std::setfill('0') << std::setw(sizeof(T)*2);
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
   core_->output_.verbose(CALL_INFO, 0, RISCVCore::DEBUG_MEMORY
                           ,"PC=%08" PRIx64 ": LOAD: 0x%016" PRIx64 "\n"
                           ,static_cast<uint64_t>(shart.pc())
                           ,static_cast<uint64_t>(addr));
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
    core_->output_.verbose(CALL_INFO, 0, RISCVCore::DEBUG_MEMORY
                            ,"PC=%08" PRIx64 ": STORE: 0x%016" PRIx64 "\n"
                            ,static_cast<uint64_t>(shart.pc())
                            ,static_cast<uint64_t>(addr));
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
    case CSR_MCOREID: // read-only
        rval = core_->getCoreId();
        break;
    case CSR_MPODID: // read-only
        rval = core_->getPodId();
        break;
    case CSR_MPXNID: // read-only
        rval = core_->getPXNId();
        break;
    case CSR_MCOREHARTS: // read-only
        rval = core_->numHarts();
        break;
    case CSR_MPODCORES: // read-only
        rval = core_->sys().numPodCores();
        break;
    case CSR_MPXNPODS: // read-only
        rval = core_->sys().numPXNPods();
        break;
    case CSR_MNUMPXN: // read-only
        rval = core_->sys().numPXN();
        break;
    default:
        core_->output_.fatal(CALL_INFO, -1, "CSR %" PRIx64 " is not implemented", csr);
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

    std::function<void(std::vector<uint8_t>&)> completion
        ([this, buf, &hart, fd, len](std::vector<uint8_t> &data) {
            this->core_->output_.verbose(CALL_INFO, 1, RISCVCore::DEBUG_SYSCALLS, "WRITE: fd=%d, buf=%#lx, len=%lu\n", fd, buf, len);
            hart.ready() = true;
            hart.a(0) = write(fd, &data[0], len);
        });

    // issue a request for the buffer
    // then call write when buffer returns
    hart.ready() = false;
    sysReadBuffer(hart, buf, len, std::move(completion));
}

void RISCVSimulator::sysREAD(RISCVSimHart &shart, RISCVInstruction &i) {
    int fd = shart.sa(0);
    uint64_t buf = shart.a(1);
    uint64_t len = shart.a(2);
    core_->output_.verbose(CALL_INFO, 1, RISCVCore::DEBUG_SYSCALLS, "READ: fd=%d, buf=%#lx, len=%lu\n", fd, buf, len);
    // call read on a simulation space buffer
    std::vector<uint8_t> data(len);
    shart.a(0) = read(fd, &data[0], len);

    // issue a write request to the userspace buffer
    std::function<void(void)> completion
        ([&shart](void) {
            shart.ready() = true;
        });

    shart.ready() = false;
    sysWriteBuffer(shart, buf, data, std::move(completion));
}

void RISCVSimulator::sysBRK(RISCVSimHart &shart, RISCVInstruction &i) {
    uint64_t addr = shart.a(0);
    core_->output_.verbose(CALL_INFO, 1, RISCVCore::DEBUG_SYSCALLS, "BRK: addr=%#lx\n", addr);
    shart.a(0) = -1;
}

void RISCVSimulator::sysEXIT(RISCVSimHart &shart, RISCVInstruction &i) {
    shart.ready() = false;
    shart.exit() = true;
}

void RISCVSimulator::sysFSTAT(RISCVSimHart &shart, RISCVInstruction &i) {
    int fd = shart.sa(0);
    uint64_t stat_buf = shart.a(1);
    core_->output_.verbose(CALL_INFO, 1, RISCVCore::DEBUG_SYSCALLS, "FSTAT: fd=%d, stat_buf=%#lx\n", fd, stat_buf);
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
    int32_t flags = _type_translator.simulatorToNative_openflags(shart.a(1));

    std::function<void(std::vector<uint8_t>&)> completion
        ([&shart, this, flags](std::vector<uint8_t> &data) {
            mode_t mode = 0644;
            char *path = (char *)&data[0];
            core_->output_.verbose(CALL_INFO, 1, RISCVCore::DEBUG_SYSCALLS
                                   ,"OPEN: path=%s, flags=%" PRIx32 ", mode=%u\n"
                                   ,path, flags, mode);
            if (strnlen(path, data.size()) == data.size()) {
                // no null terminator found
                core_->output_.fatal(CALL_INFO, -1, "OPEN: file name too long\n");
            }
            shart.a(0) = open(path, flags, mode);
            shart.ready() = true;
        });
    shart.ready() = false;

    // issue the read requests
    sysReadBuffer(shart, path, 1024, std::move(completion));
}

/**
 * Write an arbitrarily large buffer to the simulator's memory
 */
void RISCVSimulator::sysWriteBuffer(RISCVSimHart &shart, StandardMem::Addr paddr, std::vector<uint8_t> &data, std::function<void(void)> && cont) {
    // create a large request handler
    size_t reqSz = core_->getMaxReqSize();
    size_t nReqs = (data.size() + reqSz - 1)/ reqSz;
    std::shared_ptr<LargeWriteHandler> handler(new LargeWriteHandler(nReqs, std::move(cont)));

    // create a completion handler for when small requests return
    RISCVCore::ICompletionHandler ch([handler](StandardMem::Request *req) {
        handler->recvRsp(req);
    });

    for (size_t i = 0; i < nReqs; ++i) {
        size_t sz = std::min(data.size() - i * reqSz, reqSz);
        std::vector<uint8_t> wdata(data.begin() + i * reqSz, data.begin() + i * reqSz + sz);
        auto wr = new StandardMem::Write(paddr + i * reqSz, sz, wdata);
        wr->tid = core_->getHartId(shart);
        core_->issueMemoryRequest(wr, wr->tid, ch);
    }
}

/**
 * Read an arbitrarily large buffer from the simulator's memory
 */
void RISCVSimulator::sysReadBuffer(RISCVSimHart &shart, StandardMem::Addr paddr, size_t n, std::function<void(std::vector<uint8_t>&)> && cont) {
    // create a large request handler
    size_t reqSz = core_->getMaxReqSize();
    size_t nReqs = (n + reqSz - 1)/ reqSz;
    std::shared_ptr<LargeReadHandler> handler(new LargeReadHandler(nReqs, std::move(cont)));

    // create a completion handler for when small requests return
    RISCVCore::ICompletionHandler ch([handler](StandardMem::Request *req) {
        handler->recvRsp(req);
    });

    for (size_t i = 0; i < nReqs; ++i) {
        size_t sz = std::min(n - i * reqSz, reqSz);
        auto rd = new StandardMem::Read(paddr + i * reqSz, sz);
        rd->tid = core_->getHartId(shart);
        core_->issueMemoryRequest(rd, rd->tid, ch);
    }
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
