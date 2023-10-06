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

#define DRV_SYS_CONFIG_PARAMETERS                       \
    {"sys_num_pxn", "Number of PXN in system", "1"},    \
    {"sys_pxn_pods", "Number of pods per PXN", "1"},    \
    {"sys_pod_cores", "Number of cores per pod", "1"},

    /**
     * initialize the system configuration
     */
    void init(Params &params) {
        data_.num_pxn_ = params.find<int64_t>("sys_num_pxn", 1);
        data_.pxn_pods_ = params.find<int64_t>("sys_pxn_pods", 1);
        data_.pod_cores_ = params.find<int64_t>("sys_pod_cores", 1);
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
