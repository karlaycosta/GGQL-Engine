#!/bin/bash
# T5_memory_r2.sh — Memory peak (M) das queries do R2 via /usr/bin/time -v.
#
# Complementa r2_perf_rigorous.sh (que mede tempo via hyperfine) capturando
# RSS pico de cada query. hyperfine não expõe RSS — precisamos do /usr/bin/time.
#
# Estratégia: para cada query, rodar 3 vezes (descartar 1 warmup + mediana de 2)
# e reportar Maximum resident set size.
#
# Output: tests/validation/T5_memory_results/
#   - hardware.txt    (mesmo do R2 — para reprodução)
#   - per_query.tsv   (query \t run \t rss_kb)
#   - summary.tsv     (query \t rss_min \t rss_median \t rss_max)
#
# Custo: 13 queries × 3 runs × ~1.5s = ~60s + overhead /usr/bin/time = ~2 min.

set -eo pipefail
cd "$(dirname "$0")/../.."

GFA=data/chm13_subset_100w.gfa
IDX=$GFA.idx
RUNQ=build/tests/run_queries

[[ -x "$RUNQ" && -d "$IDX" ]] || { echo "FALTA tool/idx"; exit 1; }

export GGQL_MAX_TOTAL_HITS=10000000

OUT=tests/validation/T5_memory_results
mkdir -p "$OUT"

# ── Captura HW context ─────────────────────────────────────────────────────────
{
    echo "=== Hardware/SW context — T5 (memory) ==="
    lscpu | grep -E "Model name|CPU\(s\):" | head -3
    free -h | head -3
    uname -a
    g++-14 --version | head -1
    /usr/bin/time --version 2>&1 | head -1
    echo "Date: $(date -Iseconds)"
    echo "Commit: $(git rev-parse --short HEAD)"
} > "$OUT/hardware.txt"
cat "$OUT/hardware.txt"
echo

# ── Lista de queries (espelha r2_perf_rigorous.sh) ──────────────────────────
declare -a QUERIES=(
    "FIND_SEGMENT|FIND SEGMENT"
    "FIND_EDGE|FIND EDGE"
    "FIND_TIP|FIND TIP"
    "FIND_REPEAT|FIND REPEAT"
    "FIND_PATH|FIND PATH"
    "FIND_BUBBLE|FIND BUBBLE"
    "KMER_AAAAAAAA|SEARCH KMER \"AAAAAAAA\""
    "KMER_CGATCGATCG|SEARCH KMER \"CGATCGATCG\""
    "WHERE_length_gt_1000|FIND SEGMENT WHERE length > 1000"
    "WHERE_name_eq_1|FIND SEGMENT WHERE name = \"1\""
    "WHERE_length_geq_32|FIND SEGMENT WHERE length >= 32 LIMIT 100000000"
    "WHERE_length_AND|FIND SEGMENT WHERE length > 30 AND length < 32"
    "WHERE_length_OR|FIND SEGMENT WHERE length = 1 OR length > 30"
)

PER_QUERY="$OUT/per_query.tsv"
SUMMARY="$OUT/summary.tsv"
echo -e "label\trun\trss_kb\twall_s" > "$PER_QUERY"
echo -e "label\trss_min_mb\trss_median_mb\trss_max_mb\twall_median_s" > "$SUMMARY"

echo "── Rodando T5: 13 queries × 3 runs com /usr/bin/time -v ──"
echo

for entry in "${QUERIES[@]}"; do
    label="${entry%%|*}"
    query="${entry#*|}"

    # 3 runs (1 warmup + 2 reais; reporta mediana de todos 3)
    declare -a rss_vals
    declare -a wall_vals
    rss_vals=()
    wall_vals=()
    for run in 1 2 3; do
        # /usr/bin/time -v escreve em stderr; capturamos em arquivo temp
        tmp=$(mktemp)
        /usr/bin/time -v "$RUNQ" "$IDX" "$query" 2> "$tmp" 1> /dev/null
        rss=$(grep "Maximum resident" "$tmp" | awk -F': ' '{print $2}')
        wall=$(grep "Elapsed" "$tmp" | awk -F': ' '{print $2}' | awk -F: 'NF==2{print $1*60+$2} NF==3{print $1*3600+$2*60+$3}')
        rm -f "$tmp"
        echo -e "${label}\t${run}\t${rss}\t${wall}" >> "$PER_QUERY"
        rss_vals+=("$rss")
        wall_vals+=("$wall")
    done

    # Estatísticas via awk
    stats=$(printf '%s\n' "${rss_vals[@]}" | sort -n | awk '
        BEGIN { c=0 }
        { v[c++]=$1; total += $1 }
        END {
            min = v[0]
            max = v[c-1]
            # Mediana para 3 valores = v[1]
            median = v[int(c/2)]
            printf "%d\t%d\t%d", min, median, max
        }
    ')
    wall_median=$(printf '%s\n' "${wall_vals[@]}" | sort -n | awk 'NR==2{printf "%.2f", $1}')

    # Reformata KB → MB
    rss_min_mb=$(awk -v k="$(echo $stats | awk '{print $1}')" 'BEGIN{printf "%.0f", k/1024}')
    rss_med_mb=$(awk -v k="$(echo $stats | awk '{print $2}')" 'BEGIN{printf "%.0f", k/1024}')
    rss_max_mb=$(awk -v k="$(echo $stats | awk '{print $3}')" 'BEGIN{printf "%.0f", k/1024}')

    echo -e "${label}\t${rss_min_mb}\t${rss_med_mb}\t${rss_max_mb}\t${wall_median}" >> "$SUMMARY"
    printf '  %-25s  med RSS=%s MB  wall=%s s  (range %s-%s MB)\n' \
        "$label" "$rss_med_mb" "$wall_median" "$rss_min_mb" "$rss_max_mb"
done

echo
echo "═══ T5 — Summary ═══"
column -t -s $'\t' "$SUMMARY"
echo
echo "✅ T5: memory peak medido em 13 queries"
echo "Resultados em $OUT/"
