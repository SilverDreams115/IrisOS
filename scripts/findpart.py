#!/usr/bin/env python3
"""
findpart.py — where IRIS's partition is in a disk image.

Locates the GPT entry whose type is the ASCII in `BLK_PART_TYPE` the same way
the block service does, so a check reads the same bytes the system wrote.

Usage: findpart.py <image>        prints "<byte_offset> <byte_length>"
"""
import struct
import sys

SECTOR = 512
IRIS_TYPE = b"IRISFS-PARTITION"


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = open(sys.argv[1], "rb").read()
    if d[SECTOR:SECTOR + 8] != b"EFI PART":
        raise SystemExit("findpart: no GPT header in " + sys.argv[1])
    ent_lba, = struct.unpack_from("<Q", d, SECTOR + 72)
    count, size = struct.unpack_from("<II", d, SECTOR + 80)
    for i in range(min(count, 128)):
        off = ent_lba * SECTOR + i * size
        if d[off:off + 16] != IRIS_TYPE:
            continue
        first, last = struct.unpack_from("<QQ", d, off + 32)
        print("%d %d" % (first * SECTOR, (last - first + 1) * SECTOR))
        return
    raise SystemExit("findpart: no IRIS partition in " + sys.argv[1])


if __name__ == "__main__":
    main()
