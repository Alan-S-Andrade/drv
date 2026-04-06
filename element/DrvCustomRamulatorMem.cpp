// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <DrvAPIReadModifyWrite.hpp>
#include "DrvCustomStdMem.hpp"
#include "DrvCustomRamulatorMem.hpp"
#include "Request.h" // ramulator
#include "StatType.h" // ramulator stats

using namespace SST;
using namespace Drv;
using namespace Interfaces;
using namespace MemHierarchy;

/**
 * constructor of our ramulator backend
 */
DrvRamulatorMemBackend::DrvRamulatorMemBackend(ComponentId_t id, Params &params)
    : ramulatorMemory(id, params) {
    int verbose_level = params.find<int>("verbose_level", 0);
    output_ = SST::Output("[@f:@l:@p]: ", verbose_level, 0, SST::Output::STDOUT);
    output_.verbose(CALL_INFO, 1, 0, "%s\n", __PRETTY_FUNCTION__);
    alu_latency_ = params.find<int>("alu_latency", 0);
    if (alu_latency_ > 0) {
        output_.output(CALL_INFO, "Near-cache ALU enabled: atomic latency = %d cycles\n", alu_latency_);
    }
}

/**
 * destructor
 */
DrvRamulatorMemBackend::~DrvRamulatorMemBackend() {
    output_.verbose(CALL_INFO, 1, 0, "%s\n", __PRETTY_FUNCTION__);
}

/**
 * handle custom requests for drv components
 */
bool DrvRamulatorMemBackend::issueCustomRequest(ReqId req_id, Interfaces::StandardMem::CustomData *data) {
    output_.verbose(CALL_INFO, 1, 0, "%s\n", __PRETTY_FUNCTION__);
    AtomicReqData *atomic_data = dynamic_cast<AtomicReqData*>(data);
    if (atomic_data) {
        output_.verbose(CALL_INFO, 1, 0, "Received atomic request\n");

        if (alu_latency_ > 0 && atomic_data->shootdown_occurred_) {
            // Near-cache ALU mode: shootdown already fetched the data,
            // so skip Ramulator timing and complete with fixed ALU latency
            alu_queue_.push_back({alu_latency_, req_id});
            return true;
        }

        // Default: model the atomic as a read through Ramulator
        Addr addr = atomic_data->pAddr;
        ramulator::Request request
            (addr
             ,ramulator::Request::Type::READ
             ,callBackFunc
             ,0 // context or core id
             );
        bool ok = memSystem->send(request);
        if (!ok) return false;
        dramReqs[addr].push_back(req_id);
        return true;
    }
    output_.fatal(CALL_INFO, -1, "Error: unknown custom request type\n");
    return false;
}

/**
 * clock handler - tick Ramulator and drain ALU queue
 */
bool DrvRamulatorMemBackend::clock(Cycle_t cycle) {
    // Tick Ramulator (processes normal reads/writes and pending DRAM requests)
    ramulatorMemory::clock(cycle);

    // Drain ALU queue: decrement counters, complete when ready
    auto it = alu_queue_.begin();
    while (it != alu_queue_.end()) {
        it->first--;
        if (it->first <= 0) {
            handleMemResponse(it->second);
            it = alu_queue_.erase(it);
        } else {
            ++it;
        }
    }

    return false;
}

/**
 * finish - dump ramulator internal stats to file
 */
void DrvRamulatorMemBackend::finish() {
    // call parent finish which computes ramulator's final stats
    ramulatorMemory::finish();

    // derive stats filename from parent component name
    std::string parent_name = getParentComponentName();
    std::string base_name = parent_name;
    const std::string suffix = "_memctrl";
    if (base_name.size() >= suffix.size() &&
        base_name.compare(base_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        base_name = base_name.substr(0, base_name.size() - suffix.size());
    }
    std::string stats_file = "ramulator_" + base_name + ".stats";

    Stats::statlist.output(stats_file);
    Stats::statlist.printall();
    Stats::statlist.finish();

    output_.output(CALL_INFO, "Ramulator stats written to %s\n", stats_file.c_str());
}
