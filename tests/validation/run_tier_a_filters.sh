#!/bin/bash
# run_tier_a_filters.sh — Sprint 1.bis: filtros WHERE com GT externo.
#
# Cobre:
#   - FIND SEGMENT WHERE length op N
#   - FIND SEGMENT WHERE name = "..."
#   - FIND SEGMENT WHERE contains_kmer("...")
#   - SEARCH KMER "..."
#
# Usa apenas tiny.gfa (deterministic toy) — GT computado via awk/grep.
# Re-uso de fixtures e binários do Sprint 1 (run_tier_a.sh).

set -eo pipefail   # sem -u: helpers leem $1/$2 que podem estar vazios
cd "$(dirname "$0")/../.."

GFA=data/validation/tier_a/tiny.gfa
IDX=data/validation/tier_a/tiny.gfa.idx
RUNQ=build/tests/run_queries

[[ -x "$RUNQ" ]]   || { echo "FALTA: $RUNQ — build run_queries"; exit 1; }
[[ -f "$GFA" ]]    || { echo "FALTA: $GFA — rodar Sprint 1 primeiro"; exit 1; }
[[ -d "$IDX" ]]    || build/bench_gbwt "$GFA" 1 --quiet >/dev/null

# ── Helpers — GT computado via awk sobre o GFA bruto ─────────────────────────

count_seg_length() {
    # $1 = operador (= != < > <= >=), $2 = valor
    awk -v op="$1" -v v="$2" '
    $1=="S" {
        l = length($3); pass = 0
        if (op=="=")  pass = (l == v)
        else if (op=="!=") pass = (l != v)
        else if (op=="<")  pass = (l <  v)
        else if (op==">")  pass = (l >  v)
        else if (op=="<=") pass = (l <= v)
        else if (op==">=") pass = (l >= v)
        if (pass) c++
    } END { print c+0 }' "$GFA"
}
count_seg_name() {
    # $1 = nome
    awk -v name="$1" '$1=="S" && $2==name { c++ } END { print c+0 }' "$GFA"
}
count_seg_kmer() {
    # $1 = kmer — conta segments que contêm o kmer (≥1 occurrence na seq)
    awk -v k="$1" '$1=="S" && index($3, k) > 0 { c++ } END { print c+0 }' "$GFA"
}
count_kmer_segments() {
    # $1 = kmer — conta segments DISTINTOS que contêm o kmer.
    # Modelo do GGQL: query_planner.cpp:606 deduplica por segment_id após
    # search_kmer. Diferente de "occurrences totais" que tools como vg find -k
    # reportam.
    awk -v k="$1" '$1=="S" && index($3, k) > 0 { c++ } END { print c+0 }' "$GFA"
}

# ── Casos de teste ───────────────────────────────────────────────────────────

declare -a CASES=(
    # query | gt_func | gt_args
    'FIND SEGMENT WHERE length = 1   | count_seg_length | =  1'
    'FIND SEGMENT WHERE length > 1   | count_seg_length | >  1'
    'FIND SEGMENT WHERE length >= 4  | count_seg_length | >= 4'
    'FIND SEGMENT WHERE length > 10  | count_seg_length | >  10'
    'FIND SEGMENT WHERE length = 19  | count_seg_length | =  19'
    'FIND SEGMENT WHERE length = 100 | count_seg_length | =  100'
    'FIND SEGMENT WHERE name = "1"   | count_seg_name   | 1'
    'FIND SEGMENT WHERE name = "9"   | count_seg_name   | 9'
    'FIND SEGMENT WHERE name = "999" | count_seg_name   | 999'
    'FIND SEGMENT WHERE contains_kmer("AAA")  | count_seg_kmer | AAA'
    'FIND SEGMENT WHERE contains_kmer("ATAT") | count_seg_kmer | ATAT'
    'FIND SEGMENT WHERE contains_kmer("CAA")  | count_seg_kmer | CAA'
    'FIND SEGMENT WHERE contains_kmer("ZZZ")  | count_seg_kmer | ZZZ'
    'SEARCH KMER "A"    | count_kmer_segments | A'
    'SEARCH KMER "AAA"  | count_kmer_segments | AAA'
    'SEARCH KMER "ATAT" | count_kmer_segments | ATAT'
    'SEARCH KMER "ZZZZ" | count_kmer_segments | ZZZZ'
)

printf '\n%-50s %6s %6s %s\n' "QUERY" "GGQL" "GT" "STATUS"
printf '%-50s %6s %6s %s\n' "$(printf '=%.0s' {1..50})" "----" "--" "------"

fail=0
total=0
for c in "${CASES[@]}"; do
    IFS='|' read -r query func args <<< "$c"
    query=$(echo "$query" | sed 's/^ *//;s/ *$//')
    func=$(echo "$func"   | sed 's/^ *//;s/ *$//')
    args=$(echo "$args"   | sed 's/^ *//;s/ *$//')

    ggql=$("$RUNQ" "$IDX" "$query" 2>/dev/null | tail -1)
    # shellcheck disable=SC2086  # split args intencional
    gt=$($func $args)

    if [[ "$ggql" == "$gt" ]]; then
        status="✅"
    else
        status="❌ Δ=$((ggql-gt))"
        fail=$((fail+1))
    fi
    printf '%-50s %6s %6s %s\n' "$query" "$ggql" "$gt" "$status"
    total=$((total+1))
done

echo
if [[ $fail -eq 0 ]]; then
    echo "✅ $total/$total comparações Tier 1.bis passaram"
    exit 0
else
    echo "❌ $fail/$total divergências"
    exit 1
fi
