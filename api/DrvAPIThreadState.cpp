#include "DrvAPIThreadState.hpp"
#include "DrvAPIAddressMap.hpp"
#include "DrvAPIInfo.hpp"
namespace DrvAPI {

DrvAPIMem::DrvAPIMem(DrvAPIAddress address)
    : can_resume_(false)
    , address_(0) {
    address_= DrvAPIVAddress::to_physical
        (address
         ,myPXNId()
         ,myPodId()
         ,myCoreId()>>3
         ,myCoreId()&0x7
         ).encode();
}

}
