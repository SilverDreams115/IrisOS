#!/usr/bin/env python3
"""sha256.py <path> — print a file's sha256, for checks that compare bytes."""
import hashlib
import sys

if len(sys.argv) < 2:
    raise SystemExit(__doc__)
print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())
