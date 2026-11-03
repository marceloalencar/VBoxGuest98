"""Builds the installer's 1.44 MB floppy image, for guests that have no
VBoxGuest98 yet (no guest control): a FAT12 volume with the given files in
its root directory, read back and compared once written.

  mkfloppy.py IMG SRC[=NAME.EXT]...

build.cmd runs it for the free build:
  mkfloppy.py obj\\VBoxGuest98-x.y.z.img obj\\VBoxGuest98-x.y.z.exe=SETUP.EXE

The boot sector hands the boot over to the hard disk, so a floppy left in
the drive doesn't stop the guest from starting (VirtualBox tries the floppy
first).
"""
import os
import struct
import sys
import time

SECT = 512                              # bytes per sector, one sector per cluster
RES = 1                                 # reserved sectors (the boot sector)
NFAT = 2                                # FAT copies
SPF = 9                                 # sectors per FAT
NROOT = 224                             # root directory entries
TOTAL = 2880                            # sectors: 80 cylinders, 2 heads, 18 sectors
ROOT_SECT = RES + NFAT * SPF
DATA_SECT = ROOT_SECT + NROOT * 32 // SECT
BOOT_CODE_AT = 0x3e                     # after the extended BIOS parameter block


def short_name(name):
    """8.3 directory entry name."""
    base, _, ext = name.upper().partition('.')
    if not 1 <= len(base) <= 8 or len(ext) > 3:
        sys.exit('mkfloppy: %s is not an 8.3 name' % name)
    return (base.ljust(8) + ext.ljust(3)).encode('ascii')


def boot_code():
    """Real mode code at 7C3Eh: moves itself to 0600h, loads the hard disk's
    MBR at 7C00h and jumps to it with DL = 80h.  Without a hard disk it
    prints a message and halts."""
    msg = b'Not a boot disk. Remove the floppy and restart.\r\n\0'
    c = bytearray()
    c += b'\xfa\x31\xc0\x8e\xd0\xbc\x00\x7c\x8e\xd8\x8e\xc0\xfb'  # cli; ss = ds = es = 0; sp = 7C00h; sti
    c += b'\xbe\x00\x7c\xbf\x00\x06\xb9\x00\x01\xfc\xf3\xa5'      # copy the 512 bytes to 0600h
    jmp_at = len(c)
    c += b'\xea\0\0\0\0'                                          # jmp 0000:<next, at 0600h>
    nxt = len(c)
    c += b'\xbb\x00\x7c\xb8\x01\x02\xb9\x01\x00\xba\x80\x00'      # es:bx = 7C00h, 1 sector, C0 S1, H0 of 80h
    c += b'\xcd\x13\x72\x05'                                      # int 13h; jc err
    c += b'\xea\x00\x7c\x00\x00'                                  # jmp 0000:7C00h
    err = len(c)
    c += b'\xbe\0\0'                                              # mov si, msg
    c += b'\xac\x08\xc0\x74\x09'                                  # next char: lodsb; or al, al; jz halt
    c += b'\xb4\x0e\xbb\x07\x00\xcd\x10\xeb\xf2'                  # int 10h function 0Eh; jmp next char
    c += b'\xf4\xeb\xfd'                                          # halt: hlt; jmp halt
    struct.pack_into('<H', c, jmp_at + 1, 0x0600 + BOOT_CODE_AT + nxt)
    struct.pack_into('<H', c, err + 1, 0x0600 + BOOT_CODE_AT + len(c))
    c += msg
    assert BOOT_CODE_AT + len(c) <= 510
    return bytes(c)


def boot_sector():
    bs = bytearray(SECT)
    bs[0:3] = b'\xeb\x3c\x90'                                     # jmp short to the boot code
    bs[3:11] = b'MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', bs, 11, SECT, 1, RES, NFAT, NROOT, TOTAL, 0xF0, SPF, 18, 2, 0, 0)
    struct.pack_into('<BBBI11s8s', bs, 36, 0, 0, 0x29, 0x1234ABCD, b'VBOXGUEST98', b'FAT12   ')
    code = boot_code()
    bs[BOOT_CODE_AT:BOOT_CODE_AT + len(code)] = code
    bs[510:512] = b'\x55\xaa'
    return bs


def dos_time(t):
    """Directory entry time and date of a time.localtime() value."""
    return ((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2),
            ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday)


def build(files):
    """Returns the image of the (source path, 8.3 name) pairs."""
    data = bytearray(TOTAL * SECT)
    data[0:SECT] = boot_sector()
    fat = [0xFF0, 0xFFF]
    root = bytearray(NROOT * 32)
    for i, (src, name) in enumerate(files):
        with open(src, 'rb') as f:
            content = f.read()
        nclus = (len(content) + SECT - 1) // SECT
        first = len(fat) if nclus else 0
        if DATA_SECT + first + nclus - 2 > TOTAL:
            sys.exit('mkfloppy: the files don\'t fit on a 1.44 MB floppy')
        for n in range(nclus):
            fat.append(first + n + 1 if n < nclus - 1 else 0xFFF)
            off = (DATA_SECT + first + n - 2) * SECT
            data[off:off + SECT] = content[n * SECT:(n + 1) * SECT].ljust(SECT, b'\0')
        ftime, fdate = dos_time(time.localtime(os.path.getmtime(src)))
        struct.pack_into('<11sB10sHHHI', root, i * 32, short_name(name), 0x20, b'\0' * 10,
                         ftime, fdate, first, len(content))
    fatb = bytearray(SPF * SECT)
    for n in range(0, len(fat), 2):
        a = fat[n]
        b = fat[n + 1] if n + 1 < len(fat) else 0
        fatb[n * 3 // 2:n * 3 // 2 + 3] = bytes([a & 0xFF, ((a >> 8) & 0xF) | ((b & 0xF) << 4), b >> 4])
    for n in range(NFAT):
        off = (RES + n * SPF) * SECT
        data[off:off + len(fatb)] = fatb
    data[ROOT_SECT * SECT:ROOT_SECT * SECT + len(root)] = root
    return data


def read_file(data, entry):
    """Contents of a root directory entry, following the FAT."""
    first, size = struct.unpack_from('<HI', entry, 26)
    out, n = bytearray(), first
    while 2 <= n < 0xFF8 and len(out) < size:
        off = (DATA_SECT + n - 2) * SECT
        out += data[off:off + SECT]
        fat_off = RES * SECT + n * 3 // 2
        v = data[fat_off] | (data[fat_off + 1] << 8)
        n = (v >> 4) if n & 1 else (v & 0xFFF)
    return bytes(out[:size])


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    files = []
    for spec in argv[2:]:
        src, _, name = spec.partition('=')
        files.append((src, name or os.path.basename(src)))
    data = build(files)
    with open(argv[1], 'wb') as f:
        f.write(data)

    with open(argv[1], 'rb') as f:
        data = f.read()
    for i, (src, name) in enumerate(files):
        entry = data[ROOT_SECT * SECT + i * 32:ROOT_SECT * SECT + (i + 1) * 32]
        with open(src, 'rb') as f:
            if entry[0:11] != short_name(name) or read_file(data, entry) != f.read():
                sys.exit('mkfloppy: %s differs in %s' % (name, argv[1]))
        print('%s: %s, %d bytes' % (argv[1], name, struct.unpack_from('<I', entry, 28)[0]))


if __name__ == '__main__':
    if sys.version_info[0] < 3:
        sys.exit('mkfloppy: needs Python 3')
    main(sys.argv)
