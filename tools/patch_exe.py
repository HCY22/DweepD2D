"""Make Dweep_D2D.exe: a copy of Dweep.exe whose DirectDraw import points at DWGFX.dll.

Only the import name string changes ("DDRAW.dll" -> "DWGFX.dll", same length); the original exe is not touched.
"""
import struct
import sys


def main(src, dst):
    d = bytearray(open(src, 'rb').read())
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    secs = []
    for i in range(nsec):
        o = opt + optsz + 40 * i
        vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, max(vsz, rsz), rptr))

    def off(rva):
        for va, sz, rptr in secs:
            if va <= rva < va + sz:
                return rva - va + rptr
        raise ValueError(hex(rva))

    imp_rva = struct.unpack_from('<I', d, opt + 104)[0]
    o = off(imp_rva)
    done = False
    while True:
        name_rva = struct.unpack_from('<I', d, o + 12)[0]
        if not name_rva:
            break
        no = off(name_rva)
        name = bytes(d[no:no + 16]).split(b'\0')[0]
        if name.upper() == b'DDRAW.DLL':
            d[no:no + 9] = b'DWGFX.dll'
            done = True
        o += 20
    if not done:
        sys.exit('DDRAW.dll import not found')
    open(dst, 'wb').write(d)
    print('wrote', dst)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
