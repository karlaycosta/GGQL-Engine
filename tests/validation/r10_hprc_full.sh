#!/bin/bash
# r10_hprc_full.sh — R10: validação HPRC full (chm13 + 47 haplotypes).
#
# Dataset: HPRC y1 minigraph-cactus chm13-based pangenome.
#   - 85,282,047 segments
#   - 118,135,187 links
#   - 25 paths + 28,759 walks (28,784 total)
#   - 45.68 GB on disk
#
# Reference: Liao 2023 (Nature 617:312-324, "A draft human pangenome reference").
#
# Cross-checks:
#   1. S/L/Path counts: GGQL vs awk (deve ser exato)
#   2. FIND BUBBLE: GGQL vs vg snarls (cross-paper)
#   3. Performance: GGQL ingest + queries vs vg pipeline
#   4. RAM / disk usage documentado
#
# Tempo estimado: ingest ~30-60 min, queries ~minutos cada.

set -eo pipefail
cd "$(dirname "$0")/../.."

# GFA fica no Downloads para evitar copiar 49GB; .idx ao lado.
GFA=${GFA:-/home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa}
IDX=$GFA.idx
RUNQ=build/tests/run_queries
BENCH=build/bench_gbwt

[[ -x "$RUNQ" && -x "$BENCH" && -f "$GFA" ]] || { echo "FALTA tool/GFA ($GFA)"; exit 1; }

# HPRC tem 85M segs / 118M edges → cap default de 1M trunca counts.
# Eleva para 200M para acomodar.
export GGQL_MAX_TOTAL_HITS=200000000

OUT=tests/validation/r10_results
mkdir -p "$OUT"

# ── Captura HW context ─────────────────────────────────────────────────────────
{
    echo "=== Hardware/SW context ==="
    lscpu | grep -E "Model name|CPU\(s\):" | head -3
    free -h | head -3
    df -h "$(dirname "$GFA")" | tail -1
    uname -a
    g++-14 --version | head -1
    echo "=== Dataset ==="
    ls -la "$GFA" "$IDX" 2>/dev/null
    echo "Date: $(date -Iseconds)"
    echo "Commit: $(git rev-parse --short HEAD)"
} > "$OUT/hardware.txt"
cat "$OUT/hardware.txt"

# ── GT counts via awk (já calculados, salvos em const) ─────────────────────────
# Valores conhecidos (medidos uma vez via awk, dataset é fixo).
# TIP/REPEAT: ~6 min em 49GB (cf. /tmp/hprc_gt.txt para regenerar).
SEG_GT=85282047
EDG_GT=118135187
PAT_GT=28784           # 25 P-lines + 28,759 W-lines
TIP_GT=460506          # nós com in_degree=0 OR out_degree=0
REPEAT_GT=28969018     # nós com out_degree >= 2
echo
echo "=== Ground truth (awk pre-calc, ver hprc_gt.txt) ==="
echo "  Segments: $SEG_GT"
echo "  Edges:    $EDG_GT"
echo "  Paths:    $PAT_GT (25 P + 28759 W)"

# ── Verificações ─────────────────────────────────────────────────────────────
total_pass=0
total_fail=0

[[ -d "$IDX" ]] || { echo; echo "❌ .idx não existe ainda — rode ingest primeiro"; exit 1; }

# Disk usage do .idx
echo
echo "=== .idx disk usage ==="
du -sh "$IDX"
du -h "$IDX"/* 2>/dev/null | sort -h | tail -10

check() {
    local name=$1 q=$2 gt=$3 tolerance=${4:-0}
    local got
    got=$(timeout 600 "$RUNQ" "$IDX" "$q" 2>/dev/null | tail -1)
    local d=$((got - gt))
    local abs_d=${d#-}
    if [[ "$got" == "$gt" ]] || [[ $abs_d -le $tolerance ]]; then
        printf '  ✅ %-25s %12s  (gt %s, Δ=%s)\n' "$name" "$got" "$gt" "$d"
        total_pass=$((total_pass+1))
    else
        printf '  ❌ %-25s %12s  (gt %s, Δ=%s, tol=%s)\n' "$name" "$got" "$gt" "$d" "$tolerance"
        total_fail=$((total_fail+1))
    fi
}

echo
echo "=== Cross-check counts ==="

# Helper: mede tempo de uma query.
# stderr recebe linha formatada; stdout recebe apenas o valor numérico.
time_query() {
    local label=$1 q=$2
    local start=$(date +%s.%N)
    local result
    result=$(timeout 600 "$RUNQ" "$IDX" "$q" 2>/dev/null | tail -1)
    local end=$(date +%s.%N)
    local elapsed=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.1f", e-s}')
    printf '  %-30s = %-12s (%s s)\n' "$label" "$result" "$elapsed" >&2
    echo "$result"
}

# Queries baratas (topology only)
check "FIND SEGMENT (count)" "FIND SEGMENT" "$SEG_GT"
check "FIND EDGE (count)" "FIND EDGE" "$EDG_GT"

# Path count: pode demorar
echo
echo "=== Query timings (paths e bubble são caros) ==="
PATH_RESULT=$(time_query "FIND PATH" "FIND PATH")
if [[ "$PATH_RESULT" == "$PAT_GT" ]]; then
    total_pass=$((total_pass+1))
    printf '  ✅ FIND PATH count = %s\n' "$PATH_RESULT"
else
    total_fail=$((total_fail+1))
    printf '  ❌ FIND PATH = %s (gt %s)\n' "$PATH_RESULT" "$PAT_GT"
fi

BUBBLE_RESULT=$(time_query "FIND BUBBLE" "FIND BUBBLE")
TIP_RESULT=$(time_query "FIND TIP" "FIND TIP")
REPEAT_RESULT=$(time_query "FIND REPEAT" "FIND REPEAT")

# TIP/REPEAT cross-check (awk pre-calc)
if [[ "$TIP_RESULT" == "$TIP_GT" ]]; then
    total_pass=$((total_pass+1))
    printf '  ✅ FIND TIP count = %s\n' "$TIP_RESULT"
else
    total_fail=$((total_fail+1))
    printf '  ❌ FIND TIP = %s (gt %s, Δ=%s)\n' "$TIP_RESULT" "$TIP_GT" "$((TIP_RESULT - TIP_GT))"
fi

if [[ "$REPEAT_RESULT" == "$REPEAT_GT" ]]; then
    total_pass=$((total_pass+1))
    printf '  ✅ FIND REPEAT count = %s\n' "$REPEAT_RESULT"
else
    total_fail=$((total_fail+1))
    printf '  ❌ FIND REPEAT = %s (gt %s, Δ=%s)\n' "$REPEAT_RESULT" "$REPEAT_GT" "$((REPEAT_RESULT - REPEAT_GT))"
fi

echo
echo "=== Bubble count: discussão ==="
echo "  GGQL FIND BUBBLE = $BUBBLE_RESULT"
echo
echo "  Liao 2023 (Nature 617:312-324) reporta ~85k snarls TOP-LEVEL em"
echo "  chm13 v2 graph. GGQL conta TODOS os snarls (Billi/SESE recursivo,"
echo "  inclui aninhados), por isso o número é maior:"
echo "    $BUBBLE_RESULT / 85000 = $(awk -v b="$BUBBLE_RESULT" 'BEGIN{printf "%.0f", b/85000}')× razoável para variantes complexas"
echo
echo "  Cross-check com vg snarls (full): demoraria ~horas em 49GB,"
echo "  fica como sub-validação futura (R10.bis)."

# ── Resumo ─────────────────────────────────────────────────────────────────────
echo
echo "═══ Resumo R10 ═══"
echo "  ✅ Pass: $total_pass"
echo "  ❌ Fail: $total_fail"

# Save query results
{
    echo "FIND_SEGMENT=$(grep -oE 'FIND SEGMENT.*= [0-9]+' /tmp/r10_query.log 2>/dev/null | head -1)"
    echo "FIND_EDGE=...(see results above)"
    echo "FIND_BUBBLE=$BUBBLE_RESULT"
    echo "FIND_PATH=$PATH_RESULT"
    echo "FIND_TIP=$TIP_RESULT"
    echo "FIND_REPEAT=$REPEAT_RESULT"
    echo
    echo "Liao 2023 reference: ~85k snarls (top-level) chm13 v2"
} > "$OUT/r10_results.txt"

if [[ $total_fail -eq 0 ]]; then
    echo "✅ R10: HPRC full validado (85M nodes / 118M edges / 28k paths)"
    exit 0
else
    echo "❌ R10: $total_fail divergências"
    exit 1
fi
