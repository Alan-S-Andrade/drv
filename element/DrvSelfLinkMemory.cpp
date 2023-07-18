#include "DrvSelfLinkMemory.hpp"
#include "DrvCore.hpp"
#include "DrvEvent.hpp"
#include <cstdio>

using namespace SST;
using namespace Drv;

DrvSelfLinkMemory::DrvSelfLinkMemory(DrvCore*core, const std::string &link_name)
  : link_name_(link_name)
  , link_(nullptr)
  , core_(core)
  , data_(32*1024) {
  link_ = core_->configureCoreLink(link_name_
                                   ,new Event::Handler<DrvSelfLinkMemory>(this, &DrvSelfLinkMemory::handleEvent));
}


void DrvSelfLinkMemory::handleEvent(SST::Event *ev) {
  core_->output()->verbose(CALL_INFO, 2, DrvCore::DEBUG_REQ, "Received event\n");
  auto mem_evt = dynamic_cast<DrvSelfLinkMemory::Event*>(ev);
  if (!mem_evt) {
    core_->output()->fatal(CALL_INFO, -1, "ERROR: %s: invalid event type\n", __FUNCTION__);
    return;
  }
  
  std::shared_ptr<DrvAPI::DrvAPIMem> mem_req = mem_evt->req_;
  
  auto read = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemRead>(mem_req);
  if (read) {
    read->setResult(&data_[read->getAddress().offset()]);
    read->complete();
    return;
  }
  
  auto write = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemWrite>(mem_req);
  if (write) {
    write->getPayload(&data_[write->getAddress().offset()]);
    write->complete();
    return;
  }
  
  auto atomic = std::dynamic_pointer_cast<DrvAPI::DrvAPIMemAtomic>(mem_req);
  if (atomic) {
    atomic->setResult(&data_[atomic->getAddress().offset()]);
    atomic->modify();
    atomic->getPayload(&data_[atomic->getAddress().offset()]);
    atomic->complete();
    return;
  }
  
  delete ev;
}


void DrvSelfLinkMemory::sendRequest(DrvCore *core
                                    ,DrvThread *thread
                                    ,const std::shared_ptr<DrvAPI::DrvAPIMem> &mem_req) {
  core->output()->verbose(CALL_INFO, 2, DrvCore::DEBUG_REQ, "Sending request\n");
  auto ev = new DrvSelfLinkMemory::Event();
  ev->req_ = mem_req;
  link_->send(0, ev);
}
