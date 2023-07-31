#include <DrvAPIMain.hpp>
#include <DrvAPIThread.hpp>
#include <DrvAPIMemory.hpp>
#include <DrvAPIFunction.hpp>
#include <mpi.h>

#define PRINT_VAL(x, fmt)                                               \
    do  {                                                               \
        printf("%s = %" fmt "\n", #x, x);                               \
    } while (0)

__attribute__((noinline))
static void common(const char *pretty_function, const char *message)
{
    using namespace DrvAPI;
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    printf("TRACE: %s: Core %4d: Thread %4d: Rank = %4d: \"%s\"\n"
           ,pretty_function
           ,DrvAPIThread::current()->coreId()
           ,DrvAPIThread::current()->threadId()
           ,rank
           ,message);
}

int PIDMain(int argc, char *argv[])
{
    using namespace DrvAPI;
    uint64_t signal = 0xa5a5a5a5a5a5a5a5;
    DrvAPIAddress signal_addr = 0x00000000;
    DrvAPIAddress addr        = 0x00000010;
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int arg = 1;
    int f1_not_f0 = 0;
    if (argc > arg) {
        f1_not_f0 = atoi(argv[arg++]);
    }
    auto f0_body = [](){
        common(__PRETTY_FUNCTION__, "inside f0_body");
    };
    auto f1_body = [](){
        common(__PRETTY_FUNCTION__, "inside f1_body");
    };
    if (DrvAPIThread::current()->coreId() == 0 &&
        DrvAPIThread::current()->threadId() == 0) {
        DrvAPIFunction *f0 = MakeDrvAPIFunction(f0_body);
        DrvAPIFunction *f1 = MakeDrvAPIFunction(f1_body);
        f0->execute();
        f1->execute();
        DrvAPIFunction *f = f1_not_f0 ? f1 : f0;
        // write the function pointer to the addr
        printf("Core %4d: Thread %4d: Rank = %4d: writing function pointer with type_id = %d\n"
               ,DrvAPIThread::current()->coreId()
               ,DrvAPIThread::current()->threadId()
               ,rank
               ,f->getFunctionTypeId());
        DrvAPI::write_function_ptr(addr, f);
        printf("Core %4d: Thread %4d: Rank = %4d: writing signal\n"
               ,DrvAPIThread::current()->coreId()
               ,DrvAPIThread::current()->threadId()
               ,rank);
        DrvAPI::write(signal_addr, signal);
    } else {
        while (DrvAPI::read<decltype(signal)>(signal_addr) != signal) {
            // wait for the signal
            printf("Core %4d: Thread %4d: Rank = %4d: waiting for signal\n"
                   ,DrvAPIThread::current()->coreId()
                   ,DrvAPIThread::current()->threadId()
                   ,rank);
        }
        printf("Core %4d: Thread %4d: Rank = %4d: signal received\n"
               ,DrvAPIThread::current()->coreId()
               ,DrvAPIThread::current()->threadId()
               ,rank);
        DrvAPIFunction *f = DrvAPI::read_function_ptr(addr);
        printf("Core %4d: Thread %4d: Rank = %4d: read function pointer with type_id = %d\n"
               ,DrvAPIThread::current()->coreId()
               ,DrvAPIThread::current()->threadId()
               ,rank
               ,f->getFunctionTypeId());
        f->execute();
    }

    return 0;
}

declare_drv_api_main(PIDMain);
