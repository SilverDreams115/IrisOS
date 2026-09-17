#!/usr/bin/env bash
set -euo pipefail

# check_persistence.sh — does the filesystem survive the machine being off?
#
# It is the only claim in this repository that cannot be checked by one run,
# because "persistent" is a statement about what happens BETWEEN runs.  So:
#
#   1. throw the image away and boot.  The filesystem finds a blank disk,
#      FORMATS it, and reports generation 1;
#   2. boot again over the same image.  It must find an EXISTING filesystem and
#      report generation 2 — the number it wrote last time, plus one;
#   3. read the image from the HOST and check what is on it.
#
# Step 3 is the one that matters.  Steps 1 and 2 are IRIS reading back its own
# writes, which a filesystem that merely remembered things in RAM would also
# pass; the host reading the bytes does not depend on IRIS being
# self-consistent about anything.

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$PROJECT_ROOT/build/iris-disk.img"
LOG="$PROJECT_ROOT/build/persist"

rm -f "$IMG"

for boot in 1 2; do
  IRIS_QEMU_TIMEOUT_SECS="${IRIS_QEMU_TIMEOUT_SECS:-180}" \
  IRIS_QEMU_LOG="$LOG-$boot.log" \
    bash "$PROJECT_ROOT/scripts/run_qemu_headless.sh" >/dev/null 2>&1 || {
      echo "[persist] boot $boot did not produce a healthy runtime"
      grep -F "fs:" "$LOG-$boot.log" || true
      exit 1
    }
done

want1='fs: mounted gen 1 formatted file 1'
want2='fs: mounted gen 2 existing file 1'
if ! grep -Fq "$want1" "$LOG-1.log"; then
  echo "[persist] the first boot did not format a blank disk; wanted: $want1"
  grep -F "fs:" "$LOG-1.log" || echo "           (no fs line at all)"
  exit 1
fi
if ! grep -Fq "$want2" "$LOG-2.log"; then
  echo "[persist] the second boot did not find what the first one wrote;"
  echo "          wanted: $want2"
  grep -F "fs:" "$LOG-2.log" || echo "           (no fs line at all)"
  exit 1
fi

python3 - "$IMG" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
magic, version, generation = struct.unpack('<QII', d[:16])
name = d[512:528].rstrip(b'\0').decode('ascii', 'replace')
size = struct.unpack('<I', d[528:532])[0]
body = d[16 * 512:16 * 512 + 8]

problems = []
if magic != 0x53464953495253:      problems.append('superblock magic is %#x' % magic)
if version != 1:                   problems.append('version is %d' % version)
if generation != 2:                problems.append('generation is %d, not 2' % generation)
if name != 'boot.log':             problems.append('directory entry is %r' % name)
if size != 8:                      problems.append('file size is %d' % size)
# "IRIS" then the generation the SECOND boot wrote.
if body != b'IRIS' + struct.pack('<I', 2):
    problems.append('file contents are %s' % body.hex())

if problems:
    print('[persist] the host cannot read what IRIS wrote:')
    for p in problems:
        print('           - ' + p)
    raise SystemExit(1)
print('[persist] the host reads generation %d and %r (%d bytes) written by IRIS'
      % (generation, name, size))
PY

echo "[persist] a filesystem written by IRIS survived the machine being off"
