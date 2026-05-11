#!/bin/bash
# run_tier_b_alt.sh — Sprint 9 (O-4 parcial): Tier B alternativo
#                     usando micb-kir3dl1.gfa (HLA MICB/KIR3DL1 region).
#
# Complementa Sprint 2 (DRB1-3123) com um dataset de TOPOLOGIA distinta:
#   - 169 walks PanSN-spec (vs DRB1 com 12 walks)
#   - Tamanho intermediário (2889 segs vs DRB1 5581 segs)
#   - Multi-haplotype real de pangenoma HLA (KIR locus = altamente diverso)
#
# Tier B "yeast/C. elegans" original (O-4) requer download externo de
# datasets maiores (Sirén 2021 / pggb examples) — DOCUMENTADO como
# trabalho futuro. micb-kir3dl1 funciona como Tier B alternativo já
# disponível no vg test corpus.

set -eo pipefail
cd "$(dirname "$0")/../.."

SRC=data/validation/tools/vg-src/test/haplotype-sampling/micb-kir3dl1.gfa
TIER_DIR=data/validation/tier_b_alt
mkdir -p "$TIER_DIR"
GFA=$TIER_DIR/micb-kir3dl1.gfa
[[ -f "$GFA" ]] || cp "$SRC" "$GFA"

IDX=$GFA.idx
GFATOOLS=data/validation/tools/gfatools/gfatools
VG=data/validation/tools/vg
RUNQ=build/tests/run_queries

[[ -x "$RUNQ" && -x "$GFATOOLS" && -x "$VG" ]] || { echo "FALTA tool"; exit 1; }

# Ingere
[[ -d "$IDX" ]] || build/bench_gbwt "$GFA" 4 --quiet >/dev/null 2>&1

# Ground truth via awk + vg
seg_gt=$(awk '$1=="S"' "$GFA" | wc -l)
edg_gt=$(awk '$1=="L"' "$GFA" | wc -l)
pat_gt=$(awk '$1=="P" || $1=="W"' "$GFA" | wc -l)
tip_gt=$(awk '$1=="L"{out[$2]++; in_[$4]++; n[$2]=1; n[$4]=1}
              $1=="S"{n[$2]=1}
              END{c=0; for(x in n) if(in_[x]==0||out[x]==0)c++; print c}' "$GFA")
rep_gt=$(awk '$1=="L"{out[$2]++}
              END{c=0; for(x in out) if(out[x]>=2)c++; print c}' "$GFA")
"$VG" convert -g "$GFA" -p > /tmp/micb.pg 2>/dev/null
bub_total=$("$VG" stats -R /tmp/micb.pg 2>/dev/null | tail -n +2 | wc -l)
bub_inner=$("$VG" stats -R /tmp/micb.pg 2>/dev/null | tail -n +2 | awk '$17>2' | wc -l)
rm -f /tmp/micb.pg

# GGQL
declare -A G
while IFS=$'\t' read -r k v; do G[$k]=$v; done < <("$RUNQ" "$IDX" 2>/dev/null | grep -E '^FIND_')

fail=0
check() {
    local name=$1 gt=$2 ext=${3:-}
    local v=${G[$name]}
    if [[ "$v" == "$gt" ]]; then
        printf '%-20s %8s %12s ✅ %s\n' "$name" "$v" "$gt" "$ext"
    else
        d=$((v - gt))
        if [[ "$ext" == *"Δ≤"* ]] && [[ ${d#-} -le 5 ]]; then
            printf '%-20s %8s %12s ≈ Δ=%-3s %s\n' "$name" "$v" "$gt" "$d" "$ext"
        else
            printf '%-20s %8s %12s ❌ Δ=%-3s %s\n' "$name" "$v" "$gt" "$d" "$ext"
            fail=$((fail+1))
        fi
    fi
}

echo "═══ Tier B-alt: micb-kir3dl1 (HLA MICB/KIR3DL1, 169 walks) ═══"
check FIND_SEGMENT "$seg_gt" "(gfatools)"
check FIND_EDGE    "$edg_gt" "(gfatools)"
check FIND_TIP     "$tip_gt" "(awk)"
check FIND_REPEAT  "$rep_gt" "(awk)"
check FIND_BUBBLE  "$bub_inner" "(vg Net-Graph>2, Δ≤5 ok)"
check FIND_PATH    "$pat_gt" "(W-lines)"

echo
echo "Notas:"
echo "  - vg snarls total = $bub_total | com conteúdo interno = $bub_inner"
echo "  - Pangenoma HLA MICB/KIR3DL1 (altamente diverso): mais snarls por kb que DRB1"

# Sprint 5-style crosses validation (com GT externo via Python)
echo
echo "═══ Sub-Sprint 9.bis: crosses() em Tier B-alt ═══"
python3 - <<PYEOF > /tmp/sprint9_gt.tsv
import re
walks = {}
with open('$GFA') as f:
    for line in f:
        if not line.startswith('W\t'): continue
        parts = line.rstrip('\n').split('\t')
        if len(parts) < 7: continue
        sample, hap, contig, start, end, walk = parts[1:7]
        name = f"{sample}#{hap}#{contig}:{start}-{end}"
        nodes = set(int(x) for x in re.findall(r'\d+', walk))
        walks[name] = nodes
items = list(walks.items())
for target_name, target_nodes in items:
    count = sum(1 for n, nodes in items if nodes & target_nodes)
    print(f"{target_name}\t{count}")
PYEOF

# Pega 3 targets para validar (primeiro, meio, último)
n=$(wc -l < /tmp/sprint9_gt.tsv)
mid=$((n / 2))
selected=$(awk -v mid=$mid -v n=$n 'NR==1 || NR==mid || NR==n' /tmp/sprint9_gt.tsv)
while IFS=$'\t' read -r target gt; do
    [[ -z "$target" ]] && continue
    ggql=$("$RUNQ" "$IDX" "FIND PATH WHERE crosses(\"$target\")" 2>/dev/null | tail -1)
    short=$(echo "$target" | cut -c1-40)
    if [[ "$ggql" == "$gt" ]]; then
        printf '  %-50s %8s == %-5s ✅\n' "$short" "$ggql" "$gt"
    else
        printf '  %-50s %8s != %-5s ❌\n' "$short" "$ggql" "$gt"
        fail=$((fail+1))
    fi
done <<< "$selected"

echo
if [[ $fail -eq 0 ]]; then
    echo "✅ Tier B-alt: todos os 9 casos passaram"
    exit 0
else
    echo "❌ Tier B-alt: $fail divergências"
    exit 1
fi
