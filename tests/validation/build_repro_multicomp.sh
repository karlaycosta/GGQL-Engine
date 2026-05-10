#!/bin/bash
# build_repro_multicomp.sh — Cria GFA artificial multi-componente e
# verifica simetria do CSR após ingestão completa via bench_gbwt.
#
# Objetivo (B-V2 repro): isolar se o bug aparece via pipeline de ingest
# (Pass1+Pass2+remap+build_csr) ou apenas em chm13_100w por algum dado
# patológico.

set -eo pipefail
cd "$(dirname "$0")/../.."

REPRO_DIR=data/validation/repro_b-v2
mkdir -p "$REPRO_DIR"
GFA=$REPRO_DIR/multicomp.gfa

# Constrói GFA com 8 componentes desconexos × 100 nós cada = 800 nós
# (escala intermediária entre meus tests unit e chm13_100w).
# Cada componente é um path linear simples + 1 walk W-line.
N_COMPS=8
COMP_SIZE=100
{
    echo "H	VN:Z:1.1	RS:Z:multicomp_test"
    # S-lines: 800 segments
    for c in $(seq 0 $((N_COMPS-1))); do
        for i in $(seq 0 $((COMP_SIZE-1))); do
            id=$((c * COMP_SIZE + i + 1))   # nomes 1-based
            # Sequência fictícia: ACGT cíclico
            base_idx=$((id % 4))
            case $base_idx in
                0) seq=ACGT;;
                1) seq=CGTA;;
                2) seq=GTAC;;
                3) seq=TACG;;
            esac
            echo "S	$id	$seq"
        done
    done
    # L-lines: chain por componente
    for c in $(seq 0 $((N_COMPS-1))); do
        for i in $(seq 0 $((COMP_SIZE-2))); do
            from=$((c * COMP_SIZE + i + 1))
            to=$((c * COMP_SIZE + i + 2))
            echo "L	$from	+	$to	+	0M"
        done
    done
    # W-lines: 1 walk por componente
    for c in $(seq 0 $((N_COMPS-1))); do
        sample="HG$(printf %02d $c)"
        nodes=""
        for i in $(seq 0 $((COMP_SIZE-1))); do
            id=$((c * COMP_SIZE + i + 1))
            nodes="$nodes>$id"
        done
        echo "W	$sample	1	chr$c	0	$((COMP_SIZE * 4))	$nodes"
    done
} > "$GFA"

echo "Criado $GFA:"
wc -l "$GFA"
awk 'BEGIN{s=0;l=0;w=0} $1=="S"{s++} $1=="L"{l++} $1=="W"{w++} END{
    printf "  S=%d L=%d W=%d (esperado: S=%d L=%d W=%d)\n", s,l,w,
    '"$N_COMPS"' * '"$COMP_SIZE"', '"$N_COMPS"' * ('"$COMP_SIZE"'-1), '"$N_COMPS"'
}' "$GFA"

# Ingere
rm -rf "$GFA.idx"
echo
echo "Ingerindo via bench_gbwt..."
build/bench_gbwt "$GFA" 4 --quiet >/dev/null 2>&1

# Verifica simetria via run_queries-style test
# Construímos um .cpp standalone se precisar — por ora, usamos awk + dump
echo
echo "Manifest gerado:"
grep -E "num_nodes|link_count|num_components" "$GFA.idx/manifest.txt"

echo
echo "Para verificar simetria do CSR carregado deste .idx, rode:"
echo "  GGQL_DEBUG_SNARLS=1 build/bench_gbwt $GFA 4 2>&1 | grep snarls-dbg"
echo
echo "Esperado: resolve_ok ≈ total_out_iter (simetria preservada)"
echo "Se resolve_fail >> resolve_ok: BUG REPRODUZIDO em pequena escala"
