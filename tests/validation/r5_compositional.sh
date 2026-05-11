#!/bin/bash
# r5_compositional.sh — R5: compositional queries (AND/OR/parens).
#
# Sprints anteriores testaram WHERE com 1 predicado por vez. R5 audita
# COMBINAÇÕES com 2+ predicados, parens, e mistura de caches/índices
# distintos.
#
# Padrão: GT via awk/python externo, GGQL via run_queries.
# Foco: detectar bugs em ordem de avaliação, short-circuit, ou interação
# entre múltiplos índices (length_index + name_index + KmerCache).
#
# Dataset: DRB1-3123 (rápido e tem suficiente diversidade de length).

set -eo pipefail
cd "$(dirname "$0")/../.."

GFA=data/validation/tier_b/DRB1-3123.gfa
IDX=$GFA.idx
RUNQ=build/tests/run_queries
[[ -x "$RUNQ" && -d "$IDX" && -f "$GFA" ]] || { echo "FALTA tool/idx/gfa"; exit 1; }

fail=0

check() {
    local label=$1 query=$2 gt=$3
    local got
    got=$("$RUNQ" "$IDX" "$query" 2>/dev/null | tail -1)
    if [[ "$got" == "$gt" ]]; then
        printf '%-60s ✅ %s\n' "$label" "$got"
    else
        printf '%-60s ❌ ggql=%s gt=%s\n' "$label" "$got" "$gt"
        fail=$((fail+1))
    fi
}

echo "═══ R5: compositional queries (AND/OR/parens) ═══"
echo

# ── Grupo A: AND/OR puro sobre length ─────────────────────────────────────────
echo "── Grupo A: AND/OR sobre length ──"

gt=$(awk '$1=="S" && length($3)>30 && length($3)<32 {c++} END {print c+0}' "$GFA")
check "AND  (len>30 AND len<32)"      'FIND SEGMENT WHERE length > 30 AND length < 32' "$gt"

gt=$(awk '$1=="S" && (length($3)==1 || length($3)>30) {c++} END {print c+0}' "$GFA")
check "OR   (len=1 OR len>30)"        'FIND SEGMENT WHERE length = 1 OR length > 30' "$gt"

gt=$(awk '$1=="S" && length($3)>=20 && length($3)<=29 {c++} END {print c+0}' "$GFA")
check "AND  (len>=20 AND len<=29)"    'FIND SEGMENT WHERE length >= 20 AND length <= 29' "$gt"

gt=$(awk '$1=="S" && (length($3)<5 || length($3)>=30) {c++} END {print c+0}' "$GFA")
check "OR   (len<5 OR len>=30)"       'FIND SEGMENT WHERE length < 5 OR length >= 30' "$gt"

# ── Grupo B: Parens (precedência operadora) ───────────────────────────────────
echo
echo "── Grupo B: Parens (precedência) ──"

gt=$(awk '$1=="S" && ((length($3)>30 && length($3)<32) || length($3)==1) {c++} END {print c+0}' "$GFA")
check "(AND) OR        ((len>30 AND len<32) OR len=1)" \
    'FIND SEGMENT WHERE (length > 30 AND length < 32) OR length = 1' "$gt"

gt=$(awk '$1=="S" && (length($3)>20 && (length($3)<25 || length($3)>=30)) {c++} END {print c+0}' "$GFA")
check "AND (OR)        (len>20 AND (len<25 OR len>=30))" \
    'FIND SEGMENT WHERE length > 20 AND (length < 25 OR length >= 30)' "$gt"

# Sem parens: AND tem precedência sobre OR (default em SQL/lógica)
# (A AND B) OR C  ≡  A AND B OR C
gt=$(awk '$1=="S" && (length($3)>30 && length($3)<32 || length($3)==1) {c++} END {print c+0}' "$GFA")
check "AND OR sem parens (deve = (AND) OR)" \
    'FIND SEGMENT WHERE length > 30 AND length < 32 OR length = 1' "$gt"

# ── Grupo C: AND com name (índice diferente) ──────────────────────────────────
echo
echo "── Grupo C: AND com name (mistura length_index + name_index) ──"

# DRB1 tem nome de segment = string numérica '1', '2', ... — vamos pegar um
# nome real com length suficiente.
SAMPLE_NAME=$(awk '$1=="S" && length($3)>30 && length($3)<32 {print $2; exit}' "$GFA")
echo "  (sample_name='$SAMPLE_NAME')"

gt=$(awk -v n="$SAMPLE_NAME" '$1=="S" && $2==n && length($3)>30 {c++} END {print c+0}' "$GFA")
check "AND  name+length  (name=$SAMPLE_NAME AND len>30)" \
    "FIND SEGMENT WHERE name = \"$SAMPLE_NAME\" AND length > 30" "$gt"

gt=$(awk -v n="$SAMPLE_NAME" '$1=="S" && ($2==n || length($3)>100) {c++} END {print c+0}' "$GFA")
check "OR   name+length  (name=$SAMPLE_NAME OR len>100)" \
    "FIND SEGMENT WHERE name = \"$SAMPLE_NAME\" OR length > 100" "$gt"

# ── Grupo D: crosses() AND/OR crosses() (CrossesCache stress) ────────────────
echo
echo "── Grupo D: crosses() AND/OR crosses() (CrossesCache) ──"

P1=$(awk '$1=="P" {print $2; exit}' "$GFA")
P2=$(awk '$1=="P" && NR>1 {print $2}' "$GFA" | sed -n '2p')
[[ -z "$P2" ]] && P2=$(awk '$1=="P"' "$GFA" | sed -n '2p' | cut -f2)
echo "  (P1=$P1)"
echo "  (P2=$P2)"

# GT via python: extrai sets de nós dos paths e usa set ops
python3 - <<PYEOF > /tmp/r5_p1p2.tsv
import re
paths = {}
with open('$GFA') as f:
    for line in f:
        if not line.startswith('P\t'): continue
        parts = line.rstrip('\n').split('\t')
        name = parts[1]
        nodes = set(int(x.rstrip('+-')) for x in parts[2].split(','))
        paths[name] = nodes

p1_nodes = paths.get('$P1', set())
p2_nodes = paths.get('$P2', set())

# Para cada path Q, ele "crosses(P1)" se Q ∩ P1 != ∅
def crosses(q_name, target_nodes):
    q = paths.get(q_name, set())
    return bool(q & target_nodes)

count_p1 = sum(1 for q in paths if crosses(q, p1_nodes))
count_p2 = sum(1 for q in paths if crosses(q, p2_nodes))
count_and = sum(1 for q in paths if crosses(q, p1_nodes) and crosses(q, p2_nodes))
count_or = sum(1 for q in paths if crosses(q, p1_nodes) or crosses(q, p2_nodes))
print(f"{count_p1}\t{count_p2}\t{count_and}\t{count_or}")
PYEOF
IFS=$'\t' read -r gt_p1 gt_p2 gt_and gt_or < /tmp/r5_p1p2.tsv

check "crosses(P1) só (baseline)" \
    "FIND PATH WHERE crosses(\"$P1\")" "$gt_p1"
check "crosses(P2) só (baseline)" \
    "FIND PATH WHERE crosses(\"$P2\")" "$gt_p2"
check "crosses(P1) AND crosses(P2)" \
    "FIND PATH WHERE crosses(\"$P1\") AND crosses(\"$P2\")" "$gt_and"
check "crosses(P1) OR crosses(P2)" \
    "FIND PATH WHERE crosses(\"$P1\") OR crosses(\"$P2\")" "$gt_or"

# ── Grupo E: contains_kmer com AND length (KmerCache + length_index) ──────────
echo
echo "── Grupo E: contains_kmer AND/OR length (cross-index) ──"

KMER="AAA"  # 3-mer com vários hits
gt=$(awk -v k="$KMER" '$1=="S" && index($3,k)>0 && length($3)>30 {c++} END {print c+0}' "$GFA")
check "kmer+length  AND  ($KMER + len>30)" \
    "FIND SEGMENT WHERE contains_kmer(\"$KMER\") AND length > 30" "$gt"

gt=$(awk -v k="$KMER" '$1=="S" && (index($3,k)>0 || length($3)>100) {c++} END {print c+0}' "$GFA")
check "kmer+length  OR   ($KMER + len>100)" \
    "FIND SEGMENT WHERE contains_kmer(\"$KMER\") OR length > 100" "$gt"

# ── Resumo ────────────────────────────────────────────────────────────────────
echo
if [[ $fail -eq 0 ]]; then
    echo "✅ R5: TODAS as queries compositionais conferem com GT externo"
    exit 0
else
    echo "❌ R5: $fail divergência(s)"
    exit 1
fi
