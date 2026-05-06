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
  /usr/share/edk2/x64/OVMF_CODE.fd
)"

OVMF_VARS_TEMPLATE="$(pick_first \
  /usr/share/OVMF/OVMF_VARS_4M.fd \
  /usr/share/OVMF/OVMF_VARS.fd \
  /usr/share/OVMF/OVMF_VARS.ms.fd \
  /usr/share/qemu/OVMF_VARS_4M.fd \
  /usr/share/qemu/OVMF_VARS.fd \
  /usr/share/edk2/ovmf/OVMF_VARS.fd \
  /usr/share/edk2/x64/OVMF_VARS.fd
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

if [ "$EXPECT_SELFTESTS" = "1" ]; then
  if ! grep -Fq "[IRIS][P3] handle/lifecycle selftests OK" "$LOG_FILE"; then
    echo "[headless] missing phase-3 selftest marker"
    cat "$LOG_FILE"
    exit 1
  fi
  if ! grep -Fq "[USER][INIT][DIAG] reply" "$LOG_FILE"; then
    echo "[headless] missing init diag reply marker"
    cat "$LOG_FILE"
    exit 1
  fi
  if ! grep -Fq "[SVCMGR][DIAG] kbd status OK" "$LOG_FILE"; then
    echo "[headless] missing svcmgr kbd diag marker"
    cat "$LOG_FILE"
    exit 1
  fi
fi

if [ "$qemu_rc" -ne 0 ] && [ "$qemu_rc" -ne 124 ]; then
  echo "[headless] qemu exited unexpectedly with code $qemu_rc"
  cat "$LOG_FILE"
  exit "$qemu_rc"
fi

echo "[headless] healthy runtime signature observed"
echo "[headless] log: $LOG_FILE"
