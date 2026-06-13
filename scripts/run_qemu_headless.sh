#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EFI_ROOT="$PROJECT_ROOT/build/efi_root"
LOG_FILE="${IRIS_QEMU_LOG:-$PROJECT_ROOT/build/qemu-headless.log}"
TIMEOUT_SECS="${IRIS_QEMU_TIMEOUT_SECS:-25}"
EXPECT_SELFTESTS="${IRIS_QEMU_EXPECT_SELFTESTS:-0}"

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
  -cpu max \
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

if ! grep -Fq "[USER] kbd shared reply OK" "$LOG_FILE"; then
  echo "[headless] missing shared-reply guard marker"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "VFS ready" "$LOG_FILE"; then
  echo "[headless] missing VFS ready marker"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[VFS] ep ready" "$LOG_FILE"; then
  echo "[headless] missing VFS endpoint-ready marker (Fase 7.1)"
  cat "$LOG_FILE"
  exit 1
fi

# Fase 8: sh is a pure CPtr-first client — every core service path is gated
# on a "cptr OK" marker printed only after a live PING through the slot.
if ! grep -Fq "[SH] vfs cptr OK" "$LOG_FILE"; then
  echo "[headless] missing SH vfs-CPtr marker (Fase 8)"
  cat "$LOG_FILE"
  exit 1
fi

# NOTE: kbd itself has no console cap (give_console=0), so its boot prints
# never reach this log; kbd.ep liveness is covered by "[SH] kbd cptr OK" and
# iris_test T034/T035/T044 instead.

if ! grep -Fq "[USER] console ep OK" "$LOG_FILE"; then
  echo "[headless] missing init console-endpoint marker (Fase 7.3)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[SH] console cptr OK" "$LOG_FILE"; then
  echo "[headless] missing SH console-CPtr marker (Fase 8)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[VFS] console cptr OK" "$LOG_FILE"; then
  echo "[headless] missing VFS console-CPtr marker (Fase 8)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[IRIS][TEST] console cptr write OK" "$LOG_FILE"; then
  echo "[headless] missing iris_test console-CPtr write marker (Fase 8 / T043)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[SH] svcmgr cptr OK" "$LOG_FILE"; then
  echo "[headless] missing SH CPtr-first discovery marker (Fase 8)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[SH] kbd cptr OK" "$LOG_FILE"; then
  echo "[headless] missing SH kbd-CPtr marker (Fase 8)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[USER] vfs ep list OK" "$LOG_FILE"; then
  echo "[headless] missing init VFS-endpoint LIST marker (Fase 7.2)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[USER] vfs ep read OK" "$LOG_FILE"; then
  echo "[headless] missing init VFS-endpoint READ_AT marker (Fase 7.2)"
  cat "$LOG_FILE"
  exit 1
fi

# Fase 13 (Track E/F): legacy KChannel DIAG marker retired — diagnostics are
# now served over IRIS_SVCMGR_EP_DIAG and covered by runtime test T067.

# Fase 13 (Track F): the init TIMED / S9 (channel seal) / S10 (rights reduction)
# KChannel selftests are retired — their coverage moved to iris_test endpoint /
# notification / cap-transfer tests (T010/T019/T052/T064).

if ! grep -Fq "[USER][INIT][S8] exception delivery OK" "$LOG_FILE"; then
  echo "[headless] missing exception delivery selftest marker"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[IRIS][TEST] SUITE PASS" "$LOG_FILE"; then
  echo "[headless] missing iris_test SUITE PASS marker"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[SVCMGR] ep ready" "$LOG_FILE"; then
  echo "[headless] missing svcmgr endpoint-ready marker (Fase 7)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[IRIS][USER] boot untyped CSpace grants:" "$LOG_FILE"; then
  echo "[headless] missing boot-untyped-CSpace-grants marker (Fase 3.4)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[IRIS][USER] boot bootstrap cap CSpace grants OK" "$LOG_FILE"; then
  echo "[headless] missing boot-bootstrap-cap-CSpace-grants marker (Fase 3.5)"
  cat "$LOG_FILE"
  exit 1
fi

if ! grep -Fq "[IRIS][USER] boot vspace CSpace grants OK" "$LOG_FILE"; then
  echo "[headless] missing boot-vspace-CSpace-grants marker (Fase 4)"
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
  # Fase 13 (Track E/F): legacy svcmgr KChannel DIAG kbd-status aggregation
  # retired; svcmgr diagnostics are served over IRIS_SVCMGR_EP_DIAG (T067).
fi

if [ "$qemu_rc" -ne 0 ] && [ "$qemu_rc" -ne 124 ]; then
  echo "[headless] qemu exited unexpectedly with code $qemu_rc"
  cat "$LOG_FILE"
  exit "$qemu_rc"
fi

echo "[headless] healthy runtime signature observed"
echo "[headless] log: $LOG_FILE"
