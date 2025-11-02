from compute import XCoreBuilder, RCoreBuilder, ComputeBuilder
from memory import L1SPBuilder, L2SPBuilder, DRAMBuilder, CachedDRAMBuilder, NoCacheDRAMBuilder
from pod import PodBuilder
from pxn import PXNBuilder
from system import SystemBuilder

class PANDOHammer(object):
    """
    A PANDOHammer Simulation
    """
    def __init__(self, arguments, core_builder=XCoreBuilder):
        """
        Initialize the PANDOHammer Simulation
        arguments are parsed from the command line
        """
        pod_cores = arguments.pod_cores_x*arguments.pod_cores_y
        bandwidth_bytes_per_second_per_core = 24e9
        bandwidth_bytes_per_second_per_pod = bandwidth_bytes_per_second_per_core*pod_cores
        bandwidth_bytes_per_second_per_pxn = bandwidth_bytes_per_second_per_pod*arguments.pxn_pods

        # l1sp
        l1sp = L1SPBuilder()
        l1sp.clock = "1GHz"
        l1sp.access_time = "1ns"
        l1sp.size = 128*1024
        l1sp.network_bw = f"{bandwidth_bytes_per_second_per_core}B/s"
        
        # core
        core = core_builder()
        core.clock = "1GHz"
        core.threads = arguments.core_threads
        core.executable = arguments.program
        core.argv = ' '.join(arguments.argv)
        core.threads = arguments.core_threads
        core.network_bw = f"{bandwidth_bytes_per_second_per_core}B/s"
        
        # compute tile
        compute = ComputeBuilder()
        compute.l1sp = l1sp
        compute.core = core
        compute.network_bw = f"{bandwidth_bytes_per_second_per_core}B/s"
        compute.xbar_bw = f"{bandwidth_bytes_per_second_per_core}B/s"
        compute.link_bw = f"{bandwidth_bytes_per_second_per_core}B/s"
        
        # l2sp tile
        l2sp = L2SPBuilder()
        l2sp.clock = "1GHz"
        l2sp.access_time = "1ns"
        l2sp.network_bw = f"{bandwidth_bytes_per_second_per_core}B/s"

        # pod
        pod = PodBuilder()
        pod.compute = compute
        pod.l2sp = l2sp
        pod.cores = pod_cores
        pod.l2sp_size = arguments.pod_l2sp_size
        pod.l2sp_banks = arguments.pod_l2sp_banks
        pod.l2sp_interleave = arguments.pod_l2sp_interleave
        pod.network_bw = f"{bandwidth_bytes_per_second_per_pod}B/s"
        pod.xbar_bw = f"{bandwidth_bytes_per_second_per_pod}B/s"
        pod.link_bw = f"{bandwidth_bytes_per_second_per_pod}B/s"

        # host core
        hostcore = XCoreBuilder()
        hostcore.clock = "1GHz"
        hostcore.threads = 1
        hostcore.executable = arguments.with_command_processor
        hostcore.argv = ' '.join([arguments.program] + arguments.argv)
        hostcore.group = "0"
        hostcore.is_host = True

        # dram
        if arguments.without_pxn_dram_cache:
            dram = NoCacheDRAMBuilder()
        else:
            dram = CachedDRAMBuilder()

        dram.backend = "simple"
        dram.clock = "1GHz"
        dram.access_time = arguments.dram_access_time
        dram.network_bw = f"{bandwidth_bytes_per_second_per_pxn}B/s"

        # pxn
        pxn = PXNBuilder()
        pxn.pod = pod
        pxn.pods = arguments.pxn_pods
        pxn.hostcore_present = bool(arguments.with_command_processor)
        pxn.hostcore = hostcore
        pxn.dram = dram
        pxn.dram_size = arguments.pxn_dram_size
        pxn.dram_banks = arguments.pxn_dram_banks
        pxn.dram_interleave = arguments.pxn_dram_interleave
        pxn.network_bw = f"{bandwidth_bytes_per_second_per_pxn}B/s"
        pxn.xbar_bw = f"{bandwidth_bytes_per_second_per_pxn}B/s"
        pxn.link_bw = f"{bandwidth_bytes_per_second_per_pxn}B/s"

        # system
        system = SystemBuilder()
        system.pxn = pxn
        system.pxns = arguments.num_pxn
        self.system = system

        return

    def build(self):
        """
        Build the PANDOHammer Simulation
        """
        return self.system.build()

