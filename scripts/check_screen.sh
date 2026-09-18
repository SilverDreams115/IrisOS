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
# Sixty at a fifth of a second is twelve seconds of sampling.
#
# It was thirty, which is six, and six was enough on this machine and not on a
# CI runner, where firmware takes longer to hand over and the kernel's window
# lands later.  A capture window tuned to the fast machine is a capture window
# that fails on the slow one, and screendumps are cheap.
FRAMES="${IRIS_SCREEN_FRAMES:-400}"
# Dense, because a frame is now only KEPT when it qualifies: the cost of
# sampling often is one screendump and eight decoded scanlines, not a file.
INTERVAL="${IRIS_SCREEN_INTERVAL:-0.05}"
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

python3 - "$WORK" "$FRAMES" "$INTERVAL" <<'CAPTURE'
import json, os, socket, sys, time

work, frames, interval = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
sys.path.insert(0, "scripts")
import fbcon_ocr as ocr

FONT = ocr.load_font()
MARKERS = "KFSBPGg"


def top_line(path):
    """Decode row 0 only -- eight scanlines, which is all the marker line is."""
    try:
        w, h, d = ocr.read_ppm(path)
    except Exception:
        return ""
    out = ""
    for cx in range(min(w // 8, 16)):
        cell = []
        for gy in range(8):
            bits = 0
            for gx in range(8):
                i = (gy * w + cx * 8 + gx) * 3
                if i + 2 < len(d) and (d[i] > 100 or d[i + 1] > 100 or d[i + 2] > 100):
                    bits |= 1 << gx
            cell.append(bits)
        out += FONT.get(tuple(cell), " " if not any(cell) else "?")
    return out.rstrip()


sock = os.path.join(work, "qmp.sock")
for _ in range(400):
    if os.path.exists(sock):
        break
    time.sleep(0.05)
else:
    sys.exit("[screen] qemu never opened its monitor")
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
f = s.makefile("rw")
f.readline()
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
f.flush()
f.readline()

# Sample into ONE rotating file and keep only what qualifies.
#
# The window in which the kernel owns the screen is short: a ring-3 service
# takes the framebuffer within a second of the boot reaching userspace.  A
# fixed burst of screenshots either misses it or keeps hundreds of megabytes of
# frames that do not matter -- and on this machine only two of sixty qualified,
# which is luck rather than a check.  Decoding the marker line after every
# capture costs eight scanlines, and a frame is kept only while that line is
# complete.  The LAST kept frame is the richest, because the kernel is still
# logging into it.
probe = os.path.join(work, "probe.ppm")
kept = 0
for n in range(frames):
    f.write(json.dumps({"execute": "screendump",
                        "arguments": {"filename": probe}}) + "\n")
    f.flush()
    f.readline()
    if top_line(probe).startswith(MARKERS):
        os.replace(probe, os.path.join(work, "f%03d.ppm" % n))
        kept += 1
    time.sleep(interval)
if kept == 0 and os.path.exists(probe):
    # Keep the last one so a failure can show what WAS on the screen.
    os.replace(probe, os.path.join(work, "f999.ppm"))
print("[screen] %d frame(s) captured with a complete marker line" % kept)
CAPTURE

rc=$?
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
if [ "$rc" != "0" ]; then echo "[screen] capture failed"; exit 1; fi

# Pick a frame that shows a COMPLETE boot, not merely a busy one.
#
# The first version scored frames only by how many kernel log lines they
# carried and then demanded the full marker line from whichever won.  Those are
# two different questions, and on a slow machine they have two different
# answers: a frame can be captured with plenty of log on it and the marker line
# still being written.  CI found that, which is what CI is for.
#
# So markers first -- a frame whose top line is the whole sequence is a frame
# where the kernel got at least as far as paging -- and among those, the one
# carrying the most log.
best=""; best_n=-1
best_any=""; best_any_n=-1
for shot in "$WORK"/f*.ppm; do
  [ -f "$shot" ] || continue
  python3 scripts/fbcon_ocr.py "$shot" --rows 48 > "$shot.txt" 2>/dev/null || continue
  n=$(grep -c "IRIS" "$shot.txt" 2>/dev/null || true); n=${n:-0}
  if [ "$n" -gt "$best_any_n" ]; then best_any_n="$n"; best_any="$shot.txt"; fi
  head -1 "$shot.txt" | grep -q "^KFSBPGg" || continue
  if [ "$n" -gt "$best_n" ]; then best_n="$n"; best="$shot.txt"; fi
done

if [ -z "$best" ] && [ -n "$best_any" ]; then
  echo "[screen] no captured frame shows a complete marker line."
  echo "         the best frame's top line was: $(head -1 "$best_any")"
  echo "         (the markers are K F S B P G g, in that order, and a short"
  echo "          line means the boot stopped where the sequence stops)"
  cat "$best_any"
  exit 1
fi

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
