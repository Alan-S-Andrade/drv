// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
#include <DrvAPI.hpp>
#include <iostream>
#include <sstream>
#include <inttypes.h>

#define FMT_ADDR(x)                             \
    std::hex << std::setfill('0') << std::setw(16) << x

using namespace DrvAPI;

void show_addr(const DrvAPIAddressDecoder &decoder, DrvAPIAddress address)
{
    auto info = decoder.decode(address);
    std::stringstream ss;
    ss << "PXN=" << myPXNId() <<", Pod=" << myPodId() << ", Core=" << myCoreId() << ": ";
    std::string prefix = ss.str();
    std::cout << prefix << "Address Decoded : " << FMT_ADDR(address) << ": " << info.to_string() << std::endl;
    DrvAPIAddress abs = decoder.to_absolute(address);
    info = decoder.decode(abs);
    std::cout << prefix << "Absolute Address: " << FMT_ADDR(abs) << ": " << info.to_string() << std::endl;
}

int AddrMapMain(int argc, char *argv[])
{
    using namespace DrvAPI;
    DrvAPIAddressDecoder decoder(myPXNId(), myPodId(), myCoreId());

    show_addr(decoder, 0b110ul << 61ul);
    show_addr(decoder, 0b101ul << 61ul);
    show_addr(decoder, 0b100ul << 61ul);
    show_addr(decoder, (0b100ul << 61ul) | (0b1ul << 30ul));
    show_addr(decoder, (0b0ul << 63ul)|(0b00ul << 30ul));
    show_addr(decoder, (0b0ul << 63ul)|(0b01ul << 30ul));
    show_addr(decoder, (0b0ul << 63ul)|(0b10ul << 30ul));
    return 0;
}

declare_drv_api_main(AddrMapMain);
