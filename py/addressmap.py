import enum

class Bitfield(object):
    def __init__(self, hi, lo):
        """
        @brief Constructor
        @param hi: the high bit
        @param lo: the low bit
        """
        self._hi = hi
        self._lo = lo

    def lo(self):
        """
        @brief get the low bit
        """
        return self._lo

    def hi(self):
        """
        @brief get the high bit
        """
        return self._hi

    def bits(self):
        """
        @brief get the number of bits
        """
        return self.hi() - self.lo() + 1

    def mask(self):
        """
        @brief get the mask
        """
        return (1 << self.bits()) - 1

    def get(self, word):
        """
        @brief get the value of the bitfield from a word
        @param word: the word to extract the bitfield from
        """
        return (word >> self.lo()) & self.mask()

    def set(self, word, value):
        """
        @brief set the value of the bitfield in a word
        @param word: the word to set the bitfield in
        @param value: the value to set
        """
        word &= ~(self.mask() << self.lo())
        word |= (value & self.mask()) << self.lo()
        return word

    def __call__(self, word):
        """
        @brief get the value of the bitfield from a word
        @param word: the word to extract the bitfield from
        """
        return self.get(word)


class AddressType(enum.Enum):
    L1SP = 0
    L2SP = 1
    DRAM = 2
    CTRL = 3

class AddressInfo(object):
    def __init__(self):
        """
        @brief Constructor
        """
        self._type = AddressType.CTRL
        self._is_absolute = False
        self._offset = 0
        self._pxn = 0
        self._pod = 0
        self._core = 0

    def is_l1sp(self):
        """
        @brief is this address in the L1 scratchpad?
        """
        return self._type == AddressType.L1SP

    def set_l1sp(self):
        """
        @brief set the L1 scratchpad bit
        """
        self._type = AddressType.L1SP
        return self

    def is_l2sp(self):
        """
        @brief is this address in the L2 scratchpad?
        """
        return self._type == AddressType.L2SP

    def set_l2sp(self):
        """
        @brief set the L2 scratchpad bit
        """
        self._type = AddressType.L2SP
        return self

    def is_dram(self):
        """
        @brief is this address in the DRAM?
        """
        return self._type == AddressType.DRAM

    def set_dram(self):
        """
        @brief set the DRAM bit
        """
        self._type = AddressType.DRAM
        return self

    def is_core_ctrl(self):
        """
        @brief is this address in the core control registers?
        """
        return self._type == AddressType.CTRL

    def set_core_ctrl(self):
        """
        @brief set the core control bit
        """
        self._type = AddressType.CTRL
        return self

    def is_absolute(self):
        """
        @brief is this address absolute?
        """
        return self._is_absolute

    def set_absolute(self):
        """
        @brief set the absolute bit
        """
        self._is_absolute = True
        return self

    def is_relative(self):
        """
        @brief is this address relative?
        """
        return not self._is_absolute

    def set_relative(self):
        """
        @brief set the relative bit
        """
        self._is_absolute = False
        return self

    def offset(self):
        """
        @brief get the offset
        """
        return self._offset

    def set_offset(self, offset):
        """
        @brief set the offset
        """
        self._offset = offset
        return self

    def pxn(self):
        """
        @brief get the PXN bit
        """
        return self._pxn

    def set_pxn(self, pxn):
        """
        @brief set the PXN bit
        """
        self._pxn = pxn
        return self

    def pod(self):
        """
        @brief get the POD bit
        """
        return self._pod

    def set_pod(self, pod):
        """
        @brief set the POD bit
        """
        self._pod = pod
        return self

    def core(self):
        """
        @brief get the core bit
        """
        return self._core

    def set_core(self, core):
        """
        @brief set the core bit
        """
        self._core = core
        return self

    def __str__(self):
        """
        @brief encode as a string
        """
        s = ""
        if self.is_absolute():
            s += "{ABSOLUTE,"
            if self.is_dram():
                s += "DRAM,"
                s += "PXN=%d," % self.pxn()
                s += "0x" + hex(self.offset())
            elif self.is_l2sp():
                s += "L2SP,"
                s += "PXN=%d," % self.pxn()
                s += "POD=%d," % self.pod()
                s += "0x" + hex(self.offset())
            elif self.is_l1sp():
                s += "L1SP,"
                s += "PXN=%d," % self.pxn()
                s += "POD=%d," % self.pod()
                s += "CORE=%d," % self.core()
                s += "0x" + hex(self.offset())
            elif self.is_core_ctrl():
                s += "CTRL,"
                s += "PXN=%d," % self.pxn()
                s += "POD=%d," % self.pod()
                s += "CORE=%d," % self.core()
                s += "0x" + hex(self.offset())
            s += "}"
        else:
            s += "{RELATIVE,"
            if self.is_dram():
                s += "DRAM,"
                s += "0x" + hex(self.offset())
            elif self.is_l2sp():
                s += "L2SP,"
                s += "PXN=%d," % self.pxn()
                s += "POD=%d," % self.pod()
                s += "0x" + hex(self.offset())
            elif self.is_l1sp():
                s += "L1SP,"
                s += "PXN=%d," % self.pxn()
                s += "POD=%d," % self.pod()
                s += "CORE=%d," % self.core()
                s += "0x" + hex(self.offset())
            elif self.is_core_ctrl():
                raise ValueError("relative address cannot be core control")
            s += "}"
        return s

class AddressMap(object):
    def __init__(self, sysconfig):
        """
        @brief Constructor
        @param my_pxn: the pxn of this core
        @param my_pod: the pod of this core
        @param my_core: the core id
        """
        self._is_absolute = Bitfield(63, 63)

        # absolute encoding
        self._absolute_is_dram = Bitfield(62, 62)
        self._absolute_is_l2sp = Bitfield(61, 61)
        self._absolute_is_ctrl = Bitfield(29, 29)
        pxn_bits = int.bit_length(sysconfig.pxns()-1)
        pod_bits = int.bit_length(sysconfig.pods()-1)
        core_bits = int.bit_length(sysconfig.cores()-1)
        self._absolute_pxn = Bitfield(self._absolute_is_l2sp.lo()-1,
                                      self._absolute_is_l2sp.lo()-pxn_bits)
        self._absolute_pod = Bitfield(self._absolute_pxn.lo()-1,
                                      self._absolute_pxn.lo()-pod_bits)
        self._absolute_core = Bitfield(self._absolute_pod.lo()-1,
                                        self._absolute_pod.lo()-core_bits)
        self._absolute_dram_offset = Bitfield(self._absolute_pxn.lo()-1, 0)
        self._absolute_l2sp_offset = Bitfield(self._absolute_pod.lo()-1, 0)
        self._absolute_l1sp_offset = Bitfield(self._absolute_is_ctrl.lo()-1, 0)

        # relative encoding
        self._relative_is_dram = Bitfield(30, 30)
        self._relative_is_l2sp = Bitfield(29, 29)
        self._relative_l1sp_offset = Bitfield(28, 0)
        self._relative_l2sp_offset = Bitfield(28, 0)
        self._relative_dram_offset = Bitfield(29, 0)

    def decode(self, address, my_pxn, my_pod, my_core):
        """
        @brief decode an address
        @param address_info: the address info
        @param my_pxn: the pxn of this core
        @param my_pod: the pod of this core

        @return the decoded address
        """
        if (self._is_absolute(address)):
            return self.decode_absolute(address_info)
        else:
            return self.decode_relative(address_info, my_pxn, my_pod, my_core)

    def decode_absolute(self, info):
        """
        @brief decode an absolute address
        @param address_info: the address info
        """
        info = AddressInfo()
        info.set_absolute()
        if self._absolute_is_dram(address):
            info.set_dram()
            info.set_pxn(self._absolute_pxn(address))
            info.set_offset(self._absolute_dram_offset(address))
        elif self._absolute_is_l2sp(address):
            info.set_l2sp()
            info.set_pxn(self._absolute_pxn(address))
            info.set_pod(self._absolute_pod(address))
            info.set_offset(self._absolute_l2sp_offset(address))
        elif self._absolute_is_ctrl(address):
            info.set_core_ctrl()
            info.set_pxn(self._absolute_pxn(address))
            info.set_pod(self._absolute_pod(address))
            info.set_core(self._absolute_core(address))
            info.set_offset(self._absolute_l1sp_offset(address))
        else:
            info.set_l1sp()
            info.set_pxn(self._absolute_pxn(address))
            info.set_pod(self._absolute_pod(address))
            info.set_core(self._absolute_core(address))
            info.set_offset(self._absolute_l1sp_offset(address))
        return info

    def decode_relative(self, info, my_pxn, my_pod, my_core):
        """
        @brief decode a relative address
        @param address_info: the address info
        @param my_pxn: the pxn of this core
        @param my_pod: the pod of this core
        @param my_core: the core id
        """
        info = AddressInfo()
        info.set_relative()
        if self._relative_is_dram(address):
            info.set_dram()
            info.set_offset(self._relative_dram_offset(address))
        elif self._relative_is_l2sp(address):
            info.set_l2sp()
            info.set_offset(self._relative_l2sp_offset(address))
        else:
            info.set_l1sp()
            info.set_offset(self._relative_l1sp_offset(address))
        return info

    def encode(self, address_info, my_pxn=0, my_pod=0, my_core=0):
        """
        @brief encode an address
        @param address_info: the address info
        @param my_pxn: the pxn of this core
        @param my_pod: the pod of this core
        @param my_core: the core id
        """
        if address_info.is_absolute():
            return self.encode_absolute(address_info)
        else:
            return self.encode_relative(address_info, my_pxn, my_pod, my_core)

    def encode_absolute(self, info):
        """
        @brief encode an absolute address
        @param address_info: the address info
        """
        address = 0
        address = self._is_absolute.set(address, 1)
        if info.is_dram():
            address = self._absolute_is_dram.set(address, 1)
            address = self._absolute_pxn.set(address, info.pxn())
            address = self._absolute_dram_offset.set(address, info.offset())
        elif info.is_l2sp():
            address = self._absolute_is_l2sp.set(address, 1)
            address = self._absolute_pxn.set(address, info.pxn())
            address = self._absolute_pod.set(address, info.pod())
            address = self._absolute_l2sp_offset.set(address, info.offset())
        elif info.is_l1sp():
            address = self._absolute_pxn.set(address, info.pxn())
            address = self._absolute_pod.set(address, info.pod())
            address = self._absolute_core.set(address, info.core())
            address = self._absolute_l1sp_offset.set(address, info.offset())
        elif info.is_core_ctrl():
            address = self._absolute_is_ctrl.set(address, 1)
            address = self._absolute_pxn.set(address, info.pxn())
            address = self._absolute_pod.set(address, info.pod())
            address = self._absolute_core.set(address, info.core())
            address = self._absolute_l1sp_offset.set(address, info.offset())
        else:
            raise ValueError("Invalid address type")

        return address

    def encode_relative(self, info, my_pxn, my_pod, my_core):
        """
        @brief encode a relative address
        @param address_info: the address info
        @param my_pxn: the pxn of this core
        @param my_pod: the pod of this core
        @param my_core: the core id
        """
        address = 0
        if info.is_dram():
            address = self._relative_is_dram.set(address, 1)
            address = self._relative_dram_offset.set(address, info.offset())
        elif info.is_l2sp():
            address = self._relative_is_l2sp.set(address, 1)
            address = self._relative_l2sp_offset.set(address, info.offset())
        elif info.is_l1sp():
            address = self._relative_l1sp_offset.set(address, info.offset())
        else:
            raise ValueError("Invalid address type")

        return address

class AddressRangeBuilder(object):
    def __init__(self, address_map, memsize, interleave_size=0, interleave_step=0):
        """
        @brief constructor
        @param address_map: the address map
        """
        self._address_map = address_map
        self._memsize = memsize
        self._interleave_size = interleave_size
        self._interleave_step = interleave_step

    def banks(self):
        """
        @brief get the number of banks
        """
        if self._interleave_size == 0:
            return 1

        return self._interleave_step//self._interleave_size

    def total_size(self):
        """
        @brief get the total size of the address range (all banks)
        """
        return self._memsize * self.banks()

    def interleaved_address_range(self, bank):
        """
        Returns the address info for start and end addresses for this bank
        """
        banks = self.banks()
        total_size = self.total_size()
        start = bank * self._interleave_size
        end = total_size - (banks - bank - 1) * self._interleave_size - 1
        start_info = AddressInfo().set_absolute().set_offset(start)
        stop_info  = AddressInfo().set_absolute().set_offset(end)
        return (start_info, stop_info)

    def call_outputs(self, start_info, stop_info):
        """
        return the outputs from the __call__ method
        """
        return (self._address_map.encode(start_info), self._address_map.encode(stop_info), self._interleave_size, self._interleave_step)

class L1SPAddressBuilder(AddressRangeBuilder):
    def __init__(self, address_map, memsize):
        """
        @brief constructor
        @param address_map: the address map
        @param memsize: the size of the memory
        """
        super().__init__(address_map, memsize)

    def __call__(self, pxn, pod, core):
        """
        @brief build an address range for a L1SP memory
        @param pxn: the pxn
        @param pod: the pod
        @param core: the core
        @return (first byte, last byte)
        """
        start_info, stop_info = self.interleaved_address_range(0)
        start_info.set_l1sp().set_pxn(pxn).set_pod(pod).set_core(core)
        stop_info.set_l1sp().set_pxn(pxn).set_pod(pod).set_core(core)
        return self.call_outputs(start_info, stop_info)

class L2SPAddressBuilder(AddressRangeBuilder):
    def __init__(self, address_map, memsize, interleave_size, interleave_step):
        """
        @brief constructor
        @param address_map: the address map
        @param interleave_size: the size of the interleave
        @param interleave_step: the step of the interleave
        @param memsize: the size of the memory
        """
        super().__init__(address_map, memsize, interleave_size, interleave_step)


    def __call__(self, pxn, pod, bank):
        """
        @brief build an address range for a L2SP memory
        @param pxn: the pxn
        @param pod: the pod
        @param bank: the bank
        @return (first byte, last byte)
        """
        start_info, stop_info = self.interleaved_address_range(bank)
        start_info.set_l2sp().set_pxn(pxn).set_pod(pod)
        stop_info.set_l2sp().set_pxn(pxn).set_pod(pod)
        return self.call_outputs(start_info, stop_info)


class DRAMAddressBuilder(AddressRangeBuilder):
    def __init__(self, address_map, memsize, interleave_size, interleave_step):
        """
        @brief constructor
        @param address_map: the address map
        @param interleave_size: the size of the interleave
        @param interleave_step: the step of the interleave
        @param memsize: the size of the memory
        """
        super().__init__(address_map, memsize, interleave_size, interleave_step)

    def __call__(self, pxn, bank):
        """
        @brief build an address range for a DRAM memory
        @param bank: the bank
        @return (first byte, last byte)
        """
        start_info, stop_info = self.interleaved_address_range(bank)
        start_info.set_dram().set_pxn(pxn)
        stop_info.set_dram().set_pxn(pxn)
        return (self._address_map.encode(start_info), self._address_map.encode(stop_info), self._interleave_size, self._interleave_step)

class CHeaderBuilder(object):
    def __init__(self, address_map):
        self._address_map = address_map
        self._header = """
/* This file is automatically generated by the address map generator */
#ifndef __DRV_ADDRESS_MAP_H__
#define __DRV_ADDRESS_MAP_H__

/* Is absolute bit */
{define_is_absolute_bits:}

/* Absolute address bits */
{define_absolute_is_dram_bits:}
{define_absolute_is_l2sp_bits:}
{define_absolute_is_ctrl_bits:}
{define_absolute_pxn_bits:}
{define_absolute_pod_bits:}
{define_absolute_core_bits:}
{define_absolute_dram_offset_bits:}
{define_absolute_l2sp_offset_bits:}
{define_absolute_l1sp_offset_bits:}
{define_absolute_ctrl_offset_bits:}

/* Relative address bits */
{define_relative_is_dram_bits:}
{define_relative_is_l2sp_bits:}
{define_relative_l1sp_offset_bits:}
{define_relative_l2sp_offset_bits:}
{define_relative_dram_offset_bits:}
#endif
        """

    def __call__(self):
        return self._header.format(**self.make_defines())

    def make_defines(self):
        return {
            "define_is_absolute_bits"          : self.make_define("IS_ABSOLUTE", self._address_map._is_absolute),
            "define_absolute_is_dram_bits"     : self.make_define("ABSOLUTE_IS_DRAM", self._address_map._absolute_is_dram),
            "define_absolute_is_l2sp_bits"     : self.make_define("ABSOLUTE_IS_L2SP", self._address_map._absolute_is_l2sp),
            "define_absolute_is_ctrl_bits"     : self.make_define("ABSOLUTE_IS_CTRL", self._address_map._absolute_is_ctrl),
            "define_absolute_pxn_bits"         : self.make_define("ABSOLUTE_PXN", self._address_map._absolute_pxn),
            "define_absolute_pod_bits"         : self.make_define("ABSOLUTE_POD", self._address_map._absolute_pod),
            "define_absolute_core_bits"        : self.make_define("ABSOLUTE_CORE", self._address_map._absolute_core),
            "define_absolute_dram_offset_bits" : self.make_define("ABSOLUTE_DRAM_OFFSET", self._address_map._absolute_dram_offset),
            "define_absolute_l2sp_offset_bits" : self.make_define("ABSOLUTE_L2SP_OFFSET", self._address_map._absolute_l2sp_offset),
            "define_absolute_l1sp_offset_bits" : self.make_define("ABSOLUTE_L1SP_OFFSET", self._address_map._absolute_l1sp_offset),
            "define_absolute_ctrl_offset_bits" : self.make_define("ABSOLUTE_CTRL_OFFSET", self._address_map._absolute_l1sp_offset),
            "define_relative_is_dram_bits"     : self.make_define("RELATIVE_IS_DRAM", self._address_map._relative_is_dram),
            "define_relative_is_l2sp_bits"     : self.make_define("RELATIVE_IS_L2SP", self._address_map._relative_is_l2sp),
            "define_relative_l1sp_offset_bits" : self.make_define("RELATIVE_L1SP_OFFSET", self._address_map._relative_l1sp_offset),
            "define_relative_l2sp_offset_bits" : self.make_define("RELATIVE_L2SP_OFFSET", self._address_map._relative_l2sp_offset),
            "define_relative_dram_offset_bits" : self.make_define("RELATIVE_DRAM_OFFSET", self._address_map._relative_dram_offset),
        }

    def make_define(self, macro, bitfield):
        return "#define {}_HI {}ul\n#define {}_LO {}ul".format(
            macro, bitfield.hi(),
            macro, bitfield.lo())

class LdScriptBuilder(object):
    def __init__(self, address_map):
        self._address_map = address_map

    def encode(self, address_info):
        return self._address_map.encode(address_info)

    def l1sp_start(self):
        return self.encode(AddressInfo().set_relative().set_l1sp().set_offset(0))

    def l1sp_size(self):
        return 1 << self._address_map._relative_l1sp_offset.bits()

    def l2sp_start(self):
        return self.encode(AddressInfo().set_relative().set_l2sp().set_offset(0))

    def l2sp_size(self):
        return 1 << self._address_map._relative_l2sp_offset.bits()

    def dram_start(self):
        return self.encode(AddressInfo().set_relative().set_dram().set_offset(0))

    def dram_size(self):
        return 1 << self._address_map._relative_dram_offset.bits()

    def dram_text_start(self):
        return self.dram_start()

    def dram_text_size(self):
        return 8 * (2**20)

    def dram_data_start(self):
        return self.dram_text_start() + self.dram_text_size()

    def dram_data_size(self):
        return self.dram_size() - self.dram_text_size()

    def memory(self):
        body = """
L1SP_VMA (rw)    : ORIGIN = 0x{L1SP_START:08x}, LENGTH = 0x{L1SP_SIZE:08x}
L2SP_VMA (rw)    : ORIGIN = 0x{L2SP_START:08x}, LENGTH = 0x{L2SP_SIZE:08x}
DRAM_T_VMA (rwx) : ORIGIN = 0x{DRAM_T_START:08x}, LENGTH = 0x{DRAM_T_SIZE:08x}
DRAM_D_VMA (rwx) : ORIGIN = 0x{DRAM_D_START:08x}, LENGTH = 0x{DRAM_D_SIZE:08x}"""\
            .format(
                L1SP_START = self.l1sp_start(),
                L1SP_SIZE  = self.l1sp_size(),
                L2SP_START = self.l2sp_start(),
                L2SP_SIZE  = self.l2sp_size(),
                DRAM_T_START = self.dram_text_start(),
                DRAM_T_SIZE  = self.dram_text_size(),
                DRAM_D_START = self.dram_data_start(),
                DRAM_D_SIZE  = self.dram_data_size()
            )
        return "MEMORY\n{" + body + "\n}\n"

    def l1sp_sections(self):
        return """
.l1sp :
{
*(.l1sp.interrupt)
*(.l1sp)
*(.l1sp.*)
. = ALIGN(16);
} > L1SP_VMA"""

    def l2sp_sections(self):
        return """
.l2sp :
{
*(.l2sp.interrupt)
*(.l2sp)
*(.l2sp.*)
. = ALIGN(16);
}> L2SP_VMA"""

    def dram_sections(self):
        return """
.text.dram :
{
*(.text.interrupt)
*(.crtbegin)
*(.text)
*(.text.startup)
*(.text.*)
. = ALIGN(16);
} > DRAM_T_VMA

.eh_frame.dram :
{
*(.eh_frame)
*(.eh_frame*)
. = ALIGN(16);
} > DRAM_D_VMA

.rodata.dram :
{
*(.rodata)
*(.rodata.*)
*(.srodata.cst16)
*(.srodata.cst8)
*(.srodata.cst4)
*(.srodata.cst2)
*(.srodata)
. = ALIGN(16);
} > DRAM_D_VMA

.data.dram :
{
*(.dram)
*(.dram.*)
*(.data)
*(.data*)
. = ALIGN(16);
} > DRAM_D_VMA

.sdata.dram :
{
*(.sdata)
*(.sdata.*)
*(.sdata*)
*(.sdata*.gnu.linkonce.s.*)
*(.sbss)
*(.sbss*)
*(.gnu.linkonce.sb.*)
*(.scommon)
. = ALIGN(16);
} > DRAM_D_VMA

.bss.dram :
{
*(.bss)
*(.bss*)
. = ALIGN(16);
} > DRAM_D_VMA
"""

    def sections(self):
        body =  '\n\n'.join([self.l1sp_sections(), self.l2sp_sections(), self.dram_sections(), self.symbols()])
        return "SECTIONS\n{" + body + "\n}\n"

    def symbols(self):
        return '\n'.join([
            "__global_pointer$ = 0x{:08x};".format(self.dram_start()),
            "_end = .;",
            "end = .;",
            "_edata = .;",
            "ENTRY(_start)"
        ])

    def __call__(self):
        return self.memory() + "\n" + self.sections()


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("output", choices=["ldscript","cheader"])
    parser.add_argument("--core-threads", type=int, default=1)
    parser.add_argument("--pod-cores", type=int, default=1)
    parser.add_argument("--pxn-pods", type=int, default=1)
    parser.add_argument("--num-pxn", type=int, default=1)
    arguments = parser.parse_args()

    class sysconfig(object):
        def pxns(self):
            return arguments.num_pxn
        def pods(self):
            return arguments.pxn_pods
        def cores(self):
            return arguments.pod_cores

    adressmap = AddressMap(sysconfig())

    if arguments.output == "ldscript":
        builder = LdScriptBuilder(adressmap)
        print(builder())

    elif arguments.output == "cheader":
        builder = CHeaderBuilder(adressmap)
        print(builder())
