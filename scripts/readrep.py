#!/usr/bin/env python3
"""
readrep.py -- read IRIS's boot report off a disk, from another computer.

This is the half of the loop that works when nobody is watching the screen.
A machine with no serial port can be photographed, and a photograph has to be
read by a person and typed back in; a partition can be carried somewhere else
and read exactly.

Works on a whole-disk image (finds the IRIS partition in the GPT) or on a raw
partition device -- on Windows that is \\\\.\\Harddisk0Partition2, which reads
without administrator rights.

Usage:
    readrep.py <image-or-device> [--partition-offset N]
"""
import struct
import sys

SECTOR = 512
FS_MAGIC = 0x53464953495253
FS_DIR_LBA = 1
FS_DATA_LBA = 16
FS_MAX_FILES = 16
IRIS_TYPE = b"IRISFS-PARTITION"


def find_partition(f):
    """Locate the IRIS partition through the GPT, or return 0 for a raw one."""
    f.seek(SECTOR)
    hdr = f.read(SECTOR)
    if hdr[:8] != b"EFI PART":
        return 0                      # not a whole disk: assume a partition
    ent_lba, = struct.unpack_from("<Q", hdr, 72)
    count, size = struct.unpack_from("<II", hdr, 80)
    f.seek(ent_lba * SECTOR)
    ents = f.read(min(count, 128) * size)
    for i in range(min(count, 128)):
        off = i * size
        if ents[off:off + 16] == IRIS_TYPE:
            first, _last = struct.unpack_from("<QQ", ents, off + 32)
            return first * SECTOR
    raise SystemExit("readrep: this disk has no IRIS partition")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    base = None
    if "--partition-offset" in sys.argv:
        base = int(sys.argv[sys.argv.index("--partition-offset") + 1])

    with open(path, "rb") as f:
        if base is None:
            base = find_partition(f)

        f.seek(base)
        magic, version, generation = struct.unpack("<QII", f.read(16))
        if magic != FS_MAGIC:
            raise SystemExit(
                "readrep: no IRIS filesystem here (magic %#x).\n"
                "          If IRIS has not booted on this disk yet, that is "
                "exactly what this looks like." % magic)

        print("IRIS filesystem, version %d, generation %d "
              "(this disk has been booted %d time%s)"
              % (version, generation, generation, "" if generation == 1 else "s"))

        f.seek(base + FS_DIR_LBA * SECTOR)
        dirent = f.read(SECTOR)
        found = {}
        for i in range(FS_MAX_FILES):
            off = i * 32
            name = dirent[off:off + 16].rstrip(b"\0").decode("ascii", "replace")
            size, = struct.unpack_from("<I", dirent, off + 16)
            if size:
                found[name] = (i, size)

        print("files: " + (", ".join(sorted(found)) if found else "(none)"))

        if "boot.rep" not in found:
            raise SystemExit(
                "\nreadrep: no boot.rep on this disk.\n"
                "         IRIS writes it last, so a boot that stopped earlier "
                "leaves none.\n"
                "         The screen is the record in that case.")

        slot, size = found["boot.rep"]
        f.seek(base + (FS_DATA_LBA + slot) * SECTOR)
        body = f.read(size).decode("ascii", "replace")
        print("\n--- what IRIS found, written by the machine itself ---")
        print(body.rstrip())


if __name__ == "__main__":
    main()
