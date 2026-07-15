#!/usr/bin/env python3
"""Regenerate the file-backed-memory test fixtures (Fase 28 Bloque B).

Each fixture is a deterministic byte pattern so the ring-3 suite (T217–T230)
can verify, byte-for-byte, that a page a target read through the pager came
from the file at the right offset.  The kernel embeds each .dat as an initrd
image (objcopy'd .rodata); VFS exports it by name; the pager reads it via
VFS READ_AT.  Keep the patterns in sync with services/iris_test/main.c
(t28_pat / t28_pat2 / t28_patseg / t28_pats).

Run from the repo root:  python3 scripts/gen_fixtures.py
"""
import os

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILEBK = os.path.join(HERE, "services", "filebk")

FIXTURES = [
    # (name, size_bytes, lambda i -> byte)   pattern must match iris_test mirror
    ("fbk.dat",    20480, lambda i: (i * 31 + 7) & 0xFF),    # 5 pages   — primary
    ("fbk2.dat",   12288, lambda i: (i * 17 + 101) & 0xFF),  # 3 pages   — second file (T227)
    ("elfseg.dat", 16384, lambda i: (i * 13 + 0x40) & 0xFF), # 4 pages   — ELF-segment groundwork (T229)
    ("small.dat",    100, lambda i: (i * 7 + 1) & 0xFF),     # sub-page  — EOF / short-file edge (T220/T224)
]

def main():
    os.makedirs(FILEBK, exist_ok=True)
    for name, size, fn in FIXTURES:
        data = bytes(fn(i) for i in range(size))
        path = os.path.join(FILEBK, name)
        with open(path, "wb") as f:
            f.write(data)
        print(f"[gen_fixtures] {name}: {size} bytes")

if __name__ == "__main__":
    main()
