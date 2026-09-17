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

set +e
timeout "${TIMEOUT_SECS}s" qemu-system-x86_64 \
  -machine q35 \
  "${IOMMU_ARGS[@]}" \
  -cpu max \
  -smp "$SMP" \
  -m 512M \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$PROJECT_ROOT/build/OVMF_VARS.headless.fd" \
  -drive format=raw,file=fat:rw:"$EFI_ROOT" \
  -serial "file:$LOG_FILE" \
  -display none \
  -monitor none \
  -net none \
  -no-reboot \
  -no-shutdown
qemu_rc=$?
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
else
  if ! grep -Fq "[IRIS][IOMMU]" "$LOG_FILE"; then
    echo "[headless] the kernel said nothing about DMA remapping"
    cat "$LOG_FILE"
    exit 1
  fi
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

if [ "$qemu_rc" -ne 0 ] && [ "$qemu_rc" -ne 124 ]; then
  echo "[headless] qemu exited unexpectedly with code $qemu_rc"
  cat "$LOG_FILE"
  exit "$qemu_rc"
fi

echo "[headless] healthy runtime signature observed"
echo "[headless] log: $LOG_FILE"
