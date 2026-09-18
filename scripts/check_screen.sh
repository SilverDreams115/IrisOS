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
FRAMES="${IRIS_SCREEN_FRAMES:-600}"
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

# A disk with an IRIS partition on it, so the boot report this check looks for
# is a report about SOMETHING.  Without one the machine correctly says it found
# no partition of its own -- true, and not the sentence worth gating on.
python3 "$PROJECT_ROOT/scripts/mkdisk.py" "$WORK/disk.img" 8 >/dev/null

qemu-system-x86_64 \
  -machine q35 -cpu max -smp 1 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$WORK/VARS.fd" \
  -drive format=raw,file=fat:rw:"$PROJECT_ROOT/build/efi_root" \
  -drive "file=$WORK/disk.img,format=raw,if=none,id=irisdisk" \
  -device ide-hd,drive=irisdisk,bus=ide.1 \
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


def decode_rows(path, rows, cols=48):
    """Decode the top few text rows.

    Not the whole screen: this runs after every screendump, and the two things
    it has to answer -- is the marker line complete, is the kernel banner still
    there -- both live in the first dozen rows.
    """
    try:
        w, h, d = ocr.read_ppm(path)
    except BaseException:
        # A screendump that is not there yet, half written, or empty because
        # the display has produced nothing so far.  All of them mean the same
        # thing to this loop: not yet, try again.
        return []
    out = []
    for cy in range(min(rows, h // 8)):
        line = ""
        for cx in range(min(w // 8, cols)):
            cell = []
            for gy in range(8):
                bits = 0
                for gx in range(8):
                    i = ((cy * 8 + gy) * w + cx * 8 + gx) * 3
                    if i + 2 < len(d) and (d[i] > 100 or d[i + 1] > 100 or d[i + 2] > 100):
                        bits |= 1 << gx
                cell.append(bits)
            line += FONT.get(tuple(cell), " " if not any(cell) else "?")
        out.append(line.rstrip())
    return out


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
# Keep exactly TWO frames, because two different things have to be proved and
# they are never true at the same moment.
#
# The FIRST frame with a complete marker line still carries the kernel's banner:
# that is the proof the kernel log reaches a screen at all.  The LAST frame is
# what a person standing at the machine actually ends up looking at, and by then
# the kernel's lines have scrolled away and the ring-3 boot report has arrived.
#
# Keeping every qualifying frame was fine when there were two of them.  Now that
# the marker line survives into ring 3, every frame qualifies -- four hundred of
# them, three megabytes each.
probe = os.path.join(work, "probe.ppm")
first = os.path.join(work, "f000-first.ppm")
last  = os.path.join(work, "f999-last.ppm")
serial = os.path.join(work, "serial.log")


def boot_settled():
    """Has init finished?

    Read from the SERIAL log, which is a statement about the test harness and
    not about the system.  QEMU always has a serial port; the claim this whole
    check exists to make is that IRIS does not NEED one.  Using it here to know
    when to stop looking is the harness knowing when the subject is done, and
    it beats guessing a frame count -- the previous version guessed, stopped
    four hundred frames in, and killed the machine before the boot report it
    was looking for had been printed.
    """
    try:
        with open(serial, "rb") as fh:
            return b"init idle loop start" in fh.read()
    except OSError:
        return False


# Two phases, because the two frames are found in completely different ways.
#
# The FIRST needs dense sampling: the kernel's banner is on screen from very
# early, and the marker line completes moments later.  Once that frame is in
# hand there is nothing more to hunt for -- the marker line survives into ring
# 3 now, so every later frame qualifies too.
#
# The LAST needs PATIENCE, not density.  The boot has a test suite in it and
# takes the better part of a minute, and sampling densely for that long means
# either hundreds of screendumps or a frame budget that runs out before the
# machine is finished -- which is what happened: the capture stopped four
# hundred frames in, and killed the machine before the report it was looking
# for had been printed.
import shutil

kept = 0
settled_extra = 0
for n in range(frames):
    f.write(json.dumps({"execute": "screendump",
                        "arguments": {"filename": probe}}) + "\n")
    f.flush()
    f.readline()
    rows = decode_rows(probe, 12)
    if rows and rows[0].startswith(MARKERS):
        kept += 1
        # Keep overwriting `first` while the KERNEL's banner is still up.  The
        # very first qualifying frame is torn -- a line is being painted as the
        # screenshot is taken, and half-drawn glyphs decode to nothing.  The
        # LAST frame that still shows the banner is the one with the most
        # kernel log on it, and it is the frame that goes when the console
        # service clears the screen to start its own.
        if any("IRIS KERNEL" in r for r in rows):
            shutil.copyfile(probe, first)
        os.replace(probe, last)
    if boot_settled():
        # One more frame after the last line is printed, then stop: what is on
        # the screen NOW is what a person at this machine would be looking at.
        settled_extra += 1
        if settled_extra > 2:
            break
    # Dense until the first frame is found, unhurried afterwards.
    time.sleep(interval if kept == 0 else 0.5)
if kept == 0 and os.path.exists(probe):
    os.replace(probe, last)
print("[screen] %d frame(s) had a complete marker line; kept the first and the last" % kept)
CAPTURE

rc=$?
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
if [ "$rc" != "0" ]; then echo "[screen] capture failed"; exit 1; fi

# The two frames the capture kept, and what each is asked to prove.
first_ppm="$WORK/f000-first.ppm"
last_ppm="$WORK/f999-last.ppm"
if [ ! -f "$first_ppm" ] || [ ! -f "$last_ppm" ]; then
  echo "[screen] the kernel never reached the framebuffer:"
  echo "         no captured frame decoded to a complete marker line."
  echo "         markers on serial: $(grep -ao 'KFSBPGg' "$WORK/serial.log" | head -1)"
  exit 1
fi
python3 scripts/fbcon_ocr.py "$first_ppm" --rows 48  > "$first_ppm.txt" 2>/dev/null
python3 scripts/fbcon_ocr.py "$last_ppm"  --rows 120 > "$last_ppm.txt"  2>/dev/null
best="$first_ppm.txt"

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

# ---- and the half that only ring 3 can answer -------------------------------
#
# Everything above is the KERNEL's log.  It proves the machine started; it says
# nothing about whether the disk driver found a disk, or the filesystem
# mounted, or a frame ever left the network card -- and on a machine with no
# serial port those were exactly the answers that went nowhere.
#
# The console service paints them now, and init prints them once more as a
# block at the very end, because a log that scrolls loses what matters by the
# time anyone reads it.  This looks for that block on the LAST frame captured,
# which is what a person standing at the machine would actually see.
last="$last_ppm"
if [ -n "$last" ]; then
  if grep -q "IRIS on this machine" "$last.txt" 2>/dev/null; then
    echo "[screen] and the ring-3 findings are on it too:"
    grep -A 6 "IRIS on this machine" "$last.txt" | sed 's/^/         /'
  else
    echo "[screen] the kernel log reached the screen but the boot report did not."
    echo "         a machine with no serial port would show that it STARTED and"
    echo "         nothing about what it found, which is the half that matters."
    tail -14 "$last.txt" 2>/dev/null | sed 's/^/         /'
    exit 1
  fi
fi

echo "[screen] the kernel log is readable on the framebuffer:"
sed 's/^/         /' "$best" | head -12
echo "[screen] a machine with no serial port can be diagnosed"
