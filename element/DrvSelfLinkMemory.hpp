#pragma once
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <string>
#include "DrvMemory.hpp"
#include "DrvEvent.hpp"
#include "DrvAPIThreadState.hpp"

namespace SST {
namespace Drv {

/**
 * @brief The memory class using a self-link to model latency
 * 
 */
class DrvSelfLinkMemory : public DrvMemory {
public:
    /**
     * @brief Construct a new DrvSelfLinkMemory object
     */
    DrvSelfLinkMemory(DrvCore*core, const std::string &link_name);

    /**
     * @brief Destroy the DrvSelfLinkMemory object
     */
    virtual ~DrvSelfLinkMemory() {}

    /**
     * @brief Send a memory request
     */
    void sendRequest(DrvCore *core
                     ,DrvThread *thread
                     ,const std::shared_ptr<DrvAPI::DrvAPIMem> &mem_req);

    /**
     * Event class for self-link
     */
    class Event : public SST::Event {
    public:
        /**
         * @brief Construct a new DrvEvent object
         */
        Event() {}

        /**
         * @brief Destroy the DrvEvent object
         */
        virtual ~Event() {}

        std::shared_ptr<DrvAPI::DrvAPIMem> req_; //!< The memory request
        
        ImplementSerializable(SST::Drv::DrvSelfLinkMemory::Event);
    };
    
private:
    /**
     * handleEvent is called when a message is received
     */
    void handleEvent(SST::Event *ev);
    
    std::string link_name_; //!< Name of the link
    SST::Link  *link_; //!< A link
    DrvCore    *core_; //!< The core
    std::vector<uint8_t> data_; //!< The data store
};

}
}
