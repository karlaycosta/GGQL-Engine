#!/bin/bash
# billi_test_compare.sh — compara nossa implementação SESE com Billi
# nos arquivos de teste oficiais do Billi (at-cg/billi/test_files/).
#
# Uso: ./tools/billi_test_compare.sh [billi_path] [test_dir]
# Default: /tmp/billi/billi e /tmp/billi/test_files/

set -u
BILLI="${1:-/tmp/billi/billi}"
TESTDIR="${2:-/tmp/billi/test_files}"
GGQL="$(dirname "$0")/.."
BENCH="$GGQL/build/bench_gbwt"
BVS="$GGQL/build/bubble_vs_snarl"

if [[ ! -x "$BILLI" ]]; then
    echo "Billi não encontrado em $BILLI" >&2
    exit 1
fi

printf "%-40s %-12s %-12s %-12s %s\n" "test" "billi(BB+HP)" "ggql_paten" "ggql_sese" "ggql_vgstyle"
printf "%-40s %-12s %-12s %-12s %s\n" "----" "------------" "----------" "---------" "------------"

for gfa in $(find "$TESTDIR" -name "*.gfa" | sort); do
    name="$(basename "$gfa" .gfa)"
    cat="$(basename "$(dirname "$gfa")")"
    label="$cat/$name"

    # Billi reference
    billi_out=$("$BILLI" decompose -i "$gfa" 2>/dev/null)
    bb=$(echo "$billi_out" | grep -c "^BB")
    hp=$(echo "$billi_out" | grep -c "^HP")
    billi_total=$((bb + hp))

    # Ingest com GGQL (perfil assembly = topology + snarls; sem gBWT/FM-Index)
    idx="/tmp/billi_ggql/${name}.idx"
    rm -rf "$idx"
    "$BENCH" "$gfa" 1 0 -q --profile assembly --threads 1 >/dev/null 2>&1
    real_idx="${gfa}.idx"
    if [[ ! -d "$real_idx" ]]; then
        printf "%-40s %-12s [ingest fail]\n" "$label" "${billi_total}(${bb}/${hp})"
        continue
    fi

    # Roda nossas 3 implementações via bubble_vs_snarl
    bvs_out=$("$BVS" "$real_idx" 2>&1)
    paten=$(echo "$bvs_out" | grep -oP "F1\+F2\): +\K\d+")
    sese=$(echo "$bvs_out" | grep -oP "Billi/SESE\): +\K\d+")
    vgstyle=$(echo "$bvs_out" | grep -oP "vgstyle, refactor\): +\K\d+")

    printf "%-40s %-12s %-12s %-12s %s\n" "$label" "${billi_total}(${bb}/${hp})" "${paten:-0}" "${sese:-0}" "${vgstyle:-0}"
done
