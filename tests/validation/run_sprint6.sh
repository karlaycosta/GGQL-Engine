#!/bin/bash
# run_sprint6.sh — Sprint 6: L-V6 (SEARCH KMER em Tier B e Tier C com GT externo).
#
# Semântica GGQL (clarificada em Sprint 1.bis):
#   SEARCH KMER "X" retorna o número de SEGMENTS DISTINTOS que contêm X
#   (deduplicado por segment_id via std::unique em query_planner.cpp:606).
#   NÃO retorna occurrences totais (vg find -k faz isso, GGQL não).
#
# GT: awk index sobre S-lines — count_seg_kmer (linha S contém k-mer).
# Equivale ao mesmo que `contains_kmer("X")` no GGQL.

set -eo pipefail
cd "$(dirname "$0")/../.."

RUNQ=build/tests/run_queries
[[ -x "$RUNQ" ]] || { echo "FALTA: $RUNQ"; exit 1; }

pass=0; fail=0
check() {
    local label=$1 ggql=$2 gt=$3 ext=${4:-}
    if [[ "$ggql" == "$gt" ]]; then
        printf '  %-55s %10s == %-10s ✅ %s\n' "$label" "$ggql" "$gt" "$ext"
        pass=$((pass+1))
    else
        printf '  %-55s %10s != %-10s ❌ Δ=%-6s %s\n' "$label" "$ggql" "$gt" "$((ggql-gt))" "$ext"
        fail=$((fail+1))
    fi
}

# ════════════════════════════════════════════════════════════════════════════
# Tier B: SEARCH KMER em DRB1-3123 (5581 segs, 21.109 bp total)
# ════════════════════════════════════════════════════════════════════════════
echo "═══ Tier B: SEARCH KMER em DRB1-3123 ═══"

GFA_B=data/validation/tier_b/DRB1-3123.gfa
IDX_B=$GFA_B.idx

# K-mers com tamanhos e frequências variadas
for kmer in 'ACGT' 'AAAA' 'TTTT' 'GCGC' 'AAAAAA' 'GCGCGCGC' 'NNNNNN' 'ZZZZ'; do
    ggql=$("$RUNQ" "$IDX_B" "SEARCH KMER \"$kmer\"" 2>/dev/null | tail -1)
    gt=$(awk -v k="$kmer" '$1=="S" && index($3,k)>0 {c++} END {print c+0}' "$GFA_B")
    check "SEARCH KMER \"$kmer\"" "$ggql" "$gt" "(awk index)"
done

# ════════════════════════════════════════════════════════════════════════════
# Tier C: SEARCH KMER em chm13_100w (4,3M segs, 231M bp)
# ════════════════════════════════════════════════════════════════════════════
echo
echo "═══ Tier C: SEARCH KMER em chm13_subset_100w ═══"

GFA_C=data/chm13_subset_100w.gfa
IDX_C=$GFA_C.idx

if [[ ! -d "$IDX_C" ]]; then
    echo "  SKIP — sem chm13_subset_100w.gfa.idx"
    echo
    total=$((pass + fail))
    [[ $fail -eq 0 ]] && exit 0 || exit 1
fi

# K-mers em HPRC. Cap 10M no env para count global.
export GGQL_MAX_TOTAL_HITS=10000000

# K-mers mais curtos têm mais segments — testar variedade
for kmer in 'ACGTACGT' 'AAAAAAAA' 'GCGCGCGC' 'TTTTTTTTTT' 'CGATCGATCG' 'NNNNNNNN' 'ACGTACGTACGTACGT' 'ZZZZZZZZ'; do
    # GT via awk (pode demorar 5-15s em 4.3M segs)
    gt=$(awk -v k="$kmer" '$1=="S" && index($3,k)>0 {c++} END {print c+0}' "$GFA_C")
    ggql=$("$RUNQ" "$IDX_C" "SEARCH KMER \"$kmer\"" 2>/dev/null | tail -1)
    check "SEARCH KMER \"$kmer\"" "$ggql" "$gt" "(awk index)"
done

# ════════════════════════════════════════════════════════════════════════════
echo
total=$((pass + fail))
if [[ $fail -eq 0 ]]; then
    echo "✅ Sprint 6: $pass/$total casos passaram (L-V6 validado Tier B + C)"
    exit 0
else
    echo "❌ Sprint 6: $fail/$total divergências"
    exit 1
fi
