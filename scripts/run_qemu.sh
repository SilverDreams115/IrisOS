#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EFI_ROOT="$PROJECT_ROOT/build/efi_root"

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
  echo "No se encontraron archivos OVMF válidos."
  ls -l /usr/share/OVMF 2>/dev/null || true
  ls -l /usr/share/qemu 2>/dev/null || true
  ls -l /usr/share/edk2/ovmf 2>/dev/null || true
  exit 1
fi

mkdir -p "$PROJECT_ROOT/build"
cp -f "$OVMF_VARS_TEMPLATE" "$PROJECT_ROOT/build/OVMF_VARS.fd"

qemu-system-x86_64 \
  -machine q35 \
  -cpu max \
  -m 512M \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$PROJECT_ROOT/build/OVMF_VARS.fd" \
  -drive format=raw,file=fat:rw:"$EFI_ROOT" \
  -serial stdio \
  -vga std \
  -display gtk,zoom-to-fit=on \
 -no-reboot \
 -no-shutdown \
 -d cpu_reset
