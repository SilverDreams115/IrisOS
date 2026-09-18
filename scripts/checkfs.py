#!/usr/bin/env python3
"""
checkfs.py — read, from the host, what IRIS wrote, and confirm it wrote only there.

Two claims, and the second is the one that could not be made before the disk
had more than one partition on it:

  1. the IRIS partition holds the filesystem, at the generation expected
  2. NOTHING outside that partition changed -- not the GPT, not its backup,
     not the decoy partition

The second is checked against a pristine copy built the same way, because
"IRIS says it stayed inside its partition" is exactly what a system that did
not would also say.

Usage: checkfs.py <image> <pristine> <expect_generation>
"""
import struct
import subprocess
import sys
import os

SECTOR = 512
HERE = os.path.dirname(os.path.abspath(__file__))


def iris_part(path):
    out = subprocess.run([sys.executable, os.path.join(HERE, "findpart.py"), path],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("[persist] " + out.stderr.strip())
    off, ln = out.stdout.split()
    return int(off), int(ln)


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    img, pristine, want_gen = sys.argv[1], sys.argv[2], int(sys.argv[3])

    d = open(img, "rb").read()
    p = open(pristine, "rb").read()
    base, length = iris_part(img)

    problems = []

    # ── 1. the filesystem, read at the partition's own offset ───────────────
    magic, version, generation = struct.unpack("<QII", d[base:base + 16])
    name = d[base + SECTOR:base + SECTOR + 16].rstrip(b"\0").decode("ascii", "replace")
    size, = struct.unpack("<I", d[base + SECTOR + 16:base + SECTOR + 20])
    body = d[base + 16 * SECTOR:base + 16 * SECTOR + 8]

    if magic != 0x53464953495253:
        problems.append("superblock magic is %#x" % magic)
    if version != 1:
        problems.append("version is %d" % version)
    if generation != want_gen:
        problems.append("generation is %d, not %d" % (generation, want_gen))
    if name != "boot.log":
        problems.append("directory entry is %r" % name)
    if size != 8:
        problems.append("file size is %d" % size)
    if body != b"IRIS" + struct.pack("<I", want_gen):
        problems.append("file contents are %s" % body.hex())

    # ── 2. everything else, byte for byte ───────────────────────────────────
    if len(d) != len(p):
        problems.append("the image changed size")
    else:
        before = d[:base] + d[base + length:]
        after = p[:base] + p[base + length:]
        if before != after:
            # Say WHERE, because "something changed" is not actionable.
            for i in range(0, len(before), SECTOR):
                if before[i:i + SECTOR] != after[i:i + SECTOR]:
                    lba = (i if i < base else i + length) // SECTOR
                    problems.append("a sector outside the IRIS partition changed "
                                    "(LBA %d)" % lba)
                    break

    if problems:
        print("[persist] the host cannot confirm what IRIS did:")
        for x in problems:
            print("           - " + x)
        raise SystemExit(1)

    print("[persist] the host reads generation %d and %r (%d bytes) from the IRIS "
          "partition" % (generation, name, size))
    print("[persist] and every byte outside that partition is unchanged: the GPT, "
          "its backup and the decoy")


if __name__ == "__main__":
    main()
