#!/usr/bin/env python3
"""
mkbootdisk.py — build a UEFI-bootable disk image for IRIS.

QEMU boots IRIS from a FAT filesystem it SYNTHESISES out of a host directory
(`-drive file=fat:rw:build/efi_root`).  Nothing else can do that: VirtualBox,
VMware and real firmware all want a partitioned disk with a real EFI System
Partition on it.  This writes one.

No mkfs, no mtools, no loop device and no root: the GPT and the FAT16 are both
written by hand, the same way scripts/mkdisk.py writes the data disk's GPT.
FAT16 rather than FAT32 because a 64 MiB partition with 4 KiB clusters is
16384 clusters -- inside FAT16's 4085..65524 range, and outside FAT32's
minimum, so FAT16 is the one of the two that is actually VALID at this size.
Every name here is 8.3, so no long-filename entries are needed.

Layout:
    LBA 0            protective MBR
    LBA 1            GPT header
    LBA 2..33        partition entry array
    LBA 2048..       the EFI System Partition (FAT16)
    last 33 sectors  backup entry array + header
"""
import os, struct, sys, zlib

SECTOR = 512
ESP_TYPE = bytes.fromhex('28732AC11FF8D211BA4B00A0C93EC93B')   # mixed-endian
ESP_UUID = bytes.fromhex('a1a2a3a4b1b2c1c2d1d2e1e2e3e4e5e6')

def gpt_crc(b): return zlib.crc32(b) & 0xFFFFFFFF

class Fat16:
    def __init__(self, total_sectors, spc=8):
        self.spc = spc
        self.total = total_sectors
        self.reserved = 1
        self.nfats = 2
        self.root_entries = 512
        self.root_sectors = (self.root_entries * 32 + SECTOR - 1) // SECTOR
        fat = 1
        for _ in range(64):                      # converge on the FAT size
            data = self.total - self.reserved - self.nfats * fat - self.root_sectors
            clusters = data // spc
            need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
            if need == fat: break
            fat = need
        self.fat_sectors = fat
        self.clusters = (self.total - self.reserved - self.nfats * fat
                         - self.root_sectors) // spc
        assert 4085 <= self.clusters <= 65524, "not a valid FAT16 (%d clusters)" % self.clusters
        self.fat = [0] * (self.clusters + 2)
        self.fat[0], self.fat[1] = 0xFFF8, 0xFFFF
        self.next_free = 2
        self.data = {}                            # cluster -> bytes
        self.root = []                            # 32-byte entries

    def alloc_chain(self, payload):
        """Write payload into a fresh cluster chain; return its first cluster."""
        size = self.spc * SECTOR
        chunks = [payload[i:i + size] for i in range(0, len(payload), size)] or [b'']
        first = self.next_free
        for i, ch in enumerate(chunks):
            c = self.next_free; self.next_free += 1
            assert c < self.clusters + 2, "ESP too small for the payload"
            self.data[c] = ch.ljust(size, b'\0')
            self.fat[c] = (c + 1) if i + 1 < len(chunks) else 0xFFFF
        return first

    @staticmethod
    def entry(name, ext, attr, cluster, size):
        return (name.ljust(8).encode()[:8] + ext.ljust(3).encode()[:3]
                + bytes([attr]) + b'\0' * 14
                + struct.pack('<HI', cluster, size))

    def add_dir(self, parent_cluster):
        """A directory occupies one cluster; returns its cluster number."""
        c = self.next_free; self.next_free += 1
        self.fat[c] = 0xFFFF
        ents = self.entry('.', '', 0x10, c, 0) + self.entry('..', '', 0x10, parent_cluster, 0)
        self.data[c] = ents.ljust(self.spc * SECTOR, b'\0')
        return c

    def dir_append(self, cluster, ent):
        blob = bytearray(self.data[cluster])
        off = blob.find(b'\0' * 32)
        for i in range(0, len(blob), 32):
            if blob[i] == 0: off = i; break
        blob[off:off + 32] = ent
        self.data[cluster] = bytes(blob)

    def image(self):
        boot = bytearray(SECTOR)
        boot[0:3] = b'\xEB\x3C\x90'
        boot[3:11] = b'IRISBOOT'
        struct.pack_into('<HBHBHHBHHHII', boot, 11,
                         SECTOR, self.spc, self.reserved, self.nfats,
                         self.root_entries,
                         self.total if self.total < 0x10000 else 0,
                         0xF8, self.fat_sectors, 63, 255, 2048,
                         self.total if self.total >= 0x10000 else 0)
        boot[36] = 0x80; boot[38] = 0x29
        boot[39:43] = b'\x12\x34\x56\x78'
        boot[43:54] = b'IRIS ESP   '
        boot[54:62] = b'FAT16   '
        boot[510:512] = b'\x55\xAA'

        fat = bytearray()
        for v in self.fat: fat += struct.pack('<H', v)
        fat = bytes(fat).ljust(self.fat_sectors * SECTOR, b'\0')

        root = b''.join(self.root).ljust(self.root_sectors * SECTOR, b'\0')

        data = bytearray(self.clusters * self.spc * SECTOR)
        for c, blob in self.data.items():
            off = (c - 2) * self.spc * SECTOR
            data[off:off + len(blob)] = blob

        out = bytes(boot) + fat * self.nfats + root + bytes(data)
        return out.ljust(self.total * SECTOR, b'\0')[:self.total * SECTOR]

def build(efi_root, out_path, mib=64):
    total = mib * 1024 * 1024 // SECTOR
    first = 2048
    last = total - 34
    fs = Fat16(last - first + 1)

    efi_c = fs.add_dir(0)
    fs.root.append(Fat16.entry('EFI', '', 0x10, efi_c, 0))
    for sub, fname, src in (('BOOT', ('BOOTX64', 'EFI'), 'EFI/BOOT/BOOTX64.EFI'),
                            ('IRIS', ('KERNEL', 'ELF'), 'EFI/IRIS/KERNEL.ELF')):
        d = fs.add_dir(efi_c)
        fs.dir_append(efi_c, Fat16.entry(sub, '', 0x10, d, 0))
        payload = open(os.path.join(efi_root, src), 'rb').read()
        c = fs.alloc_chain(payload)
        fs.dir_append(d, Fat16.entry(fname[0], fname[1], 0x20, c, len(payload)))
        print('  %-24s %8d bytes' % (src, len(payload)))

    img = bytearray(total * SECTOR)
    img[first * SECTOR:(last + 1) * SECTOR] = fs.image()

    # protective MBR
    img[446:462] = bytes([0x00, 0x00, 0x02, 0x00, 0xEE, 0xFF, 0xFF, 0xFF]) + \
                   struct.pack('<II', 1, min(total - 1, 0xFFFFFFFF))
    img[510:512] = b'\x55\xAA'

    ent = ESP_TYPE + ESP_UUID + struct.pack('<QQQ', first, last, 0) + \
          'EFI System Partition'.encode('utf-16-le').ljust(72, b'\0')
    arr = bytes(ent).ljust(128 * 128, b'\0')
    img[2 * SECTOR:2 * SECTOR + len(arr)] = arr
    img[(total - 33) * SECTOR:(total - 33) * SECTOR + len(arr)] = arr

    def header(my_lba, alt_lba, arr_lba):
        h = bytearray(92)
        h[0:8] = b'EFI PART'; struct.pack_into('<III', h, 8, 0x00010000, 92, 0)
        struct.pack_into('<QQQQ', h, 24, my_lba, alt_lba, 34, total - 34)
        h[56:72] = bytes.fromhex('0f1e2d3c4b5a69788796a5b4c3d2e1f0')
        struct.pack_into('<QIII', h, 72, arr_lba, 128, 128, gpt_crc(arr))
        struct.pack_into('<I', h, 16, gpt_crc(bytes(h)))
        return bytes(h).ljust(SECTOR, b'\0')

    img[SECTOR:2 * SECTOR] = header(1, total - 1, 2)
    img[(total - 1) * SECTOR:total * SECTOR] = header(total - 1, 1, total - 33)

    open(out_path, 'wb').write(bytes(img))
    print('  wrote %s (%d MiB, ESP at LBA %d, %d FAT16 clusters)'
          % (out_path, mib, first, fs.clusters))

if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else 'build/efi_root'
    out = sys.argv[2] if len(sys.argv) > 2 else 'build/iris-boot.img'
    build(root, out)
