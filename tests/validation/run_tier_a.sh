#!/bin/bash
# run_tier_a.sh — Reprodução automática da validação Sprint 1 (Tier A).
#
# Pré-requisitos:
#   - Repo do GGQL clonado, buildado em build/
#   - data/validation/tier_a/{tiny,MT}.gfa.idx existem (rodar bench_gbwt antes)
#   - data/validation/tools/{gfatools/gfatools,vg} (built/baixado)
#
# Uso:  bash tests/validation/run_tier_a.sh
# Saída: TSV no stdout + diff em /tmp/validation_tier_a.diff (vazio = ok)

set -euo pipefail
cd "$(dirname "$0")/../.."

GFATOOLS=data/validation/tools/gfatools/gfatools
VG=data/validation/tools/vg
RUNQ=build/tests/run_queries

for tool in "$GFATOOLS" "$VG" "$RUNQ"; do
    [[ -x "$tool" ]] || { echo "FALTA: $tool"; exit 1; }
done

EXPECTED=tests/validation/expected_tier_a.tsv
ACTUAL=/tmp/validation_tier_a_actual.tsv
> "$ACTUAL"

for f in tiny MT; do
    GFA=data/validation/tier_a/$f.gfa
    IDX=data/validation/tier_a/$f.gfa.idx

    # Re-ingere se cache não existe
    [[ -d "$IDX" ]] || build/bench_gbwt "$GFA" 1 --quiet >/dev/null

    # Ground truth via tools externas
    seg_gt=$(awk '$1=="S"' "$GFA" | wc -l)
    edg_gt=$(awk '$1=="L"' "$GFA" | wc -l)
    pat_gt=$(awk '$1=="P"||$1=="W"' "$GFA" | wc -l)
    tip_gt=$(awk '$1=="L"{out[$2]++; in_[$4]++; n[$2]=1; n[$4]=1}
                  $1=="S"{n[$2]=1}
                  END{c=0; for(x in n) if(in_[x]==0||out[x]==0)c++; print c}' "$GFA")
    rep_gt=$(awk '$1=="L"{out[$2]++}
                  END{c=0; for(x in out) if(out[x]>=2)c++; print c}' "$GFA")
    "$VG" convert -g "$GFA" -p > /tmp/$f.pg 2>/dev/null
    bub_gt=$("$VG" stats -R /tmp/$f.pg 2>/dev/null | tail -n +2 | wc -l)
    rm -f /tmp/$f.pg

    # GGQL via run_queries
    while IFS=$'\t' read -r q c; do
        case "$q" in
            FIND_SEGMENT) gt=$seg_gt ;;
            FIND_EDGE)    gt=$edg_gt ;;
            FIND_TIP)     gt=$tip_gt ;;
            FIND_REPEAT)  gt=$rep_gt ;;
            FIND_BUBBLE)  gt=$bub_gt ;;
            FIND_PATH)    gt=$pat_gt ;;
            *) continue ;;
        esac
        printf '%s\t%s\t%s\t%s\n' "$f" "$q" "$c" "$gt" >> "$ACTUAL"
    done < <("$RUNQ" "$IDX" 2>/dev/null | grep -E '^FIND_')
done

# Tabela legível
printf '\n%-10s %-15s %8s %8s %s\n' "DATASET" "QUERY" "GGQL" "GT" "STATUS"
printf '%-10s %-15s %8s %8s %s\n' "-------" "-----" "----" "--" "------"
fail=0
while IFS=$'\t' read -r ds q ggql gt; do
    if [[ "$ggql" == "$gt" ]]; then status="✅"; else status="❌ Δ=$((ggql-gt))"; fail=$((fail+1)); fi
    printf '%-10s %-15s %8s %8s %s\n' "$ds" "$q" "$ggql" "$gt" "$status"
done < "$ACTUAL"

echo
if [[ $fail -eq 0 ]]; then
    echo "✅ TODAS AS COMPARAÇÕES TIER A CONFEREM"
    exit 0
else
    echo "❌ $fail comparações divergiram"
    exit 1
fi
