#!/bin/bash
# run_sprint8_perf.sh — Sprint 8 (O-1): performance comparativa GGQL vs vg/awk.
#
# Mede tempo wall-clock + RSS pico para queries equivalentes em GGQL e
# ferramentas canônicas. Foco em chm13_subset_100w (4,3M nós) — escala
# representativa de pangenoma humano.
#
# Métricas para paper:
#   - Ingestão completa
#   - FIND BUBBLE / vg snarls
#   - FIND SEGMENT / EDGE / TIP / REPEAT / PATH (counts)
#   - SEARCH KMER
#
# IMPORTANTE: warm-cache em GGQL é DESLIGADO (rm -rf .idx) para medir
# ingest completo. Comparações de queries usam .idx pré-existente.

set -eo pipefail
cd "$(dirname "$0")/../.."

GFA=data/chm13_subset_100w.gfa
IDX=$GFA.idx
RUNQ=build/tests/run_queries
GFATOOLS=data/validation/tools/gfatools/gfatools
VG=data/validation/tools/vg

[[ -x "$RUNQ" && -x "$GFATOOLS" && -x "$VG" ]] || { echo "FALTA ferramenta"; exit 1; }
[[ -f "$GFA" ]] || { echo "FALTA: $GFA"; exit 1; }

export GGQL_MAX_TOTAL_HITS=10000000

# Helper: mede wall-clock em segundos via /usr/bin/time
measure() {
    local label=$1
    shift
    local out
    out=$(/usr/bin/time -f "%e %M" bash -c "$*" 2>&1 1>/dev/null)
    local sec=$(echo "$out" | tail -1 | awk '{print $1}')
    local rss_kb=$(echo "$out" | tail -1 | awk '{print $2}')
    local rss_mb=$(awk -v k="$rss_kb" 'BEGIN{printf "%.0f", k/1024}')
    printf '%-50s %8s s  %8s MB\n' "$label" "$sec" "$rss_mb"
}

printf '\n%-50s %10s  %11s\n' "Operação" "Tempo (s)" "RSS pico (MB)"
printf '%-50s %10s  %11s\n' "$(printf '=%.0s' {1..50})" "----------" "-------------"

# ════════════════════════════════════════════════════════════════════════════
# Bloco 1 — Ingestão completa (cold cache)
# ════════════════════════════════════════════════════════════════════════════
echo
echo "── Bloco 1: Ingestão completa (cold) ──"
rm -rf "$IDX" /tmp/chm13_100w.pg

# GGQL: bench_gbwt completo (Pass1+Pass2+gBWT+FM-Index+snarls)
measure "GGQL bench_gbwt (cold)"  \
    "build/bench_gbwt $GFA 8 --quiet"

# vg: converter GFA → packed graph (sem gBWT/snarls)
measure "vg convert -g (GFA → .pg)"  \
    "$VG convert -g $GFA -p > /tmp/chm13_100w.pg"

# gfatools: parse + stats
measure "gfatools stat"  \
    "$GFATOOLS stat $GFA"

# ════════════════════════════════════════════════════════════════════════════
# Bloco 2 — Queries estruturais (warm cache)
# ════════════════════════════════════════════════════════════════════════════
echo
echo "── Bloco 2: Queries estruturais (warm cache) ──"

measure "GGQL FIND SEGMENT (count global)"  \
    "$RUNQ $IDX 'FIND SEGMENT'"

measure "GGQL FIND EDGE (count global)"  \
    "$RUNQ $IDX 'FIND EDGE'"

measure "GGQL FIND TIP"  \
    "$RUNQ $IDX 'FIND TIP'"

measure "GGQL FIND REPEAT"  \
    "$RUNQ $IDX 'FIND REPEAT'"

measure "GGQL FIND PATH"  \
    "$RUNQ $IDX 'FIND PATH'"

measure "awk count segments (GT baseline)"  \
    "awk '\$1==\"S\" {c++} END {print c}' $GFA"

# ════════════════════════════════════════════════════════════════════════════
# Bloco 3 — BUBBLE / snarls
# ════════════════════════════════════════════════════════════════════════════
echo
echo "── Bloco 3: FIND BUBBLE / vg snarls ──"

measure "GGQL FIND BUBBLE (snarls.bin cached)"  \
    "$RUNQ $IDX 'FIND BUBBLE'"

# vg snarls precisa do .pg gerado no Bloco 1
measure "vg snarls (chm13_100w.pg)"  \
    "$VG stats -R /tmp/chm13_100w.pg > /dev/null"

# ════════════════════════════════════════════════════════════════════════════
# Bloco 4 — SEARCH KMER
# ════════════════════════════════════════════════════════════════════════════
echo
echo "── Bloco 4: SEARCH KMER ──"

measure "GGQL SEARCH KMER 'AAAAAAAA' (~68k hits)"  \
    "$RUNQ $IDX 'SEARCH KMER \"AAAAAAAA\"'"

measure "GGQL SEARCH KMER 'CGATCGATCG' (2 hits)"  \
    "$RUNQ $IDX 'SEARCH KMER \"CGATCGATCG\"'"

measure "awk SEARCH KMER 'AAAAAAAA' (linear scan)"  \
    "awk -v k='AAAAAAAA' '\$1==\"S\" && index(\$3,k)>0 {c++} END {print c}' $GFA"

measure "awk SEARCH KMER 'CGATCGATCG'"  \
    "awk -v k='CGATCGATCG' '\$1==\"S\" && index(\$3,k)>0 {c++} END {print c}' $GFA"

# ════════════════════════════════════════════════════════════════════════════
# Bloco 5 — WHERE filters
# ════════════════════════════════════════════════════════════════════════════
echo
echo "── Bloco 5: WHERE filters (com índices secundários) ──"

measure "GGQL WHERE length > 1000 (length_index)"  \
    "$RUNQ $IDX 'FIND SEGMENT WHERE length > 1000'"

measure "GGQL WHERE name = '1' (name_index lookup)"  \
    "$RUNQ $IDX 'FIND SEGMENT WHERE name = \"1\"'"

measure "awk WHERE length > 1000 (linear scan)"  \
    "awk '\$1==\"S\" && length(\$3)>1000 {c++} END {print c}' $GFA"

echo
echo "✅ Sprint 8 (O-1): medições concluídas. Tabela acima para paper/relatório."
