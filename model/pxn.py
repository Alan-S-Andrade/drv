from pod import *
from compute import *
from memory import *
import sst

class PXN(object):
    """
    A base class for a pxn

    A pxn has an endpoint in the on-chip network.
    It provides the network_if() method to get the network interface
    """
    def __init__(self, name):
        """
        Initialize the pxn
        """
        self.pods = []
        self.dram_banks = []
        self.hostcore = None
        self.network = None
        self.bridge = None
        self.name = name
        return

    def network_interface(self):
        """
        Returns the network interface subcomponent and port name
        (interface, portname) pair
        """
        return (self.bridge, "network1")

class PXNBuilder(object):
    """
    A base class for a pxn builder
    """
    def __init__(self):
        """
        Initialize the pxn builder
        """
        self.id = 0
        self.pod = PodBuilder()
        self.pods = 1
        self.hostcore = XCoreBuilder()
        self.hostcore_present = True
        self.dram = CachedDRAMBuilder()
        self.dram_size = 2*1024*1024
        self.dram_banks = 1
        self._dram_interleave = 0
        self.xbar_bw = "1GB/s"        
        self.link_bw = "1GB/s"
        self.network_bw = "1GB/s"
        self.input_buf_size = "1KB"
        self.output_buf_size = "1KB"
        self.router_latency = "0ns"
        return

    @property
    def dram_interleave(self):
        if self._dram_interleave == 0:
            return self.dram_size // self.dram_banks
        return self._dram_interleave

    @dram_interleave.setter
    def dram_interleave(self, value):
        self._dram_interleave = value
        return

    @property
    def dram_interleave_step(self):
        return self.dram_interleave * self.dram_banks

    def router_name(self, name):
        """
        Get the router name
        """
        return name + "_router"

    def pod_name(self, name, pod_id):
        """
        Get the pod name
        """
        return name + f"_pod{pod_id}"

    def hostcore_name(self, name):
        """
        Get the hostcore name
        """
        return name + "_hostcore"

    def dram_bank_name(self, name, bank_id):
        """
        Get the dram bank name
        """
        return name + f"_dram{bank_id}"

    def bridge_name(self, name):
        """
        Get the bridge name
        """
        return name + "_bridge"

    def ports(self):
        """
        Get the number of ports
        """
        # pods + dram banks + off-chip network + hostcore
        return self.pods \
            + self.dram_banks \
            + 1 \
            + (1 if self.hostcore_present else 0)
    
    def build(self, system_builder, name):
        """
        Make a pxn
        """
        pxn = PXN(name)
        # build the interpod network
        pxn.network = sst.Component(self.router_name(name), "merlin.hr_router")
        pxn.network.addParams({
            # semantic parameters
            "id" : 0,
            "num_ports" : self.ports(),
            "topology" : "merlin.singlerouter",            
            # performance models
            "xbar_bw" : self.xbar_bw,
            "link_bw" : self.link_bw,
            "flit_size" : "8B",
            "input_buf_size" : self.input_buf_size,
            "output_buf_size" : self.output_buf_size,
            "input_latency" : self.router_latency,
            "output_latency" : self.router_latency,
        })
        pxn.network.setSubComponent("topology", "merlin.singlerouter")

        # build the pods
        current_port = 0        
        for pod_id in range(self.pods):
            self.pod.id = pod_id
            pod = self.pod.build(system_builder, self.pod_name(name, pod_id))
            nwif, port = pod.network_interface()
            link = sst.Link(f"{pod.name}_to_{self.router_name(name)}")                            
            link.connect(
                (nwif, port, "1ns"),
                (pxn.network, f"port{current_port}", "1ns")
            )
            pxn.pods.append(pod)
            current_port += 1

        # build the dram banks
        self.dram.size = self.dram_size
        self.dram.interleave_size = self.dram_interleave
        self.dram.interleave_step = self.dram_interleave_step
        for dram_bank_id in range(self.dram_banks):
            self.dram.id = dram_bank_id
            dram = self.dram.build(system_builder, self.dram_bank_name(name, dram_bank_id))
            nwif, port = dram.network_interface()
            link = sst.Link(f"{dram.name}_to_{self.router_name(name)}")
            link.connect(
                (nwif, port, "1ns"),
                (pxn.network, f"port{current_port}", "1ns")
            )
            pxn.dram_banks.append(dram)
            current_port += 1

        # build the hostcore
        if self.hostcore_present:
            self.hostcore.id = -1
            hostcore = self.hostcore.build(system_builder, self.hostcore_name(name))
            nwif, port = hostcore.network_interface()
            link = sst.Link(f"{hostcore.name}_to_{self.router_name(name)}")
            link.connect(
                (nwif, port, "1ns"),
                (pxn.network, f"port{current_port}", "1ns")
            )
            pxn.hostcore = hostcore
            current_port += 1

        # create the network bridge to the off-chip network
        bridge = sst.Component(self.bridge_name(name), "merlin.Bridge")
        bridge.addParams({
            "translator" : "memHierarchy.MemNetBridge",
            "network_bw" : self.network_bw,
        })
        link = sst.Link(f"{self.router_name(name)}_to_{self.bridge_name(name)}")
        link.connect(
            (pxn.network, f"port{current_port}", "1ns"),
            (bridge, "network0", "1ns")
        )        
        pxn.bridge = bridge
        return pxn
