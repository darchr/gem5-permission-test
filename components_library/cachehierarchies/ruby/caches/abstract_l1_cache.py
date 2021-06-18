from abc import abstractmethod
from components_library.isas import ISA
from components_library.processors.cpu_types import CPUTypes

from m5.objects import L1Cache_Controller,


from components_library.processors.abstract_core import AbstractCore

import math

class AbstractL1Cache(L1Cache_Controller):

    _version = 0

    @classmethod
    def versionCount(cls):
        cls._version += 1  # Use count for this particular type
        return cls._version - 1

    # TODO: I don't love that we have to pass in the cache line size.
    # However, we need some way to set the index bits
    def __init__(self, network, cache_line_size):
        """ """
        super(AbstractL1Cache, self).__init__()

        self.version = self.versionCount()
        self._cache_line_size = cache_line_size
        self.connectQueues(network)

    def getBlockSizeBits(self):
        bits = int(math.log(self._cache_line_size, 2))
        if 2 ** bits != self._cache_line_size.value:
            panic("Cache line size not a power of 2!")
        return bits

    def sendEvicts(self, core: AbstractCore, target_isa: ISA):
        """True if the CPU model or ISA requires sending evictions from caches
        to the CPU. Two scenarios warrant forwarding evictions to the CPU:
        1. The O3 model must keep the LSQ coherent with the caches
        2. The x86 mwait instruction is built on top of coherence
        3. The local exclusive monitor in ARM systems
        """
        if core.get_type() is CPUTypes.O3 or target_isa in (ISA.X86, ISA.ARM):
            return True
        return False

    @abstractmethod
    def connectQueues(self, network):
        """Connect all of the queues for this controller."""
        pass