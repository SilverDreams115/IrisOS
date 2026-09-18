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

# A pristine copy, built the same way, to compare against.  Without it the
# claim "IRIS stayed inside its partition" rests on IRIS's own word.
PRISTINE="$PROJECT_ROOT/build/pristine-disk.img"
python3 "$PROJECT_ROOT/scripts/mkdisk.py" "$PRISTINE" 8 >/dev/null

python3 "$PROJECT_ROOT/scripts/checkfs.py" "$IMG" "$PRISTINE" 2 || exit 1

echo "[persist] a filesystem written by IRIS survived the machine being off"

# ── Phase 3: a disk with nothing of ours on it is not touched ───────────────
#
# This is the check that protects data rather than proving a feature, and it
# exists because the opposite behaviour shipped twice over.  `blk` handed an
# absolute LBA straight to a WRITE DMA EXT, and `fs` wrote its superblock to
# LBA 0 -- which on a raw image is the start of the image and on a real drive
# is the PARTITION TABLE of the whole disk.  The reasoning written beside that
# code was that disk 1 is IRIS's own, because the block service numbers the
# boot disk 0: true of this script, which makes the image, and false of a
# machine, where disk 1 is whatever SATA device enumerates second.
#
# So: an image with a boot signature, recognisable payloads, and no IRIS
# partition.  IRIS must find nothing it may address, and the bytes must come
# back unchanged -- compared from the host, because "I did not write anything"
# is exactly the claim a broken implementation would also make about itself.
FOREIGN="$PROJECT_ROOT/build/foreign-disk.img"
before="$(python3 "$PROJECT_ROOT/scripts/mkforeign.py" "$FOREIGN" 8)"

IRIS_QEMU_TIMEOUT_SECS="${IRIS_QEMU_TIMEOUT_SECS:-180}" \
IRIS_QEMU_LOG="$LOG-foreign.log" \
IRIS_DISK_IMG="$FOREIGN" \
  bash "$PROJECT_ROOT/scripts/run_qemu_headless.sh" >/dev/null 2>&1 || true

# Both layers are required to say no, because they say no to different things
# and either alone would be a system with one accident left in it.  `window 0`
# is the block service refusing to write anywhere on a disk it has no partition
# on; the filesystem line is `fs` refusing to format what it found there.
if ! grep -Eq "blk: disk [0-9]+ .* window 0" "$LOG-foreign.log"; then
  echo "[persist] the block service did not report an empty window:"
  grep -E "blk:|fs:" "$LOG-foreign.log" 2>/dev/null || echo "         (it said nothing at all)"
  exit 1
fi
if ! grep -Fq "fs: foreign disk, refusing to format" "$LOG-foreign.log"; then
  echo "[persist] the filesystem did not refuse a disk that is not ours:"
  grep -E "blk:|fs:" "$LOG-foreign.log" 2>/dev/null || echo "         (it said nothing at all)"
  exit 1
fi

after="$(python3 "$PROJECT_ROOT/scripts/sha256.py" "$FOREIGN")"
if [ "$before" != "$after" ]; then
  echo "[persist] a disk IRIS has no partition on came back CHANGED"
  echo "          before $before"
  echo "          after  $after"
  exit 1
fi
echo "[persist] a disk with no IRIS partition: both layers refused, and it came"
echo "[persist] back byte-for-byte unchanged"
