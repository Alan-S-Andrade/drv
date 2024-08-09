#define PANDOCOMMAND_DEBUG
#include <pandocommand/executable.hpp>
#include <pandocommand/loader.hpp>
#include <pandocommand/control.hpp>
#include <pandocommand/debug.hpp>
#include <DrvAPI.hpp>
#include <inttypes.h>
using namespace pandocommand;
using namespace DrvAPI;

int CommandProcessorMain(int argc, char *argv[])
{
    const char *exe = argv[1];
    PANDOHammerExe executable(exe);
    cmd_dbg("Loading %s\n", exe);
    loadProgram(executable);

    auto flag_ptr = executable.symbol<int64_t>("command_processor_present", {0, 0, 0});
    *flag_ptr = 1;

    cmd_dbg( "Wrote 1 to 0x%" PRIx64 "\n", (uint64_t)flag_ptr);
    cmd_dbg("Releasing %d Cores on %d Pods from reset\n"
            , numPXNPods()*numPodCores()
            , numPXNPods());

    // release all cores from reset
    assertResetAll(false);

    return 0;
}

declare_drv_api_main(CommandProcessorMain);
