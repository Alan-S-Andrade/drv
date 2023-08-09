#pragma once
#include <DrvAPIThread.hpp>
#include <DrvAPIThreadState.hpp>
namespace DrvAPI
{

inline void nop(int cycles)
{
    DrvAPIThread::current()->setState
        (std::make_shared<DrvAPINop>(cycles));
    DrvAPIThread::current()->yield();
};

inline void wait(int cycles)
{
    return nop(cycles);
};

}
