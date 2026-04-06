// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#pragma once
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/elements/memHierarchy/memEvent.h>
#include <sst/elements/memHierarchy/memEventBase.h>
#include <sst/elements/memHierarchy/memEventCustom.h>
#include <sst/elements/memHierarchy/memTypes.h>
#include <sst/elements/memHierarchy/customcmd/customCmdMemory.h>
#include <sst/elements/memHierarchy/membackend/simpleMemBackend.h>
#include <sst/elements/memHierarchy/membackend/ramulatorBackend.h>
#include "DrvAPIReadModifyWrite.hpp"
#include "DrvAPIThreadState.hpp"
#include "DrvCustomStdMem.hpp"
#include <deque>
#include <utility>

namespace SST {
namespace Drv {

/**
 * @brief our specialized ramulator memory backend
 *
 * Supports an optional near-cache ALU mode: when alu_latency > 0,
 * custom requests (atomics) complete with a fixed cycle latency
 * instead of going through Ramulator's HBM timing model.
 * This models a hardware ALU co-located with the DRAM cache.
 */
class DrvRamulatorMemBackend : public SST::MemHierarchy::ramulatorMemory {
public:
  /* Element library info */
  SST_ELI_REGISTER_SUBCOMPONENT(DrvRamulatorMemBackend, "Drv", "DrvRamulatorMemBackend", SST_ELI_ELEMENT_VERSION(1,0,0),
                                "Custom ramulator memory backend for drv element", SST::Drv::DrvRamulatorMemBackend)
  /* parameters */
  SST_ELI_DOCUMENT_PARAMS(
                          {"verbose_level", "Sets the verbosity of the backend output", "0"},
                          {"alu_latency", "Fixed cycle latency for atomic ALU (0=disabled, use Ramulator timing)", "0"}
                          )

  /* constructor */
  DrvRamulatorMemBackend(ComponentId_t id, Params &params);

  /* destructor */
  ~DrvRamulatorMemBackend() override;

  bool issueCustomRequest(ReqId, Interfaces::StandardMem::CustomData *) override;

  bool clock(Cycle_t cycle) override;

  void finish() override;

private:
    SST::Output output_;
    int alu_latency_;
    std::deque<std::pair<int, ReqId>> alu_queue_;
};

}
}
