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
BENCH_PORT="$GGQL/build/bench_snarl"

if [[ ! -x "$BILLI" ]]; then
    echo "Billi não encontrado em $BILLI" >&2
    exit 1
fi

printf "%-40s %-13s %s\n" "test" "billi(BB+HP)" "port"
printf "%-40s %-13s %s\n" "----" "-------------" "-----"

pass=0
fail=0
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
    rm -rf "${gfa}.idx"
    "$BENCH" "$gfa" 1 0 -q --profile assembly --threads 1 >/dev/null 2>&1
    real_idx="${gfa}.idx"
    if [[ ! -d "$real_idx" ]]; then
        printf "%-40s %-12s [ingest fail]\n" "$label" "${billi_total}(${bb}/${hp})"
        ((fail++))
        continue
    fi

    bench_out=$("$BENCH_PORT" "$real_idx" 1 2>&1)
    port=$(echo "$bench_out" | grep -oP "snarls=\K\d+" | head -1)
    port=${port:-0}

    if [[ "$billi_total" == "$port" ]]; then
        ((pass++))
    else
        ((fail++))
    fi
    printf "%-40s %-13s %s\n" "$label" "${billi_total}(${bb}/${hp})" "$port"
done
echo ""
echo "paridade: $pass/$((pass+fail))"
