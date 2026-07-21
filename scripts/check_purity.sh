#!/usr/bin/env bash
#
# check_purity.sh — guarda ejecutable del charter de pureza seL4.
#
# Congela los consumidores/productores legacy de autoridad-por-handle y los
# usos de kslab en el código PRODUCTIVO (kernel/ + services/, excluyendo el
# código test-only).  La allowlist (scripts/purity_allowlist.txt) fija el
# máximo de apariciones permitidas por archivo de cada identificador
# congelado; este gate FALLA si:
#
#   1. un archivo fuera de la allowlist contiene un identificador congelado;
#   2. un archivo supera el conteo congelado para un identificador.
#
# Bajar un conteo es progreso (el gate lo informa; actualizar la allowlist a
# la baja en el mismo cambio).  SUBIRLO exige modificar el charter y el
# ledger en el mismo commit con justificación técnica (charter §3).
#
# Identificadores congelados:
#   handle_table_insert        — productores de handles (incluye _badged/_derived)
#   handle_table_get_object    — consumidores de handles
#   cspace_or_handle_resolve_  — resolución dual CPtr/handle
#   kslab_alloc                — objetos kernel nacidos del heap global
#
# Test-only (excluido del escaneo): services/iris_test, services/lifecycle_probe,
# tests/.  El charter prohíbe caminos PRODUCTIVOS nuevos; los tests ejercitan
# la semántica legacy deliberadamente mientras exista.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

ALLOWLIST="scripts/purity_allowlist.txt"
PATTERNS=(handle_table_insert handle_table_get_object cspace_or_handle_resolve_ kslab_alloc)

if [ ! -f "$ALLOWLIST" ]; then
    echo "[purity] FAIL: allowlist $ALLOWLIST no existe"
    exit 1
fi

# Archivos productivos: kernel/ y services/ menos los test-only.
mapfile -t FILES < <(find kernel services \
    \( -path services/iris_test -o -path services/lifecycle_probe \) -prune -o \
    \( -name '*.c' -o -name '*.h' \) -print | sort)

fail=0
progress=0

count_in_file() {
    # grep -c cuenta líneas; -o cuenta apariciones — usamos -o | wc -l.
    # (grep sale 1 sin matches: neutralizado para set -e/pipefail.)
    { grep -oE "$2" "$1" 2>/dev/null || true; } | wc -l
}

allow_for() {
    # allowlist format: <file> <pattern> <max-count>
    awk -v f="$1" -v p="$2" '!/^#/ && $1 == f && $2 == p { print $3; found=1 } END { if (!found) print "0" }' "$ALLOWLIST" | head -1
}

for f in "${FILES[@]}"; do
    for p in "${PATTERNS[@]}"; do
        n=$(count_in_file "$f" "$p")
        [ "$n" -eq 0 ] && continue
        max=$(allow_for "$f" "$p")
        if [ "$n" -gt "$max" ]; then
            echo "[purity] FAIL: $f usa '$p' $n veces (congelado: $max)."
            echo "         Charter §3: prohibido añadir productores/consumidores de"
            echo "         handles o usos de kslab. Si esto es una reducción legítima"
            echo "         de otro sitio, la allowlist NO se toca; si es un uso nuevo,"
            echo "         el cambio debe rechazarse (o modificar charter+ledger con"
            echo "         justificación en el mismo commit)."
            fail=1
        elif [ "$n" -lt "$max" ]; then
            echo "[purity] progreso: $f '$p' $n < $max — considerar bajar la allowlist"
            progress=1
        fi
    done
done

# Entradas de la allowlist cuyo archivo ya no usa el patrón (o no existe):
# recordatorio de limpieza, no fallo.
while read -r f p max; do
    case "$f" in \#*|"") continue ;; esac
    if [ ! -f "$f" ]; then
        echo "[purity] nota: entrada huérfana en allowlist ($f) — retirar"
        continue
    fi
    n=$(count_in_file "$f" "$p")
    if [ "$n" -eq 0 ] && [ "$max" -gt 0 ]; then
        echo "[purity] progreso: $f ya no usa '$p' — retirar de la allowlist"
        progress=1
    fi
done < "$ALLOWLIST"

if [ "$fail" -ne 0 ]; then
    echo "[purity] RESULT: FAIL"
    exit 1
fi
echo "[purity] RESULT: OK (allowlist respetada$( [ $progress -eq 1 ] && echo '; hay progreso pendiente de consolidar' ))"
