#!/usr/bin/env bash
#
# check_screen.sh — the boot log reaches a machine with no serial port.
#
# ── Why this exists ────────────────────────────────────────────────────────
#
# Every other check in this repository reads the serial port.  That is a
# reasonable thing to do under QEMU and a useless one on the hardware this
# system is eventually meant to run on, because most machines have no serial
# port at all.  On such a machine every diagnostic this kernel emits before
# ring 3 exists was being written to a port that is not there, and a boot that
# died anywhere in that stretch left a black screen and no record.
#
# `fbcon` paints the kernel log onto the framebuffer instead.  This script is
# the gate on that claim, and it is a real one rather than a screenshot a human
# squints at: the console paints an 8x8 bitmap font, so the screen can be READ
# BACK exactly.  `fbcon_ocr.py` parses the font out of the kernel's own source
# and decodes each cell, which means this check cannot pass against a screen
# that says something else -- and cannot drift from the kernel, because there
# is no second copy of the font to drift from.
#
# ── Why it takes a burst of screenshots ────────────────────────────────────
#
# The kernel owns the screen only until ring 3 claims it, which on this machine
# is under a second.  There is no marker to wait for -- the whole point is that
# the serial port may not exist -- so the window is found by sampling it, and
# the richest frame is the one that is checked.
set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

WORK="${IRIS_SCREEN_WORK:-$PROJECT_ROOT/build/screen}"
FRAMES="${IRIS_SCREEN_FRAMES:-30}"
INTERVAL="${IRIS_SCREEN_INTERVAL:-0.2}"
rm -rf "$WORK"; mkdir -p "$WORK"

pick_first() { for c in "$@"; do [ -f "$c" ] && { echo "$c"; return; }; done; echo ""; }
OVMF_CODE="$(pick_first \
  /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
  /usr/share/qemu/OVMF_CODE_4M.fd /usr/share/qemu/OVMF_CODE.fd \
  /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.fd)"
if [ -z "$OVMF_CODE" ]; then
  echo "[screen] no OVMF firmware found; cannot boot"; exit 1
fi
if [ ! -d "$PROJECT_ROOT/build/efi_root" ]; then
  echo "[screen] build/efi_root missing -- run 'make all' first"; exit 1
fi
cp -f "$PROJECT_ROOT/build/OVMF_VARS.headless.fd" "$WORK/VARS.fd" 2>/dev/null || {
  echo "[screen] build/OVMF_VARS.headless.fd missing -- run 'make all' first"; exit 1; }

qemu-system-x86_64 \
  -machine q35 -cpu max -smp 1 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$WORK/VARS.fd" \
  -drive format=raw,file=fat:rw:"$PROJECT_ROOT/build/efi_root" \
  -serial "file:$WORK/serial.log" \
  -display none \
  -qmp "unix:$WORK/qmp.sock,server,nowait" \
  -no-reboot &
QPID=$!

python3 - "$WORK" "$FRAMES" "$INTERVAL" <<'PY'
import json, os, socket, sys, time
work, frames, interval = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
sock = os.path.join(work, "qmp.sock")
for _ in range(400):
    if os.path.exists(sock): break
    time.sleep(0.05)
else:
    sys.exit("[screen] qemu never opened its monitor")
s = socket.socket(socket.AF_UNIX); s.connect(sock)
f = s.makefile("rw")
f.readline()
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
for n in range(frames):
    f.write(json.dumps({"execute": "screendump",
                        "arguments": {"filename": os.path.join(work, "f%02d.ppm" % n)}}) + "\n")
    f.flush(); f.readline()
    time.sleep(interval)
PY
rc=$?
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
if [ "$rc" != "0" ]; then echo "[screen] capture failed"; exit 1; fi

# The richest frame: the one where the most of the kernel's log is on screen.
best=""; best_n=-1
for shot in "$WORK"/f*.ppm; do
  [ -f "$shot" ] || continue
  python3 scripts/fbcon_ocr.py "$shot" --rows 48 > "$shot.txt" 2>/dev/null || continue
  n=$(grep -c "IRIS" "$shot.txt" 2>/dev/null || true); n=${n:-0}
  if [ "$n" -gt "$best_n" ]; then best_n="$n"; best="$shot.txt"; fi
done

if [ -z "$best" ] || [ "$best_n" -le 0 ]; then
  echo "[screen] the kernel never reached the framebuffer:"
  echo "         no captured frame decoded to any kernel log line."
  echo "         markers on serial: $(grep -ao 'KFSBPGg' "$WORK/serial.log" | head -1)"
  exit 1
fi

fail() { echo "[screen] $1"; echo "--- what the screen said ---"; cat "$best"; exit 1; }

# 1. the marker line, which is what a dead boot leaves behind.  It is checked
#    first and checked whole: a prefix of it is a boot that stopped, and the
#    point of the line is to say WHERE.
head -1 "$best" | grep -q "^KFSBPGg" || fail "the boot markers are not on the top line"
# 2. the kernel identified itself
grep -q "IRIS KERNEL" "$best" || fail "the banner is not on the screen"
# 3. a NUMBER made it through.  Numbers take a different path into the log than
#    strings do, and that path was missed the first time -- "free RAM:  MB" is
#    a line that passes a banner check and tells you nothing.
grep -Eq "free RAM: [0-9]+ MB" "$best" || fail "a logged number did not reach the screen"
# 4. the boot got as far as paging, which is the last thing the kernel says
#    before it is doing real work
grep -q "virtual memory active" "$best" || fail "the screen stops before paging"

echo "[screen] the kernel log is readable on the framebuffer:"
sed 's/^/         /' "$best" | head -12
echo "[screen] a machine with no serial port can be diagnosed"
