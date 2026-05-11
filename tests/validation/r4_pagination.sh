#!/bin/bash
# r4_pagination.sh — R4: paginação determinística.
#
# Valida três propriedades fundamentais do LIMIT/OFFSET:
#   1) Mesma query 2x retorna as mesmas linhas em mesma ordem (idempotência)
#   2) Iterar OFFSET 0, N, 2N, 3N... cobre TODOS os hits sem overlap
#   3) União das páginas == resultado completo
#
# Datasets: DRB1-3123 (5581 segs, 12 paths) — médio o suficiente para
# múltiplas páginas, pequeno o suficiente para rodar rápido.

set -eo pipefail
cd "$(dirname "$0")/../.."

IDX=data/validation/tier_b/DRB1-3123.gfa.idx
RUNQ=build/tests/run_queries
[[ -x "$RUNQ" && -d "$IDX" ]] || { echo "FALTA tool/idx"; exit 1; }

fail=0

# ── Helper: query com LIMIT/OFFSET → TSV (sem header #) ───────────────────────
# `grep -v '^#'` retorna exit 1 quando não há output (página vazia) — usar
# `|| true` para não disparar set -e.
page() {
    local query=$1 limit=$2 offset=$3
    "$RUNQ" "$IDX" --rows "$query LIMIT $limit OFFSET $offset" 2>/dev/null | (grep -v '^#' || true)
}

# ── Helper: query sem LIMIT/OFFSET → resultado completo ────────────────────────
full() {
    local query=$1
    "$RUNQ" "$IDX" --rows "$query LIMIT 1000000" 2>/dev/null | (grep -v '^#' || true)
}

# ── 1. Idempotência ───────────────────────────────────────────────────────────
echo "═══ R4.1: idempotência (mesma query 2x = mesmo resultado em mesma ordem) ═══"
test_idempotent() {
    local label=$1 query=$2 limit=$3 offset=$4
    local a=/tmp/r4_idem_a.tsv
    local b=/tmp/r4_idem_b.tsv
    page "$query" "$limit" "$offset" > "$a"
    page "$query" "$limit" "$offset" > "$b"
    if cmp -s "$a" "$b"; then
        local n=$(wc -l < "$a")
        printf '%-50s ✅ %s linhas idênticas em mesma ordem\n' "$label" "$n"
    else
        printf '%-50s ❌ ORDEM DIFERE entre 2 execuções!\n' "$label"
        diff "$a" "$b" | head -3 | sed 's/^/    /'
        fail=$((fail+1))
    fi
}
test_idempotent "FIND SEGMENT LIMIT 100 OFFSET 0"   "FIND SEGMENT RETURN name" 100 0
test_idempotent "FIND SEGMENT LIMIT 100 OFFSET 500" "FIND SEGMENT RETURN name" 100 500
test_idempotent "FIND PATH LIMIT 5"                 "FIND PATH RETURN name"      5 0
test_idempotent "FIND EDGE LIMIT 50 OFFSET 100"     "FIND EDGE RETURN to_id"    50 100

# ── 2. Cobertura: OFFSET 0+N+2N == query completa ─────────────────────────────
echo
echo "═══ R4.2: cobertura (páginas OFFSET 0+N+2N+... == resultado completo) ═══"
test_coverage() {
    local label=$1 query=$2 page_size=$3
    local full_out=/tmp/r4_full.tsv
    local pages_out=/tmp/r4_pages.tsv

    full "$query" > "$full_out"
    local total=$(wc -l < "$full_out")
    if [[ $total -eq 0 ]]; then
        printf '%-50s ⊘ query vazia (skipping)\n' "$label"
        return
    fi

    # Itera páginas até esgotar
    > "$pages_out"
    local offset=0
    local page_count=0
    while :; do
        local p=/tmp/r4_page.tsv
        page "$query" "$page_size" "$offset" > "$p"
        local n=$(wc -l < "$p")
        if [[ $n -eq 0 ]]; then break; fi
        cat "$p" >> "$pages_out"
        offset=$((offset + page_size))
        page_count=$((page_count+1))
        # Limite de segurança: se já passou o total esperado, para
        if [[ $offset -gt $((total + page_size)) ]]; then break; fi
    done

    # Verifica: union(pages) == full em multiset?
    local pages_count=$(wc -l < "$pages_out")

    # Compara como conjunto (ordem pode diferir entre páginas)
    sort "$full_out" > /tmp/r4_full_sorted.tsv
    sort "$pages_out" > /tmp/r4_pages_sorted.tsv

    if [[ "$pages_count" -eq "$total" ]] && cmp -s /tmp/r4_full_sorted.tsv /tmp/r4_pages_sorted.tsv; then
        printf '%-50s ✅ %s páginas × %s = %s linhas (= full)\n' "$label" "$page_count" "$page_size" "$total"
    else
        printf '%-50s ❌ páginas=%s full=%s\n' "$label" "$pages_count" "$total"
        local only_full=$(comm -23 /tmp/r4_full_sorted.tsv /tmp/r4_pages_sorted.tsv | wc -l)
        local only_pages=$(comm -13 /tmp/r4_full_sorted.tsv /tmp/r4_pages_sorted.tsv | wc -l)
        printf '    only_full=%s only_pages=%s\n' "$only_full" "$only_pages"
        fail=$((fail+1))
    fi
}
test_coverage "FIND SEGMENT cobertura" "FIND SEGMENT RETURN name"    1000
test_coverage "FIND EDGE cobertura"    "FIND EDGE RETURN to_id"      2000
test_coverage "FIND PATH cobertura"    "FIND PATH RETURN name"          5
test_coverage "FIND TIP cobertura"     "FIND TIP RETURN in_degree, out_degree" 2

# ── 3. Disjunção: OFFSET 0..N e OFFSET N..2N não compartilham linhas ──────────
echo
echo "═══ R4.3: disjunção entre páginas adjacentes ═══"
test_disjoint() {
    local label=$1 query=$2 page_size=$3
    local p1=/tmp/r4_p1.tsv
    local p2=/tmp/r4_p2.tsv
    page "$query" "$page_size" 0           | sort > "$p1"
    page "$query" "$page_size" "$page_size" | sort > "$p2"
    local n1=$(wc -l < "$p1")
    local n2=$(wc -l < "$p2")
    local overlap=$(comm -12 "$p1" "$p2" | wc -l)
    if [[ $overlap -eq 0 ]]; then
        printf '%-50s ✅ p1=%s p2=%s overlap=0\n' "$label" "$n1" "$n2"
    else
        printf '%-50s ⚠  p1=%s p2=%s overlap=%s (pode ser ok se duplicatas legítimas)\n' \
            "$label" "$n1" "$n2" "$overlap"
        # Não falha imediatamente: para FIND EDGE/TIP/PATH com valores
        # repetidos legítimos, overlap em set não é bug. Para SEGMENT
        # com names únicos, é bug.
        if [[ "$query" == *"SEGMENT"*"name"* ]]; then
            echo "    SEGMENT.name DEVE ser único — overlap é BUG"
            fail=$((fail+1))
        fi
    fi
}
test_disjoint "FIND SEGMENT disjunção" "FIND SEGMENT RETURN name" 100
test_disjoint "FIND PATH disjunção"    "FIND PATH RETURN name"      3
test_disjoint "FIND TIP disjunção"     "FIND TIP RETURN in_degree, out_degree" 2

echo
if [[ $fail -eq 0 ]]; then
    echo "✅ R4: paginação determinística (idempotência + cobertura + disjunção)"
    exit 0
else
    echo "❌ R4: $fail falha(s)"
    exit 1
fi
