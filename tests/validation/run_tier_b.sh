#!/bin/bash
# run_tier_b.sh — Sprint 2 Tier B: DRB1-3123 (HLA-DRB1 12 alelos).
#
# Pré-requisitos:
#   - vg msga já rodou e gerou data/validation/tier_b/DRB1-3123.gfa
#   - Reproduzir a partir do FASTA:
#       FASTA=data/validation/tools/vg-src/test/GRCh38_alts/FASTA/HLA/DRB1-3123.fa
#       data/validation/tools/vg msga -f $FASTA > /tmp/drb1.vg
#       data/validation/tools/vg view /tmp/drb1.vg > data/validation/tier_b/DRB1-3123.gfa
#
# Critério de paridade BUBBLE: exclui snarls vazios (Net-Graph-Size=2,
# Shallow-Nodes=0) que o GGQL/Billi não considera. Δ residual de até 2
# snarls é aceito (diferença algorítmica entre Billi/SESE e integrated do vg).

set -eo pipefail
cd "$(dirname "$0")/../.."

GFA=data/validation/tier_b/DRB1-3123.gfa
IDX=$GFA.idx
GFATOOLS=data/validation/tools/gfatools/gfatools
VG=data/validation/tools/vg
RUNQ=build/tests/run_queries

[[ -x "$GFATOOLS" && -x "$VG" && -x "$RUNQ" ]] || { echo "FALTA tool ou build"; exit 1; }
[[ -f "$GFA" ]] || { echo "FALTA: $GFA — rodar vg msga primeiro"; exit 1; }
[[ -d "$IDX" ]] || build/bench_gbwt "$GFA" 4 --quiet >/dev/null

# Ground truth
seg_gt=$(awk '$1=="S"' "$GFA" | wc -l)
edg_gt=$(awk '$1=="L"' "$GFA" | wc -l)
pat_gt=$(awk '$1=="P"||$1=="W"' "$GFA" | wc -l)
tip_gt=$(awk '$1=="L"{out[$2]++; in_[$4]++; n[$2]=1; n[$4]=1}
              $1=="S"{n[$2]=1}
              END{c=0; for(x in n) if(in_[x]==0||out[x]==0)c++; print c}' "$GFA")
rep_gt=$(awk '$1=="L"{out[$2]++}
              END{c=0; for(x in out) if(out[x]>=2)c++; print c}' "$GFA")
"$VG" convert -g "$GFA" -p > /tmp/drb1.pg 2>/dev/null
# Snarls vg total
bub_total=$("$VG" stats -R /tmp/drb1.pg 2>/dev/null | tail -n +2 | wc -l)
# Snarls com conteúdo interno (Net-Graph-Size > 2) — alinhado com semântica GGQL
bub_inner=$("$VG" stats -R /tmp/drb1.pg 2>/dev/null | tail -n +2 | awk '$17>2' | wc -l)
rm -f /tmp/drb1.pg

# GGQL
declare -A ggql
while IFS=$'\t' read -r q c; do ggql[$q]=$c; done < <("$RUNQ" "$IDX" 2>/dev/null | grep -E '^FIND_')

# Tabela
printf '\n%-15s %8s %12s %s\n' "QUERY" "GGQL" "GT" "STATUS"
printf '%-15s %8s %12s %s\n' "-----" "----" "--" "------"
fail=0
check() {
    local name=$1 gt=$2 ext=${3:-}
    local v=${ggql[$name]}
    if [[ "$v" == "$gt" ]]; then
        status="✅ $ext"
    else
        d=$((v - gt))
        if [[ "$ext" == "Δ≤2 ok" && ${d#-} -le 2 ]]; then
            status="≈ Δ=$d $ext"
        else
            status="❌ Δ=$d $ext"
            fail=$((fail+1))
        fi
    fi
    printf '%-15s %8s %12s %s\n' "$name" "$v" "$gt" "$status"
}
check FIND_SEGMENT "$seg_gt" "(gfatools)"
check FIND_EDGE    "$edg_gt" "(gfatools)"
check FIND_TIP     "$tip_gt" "(awk)"
check FIND_REPEAT  "$rep_gt" "(awk)"
check FIND_BUBBLE  "$bub_inner" "Δ≤2 ok"
check FIND_PATH    "$pat_gt" "(P-lines)"

echo
echo "Notas:"
echo "  - vg snarls total = $bub_total | com conteúdo interno = $bub_inner"
echo "  - 'snarls vazios' (Net-Graph=2): $((bub_total - bub_inner)) — excluídos pelo GGQL"
if [[ $fail -eq 0 ]]; then
    echo "✅ Sprint 2 Tier B passou (com tolerância Δ≤2 em BUBBLE)"
    exit 0
else
    echo "❌ $fail divergências"
    exit 1
fi
