#!/bin/bash
# run_sprint5.sh — Sprint 5: validação L-V2 (crosses Tier C com GT externo).
#
# Estratégia: ground truth via parsing direto do GFA — extrai set de nodes
# de cada W-line e computa interseção entre paths. Compara com count
# retornado por `FIND PATH WHERE crosses(<target>)`.
#
# Nota sobre IDs: GGQL armazena internal_ids (post-MPHF remap), GT usa
# external_ids (W-line walk strings). Como a relação MPHF é bijetiva,
# COUNTS de path-interseção devem bater (mesmo conjunto de paths, IDs
# diferentes).

set -eo pipefail
cd "$(dirname "$0")/../.."

GFA=data/chm13_subset_100w.gfa
IDX=$GFA.idx
RUNQ=build/tests/run_queries

[[ -d "$IDX" ]] || { echo "FALTA $IDX — rodar bench_gbwt antes"; exit 1; }

# ── Computa GT via Python (set intersection sobre W-line node IDs) ─────────
echo "Computando GT (parsing W-lines, set intersection)... ~5s"
python3 - <<'PYEOF' > /tmp/sprint5_gt.tsv
import re
gfa = 'data/chm13_subset_100w.gfa'
walks = {}  # path_name → set(node_ids)
with open(gfa) as f:
    for line in f:
        if not line.startswith('W\t'): continue
        parts = line.rstrip('\n').split('\t')
        if len(parts) < 7: continue
        sample, hap, contig, start, end, walk = parts[1:7]
        name = f"{sample}#{hap}#{contig}:{start}-{end}"
        nodes = set(int(x) for x in re.findall(r'\d+', walk))
        walks[name] = nodes

# Para cada target, computa quantos paths crossam (interseção ≥ 1 nó)
# Inclui self-cross (true por definição). Reporta TSV: target\tcount.
items = list(walks.items())
for target_name, target_nodes in items:
    count = sum(1 for n, nodes in items if nodes & target_nodes)
    print(f"{target_name}\t{count}")
PYEOF

n_paths=$(wc -l < /tmp/sprint5_gt.tsv)
echo "  GT computado: $n_paths paths processados"

# Selecionar 5 targets espalhados ao longo do arquivo para validar
selected=$(awk 'NR==1 || NR==25 || NR==50 || NR==75 || NR==100' /tmp/sprint5_gt.tsv)

# ── Tabela de comparação ──────────────────────────────────────────────────
printf '\n%-55s %8s %8s %s\n' "TARGET (path)" "GGQL" "GT" "STATUS"
printf '%s\n' "$(printf '=%.0s' {1..85})"

pass=0; fail=0
while IFS=$'\t' read -r target gt; do
    [[ -z "$target" ]] && continue
    ggql=$("$RUNQ" "$IDX" "FIND PATH WHERE crosses(\"$target\") RETURN id" 2>/dev/null | tail -1)
    short=$(echo "$target" | head -c 50)
    if [[ "$ggql" == "$gt" ]]; then
        printf '  %-55s %8s == %-8s ✅\n' "$short" "$ggql" "$gt"
        pass=$((pass+1))
    else
        printf '  %-55s %8s != %-8s ❌ Δ=%d\n' "$short" "$ggql" "$gt" "$((ggql-gt))"
        fail=$((fail+1))
    fi
done <<< "$selected"

# Casos extras
echo
echo "  --- Casos extras ---"
# Self-cross trivial
first_target=$(head -1 /tmp/sprint5_gt.tsv | cut -f1)
first_gt=$(head -1 /tmp/sprint5_gt.tsv | cut -f2)
# (já testado acima, mas reforço da expectativa)

# Path inexistente
ggql=$("$RUNQ" "$IDX" 'FIND PATH WHERE crosses("__inexistente_HG999__") RETURN id' 2>/dev/null | tail -1)
if [[ "$ggql" == "0" ]]; then
    printf '  %-55s %8s == %-8s ✅\n' "crosses(<inexistente>)" "$ggql" "0"
    pass=$((pass+1))
else
    printf '  %-55s %8s != %-8s ❌\n' "crosses(<inexistente>)" "$ggql" "0"
    fail=$((fail+1))
fi

# Validar que crosses(target) inclui o próprio target (auto-inclusão)
self_count=$("$RUNQ" "$IDX" "FIND PATH WHERE crosses(\"$first_target\")" 2>/dev/null | tail -1)
if [[ "$self_count" -ge 1 ]]; then
    printf '  %-55s %8s ≥  %-8s ✅\n' "crosses(self) inclui self (≥1)" "$self_count" "1"
    pass=$((pass+1))
else
    printf '  %-55s %8s != %-8s ❌\n' "crosses(self) inclui self" "$self_count" "1"
    fail=$((fail+1))
fi

echo
total=$((pass + fail))
if [[ $fail -eq 0 ]]; then
    echo "✅ Sprint 5: $pass/$total casos passaram (L-V2 validado em Tier C)"
    exit 0
else
    echo "❌ Sprint 5: $fail/$total divergências"
    exit 1
fi
