import itertools
import sst
import enum
import addressmap
from addressmap import Bitfield, AddressMap, AddressInfo
from clock import Clock
from cmdline import parser
import numpy as np


#sst.setStatisticOutput("sst.statOutputConsole") set this for all stats
sst.setStatisticOutput("sst.statOutputCSV", {
    "filepath": "stats.csv",
    "separator": ","
})
sst.setStatisticLoadLevel(1)
p = parser(core_l1sp_size=128*1024)

ARGUMENTS = p.parse_args()

CORES_X = ARGUMENTS.pod_cores_x
CORES_Y = ARGUMENTS.pod_cores_y
X = CORES_X
Y = CORES_Y+2
if ARGUMENTS.with_command_processor:
    Y += 1

BASE_CORE_X = 0
BASE_CORE_Y = 1 if not ARGUMENTS.with_command_processor else 2

VICTIM_CACHES = CORES_X*2
VICTIM_CACHE_ASSOCIATIVITY = 2
VICTIM_CACHE_SIZE = 16*1024
CACHE_LINE_SIZE = 64
VICTIM_CACHE_SETS = VICTIM_CACHE_SIZE // (VICTIM_CACHE_ASSOCIATIVITY * CACHE_LINE_SIZE) 
def get_core_id(x, y, xdim, ydim):
    x = x - BASE_CORE_X
    y = y - BASE_CORE_Y
    return x + y*xdim

def hostcore_coordinate():
    return (0,0)

def core_coordinates():
    return itertools.product(
        (BASE_CORE_X + x for x in range(CORES_X)),
        (BASE_CORE_Y + y for y in range(CORES_Y))
    )

def vcache_coordinates():
    return itertools.product(
        (x for x in range(X)),
        (BASE_CORE_Y-1, Y-1)
    )
        
    
MEMSIZE = ARGUMENTS.pxn_dram_size

CPU_VERBOSE_LEVEL   = 0
NETWORK_DEBUG_LEVEL = 0
MEMORY_DEBUG_LEVEL  = 0
UPDATES_PER_CORE = 1000

CORE_CLOCK = Clock(1.5e9)
MEMORY_CLOCK = Clock(1e9)

NETWORK_BANDWIDTH = f'{CORE_CLOCK * 8 * 3}B/s'
XBAR_BANDWIDTH = f'{CORE_CLOCK * 8 * 3 * 6}B/s'

class sysconfig(object):
    def cores(self): return CORES_X * CORES_Y
    def pods(self): return 1
    def pxns(self): return 1
        

def dram_range(bank_id, banks, memsize, interleave):
    """
    Address range for a dram bank.
    return addr_start, addr_end, interleave, stride
    """
    address_map = AddressMap(sysconfig())
    bank_size = memsize // banks

    if banks == 1:
        bank_id = 0

    stride = interleave * banks
    start = bank_id * interleave
    stop = memsize - (banks - bank_id - 1) * interleave - 1

    start_info = AddressInfo().set_dram()\
                              .set_absolute()\
                              .set_offset(start)

    stop_info = AddressInfo().set_dram()\
                             .set_absolute()\
                             .set_offset(stop)

    return (address_map.encode(start_info),
            address_map.encode(stop_info),
            interleave,
            stride)

def vcache_range(xdim, ydim, x, south_not_north, memsize, interleave):
    """
    Address range for a vcache bank.
    return addr_start, addr_end, interleave, stride
    """
    banks = xdim*2 # north and south
    snn = 1 if south_not_north else 0
    bank_id = x + snn*xdim
    return dram_range(bank_id, banks, memsize, interleave)

def l1sp_range(xdim, ydim, core_id, memsize):
    """
    Address range for a scratchpad memory
    return addr_start, addr_end, interleave, stride
    """
    addressmap = AddressMap(sysconfig())
    start_info = AddressInfo().set_l1sp()\
                              .set_absolute()\
                              .set_core(core_id)\
                              .set_offset(0)
    stop_info = AddressInfo().set_l1sp()\
                             .set_absolute()\
                             .set_core(core_id)\
                             .set_offset(memsize-1)
    return (addressmap.encode(start_info),
            addressmap.encode(stop_info),
            0,
            0)

class Mesh(object):
    """
    A mesh. Composed of tiles indexed by (x,y).
    """
    def __init__(self):
        self.tiles = {}

class MeshBuilder(object):
    """
    Builds a mesh. Composed of tile builders for each point in the mesh.
    """
    # x direction
    EAST =  (0, 0)
    WEST =  (0, 1)
    # y direction
    NORTH = (1, 0)
    SOUTH = (1, 1)
    # local ports
    LOCAL0 = (2, 0)
    LOCAL1 = (2, 1)

    @classmethod
    def portof(cls, direction):
        dim, neg = direction
        return 2*dim + neg

    @classmethod
    def west_port(cls):
        return cls.portof(cls.WEST)

    @classmethod
    def east_port(cls):
        return cls.portof(cls.EAST)

    @classmethod
    def north_port(cls):
        return cls.portof(cls.NORTH)

    @classmethod
    def south_port(cls):
        return cls.portof(cls.SOUTH)

    def __init__(self, xdim, ydim, meshid):
        self.xdim = xdim
        self.ydim = ydim
        self.meshid = meshid
        self.tile_builder = {
            (x,y) : EmptyTileBuilder for (x,y) in itertools.product(range(xdim), range(ydim))
        }

    def build(self):
        mesh = Mesh()
        # build all tiles
        for (x,y) in itertools.product(range(self.xdim), range(self.ydim)):
            bldr = self.tile_builder[(x,y)](self.xdim, self.ydim, self.meshid)
            mesh.tiles[(x,y)] = bldr.build(x, y)

        # connect all tiles
        for (x,y) in itertools.product(range(self.xdim), range(self.ydim)):
            # connect to north neighbor
            if y < self.ydim-1:
                link = sst.Link(f"link_{x}x{y}_to_{x}x{y+1}_mesh{self.meshid}")
                link.connect(mesh.tiles[(x,y)].north, mesh.tiles[(x,y+1)].south)
            # connect to east neighbor
            if x < self.xdim-1:
                link = sst.Link(f"link_{x}x{y}_to_{x+1}x{y}_mesh{self.meshid}")
                link.connect(mesh.tiles[(x,y)].east, mesh.tiles[(x+1,y)].west)

        return mesh

class Identifiable(object):
    """
    Base class for objects that have an id in a mesh.
    """
    def __init__(self, xdim, ydim, meshid):
        self.xdim = xdim
        self.ydim = ydim
        self.meshid = meshid

    def id(self, x, y):
        # dimension order x than y
        return y*self.xdim + x

    def absid(self, x, y):
        return self.id(x, y) + self.meshid * self.xdim * self.ydim

class L1SP(object):
    """
    Scratchpad memory.
    """
    def __init__(self):
        self.controller = None
        self.backend = None
        self.nic = None    

    @property
    def network_interface(self):
        return (self.nic, "port", f'{CORE_CLOCK.cycle_ps}ps')
    
class L1SPBuilder(Identifiable):
    """
    Builds a scratchpad memory.
    """
    size = ARGUMENTS.core_l1sp_size
    bandwidth = 8e9 # 8GB/s
    def __init__(self, xdim, ydim, meshid):
        super().__init__(xdim, ydim, meshid)

    def core_id(self, x, y):
        return get_core_id(x, y, self.xdim, self.ydim)

    def build(self, x, y):
        memory = L1SP()
        memory.controller = sst.Component(f"l1sp_{x}_{y}_mesh{self.meshid}",
                                          "memHierarchy.MemController")
        start, end, *_ = l1sp_range(self.xdim, self.ydim, self.core_id(x, y), L1SPBuilder.size)
        #print(f"L1SP {x},{y} range {start:x} - {end:x}")
        
        memory.controller.addParams({
            "debug_level" : MEMORY_DEBUG_LEVEL,
            "debug" : 1,
            "verbose" : 0,
            "clock" : f'{CORE_CLOCK}Hz',
            "addr_range_start" : start,
            "addr_range_end" : end,
            "interleave_size" : f"{L1SPBuilder.size}B",
            "interleave_step" : f"{X*Y*L1SPBuilder.size}B",
        })
        memory.backend = memory.controller.setSubComponent("backend",
                                                           "Drv.DrvSimpleMemBackend")
        memory.backend.addParams({
            "access_time" : f'{CORE_CLOCK.cycle_ps}ps',
            "mem_size" : f"{L1SPBuilder.size}B",
        })
        memory.cmdhandler = memory.controller.setSubComponent("customCmdHandler",
                                                              "Drv.DrvCmdMemHandler")
        memory.cmdhandler.addParams({
            "cache_line_size" : VictimCacheBuilder.cache_line_size,
            "shootdowns" : "true",
        })

        memory.nic = memory.controller.setSubComponent("cpulink",
                                                       "memHierarchy.MemNIC")
        memory.nic.addParams({
            "group" : "1",
            "network_bw" : NETWORK_BANDWIDTH,
            "sources" : "0",
            "debug_level" : NETWORK_DEBUG_LEVEL,
            "debug" : 1,
        })
        return memory

class Core(object):
    """
    A core. Has a cpu model and a memory network interface.
    """
    def __init__(self):
        self.core = None
        self.generator = None
        self.interface = None
        self.nic = None
    
    @property
    def network_interface(self):
        return (self.nic, "port", f'{CORE_CLOCK.cycle_ps}ps')

class MirandaCoreBuilder(Identifiable):
    """
    Build a Miranda Core.
    """
    max_address = 0
    min_address = 0
    def __init__(self, xdim, ydim, meshid):
        super().__init__(xdim, ydim, meshid)

    def build(self, x, y):
        core = Core()
        core.core = sst.Component(f"core_{x}_{y}_mesh{self.meshid}", "miranda.BaseCPU")
        core.core.addParams({
            "verbose" : CPU_VERBOSE_LEVEL,
            "maxloadmemreqpending" : 1,
            "maxstorememreqpending" : 1,
            "maxcustommemreqpending" : 1,
        })
        core.generator = core.core.setSubComponent("generator", "miranda.GUPSGenerator")
        core.generator.addParams({
            "verbose" : 4,
            # todo: modify this to access DRAM
            "max_address" : MirandaCoreBuilder.max_address,
            "min_address" : MirandaCoreBuilder.min_address,
            "count" : UPDATES_PER_CORE if x == 1 and y == 1 else 0,
            "clock" : f'{CORE_CLOCK}Hz',
            "seed_a" : self.id(x, y),
            "seed_b" : 7*self.id(x, y)+1,
        })
        core.interface = core.core.setSubComponent("memory", "memHierarchy.standardInterface")
        core.nic = core.interface.setSubComponent("memlink", "memHierarchy.MemNIC")
        core.nic.addParams({
            "group" : "0",
            "network_bw" : NETWORK_BANDWIDTH,
            "destinations" : "1",
            "debug_level" : NETWORK_DEBUG_LEVEL,
            "debug" : 1,
        })
        return core

class DrvXCoreBuilder(Identifiable):
    """
    Build a DrvX core.
    """
    def __init__(self, xdim, ydim, meshid):
        super().__init__(xdim, ydim, meshid)

    def core_id(self, x, y):
        return get_core_id(x, y, self.xdim, self.ydim)

    def build(self, x, y):
        core = Core()
        core.core = sst.Component(f"core_{x}_{y}_mesh{self.meshid}", "Drv.DrvCore")
        core.core.addParams({
            "clock" : f'{CORE_CLOCK}Hz',
            "max_idle" : 2,
            "threads" : 1,
            "executable" : ARGUMENTS.program,
            "argv" : ' '.join(ARGUMENTS.argv),
            "id" : self.core_id(x, y),
            "pod" : 0,
            "pxn" : 0,
            "sys_num_pxn" : 1,
            "sys_pxn_pods" : 1,
            "sys_pod_cores_x" : CORES_X,
            "sys_pod_cores_y" : CORES_Y,
            "sys_core_threads" : ARGUMENTS.core_threads,
            "sys_core_clock" : f'{CORE_CLOCK}Hz',
            "sys_core_l1sp_size" : L1SPBuilder.size,            
            "sys_pod_l2sp_size" : 0,
            "sys_pod_l2sp_banks" : 0,
            "sys_pod_l2sp_interleave_size" : 0,
            "sys_pxn_dram_size" : MEMSIZE,
            "sys_pxn_dram_ports" : 1,
            "sys_pxn_dram_cache_banks" : VICTIM_CACHES,
            "sys_pxn_dram_cache_sets" : VICTIM_CACHE_SETS,
            "sys_pxn_dram_cache_ways" : VICTIM_CACHE_ASSOCIATIVITY,
            "sys_pxn_dram_interleave_size" : CACHE_LINE_SIZE, # set this to make dma work
            "sys_nw_flit_dwords" : 1,
            "sys_nw_obuf_dwords" : CACHE_LINE_SIZE//8,
            "sys_cp_present" : bool(ARGUMENTS.core_threads),
        })
        core.memory = core.core.setSubComponent("memory", "Drv.DrvStdMemory")
        core.interface = core.memory.setSubComponent("memory", "memHierarchy.standardInterface")
        core.nic = core.interface.setSubComponent("memlink", "memHierarchy.MemNIC")
        core.nic.addParams({
            "group" : "0",
            "network_bw" : NETWORK_BANDWIDTH,
            "destinations" : "1",
            "debug_level" : NETWORK_DEBUG_LEVEL,
            "debug" : 1,
        })
        return core

class DrvRCoreBuilder(Identifiable):
    """
    Builds a DrvR Core.
    """
    def __init__(self, xdim, ydim, meshid):
        super().__init__(xdim, ydim, meshid)

    def core_id(self, x, y):
        return get_core_id(x, y, self.xdim, self.ydim)

    def build(self, x, y):
        core = Core()
        core.core = sst.Component(f"core_{x}_{y}_mesh{self.meshid}", "Drv.RISCVCore")
        core.core.enableAllStatistics()
        core.core.addParams({
            "clock" : f'{CORE_CLOCK}Hz',
            "num_harts" : ARGUMENTS.core_threads,
            "program" : ARGUMENTS.program,
            "argv" : ' '.join(ARGUMENTS.argv),
            "core" : self.core_id(x, y),
            "verbose" : 100,
            "debug_clock" : ARGUMENTS.debug_clock,
            "pod" : 0,
            "pxn" : 0,
            "sys_num_pxn" : 1,
            "sys_pxn_pods" : 1,
            "sys_pod_cores_x" : CORES_X,
            "sys_pod_cores_y" : CORES_Y,
            "sys_core_threads" : ARGUMENTS.core_threads,
            "sys_core_clock" : f'{CORE_CLOCK}Hz',
            "sys_core_l1sp_size" : L1SPBuilder.size,
            "sys_pod_l2sp_size" : 256*1024,
            "sys_pod_l2sp_banks" : 1,
            "sys_pod_l2sp_interleave_size" : 0,
            "sys_pxn_dram_size" : MEMSIZE,
            "sys_pxn_dram_ports" : 1,
            "sys_pxn_dram_cache_banks" : VICTIM_CACHES,            
            "sys_pxn_dram_interleave_size" : CACHE_LINE_SIZE, # set this to make dma work
            "sys_nw_flit_dwords" : 1,
            "sys_nw_obuf_dwords" : CACHE_LINE_SIZE//8,
            "sys_cp_present" : bool(ARGUMENTS.core_threads),
        })
        core.interface = core.core.setSubComponent("memory", "memHierarchy.standardInterface")
        core.nic = core.interface.setSubComponent("memlink", "memHierarchy.MemNIC")
        core.nic.addParams({
            "group" : "0",
            "network_bw" : NETWORK_BANDWIDTH,
            "destinations" : "1",
            "debug_level" : NETWORK_DEBUG_LEVEL,
            "debug" : 1,
        })
        return core

class HostCoreBuilder(Identifiable):
    """
    Builds a host core.
    """
    def __init__(self, xdim, ydim, meshid):
        super().__init__(xdim, ydim, meshid)

    @property
    def core_id(self):
        return -1

    def build(self, x, y):
        core = Core()
        core.core = sst.Component(f"hostcore_{x}_{y}_mesh{self.meshid}", "Drv.DrvCore")
        core.core.addParams({
            "clock" : f'{CORE_CLOCK}Hz',
            "max_idle" : 2,
            "threads" : 1,
            "executable" : ARGUMENTS.with_command_processor,
            "argv" : ' '.join([ARGUMENTS.program] + ARGUMENTS.argv),
            "id" : self.core_id,
            "pod" : 0,
            "pxn" : 0,
            "sys_num_pxn" : 1,
            "sys_pxn_pods" : 1,
            "sys_pod_cores_x" : CORES_X,
            "sys_pod_cores_y" : CORES_Y,
            "sys_core_threads" : 1,
            "sys_core_clock" : f'{CORE_CLOCK}Hz',
            "sys_core_l1sp_size" : L1SPBuilder.size,
            "sys_pod_l2sp_size" : 0,
            "sys_pod_l2sp_banks" : 0,
            "sys_pod_l2sp_interleave_size" : 0,
            "sys_pxn_dram_size" : MEMSIZE,
            "sys_pxn_dram_ports" : 1,
            "sys_pxn_dram_cache_banks" : VICTIM_CACHES,            
            "sys_pxn_dram_interleave_size" : CACHE_LINE_SIZE, # set this to make dma work
            "sys_nw_flit_dwords" : 1,
            "sys_nw_obuf_dwords" : CACHE_LINE_SIZE//8,
            "sys_cp_present" : bool(ARGUMENTS.with_command_processor),
        })
        core.memory = core.core.setSubComponent("memory", "Drv.DrvStdMemory")
        core.interface = core.memory.setSubComponent("memory", "memHierarchy.standardInterface")
        core.nic = core.interface.setSubComponent("memlink", "memHierarchy.MemNIC")
        core.nic.addParams({
            "group" : "0",
            "network_bw" : NETWORK_BANDWIDTH,
            "destinations" : "0,1",
            "debug_level" : NETWORK_DEBUG_LEVEL,
            "debug" : 1,
        })
        return core
    
    
class MeshTile(object):
    """
    All mesh tiles have a router.
    """
    def __init__(self):
        self.router = None
        self.memory = None
        self.core = None
        self.visual_id = 'X'

    @property
    def network_interfaces(self):
        return {
            MeshBuilder.WEST  : (self.router, f"port{MeshBuilder.west_port()}", f'{CORE_CLOCK.cycle_ps}ps'),
            MeshBuilder.EAST  : (self.router, f"port{MeshBuilder.east_port()}", f'{CORE_CLOCK.cycle_ps}ps'),
            MeshBuilder.NORTH : (self.router, f"port{MeshBuilder.north_port()}", f'{CORE_CLOCK.cycle_ps}ps'),
            MeshBuilder.SOUTH : (self.router, f"port{MeshBuilder.south_port()}", f'{CORE_CLOCK.cycle_ps}ps'),
            MeshBuilder.LOCAL0 : (self.router, f"port{MeshBuilder.portof(MeshBuilder.LOCAL0)}", f'{CORE_CLOCK.cycle_ps}ps'),
            MeshBuilder.LOCAL1 : (self.router, f"port{MeshBuilder.portof(MeshBuilder.LOCAL1)}", f'{CORE_CLOCK.cycle_ps}ps'),
        }

    @property
    def north(self):
        return self.network_interfaces[MeshBuilder.NORTH]

    @property
    def south(self):
        return self.network_interfaces[MeshBuilder.SOUTH]

    @property
    def east(self):
        return self.network_interfaces[MeshBuilder.EAST]

    @property
    def west(self):
        return self.network_interfaces[MeshBuilder.WEST]

    @property
    def local0(self):
        return self.network_interfaces[MeshBuilder.LOCAL0]

    @property
    def local1(self):
        return self.network_interfaces[MeshBuilder.LOCAL1]
    
class MeshTileBuilder(Identifiable):
    """
    A base class for building a mesh tile.
    """
    def __init__(self, xdim, ydim, meshid):
        self.local_ports = 2
        super().__init__(xdim, ydim, meshid)

    @property
    def num_ports(self):
        return self.local_ports + 4

    def build_router(self, x, y):
        router = sst.Component(f"router_{x}_{y}_mesh{self.meshid}", "merlin.hr_router")
        router.addParams({
            "id" : self.id(x, y),
            "num_vns" : 2,
            "xbar_bw" : XBAR_BANDWIDTH,
            "link_bw" : XBAR_BANDWIDTH,
            "input_latency" : f'{0*CORE_CLOCK.cycle_ps}ps',
            "output_latency" : f'{0*CORE_CLOCK.cycle_ps}ps',
            "input_buf_size" : f"{64*2*3*8}B",
            "output_buf_size" : f"{64*2*3*8}B",
            "flit_size" : "8B",
            "num_ports" : self.num_ports,
        })
        topo = router.setSubComponent("topology", "merlin.mesh")
        topo.addParams({
            "shape" : f"{X}x{Y}",
            "width" : "1",
            "local_ports" : "2",
        })
        return router
            
    def build(self, x, y):
        mesh_tile = self.make_mesh_tile()
        mesh_tile.router = self.build_router(x, y)
        self.build_local_endpoints(x, y, mesh_tile)
        return mesh_tile

    def make_mesh_tile(self):
        raise NotImplementedError("MeshTileBuilder.make_mesh_tile")
    
    def build_local_endpoints(self, x, y, mesh_tile):
        raise NotImplementedError("MeshTile.build_local_endpoints")

class EmptyTile(MeshTile):
    """
    An empty tile has just a router.
    """
    def __init__(self):
        super().__init__()
        self.visual_id = '*'
    
class EmptyTileBuilder(MeshTileBuilder):
    """
    Builds an empty tile.
    """
    def __init__(self, xdim, ydim, meshid):
        super().__init__(xdim, ydim, meshid)

    def make_mesh_tile(self):
        return EmptyTile()

    def build_local_endpoints(self, x, y, tile):
        pass


class HostCoreTile(MeshTile):
    """
    A host core tile has a core.
    """
    def __init__(self):
        super().__init__()
        self.core = None
        self.visual_id = 'H'

class HostCoreTileBuilder(MeshTileBuilder):
    """
    Builds a host core tile.
    """
    def __init__(self, xdim, ydim, meshid):
        self.core = HostCoreBuilder(xdim, ydim, meshid)
        super().__init__(xdim, ydim, meshid)

    def make_mesh_tile(self):
        return HostCoreTile()

    def build_local_endpoints(self, x, y, tile):
        #print(f"HostCoreTile {x} {y}")
        tile.core = self.core.build(x, y)
        link = sst.Link(f"link_core_router_{x}_{y}_mesh{self.meshid}")
        link.connect(tile.core.network_interface, tile.local0)
    
class ComputeTile(MeshTile):
    """
    A compute tile has a core and memory.
    """
    def __init__(self):
        super().__init__()
        self.core = None
        self.memory = None
        self.visual_id = 'C'

class ComputeTileBuilder(MeshTileBuilder):
    """
    Builds a compute tile.
    """
    core_builder = DrvXCoreBuilder
    def __init__(self, xdim, ydim, meshid):
        self.core = self.core_builder(xdim, ydim, meshid)
        self.memory = L1SPBuilder(xdim, ydim, meshid)
        super().__init__(xdim, ydim, meshid)

    def make_mesh_tile(self):
        return ComputeTile()

    def build_local_endpoints(self, x, y, tile):
        #print(f"ComputeTile {x} {y}")
        tile.core = self.core.build(x, y)
        link = sst.Link(f"link_core_router_{x}_{y}_mesh{self.meshid}")
        link.connect(tile.core.network_interface, tile.local0)

        tile.memory = self.memory.build(x, y)
        link = sst.Link(f"link_router_memory_{x}_{y}_mesh{self.meshid}")
        link.connect(tile.memory.network_interface, tile.local1)

class VictimCache(object):
    """
    A victim cache
    """
    def __init__(self):
        self.cache = None
        self.cpulink = None
        self.memlink = None

    @property
    def network_interface(self):
        return (self.cpulink, "port", f'{CORE_CLOCK.cycle_ps}ps')

    @property
    def memory_interface(self):
        return (self.memlink, "port", f'{CORE_CLOCK.cycle_ps}ps')

class VictimCacheBuilder(Identifiable):
    """
    Builds a victim cache
    """
    sysconfig = None
    banks = 0
    memsize = MEMSIZE
    cache_line_size = CACHE_LINE_SIZE
    def __init__(self, xdim, ydim, meshid):
        super().__init__(xdim, ydim, meshid)

    def addressmap(self):
        return addressmap.AddressMap(self.sysconfig)

    @property
    def bank_size(self):
        return self.memsize // self.banks
    
    def build(self, x, y):
        start, stop, interleave, stride \
            = vcache_range(self.xdim, self.ydim, x, not (y==BASE_CORE_Y-1), \
                           self.memsize, self.cache_line_size)

        #print(f"VCACHE {x} {y} {start:x} {stop:x} {interleave:x} {stride:x}")
        
        victim_cache = VictimCache()
        victim_cache.cache = sst.Component(f"victim_cache_{x}_{y}_mesh{self.meshid}",\
                                           "memHierarchy.Cache")
        victim_cache.cache.addParams({
            "cache_frequency" : f'{CORE_CLOCK}Hz',
            "cache_size" : f'{VICTIM_CACHE_SIZE}B',
            "associativity" : VICTIM_CACHE_ASSOCIATIVITY,
            "access_latency_cycles" : '1',
            "replacement_policy" : "lru",
            "mshr_num_entries" : "2",
            "debug_level" : MEMORY_DEBUG_LEVEL,
            "debug" : 1,
            "L1" : "true",
            "coherence" : "mesi",
            "cache_type" : "inclusive",            
            "cache_line_size" : self.cache_line_size,
            "addr_range_start" : start,
            "addr_range_end" : stop,
            "interleave_size" : f'{interleave}B',
            "interleave_step" : f'{stride}B',
        })
        victim_cache.cpulink = victim_cache.cache.setSubComponent("cpulink", "memHierarchy.MemNIC")
        victim_cache.cpulink.addParams({
            "group" : 1,
            "network_bw" : NETWORK_BANDWIDTH,
        })
        victim_cache.memlink = victim_cache.cache.setSubComponent("memlink", "memHierarchy.MemLink")
        return victim_cache

class VictimCacheTile(MeshTile):
    """
    A vcache tile
    """
    def __init__(self):
        super().__init__()
        self.victim_cache = None
        self.visual_id = '$'

class VictimCacheTileBuilder(MeshTileBuilder):
    """
    Builds a vcache tile
    """
    def __init__(self, xdim, ydim, meshid):
        self.victim_cache_builder = VictimCacheBuilder(xdim, ydim, meshid)
        super().__init__(xdim, ydim, meshid)

    def make_mesh_tile(self):
        return VictimCacheTile()

    def build_local_endpoints(self, x, y, tile):
        tile.victim_cache = self.victim_cache_builder.build(x, y)
        link = sst.Link(f"link_router_memory_{x}_{y}_mesh{self.meshid}")
        link.connect(tile.victim_cache.network_interface, tile.local0)

def build_hammerblade(core_builder):
    mesh_builder = MeshBuilder(X, Y, 0)
    VictimCacheBuilder.sysconfig = sysconfig()
    VictimCacheBuilder.memsize = MEMSIZE
    VictimCacheBuilder.banks = 2*X

    ComputeTileBuilder.core_builder = core_builder
    
    # create the memory address range
    start, stop, interleave, stride \
        = dram_range(0, 1, MEMSIZE, VictimCacheBuilder.cache_line_size)

    #print(f"DRAM {start:x} {stop:x} {interleave:x} {stride:x}")
    
    if (ARGUMENTS.with_command_processor):
        mesh_builder.tile_builder[hostcore_coordinate()] = HostCoreTileBuilder
    
    # create the mesh
    for (x,y) in core_coordinates():
        mesh_builder.tile_builder[(x,y)] = ComputeTileBuilder
    
    # south and north victim caches
    for (x,y) in vcache_coordinates():
        mesh_builder.tile_builder[(x,y)]   = VictimCacheTileBuilder

    MirandaCoreBuilder.max_address = stop-8
    MirandaCoreBuilder.min_address = start

    # create a memory
    memory = sst.Component("memory", "memHierarchy.CoherentMemController")
    memory.addParams({
        "clock" : f'{MEMORY_CLOCK}Hz',
        "addr_range_start" : start,
        "addr_range_end" : stop,
        "interleave_size" : f'{interleave}B',
        "interleave_step" : f'{stride}B',
        "max_requests_per_cycle" : 1,
        "debug_level" : MEMORY_DEBUG_LEVEL,
        "debug" : 1,
    })

    backend = memory.setSubComponent("backend", "Drv.DrvSimpleMemBackend")
    backend.addParams({
        "mem_size" : f"{VictimCacheBuilder.memsize}B",
        "access_time" : f'{MEMORY_CLOCK.cycle_ps * 2}ps',
    })

    cmdhandler = memory.setSubComponent("customCmdHandler", "Drv.DrvCmdMemHandler")
    cmdhandler.addParams({
        "cache_line_size" : VictimCacheBuilder.cache_line_size,
        "shootdowns" : "true",
    })

    memlink = memory.setSubComponent("cpulink", "memHierarchy.MemLink")

    # create a bus
    bus = sst.Component("bus", "memHierarchy.Bus")
    bus.addParams({
        "bus_frequency" : f'{CORE_CLOCK}Hz',
        "bus_latency" : f'{CORE_CLOCK.cycle_ps}ps',
    })

    # connect memory to bus
    link = sst.Link("link_memory_bus")
    link.connect(
        (memlink, "port", f'{CORE_CLOCK.cycle_ps}ps'),
        (bus, "low_network_0", f'{CORE_CLOCK.cycle_ps}ps')
    )

    # build the mesh
    mesh = mesh_builder.build()

    # connect bus to vcs
    for (i, (x,y)) in enumerate(vcache_coordinates()):
        # connect vc to memory backend
        vc_tile = mesh.tiles[(x,y)]
        link = sst.Link(f"link_vc_memory_{x}_{y}_mesh0")
        link.connect(vc_tile.victim_cache.memory_interface, (bus, f"high_network_{i}", "1ns"))

        
    mesh_str = ""
    for y in range(Y):
        for x in range(X):
            mesh_str += f'{mesh.tiles[(x,y)].visual_id} '
        mesh_str += "\n"

    print(
        f"""
        Core Clock: {CORE_CLOCK} Hz
        Pod Size: {CORES_X}x{CORES_Y}
        L1SP Size: {L1SPBuilder.size}
        DRAM Clock: {MEMORY_CLOCK} Hz
        DRAM Size: {MEMSIZE}
        $-Assoc: {VICTIM_CACHE_ASSOCIATIVITY}
        $-Size:  {VICTIM_CACHE_SIZE*VICTIM_CACHES} B
        """)
    print(mesh_str)
    
