"""Tiny PE helper: map virtual addresses of Dweep.exe to file data."""
import os
import struct

EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'game', 'Dweep.exe')


class PE:
    def __init__(self, path=EXE):
        self.d = open(path, 'rb').read()
        d = self.d
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<I', d, opt + 28)[0]
        self.secs = []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode('latin1')
            vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((name, va, vsz, rptr, rsz))

    def off(self, va):
        rva = va - self.base
        for name, sva, vsz, rptr, rsz in self.secs:
            if sva <= rva < sva + max(vsz, rsz):
                return rva - sva + rptr if rva - sva < rsz else None
        return None

    def read(self, va, n):
        o = self.off(va)
        return self.d[o:o + n] if o is not None else bytes(n)

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def cstr(self, va, n=256):
        return self.read(va, n).split(b'\0')[0].decode('latin1')
