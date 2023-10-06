#ifndef DRV_API_SYS_CONFIG_HPP_
#define DRV_API_SYS_CONFIG_HPP_
#include <cstdint>
namespace DrvAPI
{

/**
 * @brief The DrvAPISysConfigData struct
 * This struct is used to store system configuration data.
 */
struct DrvAPISysConfigData
{
    int64_t num_pxn_; //!< number of PXNs in the system
    int64_t pxn_pods_; //!< number of pods per PXN
    int64_t pod_cores_; //!< number of cores per pod
};


/**
 * @brief The DrvAPISysConfig class
 * This class is used to retrieve system configuration data.
 */
class DrvAPISysConfig
{    
public:
    DrvAPISysConfig(const DrvAPISysConfigData& data): data_(data) {}
    DrvAPISysConfig() = default;
    ~DrvAPISysConfig() = default;
    DrvAPISysConfig(const DrvAPISysConfig&) = default;
    DrvAPISysConfig& operator=(const DrvAPISysConfig&) = default;
    DrvAPISysConfig(DrvAPISysConfig&&) = default;
    DrvAPISysConfig& operator=(DrvAPISysConfig&&) = default;
    
    int64_t numPXN() const { return data_.num_pxn_; }
    int64_t numPXNPods() const { return data_.pxn_pods_; }
    int64_t numPodCores() const { return data_.pod_cores_; }

    static DrvAPISysConfig *Get() { return &sysconfig; }
    static DrvAPISysConfig sysconfig;
private:
    DrvAPISysConfigData data_;
};
}

/**
 * @brief DrvAPIGetSysConfig
 * @return the system configuration
 */
extern "C" DrvAPI::DrvAPISysConfig* DrvAPIGetSysConfig();

/**
 * @brief DrvAPIGetSysConfig_t
 */
typedef DrvAPI::DrvAPISysConfig* (*DrvAPIGetSysConfig_t)();

/**
 * @brief DrvAPISetSysConfig
 * @param sys_config the system configuration
 */
extern "C" void DrvAPISetSysConfig(DrvAPI::DrvAPISysConfig*);

/**
 * @brief DrvAPISetSysConfig_t
 */
typedef void (*DrvAPISetSysConfig_t)(DrvAPI::DrvAPISysConfig*);


#endif
