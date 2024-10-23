import sst
from compute import ComputeBuilder, CoreBuilder, XCoreBuilder, RCoreBuilder
from memory import L1SPBuilder, L2SPBuilder, DRAMBuilder
from pod import PodBuilder
from pxn import PXNBuilder
from addressmap import AddressMap

class System(object):
    """
    A system object
    """
    def __init__(self, name):
        """
        Initialize the system
        """
        self.pxns = []
        self.name = name
        return

class SystemBuilder(object):
    """
    A base class for a system builder
    """
    def __init__(self):
        """
        Initialize the system builder
        """
        self.pxn = PXNBuilder()
        self.pxns = 1
        self.xbar_bw = "1GB/s"
        self.link_bw = "1GB/s"
        self.input_buf_size = "1KB"
        self.output_buf_size = "1KB"
        self.router_latency = "1ns"
        return

    def addressmap(self):
        """
        Get the address map
        """
        class sysconfig(object):
            def __init__(self, system_builder):
                self.system_builder = system_builder
            def pxns(self):
                return self.system_builder.pxns
            def pods(self):
                return self.system_builder.pxn.pods
            def cores(self):
                return self.system_builder.pxn.pod.cores
        
        return AddressMap(sysconfig(self))
    
    def network_name(self, name):
        """
        Get the network name
        """
        return name + "_network"

    def pxn_name(self, name, pxn_id):
        """
        Get the PXN name
        """
        return name + f"_pxn{pxn_id}"
    
    def build(self, name = "system"):
        """
        Make a system
        """
        system = System(name)

        # build the interpxn network
        system.network = sst.Component(self.network_name(name), "merlin.hr_router")
        system.network.addParams({
            # semantic parameters
            "id" : 0,
            "num_ports" : self.pxns,
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
        system.network.setSubComponent("topology", "merlin.singlerouter")

        # build the PXNs
        for pxn_id in range(self.pxns):
            self.pxn.id = pxn_id
            pxn = self.pxn.build(self, self.pxn_name(name, pxn_id))
            nwif, port = pxn.network_interface()
            link = sst.Link(f"{pxn.name}_to_{self.network_name(name)}")
            link.connect(
                (nwif, port, "1ns"),
                (system.network, f"port{pxn_id}", "1ns")
            )
            system.pxns.append(pxn)

        return system

    
