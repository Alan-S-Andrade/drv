// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#pragma once
#include <DrvAPISysConfig.hpp>
#include <sst/core/component.h>

namespace SST
{
namespace Drv
{
class DrvSysConfig
{
public:
    /**
     * Constructor
     */
    DrvSysConfig() {}

#define DRV_SYS_CONFIG_PARAMETERS                                       \
    {"sys_num_pxn", "Number of PXN in system", "1"},                    \
    {"sys_pxn_pods", "Number of pods per PXN", "1"},                    \
    {"sys_pod_cores_x", "Number of core columns per pod", "1"},         \
    {"sys_pod_cores_y", "Number of core rows per pod", "1"},            \
    {"sys_core_threads", "Number of threads per core", "16"},                  \
    {"sys_nw_flit_dwords", "Number of dwords in a flit", "1"},          \
    {"sys_nw_obuf_dwords", "Number of dwords in an output buffer", "1"}, \
    {"sys_core_l1sp_size", "Size of core l1 scratchpad in bytes", "131072"}, \
    {"sys_pod_l2sp_size", "Size of pod l2 scratchpad", "16777216"},     \
    {"sys_pxn_dram_size", "Size of pxn dram", "1073741824"}, \
    {"sys_pxn_dram_cache_sets", "Number of sets in a cache bank", "0"}, \
    {"sys_pxn_dram_cache_ways", "Number of ways in a cache bank", "0"}, \
    {"sys_pxn_dram_cache_line_size", "Size of a cache line in bytes", "64"}, \
    {"sys_pxn_dram_ports", "Number of DRAM ports per PXN", "1"},     \
    {"sys_pxn_dram_cache_banks", "Number of DRAM cache banks per PXN", "0"}, \
    {"sys_pxn_dram_interleave_size", "Size of the address interleave for DRAM", "64"}, \
    {"sys_pod_l2sp_banks", "Number of L2SP banks per pod", "1"},        \
    {"sys_pod_l2sp_interleave_size", "Size of the address interleave for L2SP", "64"},

    /**
     * initialize the system configuration
     */
    void init(Params &params) {
        data_.num_pxn_ = params.find<int64_t>("sys_num_pxn", 1);
        data_.pxn_pods_ = params.find<int64_t>("sys_pxn_pods", 1);
        data_.pod_cores_x_ = params.find<int64_t>("sys_pod_cores_x", 1);
        data_.pod_cores_y_ = params.find<int64_t>("sys_pod_cores_y", 1);
        data_.core_threads_ = params.find<int64_t>("sys_core_threads", 16);
        data_.nw_flit_dwords_ = params.find<int16_t>("sys_nw_flit_dwords", 1);
        data_.nw_obuf_dwords_ = params.find<int16_t>("sys_nw_obuf_dwords", 1);
        data_.core_l1sp_size_ = params.find<int64_t>("sys_core_l1sp_size", 131072);
        data_.pod_l2sp_size_ = params.find<int64_t>("sys_pod_l2sp_size", 16777216);
        data_.pxn_dram_size_ = params.find<int64_t>("sys_pxn_dram_size", 1073741824);
        data_.pxn_dram_ports_ = params.find<int16_t>("sys_pxn_dram_ports", 1);
        data_.pxn_dram_cache_banks_ = params.find<int32_t>("sys_pxn_dram_cache_banks", 0);
        data_.pxn_dram_cache_sets_ = params.find<int32_t>("sys_pxn_dram_cache_sets", 0);
        data_.pxn_dram_cache_ways_ = params.find<int32_t>("sys_pxn_dram_cache_ways", 0);
        data_.pxn_dram_cache_line_size_ = params.find<int32_t>("sys_pxn_dram_cache_line_size", 64);
        data_.pxn_dram_interleave_size_ = params.find<int64_t>("sys_pxn_dram_interleave_size", 64);
        data_.pod_l2sp_banks_ = params.find<int16_t>("sys_pod_l2sp_banks", 1);
        data_.pod_l2sp_interleave_size_ = params.find<int64_t>("sys_pod_l2sp_interleave_size", 64);
    }
    
    /**
     * return the configuration data
     */
    const DrvAPI::DrvAPISysConfigData & configData() const {
        return data_;
    }    

    /**
     * return the sys config data
     */
    DrvAPI::DrvAPISysConfig config() const {
        return DrvAPI::DrvAPISysConfig(configData());
    }

private:
    DrvAPI::DrvAPISysConfigData data_; //!< system configuration data

};
}
}
