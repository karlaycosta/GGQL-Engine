#!/bin/bash
# run_tier_c.sh — Sprint 3 Tier C: HPRC chm13_subset_100w (4,3M nós, 100 paths).
#
# IMPORTANTE: 2 bugs descobertos via esta validação (Δ não-zero é diagnóstico):
#
#   B-V1: kMaxTotalHitsCount = 1M cap absoluto (query_planner.cpp:32, 326)
#         total_hits satura em 1M mesmo com LIMIT explícito. Counts globais
#         de SEGMENT (4.3M) e EDGE (5M) em HPRC são truncados.
#         → Mitigação: usar filtros que reduzam o conjunto a < 1M.
#
#   B-V2: find_all_snarls aborta em grafos com componentes sem tips
#         (snarl_decomposition.cpp:1101-1109). HPRC pangenoma "fechado"
#         retorna 0 snarls.  → FIND BUBBLE não validável em Tier C
#         até bug ser corrigido.

set -eo pipefail
cd "$(dirname "$0")/../.."

GFA=data/chm13_subset_100w.gfa
IDX=$GFA.idx
GFATOOLS=data/validation/tools/gfatools/gfatools
RUNQ=build/tests/run_queries

[[ -x "$GFATOOLS" && -x "$RUNQ" ]] || { echo "FALTA tool ou build"; exit 1; }
[[ -f "$GFA" ]] || { echo "FALTA: $GFA"; exit 1; }
[[ -d "$IDX" ]] || build/bench_gbwt "$GFA" 8 --quiet >/dev/null

# Ground truth via awk (rápido — escolhemos comparações que cabem no cap)
echo "Computando ground truth via awk… (~17s)"
gt_l1000=$(awk '$1=="S" && length($3) > 1000 {c++} END {print c+0}' "$GFA")
gt_l100=$(awk  '$1=="S" && length($3) > 100  {c++} END {print c+0}' "$GFA")
gt_l50=$(awk   '$1=="S" && length($3) == 50  {c++} END {print c+0}' "$GFA")
gt_path=$(awk  '$1=="P" || $1=="W" {c++} END {print c+0}' "$GFA")
gt_tip=$(awk   '$1=="L"{out[$2]++; in_[$4]++; n[$2]=1; n[$4]=1}
                $1=="S"{n[$2]=1}
                END{c=0; for(x in n) if(in_[x]==0||out[x]==0)c++; print c}' "$GFA")
gt_rep=$(awk   '$1=="L"{out[$2]++}
                END{c=0; for(x in out) if(out[x]>=2)c++; print c}' "$GFA")

ggql_l1000=$("$RUNQ" "$IDX" 'FIND SEGMENT WHERE length > 1000' 2>/dev/null | tail -1)
ggql_l100=$( "$RUNQ" "$IDX" 'FIND SEGMENT WHERE length > 100'  2>/dev/null | tail -1)
ggql_l50=$(  "$RUNQ" "$IDX" 'FIND SEGMENT WHERE length = 50'   2>/dev/null | tail -1)
mapfile -t lines < <("$RUNQ" "$IDX" 2>/dev/null | grep -E '^FIND_')
declare -A G; for l in "${lines[@]}"; do IFS=$'\t' read -r k v <<< "$l"; G[$k]=$v; done

printf '\n%-40s %10s %10s %s\n' "QUERY" "GGQL" "GT" "STATUS"
printf '%-40s %10s %10s %s\n' "$(printf '=%.0s' {1..40})" "----" "--" "------"

fail=0
check() {
    local name=$1 ggql=$2 gt=$3 ext=${4:-}
    if [[ "$ggql" == "$gt" ]]; then
        printf '%-40s %10s %10s %s\n' "$name" "$ggql" "$gt" "✅ $ext"
    else
        printf '%-40s %10s %10s %s\n' "$name" "$ggql" "$gt" "❌ Δ=$((ggql-gt)) $ext"
        fail=$((fail+1))
    fi
}
check "FIND TIP"                          "${G[FIND_TIP]}"     "$gt_tip"   "(awk)"
check "FIND REPEAT"                       "${G[FIND_REPEAT]}"  "$gt_rep"   "(awk)"
check "FIND PATH"                         "${G[FIND_PATH]}"    "$gt_path"  "(W-lines)"
check "WHERE length > 1000"               "$ggql_l1000"        "$gt_l1000" "(awk)"
check "WHERE length > 100"                "$ggql_l100"         "$gt_l100"  "(awk)"
check "WHERE length = 50"                 "$ggql_l50"          "$gt_l50"   "(awk)"

echo
echo "Não validados (bugs descobertos):"
printf '  %-38s %10s %10s ❌ B-V1 (cap 1M)\n' "FIND SEGMENT (count global)" "${G[FIND_SEGMENT]}" "4294760"
printf '  %-38s %10s %10s ❌ B-V1 (cap 1M)\n' "FIND EDGE (count global)"    "${G[FIND_EDGE]}"    "5025175"
printf '  %-38s %10s %10s ❌ B-V2 (algoritmo)\n' "FIND BUBBLE"               "${G[FIND_BUBBLE]}"  "?"

echo
if [[ $fail -eq 0 ]]; then
    echo "✅ 6/6 queries validáveis em Tier C passaram"
    echo "⚠ 3 queries bloqueadas por bugs B-V1 / B-V2 — ver cabeçalho"
    exit 0
else
    echo "❌ $fail divergências"
    exit 1
fi
