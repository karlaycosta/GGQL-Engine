#!/bin/bash
# r3_return_content.sh — R3: valida o CONTEÚDO de RETURN (não só counts).
#
# Sprints anteriores comparavam apenas total_hits. R3 audita os VALORES
# das linhas retornadas contra GT externo (awk sobre GFA).
#
# Estratégia:
#   1) GGQL produz TSV via run_queries --rows
#   2) awk produz TSV equivalente (mesmas colunas, mesma ordem)
#   3) sort + diff para comparar conjuntos
#
# Datasets: DRB1-3123 (Tier B) — suficiente para detectar bugs em
# conteúdo (counts já validados em Sprint 2).

set -eo pipefail
cd "$(dirname "$0")/../.."

GFA=data/validation/tier_b/DRB1-3123.gfa
IDX=$GFA.idx
RUNQ=build/tests/run_queries
[[ -x "$RUNQ" && -d "$IDX" && -f "$GFA" ]] || { echo "FALTA tool/idx/gfa"; exit 1; }

fail=0

check_rows() {
    local label=$1 query=$2 gt_script=$3
    local tmp_ggql=/tmp/r3_ggql.tsv
    local tmp_gt=/tmp/r3_gt.tsv

    # Captura conteúdo (sort para comparar conjuntos)
    "$RUNQ" "$IDX" --rows "$query" 2>/dev/null | grep -v '^#' | sort -u > "$tmp_ggql"
    eval "$gt_script" | sort -u > "$tmp_gt"

    local ggql_n=$(wc -l < "$tmp_ggql")
    local gt_n=$(wc -l < "$tmp_gt")

    if diff -q "$tmp_ggql" "$tmp_gt" >/dev/null 2>&1; then
        printf '%-50s ✅ %s linhas (conteúdo idêntico)\n' "$label" "$ggql_n"
    else
        local only_ggql=$(comm -23 "$tmp_ggql" "$tmp_gt" | wc -l)
        local only_gt=$(comm -13 "$tmp_ggql" "$tmp_gt" | wc -l)
        printf '%-50s ❌ ggql=%s gt=%s onlyGGQL=%s onlyGT=%s\n' \
            "$label" "$ggql_n" "$gt_n" "$only_ggql" "$only_gt"
        echo "    Primeiras divergências (só GGQL):"
        comm -23 "$tmp_ggql" "$tmp_gt" | head -3 | sed 's/^/      /'
        echo "    Primeiras divergências (só GT):"
        comm -13 "$tmp_ggql" "$tmp_gt" | head -3 | sed 's/^/      /'
        fail=$((fail+1))
    fi
}

echo "═══ R3: conteúdo de RETURN em DRB1-3123 ═══"

# ── 1. FIND PATH RETURN name ──
# GT: P-lines, coluna 2 (path name) ou W-lines W\tsample\thap\tcontig\tstart\tend\t...
check_rows "FIND PATH RETURN name" \
    "FIND PATH RETURN name" \
    "awk '\$1==\"P\" {print \$2} \$1==\"W\" {print \$2\"#\"\$3\"#\"\$4\":\"\$5\"-\"\$6}' '$GFA'"

# ── 2. FIND SEGMENT WHERE length > X RETURN id, name, length ──
# GT: S-lines com length(\$3) > X; ggql usa id INTERNO (não nome). Compara só nome+length.
check_rows "FIND SEGMENT len>20 RETURN name, length" \
    "FIND SEGMENT WHERE length > 20 RETURN name, length" \
    "awk '\$1==\"S\" && length(\$3)>20 {print \$2\"\t\"length(\$3)}' '$GFA'"

# ── 3. FIND SEGMENT WHERE name = X (lookup unitário) ──
# GT: linha S com nome específico. Pegamos um nome real do GFA.
SAMPLE_NAME=$(awk '$1=="S" && length($3)>5 {print $2; exit}' "$GFA")
check_rows "FIND SEGMENT WHERE name = $SAMPLE_NAME RETURN name, length" \
    "FIND SEGMENT WHERE name = \"$SAMPLE_NAME\" RETURN name, length" \
    "awk -v n=\"$SAMPLE_NAME\" '\$1==\"S\" && \$2==n {print \$2\"\t\"length(\$3)}' '$GFA'"

# ── 4. FIND EDGE RETURN to_id ──
# GT: L-lines, coluna 4 (to_seg_name). GGQL retorna to_id INTERNO (uint).
# Estratégia: contar quantos edges chegam em cada nó. Sort do TSV.
# GGQL retorna to_id por linha; comparamos count(to_id) com count(awk).
# Aqui só compara distribuição (multiset), não nomes.
"$RUNQ" "$IDX" --rows 'FIND EDGE RETURN to_id' 2>/dev/null | grep -v '^#' | sort | uniq -c | awk '{print $1}' | sort -n > /tmp/r3_edge_ggql.txt
awk '$1=="L" {print $4}' "$GFA" | sort | uniq -c | awk '{print $1}' | sort -n > /tmp/r3_edge_gt.txt
if diff -q /tmp/r3_edge_ggql.txt /tmp/r3_edge_gt.txt >/dev/null 2>&1; then
    n=$(wc -l < /tmp/r3_edge_ggql.txt)
    printf '%-50s ✅ %s nós distintos (distribuição idêntica)\n' "FIND EDGE distribuição de to_id" "$n"
else
    printf '%-50s ❌ distribuição diverge\n' "FIND EDGE distribuição de to_id"
    fail=$((fail+1))
fi

# ── 5. FIND TIP RETURN id, in_degree, out_degree ──
# GT: nó com in_degree=0 OU out_degree=0.
# GGQL retorna id INTERNO; vamos comparar (in_deg, out_deg) tuples como multiset.
"$RUNQ" "$IDX" --rows 'FIND TIP RETURN in_degree, out_degree' 2>/dev/null | grep -v '^#' | sort > /tmp/r3_tip_ggql.txt
awk '$1=="L" {out[$2]++; in_[$4]++; seen[$2]=1; seen[$4]=1}
     $1=="S" {seen[$2]=1}
     END {
       for (n in seen) {
         i = in_[n]+0; o = out[n]+0
         if (i==0 || o==0) print i"\t"o
       }
     }' "$GFA" | sort > /tmp/r3_tip_gt.txt
if diff -q /tmp/r3_tip_ggql.txt /tmp/r3_tip_gt.txt >/dev/null 2>&1; then
    n=$(wc -l < /tmp/r3_tip_ggql.txt)
    printf '%-50s ✅ %s tips (in,out) idênticos\n' "FIND TIP RETURN in_degree, out_degree" "$n"
else
    printf '%-50s ❌ (in,out) multiset diverge\n' "FIND TIP RETURN in_degree, out_degree"
    fail=$((fail+1))
fi

# ── 6. FIND SEGMENT RETURN name LIMIT 5 — sanidade ──
# Validamos que os 5 retornados são nomes válidos (existem como S-line no GFA).
set +o pipefail   # awk | head -5 produz SIGPIPE que dispara errexit
"$RUNQ" "$IDX" --rows 'FIND SEGMENT RETURN name LIMIT 5' 2>/dev/null | grep -v '^#' | sort > /tmp/r3_seg5_ggql.txt
awk '$1=="S" {print $2}' "$GFA" | sort -u > /tmp/r3_all_seg.txt
set -o pipefail
ggql_n=$(wc -l < /tmp/r3_seg5_ggql.txt)
if [[ "$ggql_n" -eq 5 ]]; then
    invalid=$(comm -23 /tmp/r3_seg5_ggql.txt /tmp/r3_all_seg.txt | wc -l)
    if [[ $invalid -eq 0 ]]; then
        printf '%-50s ✅ 5 nomes válidos no GFA\n' "FIND SEGMENT LIMIT 5 RETURN name"
    else
        printf '%-50s ❌ %s nomes inválidos\n' "FIND SEGMENT LIMIT 5 RETURN name" "$invalid"
        fail=$((fail+1))
    fi
else
    printf '%-50s ❌ esperado 5 linhas, got %s\n' "FIND SEGMENT LIMIT 5 RETURN name" "$ggql_n"
    fail=$((fail+1))
fi

echo
if [[ $fail -eq 0 ]]; then
    echo "✅ R3: 6/6 validações de CONTEÚDO passaram"
    exit 0
else
    echo "❌ R3: $fail divergência(s) de conteúdo"
    exit 1
fi
