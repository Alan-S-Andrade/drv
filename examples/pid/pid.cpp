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
    DrvAPIAddress addr        = 0x00000008;
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    auto f0_body = [](){
        common(__PRETTY_FUNCTION__, "inside f0_body");
    };
    auto f1_body = [](){
        common(__PRETTY_FUNCTION__, "inside f1_body");
    };
    if (DrvAPIThread::current()->coreId() == 0 &&
        DrvAPIThread::current()->threadId() == 0) {
        common(__PRETTY_FUNCTION__, "inside main");
        DrvAPIFunction *f0 = MakeDrvAPIFunction(f0_body);
        f0->execute();

        DrvAPIFunction *f0_clone = f0->getFactory()(f0);
        f0_clone->execute();

        DrvAPIFunction *f1 = MakeDrvAPIFunction(f1_body);
        printf("Core %4d, Rank %4d: &f0->getFactory() = %p\n"
               ,DrvAPIThread::current()->coreId()
               ,rank
               ,(void*)f0->getFactory());
        printf("Core %4d, Rank %4d: &f1->getFactory() = %p\n"
               ,DrvAPIThread::current()->coreId()
               ,rank
               ,(void*)f1->getFactory());
    } else {
        printf("Core %4d, Rank %4d: DrvAPIFunction::NumTypes() = %d\n"
               ,DrvAPIThread::current()->coreId()
               ,rank
               ,DrvAPIFunction::NumTypes());
        printf("Core %4d, Rank %4d: DrvAPIFunction::GetFactory(0) = %p\n"
               ,DrvAPIThread::current()->coreId()
               ,rank
               ,(void*)DrvAPIFunction::GetFactory(0));
        printf("Core %4d, Rank %4d: DrvAPIFunction::GetFactory(1) = %p\n"
               ,DrvAPIThread::current()->coreId()
                ,rank
               ,(void*)DrvAPIFunction::GetFactory(1));
        DrvAPIFunction *f0_clone = DrvAPIFunction::GetFactory(0)(&f0_body);
        DrvAPIFunction *f1_clone = DrvAPIFunction::GetFactory(1)(&f1_body);
        f0_clone->execute();
        f1_clone->execute();
    }
    
    return 0;
}

declare_drv_api_main(PIDMain);
