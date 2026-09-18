#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EFI_ROOT="$PROJECT_ROOT/build/efi_root"
LOG_FILE="${IRIS_QEMU_LOG:-$PROJECT_ROOT/build/qemu-headless.log}"
TIMEOUT_SECS="${IRIS_QEMU_TIMEOUT_SECS:-25}"
EXPECT_SELFTESTS="${IRIS_QEMU_EXPECT_SELFTESTS:-0}"
SMP="${IRIS_QEMU_SMP:-1}"

# An Intel IOMMU on the machine, when asked for (Stage 10-dma).  Off by default
# for the same reason -smp defaults to 1: the interesting runs are the ones
# that differ from the ordinary one, and a gate that can only be run one way
# proves nothing about the other.
IOMMU_ARGS=()
if [ "${IRIS_QEMU_IOMMU:-0}" != "0" ]; then
  IOMMU_ARGS=(-device intel-iommu)
fi

# A DMA-capable device, always.
#
# `edu` is QEMU's teaching device: a PCI function with one MMIO BAR, an
# internal 4 KiB buffer and a DMA engine that will copy between that buffer and
# any physical address a driver writes into its registers.  It is here because
# Stage 10-dma could not otherwise prove its central claim.  Everything up to
# §10.2 step 5 shows the kernel programming a remapping unit and refusing to
# hand out a space for a device no unit covers; none of it shows a DEVICE being
# stopped, because there was no device under IRIS's control that could try.
#
# On EVERY run, not only the IOMMU ones, because the two arms are the claim:
# with a unit the device reaches exactly the frame somebody mapped for it, and
# without one it reaches whatever address its driver writes down.  A test that
# could only be run in the configuration that passes proves nothing.
#
# `dma_mask` is widened from its 28-bit default so the driver may name a frame
# anywhere in RAM.  The default would confine the test to the low 256 MiB and
# make a failure above it look like a containment success.
EDU_ARGS=(-device edu,dma_mask=0xffffffffff)

# A network card, and something on the other end of it.
#
# `-netdev user` is QEMU's own userspace network stack: it answers ARP for the
# gateway it advertises at 10.0.2.2, which is the whole reason it is here.  A
# driver that only transmits proves nothing — the frame may have gone nowhere —
# and a loopback test proves the card talks to itself.  An ARP request that
# comes back answered proves the transmit path, the receive path, the
# descriptor rings and the card's receive filter, all at once, against a peer
# that is not this driver.
#
# `-net none` is therefore dropped from the command line below: it and a
# netdev are contradictory, and QEMU obeys the last word.
#
# ...and a TFTP server on the other end of it.
#
# `-netdev user` carries one, built in, at the gateway address.  It is here
# because the IP stack needs a peer that speaks UDP and answers, and this one
# needs nothing from outside the machine: no host network, no listener to
# start, no port to pick.  A read of a file this repository wrote is a complete
# UDP request and response against a real implementation.
TFTP_DIR="$PROJECT_ROOT/build/tftp"
mkdir -p "$TFTP_DIR"
printf 'IRIS-TFTP-OK\n' > "$TFTP_DIR/hello.txt"
NET_ARGS=(-device e1000,netdev=n0 -netdev "user,id=n0,tftp=$TFTP_DIR")

# A disk IRIS OWNS, separate from the one it boots from.
#
# The boot drive is `fat:rw:` — a filesystem QEMU synthesises from a host
# directory.  Reading it proves a driver works, and writing it proves nothing
# useful: the image is generated, so what lands there is not a fact about
# persistence.  A filesystem that PERSISTS needs a medium that is just bytes,
# and it needs to be the same bytes on the next boot.
#
# So there is a second disk, a plain raw file this repository creates once and
# then leaves alone.  It is the gate's evidence in both directions: IRIS writes
# to it, and the HOST can read what IRIS wrote — which is a stronger claim than
# IRIS reading back its own writes, because it does not depend on IRIS being
# self-consistent about anything.
IRIS_DISK="${IRIS_DISK_IMG:-$PROJECT_ROOT/build/iris-disk.img}"
if [ ! -f "$IRIS_DISK" ]; then
  # 8 MiB of zeroes.  The filesystem formats it on first boot; a zeroed image
  # is how it tells "never formatted" from "formatted and empty".
  # A GPT disk with TWO partitions, which is the only kind that can test the
  # thing that matters.
  #
  # A raw image proves nothing about staying inside a partition, because there
  # is nowhere else to go -- and "stay inside your partition" is exactly the
  # property that separates a test rig from a machine whose second SATA disk
  # holds somebody's data.  So the image carries a decoy partition full of
  # recognisable bytes, and an IRIS-typed one with the format-permission token
  # in its first sector.  `check_persistence.sh` then asks a question with a
  # real answer: did anything outside the IRIS partition change?
  python3 "$PROJECT_ROOT/scripts/mkdisk.py" "$IRIS_DISK" 8 >/dev/null
fi
DISK_ARGS=(-drive "file=$IRIS_DISK,format=raw,if=none,id=irisdisk"
           -device ide-hd,drive=irisdisk,bus=ide.1)

# A QEMU without it fails HERE, saying so.
#
# `-device edu` on a build that does not have the device makes QEMU exit before
# it executes an instruction, with an empty serial log — which arrives at the
# bottom of this script as "missing scheduler running marker", the same message
# a kernel that triple-faulted produces.  One probe turns that into a sentence
# about the host.
if ! qemu-system-x86_64 -device edu,help >/dev/null 2>&1; then
  echo "[headless] this qemu has no 'edu' device; the DMA containment gate"
  echo "           (Stage 10-dma §10.2 step 6) cannot run without it"
  exit 1
fi

# More processors, more wall clock — and it is QEMU that needs it, not IRIS.
# TCG emulates every vCPU on one host thread apiece and multiplexes them, so a
# four-processor guest runs the same work at roughly a third of the speed while
# doing strictly more of it (three extra cores taking a timer tick each).  A
# caller who asks for four CPUs and the same deadline is asking for a timeout,
# and a timeout reads exactly like a hang.
if [ "$SMP" -gt 1 ]; then
  TIMEOUT_SECS=$(( TIMEOUT_SECS * SMP ))
fi

pick_first() {
  for f in "$@"; do
    if [ -f "$f" ]; then
      printf '%s\n' "$f"
      return 0
    fi
  done
  return 1
}

OVMF_CODE="$(pick_first \
  /usr/share/OVMF/OVMF_CODE_4M.fd \
  /usr/share/OVMF/OVMF_CODE.fd \
  /usr/share/OVMF/OVMF_CODE.ms.fd \
  /usr/share/qemu/OVMF_CODE_4M.fd \
  /usr/share/qemu/OVMF_CODE.fd \
  /usr/share/edk2/ovmf/OVMF_CODE.fd \
  /usr/share/edk2/x64/OVMF_CODE.fd \
  /usr/share/edk2/x64/OVMF_CODE.4m.fd
)"

OVMF_VARS_TEMPLATE="$(pick_first \
  /usr/share/OVMF/OVMF_VARS_4M.fd \
  /usr/share/OVMF/OVMF_VARS.fd \
  /usr/share/OVMF/OVMF_VARS.ms.fd \
  /usr/share/qemu/OVMF_VARS_4M.fd \
  /usr/share/qemu/OVMF_VARS.fd \
  /usr/share/edk2/ovmf/OVMF_VARS.fd \
  /usr/share/edk2/x64/OVMF_VARS.fd \
  /usr/share/edk2/x64/OVMF_VARS.4m.fd
)"

if [ -z "${OVMF_CODE:-}" ] || [ -z "${OVMF_VARS_TEMPLATE:-}" ]; then
  echo "[headless] OVMF firmware not found"
  exit 1
fi

mkdir -p "$PROJECT_ROOT/build"
cp -f "$OVMF_VARS_TEMPLATE" "$PROJECT_ROOT/build/OVMF_VARS.headless.fd"
rm -f "$LOG_FILE"

#
# The deadline is a CEILING, not the runtime.
#
# `-no-shutdown` is deliberate — it keeps the machine up so nothing truncates
# the serial log — and IRIS never asks to shut down anyway: init's last act is
# to block on a notification nobody holds, so the guest is quiet and alive
# forever.  `timeout` therefore killed EVERY run at its deadline, which made
# the deadline the runtime: raising it to cover a slower suite cost that much
# wall clock on every green run, and the four-processor lane pays it four times
# over because the script scales it.
#
# So the run ends when init says it has finished starting everything, with the
# deadline left to catch the case where it never does.  The grace period after
# the marker is for whatever is still on its way into the file; the log is a
# `-serial file:` and the marker is not the last line.
set +e
qemu-system-x86_64 \
  -machine q35 \
  "${IOMMU_ARGS[@]}" \
  "${EDU_ARGS[@]}" \
  "${NET_ARGS[@]}" \
  "${DISK_ARGS[@]}" \
  -cpu max \
  -smp "$SMP" \
  -m 512M \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$PROJECT_ROOT/build/OVMF_VARS.headless.fd" \
  -drive format=raw,file=fat:rw:"$EFI_ROOT" \
  -serial "file:$LOG_FILE" \
  -display none \
  -monitor none \
  -no-reboot \
  -no-shutdown &
qemu_pid=$!

#
# QEMU is run DIRECTLY, not under `timeout`, and that is not a simplification.
#
# `timeout` forwards a signal to its child and exits; `wait` then returns as
# soon as the WRAPPER is gone, which is before qemu has exited.  Two things
# went wrong because of that, and neither looked like a timing problem:
#
#   · the disk image was not flushed yet, so a host reading it straight after
#     the run saw the state from the boot BEFORE the one that just finished;
#   · qemu holds an exclusive lock on a raw image, so the NEXT boot could not
#     open it and produced an empty log — a run that looked like a kernel that
#     died before it reached the serial port.
#
# Waiting on qemu's own pid makes `wait` mean what it says.
deadline=$(( SECONDS + TIMEOUT_SECS ))
qemu_timed_out=0
while kill -0 "$qemu_pid" 2>/dev/null; do
  if grep -Fq "[USER] init idle loop start" "$LOG_FILE" 2>/dev/null; then
    sleep 2
    kill "$qemu_pid" 2>/dev/null
    break
  fi
  if [ "$SECONDS" -ge "$deadline" ]; then
    qemu_timed_out=1
    kill "$qemu_pid" 2>/dev/null
    break
  fi
  sleep 1
done
wait "$qemu_pid"
qemu_rc=$?
if [ "$qemu_timed_out" = "1" ]; then qemu_rc=124; fi
set -e

if ! grep -Fq "[IRIS][SCHED] running" "$LOG_FILE"; then
  echo "[headless] missing scheduler running marker"
  cat "$LOG_FILE"
  exit 1
fi

# The MADT walk must find exactly the processors QEMU was told to create.
# Getting this wrong is quiet: the kernel runs perfectly well believing a
# four-CPU machine has one, and would simply never start the other three.
EXPECT_CPUS="$SMP"
if ! grep -Fq "[IRIS][ACPI] processors: ${EXPECT_CPUS}" "$LOG_FILE"; then
  echo "[headless] ACPI reported the wrong processor count (wanted ${EXPECT_CPUS}):"
  grep -F "[IRIS][ACPI]" "$LOG_FILE" | sed 's/^/           /'
  cat "$LOG_FILE"
  exit 1
fi

# ...and every one of them has to actually ARRIVE.  A processor that does not
# start is reported and left alone rather than hung on (SMP roadmap 9.3 step
# 3), so without this gate a machine quietly running on half its cores looks
# exactly like a healthy one.
if [ "$EXPECT_CPUS" -gt 1 ] && \
   ! grep -Fq "[IRIS][SMP] processors online: ${EXPECT_CPUS}" "$LOG_FILE"; then
  echo "[headless] not every processor came online (wanted ${EXPECT_CPUS}):"
  grep -F "[IRIS][SMP]" "$LOG_FILE" | sed 's/^/           /'
  echo "           A trampoline progress of 1..5 says how far the last one got;"
  echo "           AP_TRAMPOLINE_TRACE in ap_trampoline.S prints each stage."
  cat "$LOG_FILE"
  exit 1
fi

# ...and every one of them has to actually SCHEDULE, which is a different claim
# (SMP roadmap 9.3 step 4).  A processor that arrived, took its GDT and then
# halted was online too: `online=4 dispatching=1` is a machine running on one
# core that passes every gate above.  T346 prints both; this is the gate.
#
# Only when the suite ran — T346 is part of it — so a plain runtime smoke is
# unaffected.
if [ "$EXPECT_CPUS" -gt 1 ] && grep -Fq "[IRIS][TEST] T346 online=" "$LOG_FILE"; then
  t346_line="$(grep -F "[IRIS][TEST] T346 online=" "$LOG_FILE" | tail -1)"
  t346_online="$(printf '%s' "$t346_line" | sed -n 's/.*online=\([0-9]*\).*/\1/p')"
  t346_disp="$(printf '%s' "$t346_line" | sed -n 's/.*dispatching=\([0-9]*\).*/\1/p')"
  if [ "$t346_online" != "$EXPECT_CPUS" ] || [ "$t346_disp" != "$EXPECT_CPUS" ]; then
    echo "[headless] processors are online but not all of them schedule:"
    echo "           $t346_line"
    echo "           wanted online=${EXPECT_CPUS} dispatching=${EXPECT_CPUS}"
    cat "$LOG_FILE"
    exit 1
  fi
fi

# The DMA remapping units, when the machine was given one (Stage 10-dma §10.2
# step 1).  Two claims, and they fail apart: with an IOMMU attached the kernel
# must FIND it, and without one it must say so rather than stay quiet — a
# kernel that silently found nothing and a kernel that silently skipped looking
# read the same from outside.
if [ "${IRIS_QEMU_IOMMU:-0}" != "0" ]; then
  if ! grep -Eq "^\[IRIS\]\[IOMMU\] remapping units: [1-9]" "$LOG_FILE"; then
    echo "[headless] an IOMMU was attached and the kernel found no remapping unit:"
    grep -F "[IRIS][IOMMU]" "$LOG_FILE" | sed 's/^/           /'
    cat "$LOG_FILE"
    exit 1
  fi
  # ...and finding it is not containing it (Stage 10-dma §10.2 step 3).  A unit
  # that was found and left switched off is a machine where every device still
  # reaches all of memory, and it passes every other check in this script.
  if ! grep -Fq "[IRIS][IOMMU] translating units:" "$LOG_FILE" ||
     ! grep -Fq "DMA is contained" "$LOG_FILE"; then
    echo "[headless] a remapping unit was found but DMA is not contained:"
    grep -F "[IRIS][IOMMU]" "$LOG_FILE" | sed 's/^/           /'
    cat "$LOG_FILE"
    exit 1
  fi
else
  if ! grep -Fq "[IRIS][IOMMU]" "$LOG_FILE"; then
    echo "[headless] the kernel said nothing about DMA remapping"
    cat "$LOG_FILE"
    exit 1
  fi
fi

# ...and a DEVICE was actually stopped (Stage 10-dma §10.2 step 6).
#
# Every check above is about what the KERNEL did — it found the units, it
# switched them on, it says DMA is contained.  All of that is consistent with
# hardware that ignored every word of it.  T353 drives a real bus master at a
# real frame, so these lines are the only ones in this script that are evidence
# rather than report, and they are gated separately for that reason.
#
# The selftest gate below would already fail on a FAILING T353.  What it cannot
# catch is a T353 that stopped exercising the thing: the test passes trivially
# on a machine with no DMA-capable device, and a `-device edu` quietly dropped
# from the command line would read exactly like a green run.  So the device
# line is required unconditionally, and then each configuration's own claim.
if [ "$EXPECT_SELFTESTS" = "1" ]; then
  # ...and ring 3 read a real root pointer out of firmware memory, not merely
  # a region that was published.  The revision is whatever the firmware wrote;
  # what is asserted is that something checksummed.
  if ! grep -Eq "^\[IRIS\]\[TEST\] T354 RSDP at 0x[0-9a-f]+ revision " "$LOG_FILE"; then
    echo "[headless] ring 3 did not read the ACPI root pointer"
    grep -F "[IRIS][TEST] T354" "$LOG_FILE" | sed 's/^/           /'
    cat "$LOG_FILE"
    exit 1
  fi
  # ...and the bytes that came off the disk are the bytes on it.  A command
  # that completed and a transfer that landed are different claims about a bus
  # master, and only the second is worth anything.
  # ...and the system has NUMBERS.  Three of them, printed whether or not they
  # pass, because the number is the point: a ceiling says nothing got
  # catastrophically worse, and the log says what it actually costs.  These run
  # under TCG on a machine nobody controls, so they are not hardware figures
  # and are not presented as any.
  if ! grep -Eq "^\[IRIS\]\[TEST\] T356 invoke [0-9]+ ns/op" "$LOG_FILE" ||
     ! grep -Eq "^\[IRIS\]\[TEST\] T356 ipc [0-9]+ ns/op" "$LOG_FILE" ||
     ! grep -Eq "^\[IRIS\]\[TEST\] T356 disk-read-512 [0-9]+ ns/op" "$LOG_FILE"; then
    echo "[headless] the system was not measured"
    grep -F "[IRIS][TEST] T356" "$LOG_FILE" | sed 's/^/           /'
    cat "$LOG_FILE"
    exit 1
  fi
  if ! grep -Fq "[IRIS][TEST] T355 sector 0 read, boot signature ok" "$LOG_FILE"; then
    echo "[headless] the disk read did not produce the data that is on the disk"
    grep -F "[IRIS][TEST] T355" "$LOG_FILE" | sed 's/^/           /'
    cat "$LOG_FILE"
    exit 1
  fi
  if ! grep -Fq "[IRIS][TEST] T353 device 1234:11e8" "$LOG_FILE"; then
    echo "[headless] the DMA-capable device was never found; T353 proved nothing"
    grep -F "[IRIS][TEST] T353" "$LOG_FILE" | sed 's/^/           /'
    cat "$LOG_FILE"
    exit 1
  fi
  if [ "${IRIS_QEMU_IOMMU:-0}" != "0" ]; then
    for marker in \
      "[IRIS][TEST] T353 refused sid" \
      "[IRIS][TEST] T353 granted:" \
      "[IRIS][TEST] T353 revoked:"; do
      if ! grep -Fq "$marker" "$LOG_FILE"; then
        echo "[headless] a device was not contained end to end; missing: $marker"
        grep -F "[IRIS][TEST] T353" "$LOG_FILE" | sed 's/^/           /'
        cat "$LOG_FILE"
        exit 1
      fi
    done
  else
    # The other half of the claim: with no unit the device reaches an address
    # nobody granted it.  Required, because a run where the device silently did
    # nothing would otherwise be indistinguishable from containment that is not
    # there.
    if ! grep -Fq "[IRIS][TEST] T353 no unit: the device reached" "$LOG_FILE"; then
      echo "[headless] no remapping unit, and no evidence the device transferred at all"
      grep -F "[IRIS][TEST] T353" "$LOG_FILE" | sed 's/^/           /'
      cat "$LOG_FILE"
      exit 1
    fi
  fi
fi

# The ABI the kernel implements, and the fact that the root task accepted it
# (Stage 10-abi).  userboot halts the boot on a major mismatch, so reaching the
# scheduler already implies agreement — what this gate adds is that the kernel
# SAYS which ABI, because a log that does not is a log nobody can interpret
# later, and because a version silently reading 0.0 would pass every other
# check in this file.
if ! grep -Eq "^\[IRIS\]\[ABI\] version [1-9][0-9]*\.[0-9]+ " "$LOG_FILE"; then
  echo "[headless] the kernel did not report which ABI it implements"
  grep -F "[IRIS][ABI]" "$LOG_FILE" | sed 's/^/           /'
  cat "$LOG_FILE"
  exit 1
fi

# A filesystem, on a disk, written by a task that holds no hardware (Stage 10).
#
# `gen N` is how many times a boot has mounted this disk, read from the medium
# and written back — so it is the number that makes persistence VISIBLE from
# one run.  What one run cannot show is that it survived, which is what
# `make smoke-persist` is for: it boots twice over one image and then reads the
# image from the host.
#
# `file 1` is a round trip: init wrote a file through the filesystem, through
# the block driver, through the controller, and read the same bytes back.
if ! grep -Eq "^\[USER\]\[INIT\] fs: mounted gen [0-9]+ (formatted|existing) file 1$" "$LOG_FILE"; then
  echo "[headless] no filesystem mounted, or a file did not round-trip:"
  grep -F "fs:" "$LOG_FILE" | sed 's/^/           /'
  cat "$LOG_FILE"
  exit 1
fi

# Networking: a ring-3 e1000 driver moved a frame in both directions (Stage 10).
#
# Two lines, and the second is the one that means something.  `link 1` says the
# driver brought a card up; it does not say a frame ever left the machine, and
# a transmit-only check proves nothing because the card reports a descriptor
# done whether or not anything was listening.  `gateway answered` is an ARP
# round trip against QEMU's own network stack — the transmit path, the receive
# ring, the card's receive filter and a PEER that is not this driver, all at
# once.
#
# `dma contained` versus `dma open` is required per configuration for the
# reason the disk's is: a NIC reads a RING of physical addresses continuously
# and nothing tells it to stop, so it is the clearest case in the tree for the
# remapping unit, and claiming containment on a machine with no unit would be
# a lie.
if ! grep -Eq "^\[USER\]\[INIT\] net: link 1 mac [0-9a-f]{12} dma (contained|open)$" "$LOG_FILE"; then
  echo "[headless] no ring-3 driver brought a network card up:"
  grep -F "net:" "$LOG_FILE" | sed 's/^/           /'
  cat "$LOG_FILE"
  exit 1
fi
if ! grep -Fq "[USER][INIT] net: gateway answered" "$LOG_FILE"; then
  echo "[headless] the network card is up but nothing came back over it:"
  grep -F "net:" "$LOG_FILE" | sed 's/^/           /'
  cat "$LOG_FILE"
  exit 1
fi
if [ "${IRIS_QEMU_IOMMU:-0}" != "0" ]; then
  grep -Fq "net: link 1 mac" "$LOG_FILE" && ! grep -Eq "net: link 1 .* dma contained" "$LOG_FILE" && {
    echo "[headless] the NIC is loose on a machine that can contain it"; cat "$LOG_FILE"; exit 1; }
fi

# Protocol: ARP, IPv4 and UDP above that driver, as a separate service (Stage 10).
#
# The line is a completed TFTP read against the server QEMU's userspace network
# carries at the gateway, and it is here rather than a self-test because every
# part of it is something only a real peer can confirm.
#
# A peer that answers has accepted an ARP reply built by this stack, an IPv4
# header whose checksum it recomputed, and a UDP header whose checksum covers a
# pseudo-header this stack assembled — get any of the three wrong and the
# server drops the datagram silently, which is exactly what makes a
# transmit-only check worthless.  The byte count is checked because a stack can
# receive A frame without receiving THE answer: the reply has to be matched to
# the ephemeral port the request went out from, not the well-known port it went
# to, and a stack that ignores ports reads back whatever arrived first.
if ! grep -Eq "^\[USER\]\[INIT\] ip: udp round trip ok, tftp data [0-9]+ bytes$" "$LOG_FILE"; then
  echo "[headless] no UDP round trip against a real server:"
  grep -F "ip:" "$LOG_FILE" | sed 's/^/           /'
  cat "$LOG_FILE"
  exit 1
fi

# The kernel survived a fault of its own (ledger A-37).
#
# `idt.c` halts on any exception that did not come from ring 3, which is the
# right posture for a kernel that intends never to take one -- and there was
# exactly one place where the intent could not be guaranteed: the store into
# user memory, whose mapping another CPU can retire between the range check and
# the write.  Ring 3 could therefore stop the kernel.
#
# The exception table closes it, and this line is the proof that the mechanism
# WORKS rather than merely exists: the kernel aims that store at a non-canonical
# address on purpose, takes the #GP, and comes back.  Before the table, this
# marker could not have been printed, because the machine would have stopped
# inside the store.
if [ "${IRIS_QEMU_EXPECT_SELFTESTS:-0}" != "0" ]; then
  if ! grep -Fq "[IRIS][P3] exception table: a kernel fault was survived" "$LOG_FILE"; then
    echo "[headless] the kernel did not survive a fault it is supposed to survive:"
    grep -F "[IRIS][P3]" "$LOG_FILE" | sed 's/^/           /'
    cat "$LOG_FILE"
    exit 1
  fi
fi

# Storage: a ring-3 AHCI driver brought a real disk up (Stage 10).
#
# Three claims in one line, and they fail apart.  `disk N` is a COUNT of the
# disks the driver brought up — this machine has two, the one it boots from and
# the one the filesystem lives on — and a non-zero count means the driver
# claimed its controller, built its command structures, brought a port up and
# READ A SECTOR — a port that configured and could not read is not counted, so
# a service that started and could not drive anything does not read as success.  The source id is the
# controller's, which is what an IOSpace binds to.  And `dma contained` versus
# `dma open` is the difference the IOMMU makes, seen from the one driver that
# most needs it: AHCI takes physical addresses from its driver, so an
# uncontained controller writes wherever the driver says.
#
# The two arms are checked separately below, because "contained" on a machine
# with no unit and "open" on a machine with one are both lies and neither would
# be caught by looking for the line alone.
# `window N` is required to be NON-ZERO here, and that is the containment:
# it is the number of sectors the driver will let anything address, which is
# the IRIS partition and nothing else.  A window of zero is a disk that is
# present and carries no partition of ours -- correct behaviour on a stranger's
# drive, and a failure on the image this runner just built.
if ! grep -Eq "^\[USER\]\[INIT\] blk: disk [1-9] sid 0x[0-9a-f]+ dma (contained|open) window [1-9][0-9]* home [0-9]+$" "$LOG_FILE"; then
  echo "[headless] no ring-3 driver brought a disk up:"
  grep -F "blk:" "$LOG_FILE" | sed 's/^/           /'
  cat "$LOG_FILE"
  exit 1
fi
if [ "${IRIS_QEMU_IOMMU:-0}" != "0" ]; then
  if ! grep -Fq "dma contained" "$LOG_FILE"; then
    echo "[headless] a bus master is loose on a machine that can contain it"
    grep -F "blk:" "$LOG_FILE" | sed 's/^/           /'
    cat "$LOG_FILE"
    exit 1
  fi
else
  if ! grep -Fq "dma open" "$LOG_FILE"; then
    echo "[headless] the disk driver claims containment with no unit present"
    grep -F "blk:" "$LOG_FILE" | sed 's/^/           /'
    cat "$LOG_FILE"
    exit 1
  fi
fi

# The firmware's own description is reachable from ring 3 (Stage 10).
#
# ACPI tables live in memory that is neither usable RAM nor unmapped address
# space, so until this stage no capability in the system named it and ring 3
# could not read a word.  The root task reports whether the pointer that
# anchors every table is inside a region it was handed; the SUITE (T354) does
# the read.  Both are required, because "the region was published" and "the
# region is readable" are different claims and the first is cheap to get right
# while being useless.
if ! grep -Fq "[USERBOOT] ACPI: root pointer reachable from ring 3" "$LOG_FILE"; then
  echo "[headless] ACPI is not reachable from ring 3:"
  grep -F "ACPI" "$LOG_FILE" | sed 's/^/           /'
  cat "$LOG_FILE"
  exit 1
fi

# The PCI bus service came up and described the machine (Stage 10).
#
# Three separate claims, and they fail apart: the service STARTED (init's call
# returned), it FOUND devices, and it CARVED a frame over every window in the
# region it owns.  A service that started and found nothing would answer every
# driver's claim with a refusal, and from outside that is indistinguishable
# from a machine with no devices on it — which is why the counts are in the
# line and why `carve 0` is required rather than merely logged.
if ! grep -Eq "^\[USER\]\[INIT\] pci: functions [1-9][0-9]* windows [1-9][0-9]* carve 0$" "$LOG_FILE"; then
  echo "[headless] the PCI bus service did not describe the machine:"
  grep -F "pci:" "$LOG_FILE" | sed 's/^/           /'
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[SVCMGR] ready" "$LOG_FILE"; then
  echo "[headless] missing svcmgr ready marker"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[USER][INIT][BOOT] healthy path OK" "$LOG_FILE"; then
  echo "[headless] missing healthy-path marker"
  cat "$LOG_FILE"
  exit 1
fi

# Phase 13 (Track I): the init "[USER] kbd shared reply OK" legacy-KChannel probe
# is retired — kbd is endpoint-only. kbd.ep liveness is covered by
# "[SH] kbd cptr OK" plus iris_test T034/T035/T044/T058.

if ! grep -Fq "VFS ready" "$LOG_FILE"; then
  echo "[headless] missing VFS ready marker"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[VFS] ep ready" "$LOG_FILE"; then
  echo "[headless] missing VFS endpoint-ready marker (Phase 7.1)"
  cat "$LOG_FILE"
  exit 1
fi

# Phase 8: sh is a pure CPtr-first client — every core service path is gated
# on a "cptr OK" marker printed only after a live PING through the slot.
if ! grep -Fq "[SH] vfs cptr OK" "$LOG_FILE"; then
  echo "[headless] missing SH vfs-CPtr marker (Phase 8)"
  cat "$LOG_FILE"
  exit 1
fi

# NOTE: kbd itself has no console cap (give_console=0), so its boot prints
# never reach this log; kbd.ep liveness is covered by "[SH] kbd cptr OK" and
# iris_test T034/T035/T044 instead.

if ! grep -Fq "[USER] console ep OK" "$LOG_FILE"; then
  echo "[headless] missing init console-endpoint marker (Phase 7.3)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[SH] console cptr OK" "$LOG_FILE"; then
  echo "[headless] missing SH console-CPtr marker (Phase 8)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[VFS] console cptr OK" "$LOG_FILE"; then
  echo "[headless] missing VFS console-CPtr marker (Phase 8)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[IRIS][TEST] console cptr write OK" "$LOG_FILE"; then
  echo "[headless] missing iris_test console-CPtr write marker (Phase 8 / T043)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[SH] svcmgr cptr OK" "$LOG_FILE"; then
  echo "[headless] missing SH CPtr-first discovery marker (Phase 8)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[SH] kbd cptr OK" "$LOG_FILE"; then
  echo "[headless] missing SH kbd-CPtr marker (Phase 8)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[USER] vfs ep list OK" "$LOG_FILE"; then
  echo "[headless] missing init VFS-endpoint LIST marker (Phase 7.2)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[USER] vfs ep read OK" "$LOG_FILE"; then
  echo "[headless] missing init VFS-endpoint READ_AT marker (Phase 7.2)"
  cat "$LOG_FILE"
  exit 1
fi

# Phase 13 (Track E/F): legacy KChannel DIAG marker retired — diagnostics are
# now served over IRIS_SVCMGR_EP_DIAG and covered by runtime test T067.

# Phase 13 (Track F): the init TIMED / S9 (channel seal) / S10 (rights reduction)
# KChannel selftests are retired — their coverage moved to iris_test endpoint /
# notification / cap-transfer tests (T010/T019/T052/T064).

if ! grep -Fq "[USER][INIT][S8] exception delivery OK" "$LOG_FILE"; then
  echo "[headless] missing exception delivery selftest marker"
  cat "$LOG_FILE"
  exit 1
fi

# A pre-start mint that failed.  These are non-fatal by design — a child with
# an empty slot still runs — which is exactly why they need a gate: a child
# quietly missing an authority passes every other check here until something
# asks for it, and what asks might be a test three hundred cases away (A-34).
if grep -Fq "[INIT] MINT FAILED" "$LOG_FILE"; then
  echo "[headless] a spawn's pre-start capability mint failed:"
  grep -F "[INIT] MINT FAILED" "$LOG_FILE" | sed 's/^/           /'
  echo "           e=07 is ALREADY_EXISTS: two entries of one mint table"
  echo "           almost certainly name the same destination slot."
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[IRIS][TEST] SUITE PASS" "$LOG_FILE"; then
  echo "[headless] missing iris_test SUITE PASS marker"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[SVCMGR] ep ready" "$LOG_FILE"; then
  echo "[headless] missing svcmgr endpoint-ready marker (Phase 7)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[IRIS][USER] boot untyped CSpace grants:" "$LOG_FILE"; then
  echo "[headless] missing boot-untyped-CSpace-grants marker (Phase 3.4)"
  cat "$LOG_FILE"
  exit 1
fi

# Stage 5 Step 2: the monolithic bootstrap capability is gone, so its marker
# is too.  What the boot must now announce is the six control capabilities —
# one per authority — because a boot that published only some of them aborts
# the root task rather than continuing with partial authority.
if ! grep -Fq "[IRIS][USER] boot control caps CSpace grants OK" "$LOG_FILE"; then
  echo "[headless] missing boot-control-caps-CSpace-grants marker (Stage 5)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[IRIS][USER] boot vspace CSpace grants OK" "$LOG_FILE"; then
  echo "[headless] missing boot-vspace-CSpace-grants marker (Phase 4)"
  cat "$LOG_FILE"
  exit 1
fi

if [ "$EXPECT_SELFTESTS" = "1" ]; then
  if ! grep -Fq "[IRIS][P3] handle/lifecycle selftests OK" "$LOG_FILE"; then
    echo "[headless] missing phase-3 selftest marker"
    cat "$LOG_FILE"
    exit 1
  fi
  if ! grep -Fq "[IRIS][P41] rights selftests OK" "$LOG_FILE"; then
    echo "[headless] missing phase-41 rights selftest marker"
    cat "$LOG_FILE"
    exit 1
  fi
  # Phase 13 (Track E/F): legacy svcmgr KChannel DIAG kbd-status aggregation
  # retired; svcmgr diagnostics are served over IRIS_SVCMGR_EP_DIAG (T067).
fi

# 0 is a clean exit, 124 is the deadline, and 143/137 are this script ending a
# run that had already said everything it was going to say.  Anything else is
# qemu itself falling over, which is not a kernel result and must not read as
# one.
if [ "$qemu_rc" -ne 0 ] && [ "$qemu_rc" -ne 124 ] &&
   [ "$qemu_rc" -ne 143 ] && [ "$qemu_rc" -ne 137 ]; then
  echo "[headless] qemu exited unexpectedly with code $qemu_rc"
  cat "$LOG_FILE"
  exit "$qemu_rc"
fi

echo "[headless] healthy runtime signature observed"
echo "[headless] log: $LOG_FILE"
