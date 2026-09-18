#!/usr/bin/env python3
"""
mkdisk.py — build a GPT test disk that models a real machine's drive.

The point is the DECOY.  A raw image proves nothing about staying inside a
partition, because there is nowhere else to go.  This image has two: one
carrying recognisable data that IRIS must never touch, and one of IRIS's own
type that it may use.  Checking the image afterwards then asks a question with
a real answer -- did anything outside the IRIS partition change?

The IRIS partition's first sector gets the format-permission token, because
that is what makes it a disk somebody deliberately handed over.

Layout (8 MiB default, 512-byte sectors):
    LBA 0          protective MBR
    LBA 1          primary GPT header
    LBA 2..33      partition entry array (128 x 128 bytes)
    LBA 34..       the decoy partition
    ...            the IRIS partition
    last-33..      backup entry array
    last           backup GPT header

Usage: mkdisk.py <path> [size_mib]
Prints: <iris_partition_byte_offset> <iris_partition_byte_length>
"""
import struct
import sys
import zlib

SECTOR = 512
IRIS_TYPE = b"IRISFS-PARTITION"          # see iris/blk_ep_proto.h
DECOY_TYPE = bytes.fromhex("a2a0d0ebe5b9334487c068b6b72699c7")  # MS basic data
FS_SCRATCH_OFF = 496
FS_SCRATCH_TOKEN = b"S-FMT-OK"
DECOY_MARK = b"PRECIOUS-DO-NOT-TOUCH"


def guid(seed):
    """A stable, obviously-synthetic unique GUID; content does not matter."""
    return bytes((seed + i * 17) & 0xFF for i in range(16))


def entry(type_bytes, uniq, first, last, name):
    n = name.encode("utf-16-le")[:72].ljust(72, b"\0")
    return type_bytes + uniq + struct.pack("<QQQ", first, last, 0) + n


def header(my_lba, alt_lba, first_usable, last_usable, ent_lba, ents_crc, disk_guid):
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000)      # revision 1.0
    struct.pack_into("<I", h, 12, 92)             # header size
    struct.pack_into("<QQ", h, 24, my_lba, alt_lba)
    struct.pack_into("<QQ", h, 40, first_usable, last_usable)
    h[56:72] = disk_guid
    struct.pack_into("<Q", h, 72, ent_lba)
    struct.pack_into("<II", h, 80, 128, 128)      # count, size
    struct.pack_into("<I", h, 88, ents_crc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
    return bytes(h)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    mib = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    total = mib * 1024 * 1024 // SECTOR
    d = bytearray(total * SECTOR)

    ent_lba, ent_sectors = 2, 32
    first_usable = ent_lba + ent_sectors                      # 34
    backup_hdr = total - 1
    backup_ents = backup_hdr - ent_sectors                    # first backup entry LBA
    last_usable = backup_ents - 1

    split = total // 2
    decoy_first, decoy_last = first_usable, split - 1
    iris_first, iris_last = split, last_usable

    # Protective MBR: one 0xEE partition spanning the disk.
    d[446:462] = bytes([0x00, 0x00, 0x02, 0x00, 0xEE, 0xFF, 0xFF, 0xFF]) + \
        struct.pack("<II", 1, min(total - 1, 0xFFFFFFFF))
    d[510:512] = b"\x55\xaa"

    ents = bytearray(128 * 128)
    ents[0:128] = entry(DECOY_TYPE, guid(1), decoy_first, decoy_last, "PRECIOUS")
    ents[128:256] = entry(IRIS_TYPE, guid(2), iris_first, iris_last, "IRIS")
    ents_crc = zlib.crc32(bytes(ents)) & 0xFFFFFFFF
    dg = guid(3)

    d[ent_lba * SECTOR:ent_lba * SECTOR + len(ents)] = ents
    d[backup_ents * SECTOR:backup_ents * SECTOR + len(ents)] = ents
    d[1 * SECTOR:1 * SECTOR + 92] = header(
        1, backup_hdr, first_usable, last_usable, ent_lba, ents_crc, dg)
    d[backup_hdr * SECTOR:backup_hdr * SECTOR + 92] = header(
        backup_hdr, 1, first_usable, last_usable, backup_ents, ents_crc, dg)

    # The decoy's contents: something whose loss would be obvious.
    for s in range(decoy_first, decoy_last + 1, 8):
        off = s * SECTOR
        d[off:off + len(DECOY_MARK)] = DECOY_MARK

    # The IRIS partition's first sector carries permission to format it.
    off = iris_first * SECTOR + FS_SCRATCH_OFF
    d[off:off + len(FS_SCRATCH_TOKEN)] = FS_SCRATCH_TOKEN

    open(path, "wb").write(bytes(d))
    print("%d %d" % (iris_first * SECTOR, (iris_last - iris_first + 1) * SECTOR))


if __name__ == "__main__":
    main()
