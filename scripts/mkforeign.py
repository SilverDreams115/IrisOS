#!/usr/bin/env python3
"""
mkforeign.py — make a disk image that is plausibly somebody else's.

Used by check_persistence.sh for the one check here that protects data rather
than proving a feature: IRIS must refuse to format a disk it was never given
permission to destroy, and must leave it byte-for-byte as it found it.

The image gets a boot signature and recognisable payloads, and deliberately
does NOT get the token at offset 496 that says a disk is disposable.

Usage: mkforeign.py <path> [size_mib]     (prints the sha256 of the result)
"""
import hashlib
import sys


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    mib = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    d = bytearray(mib * 1024 * 1024)
    d[0:16] = b"NOT-IRIS-DATA!!!"
    d[510:512] = b"\x55\xaa"                    # a boot signature, like a real disk
    d[4096:4096 + 16] = b"PRECIOUS-PAYLOAD"     # something to notice the loss of
    # offset 496 is left as zeros on purpose: no permission to format.
    with open(path, "wb") as f:
        f.write(bytes(d))
    print(hashlib.sha256(bytes(d)).hexdigest())


if __name__ == "__main__":
    main()
