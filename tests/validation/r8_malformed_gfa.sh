#!/bin/bash
# r8_malformed_gfa.sh — R8: robustez contra GFAs malformados.
#
# Sprints anteriores só testaram GFAs válidos. R8 audita comportamento
# em casos negativos:
#
#   1. Header GFA ausente
#   2. S-line com nome vazio
#   3. L-line apontando para segmento inexistente
#   4. P-line com nó inexistente
#   5. dp:f:nan / dp:f:inf
#   6. Self-loop (L X+ X+ 0M)
#   7. dp:f:-1 (negative depth)
#   8. Linha S órfã (sem links)
#   9. P-line vazia (sem nós)
#  10. Caracteres não-ASCII no nome de path
#
# Critério: GGQL deve REJEITAR (exit ≠ 0) ou IGNORAR graciosamente,
# nunca crashar (SIGSEGV/SIGABRT) ou loop infinito.

set -eo pipefail
cd "$(dirname "$0")/../.."

BENCH=build/bench_gbwt
RUNQ=build/tests/run_queries
[[ -x "$BENCH" && -x "$RUNQ" ]] || { echo "FALTA tool"; exit 1; }

OUT=tests/validation/r8_results
mkdir -p "$OUT"

fail=0
pass=0
skipped=0

# Helper: testa que ingerir + queriar não crasha (signal ≥ 128 = crash)
test_robust() {
    local name=$1 gfa_content=$2 query=${3:-"FIND SEGMENT"}
    local gfa_path=/tmp/r8_${name}.gfa
    local idx_path=$gfa_path.idx
    rm -rf "$idx_path"
    printf '%s' "$gfa_content" > "$gfa_path"

    # Ingere (timeout 20s, captura exit code)
    local ingest_exit=0
    timeout 20 "$BENCH" "$gfa_path" 2 --quiet > "$OUT/${name}_ingest.log" 2>&1 || ingest_exit=$?

    # Detecta crash: signal 6 (SIGABRT) = 134, 11 (SIGSEGV) = 139, 124=timeout
    if [[ $ingest_exit -ge 128 && $ingest_exit -ne 130 ]]; then
        printf '  ❌ %-30s INGEST CRASH (exit=%s)\n' "$name" "$ingest_exit"
        fail=$((fail+1))
        return
    fi
    if [[ $ingest_exit -eq 124 ]]; then
        printf '  ❌ %-30s INGEST TIMEOUT (20s)\n' "$name"
        fail=$((fail+1))
        return
    fi

    # Se ingest falhou com erro gracioso (exit 1-127), considera ok
    if [[ $ingest_exit -ne 0 ]]; then
        printf '  ✅ %-30s rejeitado graciosamente (exit=%s)\n' "$name" "$ingest_exit"
        pass=$((pass+1))
        return
    fi

    # Ingest OK; tenta query
    local query_exit=0
    timeout 10 "$RUNQ" "$idx_path" "$query" > "$OUT/${name}_query.log" 2>&1 || query_exit=$?

    if [[ $query_exit -ge 128 && $query_exit -ne 130 ]]; then
        printf '  ❌ %-30s QUERY CRASH (exit=%s)\n' "$name" "$query_exit"
        fail=$((fail+1))
        return
    fi

    if [[ $query_exit -eq 0 ]]; then
        printf '  ✅ %-30s aceito + query OK\n' "$name"
    else
        printf '  ✅ %-30s aceito + query err gracioso (exit=%s)\n' "$name" "$query_exit"
    fi
    pass=$((pass+1))
}

echo "═══ R8: robustez contra GFAs malformados ═══"
echo

# ── Caso 1: Header ausente (já ok — GFA não exige header H) ─────────────────
test_robust "no_header" "$(cat <<'EOF'
S	1	ACGT
S	2	GCTA
L	1	+	2	+	0M
P	p1	1+,2+	*
EOF
)"

# ── Caso 2: S-line com nome vazio ───────────────────────────────────────────
test_robust "empty_seg_name" "$(cat <<'EOF'
H	VN:Z:1.0
S		ACGT
S	2	GCTA
L	2	+	2	+	0M
EOF
)"

# ── Caso 3: L-line para ID inexistente ──────────────────────────────────────
test_robust "link_to_missing_seg" "$(cat <<'EOF'
H	VN:Z:1.0
S	1	ACGT
S	2	GCTA
L	1	+	999	+	0M
L	2	+	1	+	0M
EOF
)"

# ── Caso 4: P-line com nó inexistente ───────────────────────────────────────
test_robust "path_to_missing_node" "$(cat <<'EOF'
H	VN:Z:1.0
S	1	ACGT
S	2	GCTA
L	1	+	2	+	0M
P	p1	1+,888+	*
EOF
)" "FIND PATH"

# ── Caso 5: dp:f:nan ────────────────────────────────────────────────────────
test_robust "depth_nan" "$(cat <<'EOF'
H	VN:Z:1.0
S	1	ACGT	dp:f:nan
S	2	GCTA	dp:f:inf
S	3	TTTT	dp:f:-inf
L	1	+	2	+	0M
L	2	+	3	+	0M
EOF
)" "FIND SEGMENT WHERE depth > 0"

# ── Caso 6: Self-loop ───────────────────────────────────────────────────────
test_robust "self_loop" "$(cat <<'EOF'
H	VN:Z:1.0
S	1	ACGT
S	2	GCTA
L	1	+	1	+	0M
L	1	+	2	+	0M
L	2	+	2	-	0M
EOF
)"

# ── Caso 7: dp:f:-1 (depth negativo) ────────────────────────────────────────
test_robust "depth_negative" "$(cat <<'EOF'
H	VN:Z:1.0
S	1	ACGT	dp:f:-1.5
S	2	GCTA	dp:f:0.0
L	1	+	2	+	0M
EOF
)" "FIND SEGMENT WHERE depth < 0"

# ── Caso 8: S-line órfã (sem links) ─────────────────────────────────────────
test_robust "orphan_seg" "$(cat <<'EOF'
H	VN:Z:1.0
S	1	ACGT
S	2	GCTA
S	3	NEVER_LINKED
L	1	+	2	+	0M
EOF
)"

# ── Caso 9: P-line vazia ────────────────────────────────────────────────────
test_robust "empty_path" "$(cat <<'EOF'
H	VN:Z:1.0
S	1	ACGT
S	2	GCTA
L	1	+	2	+	0M
P	empty_p		*
P	good_p	1+,2+	*
EOF
)" "FIND PATH"

# ── Caso 10: Caracteres não-ASCII no nome de path ───────────────────────────
test_robust "unicode_path_name" "$(printf 'H\tVN:Z:1.0\nS\t1\tACGT\nS\t2\tGCTA\nL\t1\t+\t2\t+\t0M\nP\tp1_\xc3\xa9_caf\xc3\xa9\t1+,2+\t*\n')" "FIND PATH RETURN name"

# ── Resumo ──────────────────────────────────────────────────────────────────
echo
echo "═══ R8 Resumo ═══"
echo "  ✅ Comportamento robusto: $pass"
echo "  ❌ Crashes/timeouts:      $fail"

if [[ $fail -eq 0 ]]; then
    echo "✅ R8: GGQL não crasha em 10 casos malformados testados"
    exit 0
else
    echo "❌ R8: $fail crashes detectados"
    exit 1
fi
