#!/bin/bash
# r6_coverage.sh — R6: code coverage via gcov + gcovr.
#
# Cria build dedicado em build-coverage/ com -fprofile-arcs -ftest-coverage,
# roda ctest e o set de validações (sprints + R3/R4/R5), gera report HTML
# + textual em tests/validation/r6_results/.
#
# Pré-requisito: gcovr instalado (pip3 install --user --break-system-packages gcovr)

set -eo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)

GCOVR="$HOME/.local/bin/gcovr"
[[ -x "$GCOVR" ]] || GCOVR=$(which gcovr)
[[ -n "$GCOVR" ]] || { echo "FALTA: gcovr"; exit 1; }

BUILD=build-coverage
OUT=tests/validation/r6_results
mkdir -p "$OUT"

# ── Setup build com coverage ─────────────────────────────────────────────────
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    echo "→ Configurando build-coverage (cmake)..."
    CXX=g++-14 cmake -S . -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-O0 -g --coverage -fprofile-arcs -ftest-coverage" \
        -DCMAKE_C_FLAGS="-O0 -g --coverage" \
        -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
        -DCMAKE_SHARED_LINKER_FLAGS="--coverage" \
        2>&1 | tail -5
fi

echo "→ Build coverage..."
CXX=g++-14 cmake --build "$BUILD" -j8 2>&1 | tail -3

# ── Reset profile data ───────────────────────────────────────────────────────
echo "→ Resetting profile data..."
find "$BUILD" -name "*.gcda" -delete 2>/dev/null || true

# ── Roda ctest (cobre unidades) ──────────────────────────────────────────────
echo "→ Rodando ctest..."
(cd "$BUILD" && ctest -j4 --output-on-failure 2>&1 | tail -3) || echo "  (alguns testes falharam)"

# ── Roda validações Tier A/B + R3/R4/R5 ──────────────────────────────────────
# Usa o run_queries do build-coverage para gerar gcda dos planners/ast.
RUNQ="$BUILD/tests/run_queries"
if [[ -x "$RUNQ" ]]; then
    echo "→ Rodando validation suite (com binário coverage)..."
    GFA_A=data/validation/tier_a/tiny.gfa
    GFA_B=data/validation/tier_b/DRB1-3123.gfa

    # Re-ingere com binário coverage para aumentar cobertura de ingestor
    BENCH_COV="$BUILD/bench_gbwt"
    if [[ -x "$BENCH_COV" ]]; then
        rm -rf /tmp/cov_tiny.idx /tmp/cov_drb1.idx
        cp "$GFA_A" /tmp/cov_tiny.gfa
        cp "$GFA_B" /tmp/cov_drb1.gfa
        "$BENCH_COV" /tmp/cov_tiny.gfa 4 --quiet 2>&1 | tail -3 >/dev/null || true
        "$BENCH_COV" /tmp/cov_drb1.gfa 4 --quiet 2>&1 | tail -3 >/dev/null || true
    fi

    # Roda algumas queries representativas
    IDX_A=/tmp/cov_tiny.gfa.idx
    IDX_B=/tmp/cov_drb1.gfa.idx
    [[ -d "$IDX_A" ]] && "$RUNQ" "$IDX_A" 2>/dev/null >/dev/null || true
    if [[ -d "$IDX_B" ]]; then
        for q in \
            "FIND SEGMENT" \
            "FIND EDGE" \
            "FIND PATH" \
            "FIND BUBBLE" \
            "FIND TIP" \
            "FIND SEGMENT WHERE length > 30" \
            "FIND SEGMENT WHERE length > 30 AND length < 32" \
            "FIND SEGMENT WHERE length = 1 OR length > 30" \
            "FIND SEGMENT WHERE (length > 30 AND length < 32) OR length = 1" \
            "FIND PATH RETURN name LIMIT 5 OFFSET 2" \
            "SEARCH KMER \"ACGT\"" \
            ; do
            "$RUNQ" "$IDX_B" "$q" 2>/dev/null >/dev/null || true
        done
    fi
fi

# Limpa .gcda de CMakeCompilerId (não tem source acessível)
find "$BUILD/CMakeFiles" -name "CMake*CompilerId*.gcda" -delete 2>/dev/null || true

# ── Gera report ──────────────────────────────────────────────────────────────
echo "→ Gerando report gcovr (--gcov-executable=gcov-14 para casar com g++-14)..."
"$GCOVR" \
    --gcov-executable gcov-14 \
    --root "$ROOT" \
    --object-directory "$BUILD" \
    --filter "$ROOT/src/" \
    --filter "$ROOT/include/" \
    --exclude "$ROOT/third_party/" \
    --exclude "$ROOT/build.*" \
    --gcov-ignore-errors=no_working_dir_found \
    --gcov-ignore-errors=source_not_found \
    --html-details "$OUT/index.html" \
    --txt "$OUT/summary.txt" \
    --csv "$OUT/summary.csv" \
    --json-summary "$OUT/summary.json" \
    --print-summary \
    2>&1 | tail -15

echo
echo "── Top 15 arquivos com menor cobertura (≥50 linhas executáveis) ──"
SUMMARY_CSV="$OUT/summary.csv" python3 << 'PYEOF'
import csv, os
csv_path = os.environ["SUMMARY_CSV"]
with open(csv_path) as f:
    rows = list(csv.DictReader(f))
# line_percent é fração 0-1 no CSV do gcovr
rows = [r for r in rows if int(r.get("line_total", 0)) >= 50]
rows.sort(key=lambda r: float(r.get("line_percent", 0)))
print(f"{'Arquivo':<55} {'Linhas':>7} {'%cov':>6} {'%branch':>8}")
print("-" * 78)
for r in rows[:15]:
    f = r["filename"]
    if len(f) > 54: f = "..." + f[-51:]
    n = r.get("line_total", "?")
    lp = float(r.get("line_percent", 0)) * 100
    bp = float(r.get("branch_percent", 0)) * 100 if r.get("branch_percent") else 0
    print(f"{f:<55} {n:>7} {lp:>5.1f}% {bp:>7.1f}%")
PYEOF

echo
echo "✅ R6: relatório em $OUT/"
echo "   - index.html  (drill-down por arquivo/função)"
echo "   - summary.txt (texto)"
echo "   - summary.csv (planilha)"
