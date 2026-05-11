#!/bin/bash
# run_tier_d.sh — R9: validação em datasets NÃO-HUMANOS.
#
# Generaliza Sprints 1-9 (todos pangenoma humano) para topologias distintas:
#
#   D-1: MT_primate (mitochondrial multi-primata)
#        - 8 segs, 11 links, 0 paths
#        - SN:Z:MT_human + SN:Z:MT_orang (orangotango)
#        - Origem: lh3/gfatools/test/MT.gfa (real, peer-reviewed corpus)
#        - Topologia: linear + alt-orang bypass
#
#   D-2: ecoli_synth (E. coli K12 + variants sintéticos)
#        - 32 segs (20 backbone + 12 alts), 43 links, 4 haplotypes/walks
#        - SN:Z:ecoli_K12 + SN:Z:ecoli_alt
#        - Topologia: backbone bifurcando em alts (modelo de SNP/indel)
#
# Critério: counts de FIND SEGMENT/EDGE/TIP/REPEAT devem bater com awk GT.
# FIND BUBBLE só validado em D-2 (D-1 tem só 1 alt orang, snarls = 0-1).

set -eo pipefail
cd "$(dirname "$0")/../.."

TIER_DIR=data/validation/tier_d
GFATOOLS=data/validation/tools/gfatools/gfatools
RUNQ=build/tests/run_queries
BENCH=build/bench_gbwt
[[ -x "$RUNQ" && -x "$BENCH" && -x "$GFATOOLS" ]] || { echo "FALTA tool"; exit 1; }

total_fail=0
total_pass=0

run_validations() {
    local label=$1 GFA=$2 has_paths=$3
    local IDX=$GFA.idx

    echo
    echo "═══ Tier D-${label}: $(basename "$GFA") ═══"

    # Re-ingere (limpa idx pré-existente para reprodutibilidade)
    rm -rf "$IDX"
    "$BENCH" "$GFA" 2 --quiet >/dev/null 2>&1

    # GT via awk + gfatools
    local seg_gt=$(awk '$1=="S"' "$GFA" | wc -l)
    local edg_gt=$(awk '$1=="L"' "$GFA" | wc -l)
    local tip_gt=$(awk '$1=="L"{out[$2]++; in_[$4]++; seen[$2]=1; seen[$4]=1}
                       $1=="S"{seen[$2]=1}
                       END{c=0; for(x in seen) if(in_[x]==0||out[x]==0)c++; print c+0}' "$GFA")
    local rep_gt=$(awk '$1=="L"{out[$2]++}
                       END{c=0; for(x in out) if(out[x]>=2)c++; print c+0}' "$GFA")

    # GGQL counts via run_queries
    declare -A G
    while IFS=$'\t' read -r k v; do G[$k]=$v; done < <("$RUNQ" "$IDX" 2>/dev/null | grep -E '^FIND_')

    check() {
        local name=$1 gt=$2 ext=${3:-}
        local v=${G[$name]:-?}
        if [[ "$v" == "$gt" ]]; then
            printf '  %-20s %6s %12s ✅ %s\n' "$name" "$v" "$gt" "$ext"
            total_pass=$((total_pass+1))
        else
            printf '  %-20s %6s %12s ❌ Δ=%-3s %s\n' "$name" "$v" "$gt" "$((v-gt))" "$ext"
            total_fail=$((total_fail+1))
        fi
    }

    check FIND_SEGMENT "$seg_gt" "(awk S-lines)"
    check FIND_EDGE    "$edg_gt" "(awk L-lines)"
    check FIND_TIP     "$tip_gt" "(awk in/out=0)"
    check FIND_REPEAT  "$rep_gt" "(awk out_degree≥2)"

    if [[ "$has_paths" == "true" ]]; then
        local pat_gt=$(awk '$1=="P"||$1=="W"' "$GFA" | wc -l)
        check FIND_PATH "$pat_gt" "(awk P+W lines)"
    fi

    # Bubble: usa vg snarls com semântica >2-net-graph (excl. snarls vazios)
    local bub_gt=0
    if [[ -x data/validation/tools/vg ]]; then
        data/validation/tools/vg convert -g "$GFA" -p > /tmp/tier_d.pg 2>/dev/null || true
        if [[ -f /tmp/tier_d.pg ]]; then
            bub_gt=$(data/validation/tools/vg stats -R /tmp/tier_d.pg 2>/dev/null | tail -n +2 | awk '$17>2' | wc -l)
            check FIND_BUBBLE "$bub_gt" "(vg snarls >2-net, Δ tolerado)"
            rm -f /tmp/tier_d.pg
        fi
    fi
}

run_validations "1_MT_primate" "$TIER_DIR/MT_primate.gfa" false
run_validations "2_ecoli_synth" "$TIER_DIR/ecoli_synth.gfa" true

# ── Sub-Tier D-2.bis: crosses() em E. coli synth ──────────────────────────────
echo
echo "═══ Tier D-2.bis: crosses() em ecoli_synth (CrossesCache) ═══"
ECOLI_GFA="$TIER_DIR/ecoli_synth.gfa"
ECOLI_IDX="$ECOLI_GFA.idx"

# GT via Python: set intersection de nós entre walks
python3 << PYEOF > /tmp/tier_d_crosses_gt.tsv
import re
walks = {}
with open("$ECOLI_GFA") as f:
    for line in f:
        if not line.startswith("W\t"): continue
        parts = line.rstrip("\n").split("\t")
        sample, hap, contig, start, end, walk = parts[1:7]
        name = f"{sample}#{hap}#{contig}:{start}-{end}"
        nodes = set(int(x) for x in re.findall(r"\d+", walk))
        walks[name] = nodes
items = list(walks.items())
for name, nodes in items:
    count = sum(1 for q, qnodes in items if qnodes & nodes)
    print(f"{name}\t{count}")
PYEOF

while IFS=$'\t' read -r target gt; do
    [[ -z "$target" ]] && continue
    got=$("$RUNQ" "$ECOLI_IDX" "FIND PATH WHERE crosses(\"$target\")" 2>/dev/null | tail -1)
    if [[ "$got" == "$gt" ]]; then
        printf '  crosses(%-30s) %4s == %-4s ✅\n' "$target" "$got" "$gt"
        total_pass=$((total_pass+1))
    else
        printf '  crosses(%-30s) %4s != %-4s ❌\n' "$target" "$got" "$gt"
        total_fail=$((total_fail+1))
    fi
done < /tmp/tier_d_crosses_gt.tsv

# ── Topology check: pangenome não-humano deve mostrar comportamento distinto ──
echo
echo "═══ Análise topológica (vs humano) ═══"
for f in "$TIER_DIR/MT_primate.gfa" "$TIER_DIR/ecoli_synth.gfa" data/validation/tier_b/DRB1-3123.gfa; do
    [[ -f "$f" ]] || continue
    name=$(basename "$f" .gfa)
    s=$(awk '$1=="S"' "$f" | wc -l)
    l=$(awk '$1=="L"' "$f" | wc -l)
    p=$(awk '$1=="P"||$1=="W"' "$f" | wc -l)
    ratio=$(awk -v s="$s" -v l="$l" 'BEGIN{if (s>0) printf "%.2f", l/s; else print "?"}')
    printf '  %-30s S=%-6s L=%-6s P/W=%-4s L/S=%s\n' "$name" "$s" "$l" "$p" "$ratio"
done

echo
echo "═══ Resumo R9 ═══"
echo "  ✅ Pass: $total_pass"
echo "  ❌ Fail: $total_fail"
if [[ $total_fail -eq 0 ]]; then
    echo "✅ R9: GGQL valida em datasets NÃO-HUMANOS (MT primata + E. coli synth)"
    exit 0
else
    echo "❌ R9: $total_fail divergências"
    exit 1
fi
