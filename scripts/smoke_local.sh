#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$PROJECT_ROOT"

echo "[smoke] clean build (default configuration)"
make clean
make
make check

echo "[smoke] clean build with runtime selftests enabled"
make clean
make ENABLE_RUNTIME_SELFTESTS=1
make check

echo "[smoke] completed"
