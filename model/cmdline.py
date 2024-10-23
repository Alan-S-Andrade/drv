import argparse

def parse_args():
    print("Parsing arguments")
    parser = argparse.ArgumentParser()
    parser.add_argument("program", help="program to run")
    parser.add_argument("argv", nargs=argparse.REMAINDER, help="arguments to program")
    parser.add_argument("--verbose", type=int, default=0, help="verbosity of core")
    parser.add_argument("--dram-access-time", type=str, default="70ns", help="latency of DRAM (only valid if using the latency based model)")
    parser.add_argument("--dram-backend", type=str, default="simple", choices=['simple', 'ramulator','dramsim3'], help="backend timing model for DRAM")
    parser.add_argument("--dram-backend-config", type=str, default="/root/sst-ramulator-src/configs/hbm4-pando-config.cfg",
                        help="backend timing model configuration for DRAM")
    parser.add_argument("--debug-init", action="store_true", help="enable debug of init")
    parser.add_argument("--debug-memory", action="store_true", help="enable memory debug")
    parser.add_argument("--debug-requests", action="store_true", help="enable debug of requests")
    parser.add_argument("--debug-responses", action="store_true", help="enable debug of responses")
    parser.add_argument("--debug-syscalls", action="store_true", help="enable debug of syscalls")
    parser.add_argument("--debug-clock", action="store_true", help="enable debug of clock ticks")
    parser.add_argument("--debug-mmio", action="store_true", help="enable debug of mmio requests to the core")
    parser.add_argument("--verbose-memory", type=int, default=0, help="verbosity of memory")
    parser.add_argument("--pod-cores", type=int, default=8, help="number of cores per pod")
    parser.add_argument("--pxn-pods", type=int, default=1, help="number of pods")
    parser.add_argument("--num-pxn", type=int, default=1, help="number of pxns")
    parser.add_argument("--core-threads", type=int, default=16, help="number of threads per core")
    parser.add_argument("--core-clock", type=str, default="1GHz", help="clock frequency of cores")
    parser.add_argument("--core-max-idle", type=int, default=1, help="max idle time of cores")
    parser.add_argument("--core-l1sp-size", type=int, default=128*1024, help="size of l1sp per core")

    parser.add_argument("--pod-l2sp-banks", type=int, default=1, help="number of l2sp banks per pod")
    parser.add_argument("--pod-l2sp-interleave", type=int, default=0, help="interleave size of l2sp addresses (defaults to no  interleaving)")
    parser.add_argument("--pod-l2sp-size", type=int, default=1024*1024, help=f"size of l2sp per pod (max {2**20} bytes)")
    
    parser.add_argument("--pxn-dram-banks", type=int, default=1, help="number of dram banks per pxn")
    parser.add_argument("--pxn-dram-size", type=int, default=2*(1024**3), help=f"size of main memory per pxn (max {8*(2**30)} bytes)")
    parser.add_argument("--pxn-dram-interleave", type=int, default=0, help="interleave size of dram addresses (defaults to no  interleaving)")

    parser.add_argument("--without-pxn-dram-cache", action="store_true", help="disable dram cache")
    
    parser.add_argument("--with-command-processor", type=str, default="",
                        help="Command processor program to run. Defaults to empty string, in which no command processor will be included in the model.")
    parser.add_argument("--cp-verbose", type=int, default=0, help="verbosity of command processor")
    parser.add_argument("--cp-verbose-init", action="store_true", help="command processor enable debug of init")
    parser.add_argument("--cp-verbose-requests", action="store_true", help="command processor enable debug of requests")
    parser.add_argument("--cp-verbose-responses", action="store_true", help="command processor enable debug of responses")
    parser.add_argument("--drvx-stack-in-l1sp", action="store_true", help="use l1sp backing storage as stack")
    parser.add_argument("--drvr-isa-test", action="store_true", help="Running an ISA test")
    parser.add_argument("--test-name", type=str, default="", help="Name of the test")
    parser.add_argument("--core-stats", action="store_true", help="enable core statistics")
    parser.add_argument("--stats-load-level", type=int, default=0, help="load level for statistics")
    parser.add_argument("--trace-remote-pxn-memory", action="store_true", help="trace remote pxn memory accesses")

    arguments = parser.parse_args()
    return arguments
