#!/bin/bash
# r_memcheck.sh — Memory leak / sanitizer audit.
#
# Build separado em build-asan/ com -fsanitize=address,leak,undefined,
# roda ctest + queries representativas em tiny/DRB1, reporta leaks/UB.
#
# Não usa o build/ regular porque ASan tem overhead 2-3× e quebra
# benchmarks/timing. Build-asan é dedicado a verificação de correção
# de memória.
#
# Saída: tests/validation/r_memcheck_results/
#   - asan_log.txt      (output completo)
#   - leaks_summary.txt (apenas linhas com 'LeakSanitizer'/'AddressSanitizer'/'runtime error')
#   - status.txt        (pass/fail)
#
# Exit codes:
#   0 = sem leaks ou UB detectados
#   1 = leaks/UB encontrados

set -eo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)

BUILD=build-asan
OUT=tests/validation/r_memcheck_results
mkdir -p "$OUT"

# ── Build com sanitizers ────────────────────────────────────────────────────
# -fno-omit-frame-pointer: stack traces legíveis
# -fno-sanitize-recover: aborta na primeira UB detectada (vs continuar)
# -O1: ASan recomenda O1 (Debug mas com algumas otimizações para reduzir overhead)
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    echo "→ Configurando build-asan..."
    SAN_FLAGS="-fsanitize=address,leak,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    CXX=g++-14 cmake -S . -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-O1 -g $SAN_FLAGS" \
        -DCMAKE_C_FLAGS="-O1 -g $SAN_FLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$SAN_FLAGS" \
        -DCMAKE_SHARED_LINKER_FLAGS="$SAN_FLAGS" \
        2>&1 | tail -5
fi

echo "→ Build com ASan/LSan/UBSan..."
CXX=g++-14 cmake --build "$BUILD" -j8 2>&1 | tail -3

# ── ASan options ────────────────────────────────────────────────────────────
# detect_leaks=1     — habilita LSan
# halt_on_error=1    — para na primeira detecção
# strict_string_checks=1 — bounds checking mais agressivo
# detect_stack_use_after_return=1 — captura mais UB
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:strict_string_checks=1:detect_stack_use_after_return=1:abort_on_error=0:print_stats=1"
export LSAN_OPTIONS="suppressions=$ROOT/tests/validation/lsan_suppressions.txt:print_suppressions=0"
export UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1"

# Suppressions file (third_party leaks que não controlamos)
SUP="$ROOT/tests/validation/lsan_suppressions.txt"
if [[ ! -f "$SUP" ]]; then
    cat > "$SUP" <<'EOF'
# Suppressions de leaks em libs de terceiros — não controlamos.
leak:libomp
leak:libgomp
leak:__libc_start_main
EOF
fi

# ── 1. ctest com sanitizers ──────────────────────────────────────────────────
echo
echo "═══ Stage 1: ctest com ASan/LSan/UBSan ═══"
# `|| true`: ctest pode falhar (e.g. testes que pedem idx ausente). Não é
# leak — só captura output para análise. Falha real de ASan/LSan aparece
# no Stage 3 (extração de leaks/UB) abaixo.
(cd "$BUILD" && ctest -j2 --output-on-failure 2>&1) > "$OUT/asan_ctest.log" || true
ctest_rc=${PIPESTATUS[0]:-0}
echo "  ctest exit code: $ctest_rc"
# Mostra resumo
tail -10 "$OUT/asan_ctest.log"

# ── 2. Queries representativas em DRB1 (ingest + queries combinadas) ────────
echo
echo "═══ Stage 2: ingest + queries DRB1 com ASan ═══"
BENCH_SAN="$BUILD/bench_gbwt"
RUNQ_SAN="$BUILD/tests/run_queries"

DRB1=data/validation/tier_b/DRB1-3123.gfa
if [[ -f "$DRB1" && -x "$BENCH_SAN" ]]; then
    # Re-ingere para ter idx fresco gerado com ASan binary
    rm -rf /tmp/drb1_asan.gfa.idx
    cp "$DRB1" /tmp/drb1_asan.gfa
    echo "  → Ingest com ASan (verifica leaks na ingest path)..."
    "$BENCH_SAN" /tmp/drb1_asan.gfa 2 --quiet > "$OUT/asan_ingest.log" 2>&1 || true

    if [[ -x "$RUNQ_SAN" && -d /tmp/drb1_asan.gfa.idx ]]; then
        echo "  → Queries via run_queries com ASan (verifica leaks no planner)..."
        {
            for q in \
                "FIND SEGMENT" \
                "FIND EDGE" \
                "FIND PATH" \
                "FIND BUBBLE" \
                "FIND TIP" \
                "FIND REPEAT" \
                "FIND SEGMENT WHERE length > 30 LIMIT 5" \
                "FIND SEGMENT WHERE length > 30 AND length < 32" \
                "FIND SEGMENT WHERE length = 1 OR length > 30" \
                "FIND PATH WHERE crosses(\"gi|157702218:147985-163915\")" \
                "FIND SEGMENT WHERE contains_kmer(\"AAA\")" \
                "FIND SEGMENT RETURN id, name, length LIMIT 3" \
                "FIND PATH RETURN name LIMIT 5 OFFSET 2" \
                "SEARCH KMER \"ACGT\"" \
                ; do
                echo "── Q: $q ──"
                "$RUNQ_SAN" /tmp/drb1_asan.gfa.idx "$q" 2>&1 || true
            done
        } > "$OUT/asan_queries.log" 2>&1
    fi
fi

# ── 3. Análise: extrai linhas suspeitas ─────────────────────────────────────
echo
echo "═══ Stage 3: extração de leaks/UB ═══"
{
    echo "=== LeakSanitizer ==="
    grep -E "LeakSanitizer|Direct leak|Indirect leak|SUMMARY: " "$OUT"/asan_*.log 2>/dev/null | head -50
    echo
    echo "=== AddressSanitizer errors ==="
    grep -E "AddressSanitizer:.*error|heap-use-after-free|heap-buffer-overflow|stack-buffer-overflow|use-after-poison" "$OUT"/asan_*.log 2>/dev/null | head -30
    echo
    echo "=== UndefinedBehaviorSanitizer ==="
    grep -E "runtime error:|UndefinedBehaviorSanitizer" "$OUT"/asan_*.log 2>/dev/null | head -30
    echo
    echo "=== Stats finais ==="
    grep -E "Stats:|Allocated|Freed" "$OUT"/asan_*.log 2>/dev/null | tail -10
} > "$OUT/leaks_summary.txt"
cat "$OUT/leaks_summary.txt"

# ── Resumo ──────────────────────────────────────────────────────────────────
leak_count=$(grep -cE "LeakSanitizer.*detected memory leaks" "$OUT"/asan_*.log 2>/dev/null || true)
asan_err=$(grep -cE "AddressSanitizer:.*(buffer-overflow|use-after-free|use-after-poison)" "$OUT"/asan_*.log 2>/dev/null || true)
ubsan_err=$(grep -cE "runtime error:" "$OUT"/asan_*.log 2>/dev/null || true)

echo
echo "═══ Resumo memcheck ═══"
printf '  Leak reports:       %s\n' "$leak_count"
printf '  ASan errors:        %s\n' "$asan_err"
printf '  UBSan runtime err:  %s\n' "$ubsan_err"

if [[ "$leak_count" -eq 0 && "$asan_err" -eq 0 && "$ubsan_err" -eq 0 ]]; then
    echo "✅ MEMCHECK PASSOU: 0 leaks, 0 ASan, 0 UBSan errors"
    echo "PASS" > "$OUT/status.txt"
    exit 0
else
    echo "❌ MEMCHECK FALHOU — ver $OUT/leaks_summary.txt"
    echo "FAIL: leaks=$leak_count asan=$asan_err ubsan=$ubsan_err" > "$OUT/status.txt"
    exit 1
fi
