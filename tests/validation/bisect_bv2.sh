#!/bin/bash
# bisect_bv2.sh — Bisect do B-V2: gera GFAs em diferentes escalas/comp counts
# e verifica simetria após ingestão completa via bench_gbwt.

set -eo pipefail
cd "$(dirname "$0")/../.."

OUT_DIR=data/validation/bisect
mkdir -p "$OUT_DIR"

# Gera GFA com N_COMPS componentes desconexos via awk (10x+ mais rápido
# que bash loop para N grande).
make_gfa() {
    local n_comps=$1 comp_size=$2 outfile=$3
    awk -v nc="$n_comps" -v cs="$comp_size" 'BEGIN {
        print "H\tVN:Z:1.0"
        seqs[0] = "ACGT"; seqs[1] = "CGTA"; seqs[2] = "GTAC"; seqs[3] = "TACG"
        # S-lines
        for (c = 0; c < nc; ++c) {
            for (i = 0; i < cs; ++i) {
                id = c * cs + i + 1
                print "S\t" id "\t" seqs[id % 4]
            }
        }
        # L-lines (path linear por componente)
        for (c = 0; c < nc; ++c) {
            for (i = 0; i < cs - 1; ++i) {
                from = c * cs + i + 1
                to   = from + 1
                print "L\t" from "\t+\t" to "\t+\t0M"
            }
        }
    }' > "$outfile"
}

# Cenários (cresce em escala + componentes)
declare -a SCENARIOS=(
    "10    100     1k_10comp"
    "100   100     10k_100comp"
    "1000  100     100k_1000comp"
    "100   1000    100k_100comp"
    "2500  400     1M_2500comp"
)

printf '\n%-22s %-10s %-10s %-10s %-10s %s\n' \
    "DATASET" "NODES" "EDGES" "COMP" "ASYM%" "STATUS"
printf '%-22s %-10s %-10s %-10s %-10s %s\n' \
    "$(printf '=%.0s' {1..22})" "----" "----" "----" "----" "----"

for s in "${SCENARIOS[@]}"; do
    read -r n_comps comp_size name <<< "$s"
    gfa=$OUT_DIR/$name.gfa
    [[ -f $gfa ]] || make_gfa "$n_comps" "$comp_size" "$gfa"

    n_nodes=$((n_comps * comp_size))
    n_edges=$((n_comps * (comp_size - 1)))

    # Ingere
    rm -rf "$gfa.idx"
    build/bench_gbwt "$gfa" 8 --quiet >/dev/null 2>&1

    # Check simetria
    out=$(build/tests/check_csr_symmetry "$gfa.idx" 2>&1)
    asym=$(echo "$out" | grep "asymmetric:" | head -1 | awk -F'(' '{print $2}' | tr -d '%)' )
    if [[ -z "$asym" ]]; then asym="?"; fi
    pct_num=$(echo "$asym" | awk '{print int($1+0.5)}')

    if [[ "$pct_num" == "0" ]] || [[ "$asym" == "0.0000" ]]; then
        status="✅ simetria OK"
    else
        status="❌ ASYM"
    fi

    printf '%-22s %-10s %-10s %-10s %-10s %s\n' \
        "$name" "$n_nodes" "$n_edges" "$n_comps" "$asym" "$status"
done

echo
echo "Pivot: comparar manifest reportado vs simetria"
echo "Conclusão esperada: identifica threshold (nodes OU components) onde bug aparece"
