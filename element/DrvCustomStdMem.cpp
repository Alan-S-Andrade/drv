#include "DrvCustomStdMem.hpp"

using namespace SST;
using namespace Drv;
using namespace Interfaces;
using namespace MemHierarchy;

/* constructor
 *
 * should register a read and write handler
 */
DrvCmdMemHandler::DrvCmdMemHandler(SST::ComponentId_t id, SST::Params& params,
                                   std::function<void(Addr,size_t,std::vector<uint8_t>&)> read,
                                   std::function<void(Addr,std::vector<uint8_t>*)> write)
  : CustomCmdMemHandler(id, params, read, write) {
  int verbose_level = params.find<int>("verbose_level", 0);
  output = SST::Output("DrvCmdMemHandler[@f:@l:@p]: ", verbose_level, 0, SST::Output::STDOUT);
  output.verbose(CALL_INFO, 1, 0, "%s\n", __PRETTY_FUNCTION__);
}

/* destructor */
DrvCmdMemHandler::~DrvCmdMemHandler() {
  output.verbose(CALL_INFO, 1, 0,"%s\n", __PRETTY_FUNCTION__);
}

/* Receive should decode a custom event and return an OutstandingEvent struct
 * to the memory controller so that it knows how to process the event
 */
CustomCmdMemHandler::MemEventInfo
DrvCmdMemHandler::receive(MemEventBase* ev) {
  output.verbose(CALL_INFO, 1, 0,"%s\n", __PRETTY_FUNCTION__);
  CustomCmdMemHandler::MemEventInfo MEI(ev->getRoutingAddress(),true);
  return MEI;
}

/* The memController will call ready when the event is ready to issue.
 * Events are ready immediately (back-to-back receive() and ready()) unless
 * the event needs to stall for some coherence action.
 * The handler should return a CustomData* which will be sent to the memBackendConvertor.
 * The memBackendConvertor will then issue the unmodified CustomCmdReq* to the backend.
 * CustomCmdReq is intended as a base class for custom commands to define as needed.
 */
Interfaces::StandardMem::CustomData*
DrvCmdMemHandler::ready(MemEventBase* ev) {
  output.verbose(CALL_INFO, 1, 0,"%s\n", __PRETTY_FUNCTION__);
  CustomMemEvent * cme = static_cast<CustomMemEvent*>(ev);
  // We don't need to modify the data structure sent by the CPU, so just 
  // pass it on to the backend
  return cme->getCustomData();
}

/* When the memBackendConvertor returns a response, the memController will call this function, including
 * the return flags. This function should return a response event or null if none needed.
 * It should also call the following as needed:
 *  writeData(): Update the backing store if this custom command wrote data
 *  readData(): Read the backing store if the response needs data
 *  translateLocalT
 */
MemEventBase*
DrvCmdMemHandler::finish(MemEventBase *ev, uint32_t flags) {
  output.verbose(CALL_INFO, 1, 0,"%s\n", __PRETTY_FUNCTION__);
  if(ev->queryFlag(MemEventBase::F_NORESPONSE)||
     ((flags & MemEventBase::F_NORESPONSE)>0)){
    // posted request
    // We need to delete the CustomData structure
    CustomMemEvent * cme = static_cast<CustomMemEvent*>(ev);
    if (cme->getCustomData() != nullptr)
      delete cme->getCustomData();
    cme->setCustomData(nullptr); // Just in case someone attempts to access it...
    return nullptr;
  }

  MemEventBase *MEB = ev->makeResponse();
  return MEB;
}

