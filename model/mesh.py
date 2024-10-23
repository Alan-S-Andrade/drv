import itertools
import sst
import enum

X = 1
Y = 2


CPU_VERBOSE_LEVEL = 1
NETWORK_DEBUG_LEVEL = 1
UPDATES_PER_CORE = 100

class Mesh(object):
    def __init__(self):
        self.tiles = {}

class MeshBuilder(object):
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
            (x,y) : ComputeTileBuilder for (x,y) in itertools.product(range(xdim), range(ydim))
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
    def __init__(self, xdim, ydim, meshid):
        self.xdim = xdim
        self.ydim = ydim
        self.meshid = meshid

    def id(self, x, y):
        # dimension order x than y
        return y*self.xdim + x

    def absid(self, x, y):
        return self.id(x, y) + self.meshid * self.xdim * self.ydim

class Memory(object):
    def __init__(self):
        self.controller = None
        self.backend = None
        self.nic = None    

    @property
    def network_interface(self):
        return (self.nic, "port", "1ns")
    
class MemoryBuilder(Identifiable):
    size = 1024
    def __init__(self, xdim, ydim, meshid):
        super().__init__(xdim, ydim, meshid)

    def build(self, x, y):
        memory = Memory()
        memory.controller = sst.Component(f"memory_{x}_{y}_mesh{self.meshid}", "memHierarchy.MemController")
        start = self.absid(x, y) * MemoryBuilder.size
        end = (self.absid(x, y) + 1) * MemoryBuilder.size - 1
        print(f"Memory {x} {y} {start:x}-{end:x}")
        memory.controller.addParams({
            "debug_level" : 10,
            "verbose" : 0,
            "clock" : "1GHz",
            "addr_range_start" : start,
            "addr_range_end" : end,
            "interleave_size" : f"{MemoryBuilder.size}B",
            "interleave_step" : f"{X*Y*MemoryBuilder.size}B",
        })
        memory.backend = memory.controller.setSubComponent("backend", "memHierarchy.simpleMem")
        memory.backend.addParams({
            "access_time" : "1ns",
            "mem_size" : f"{MemoryBuilder.size}B",
        })
        memory.nic = memory.controller.setSubComponent("cpulink", "memHierarchy.MemNIC")
        memory.nic.addParams({
            "group" : "1",
            "network_bw" : "1024GB/s",
            "sources" : "0",
            "debug_level" : NETWORK_DEBUG_LEVEL,
            "debug" : 1,
        })
        return memory

class Core(object):
    def __init__(self):
        self.core = None
        self.generator = None
        self.interface = None
        self.nic = None

    @property
    def network_interface(self):
        return (self.nic, "port", "1ns")

class CoreBuilder(Identifiable):
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
            "max_address" : MemoryBuilder.size * X * Y - 8,
            "count" : UPDATES_PER_CORE,
            "clock" : "1GHz",
            "seed_a" : self.id(x, y),
            "seed_b" : 7*self.id(x, y)+1,
        })
        core.interface = core.core.setSubComponent("memory", "memHierarchy.standardInterface")
        core.nic = core.interface.setSubComponent("memlink", "memHierarchy.MemNIC")
        core.nic.addParams({
            "group" : "0",
            "network_bw" : "1024GB/s",
            "destinations" : "1",
            "debug_level" : NETWORK_DEBUG_LEVEL,
            "debug" : 1,
        })
        return core

class MeshTile(object):
    def __init__(self):
        self.router = None
        self.memory = None
        self.core = None

    @property
    def network_interfaces(self):
        return {
            MeshBuilder.WEST  : (self.router, f"port{MeshBuilder.west_port()}", "1ns"),
            MeshBuilder.EAST  : (self.router, f"port{MeshBuilder.east_port()}", "1ns"),
            MeshBuilder.NORTH : (self.router, f"port{MeshBuilder.north_port()}", "1ns"),
            MeshBuilder.SOUTH : (self.router, f"port{MeshBuilder.south_port()}", "1ns"),
            MeshBuilder.LOCAL0 : (self.router, f"port{MeshBuilder.portof(MeshBuilder.LOCAL0)}", "1ns"),
            MeshBuilder.LOCAL1 : (self.router, f"port{MeshBuilder.portof(MeshBuilder.LOCAL1)}", "1ns"),
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
            "xbar_bw" : "1024GB/s",
            "link_bw" : "1024GB/s",
            "input_latency" : "1ns",
            "output_latency" : "1ns",
            "input_buf_size" : "1KB",
            "output_buf_size" : "1KB",
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

class ComputeTile(MeshTile):
    def __init__(self):
        super().__init__()
        self.core = None
        self.memory = None

class ComputeTileBuilder(MeshTileBuilder):
    def __init__(self, xdim, ydim, meshid):
        self.core = CoreBuilder(xdim, ydim, meshid)
        self.memory = MemoryBuilder(xdim, ydim, meshid)
        super().__init__(xdim, ydim, meshid)

    def make_mesh_tile(self):
        return ComputeTile()

    def build_local_endpoints(self, x, y, tile):
        print(f"ComputeTile {x} {y}")
        tile.core = self.core.build(x, y)
        link = sst.Link(f"link_core_router_{x}_{y}_mesh{self.meshid}")
        link.connect(tile.core.network_interface, tile.local0)

        tile.memory = self.memory.build(x, y)
        link = sst.Link(f"link_router_memory_{x}_{y}_mesh{self.meshid}")
        link.connect(tile.memory.network_interface, tile.local1)


class EmptyTile(MeshTile):
    def __init__(self):
        super().__init__()

class EmptyTileBuilder(MeshTileBuilder):
    def __init__(self, xdim, ydim, meshid):
        super().__init__(xdim, ydim, meshid)

    def make_mesh_tile(self):
        return EmptyTile()

    def build_local_endpoints(self, x, y, tile):
        pass


if 0:
    # unclear if this would work
    bidx = (0,1)

    mesh_builder0 = MeshBuilder(X, Y, 0)
    mesh_builder0.tile_builder[bidx] = EmptyTileBuilder
    mesh0 = mesh_builder0.build()

    mesh_builder1 = MeshBuilder(X, Y, 1)
    mesh_builder1.tile_builder[bidx] = EmptyTileBuilder
    mesh1 = mesh_builder1.build()

    # router = sst.Component("router", "merlin.hr_router")
    # router.addParams({
    #     "id" : 0,
    #     "num_vns" : 1,
    #     "xbar_bw" : "1024GB/s",
    #     "link_bw" : "1024GB/s",
    #     "input_latency" : "1ns",
    #     "output_latency" : "1ns",
    #     "input_buf_size" : "1KB",
    #     "output_buf_size" : "1KB",
    #     "flit_size" : "8B",
    #     "num_ports" : 2,
    # })
    # topo = router.setSubComponent("topology", "merlin.singlerouter")

    bridge = sst.Component("bridge", "merlin.Bridge")
    bridge.addParams({
        "translator" : "memHierarchy.MemNetBridge",
        "network_bw" : "1024GB/s",
    })

    link = sst.Link("link_router_mesh0")
    link.connect(mesh0.tiles[bidx].local0, (bridge, "network0", "1ns"))
    
    link = sst.Link("link_router_mesh1")
    link.connect(mesh1.tiles[bidx].local0, (bridge, "network1", "1ns"))
else:
    mesh_builder = MeshBuilder(X, Y, 0)
    mesh = mesh_builder.build()
