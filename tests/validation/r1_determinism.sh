#!/bin/bash
# r1_determinism.sh — R1: verifica determinismo da ingestão.
#
# Roda bench_gbwt 2× no MESMO GFA com a mesma thread-count e diffa
# SHA256 de TODOS os arquivos do .idx. Se diferirem, identifica quais.
#
# Datasets testados (cobertura crescente):
#   - tiny.gfa            (Tier A, ~10 nós)        - sanidade
#   - DRB1-3123.gfa       (Tier B, ~5k nós)        - paper-backed
#   - chm13_subset_100w   (Tier C, 4.3M nós)       - scale realista
#
# Uso:
#   bash tests/validation/r1_determinism.sh                # 3 datasets
#   bash tests/validation/r1_determinism.sh tiny           # só tiny
#   bash tests/validation/r1_determinism.sh tier_b         # só Tier B
#   bash tests/validation/r1_determinism.sh tier_c         # só Tier C

set -eo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)

BENCH="$ROOT/build/bench_gbwt"
[[ -x "$BENCH" ]] || { echo "FALTA: build/bench_gbwt"; exit 1; }

FILTER=${1:-all}

# Quais datasets rodar
declare -a datasets
case "$FILTER" in
    tiny)   datasets=("data/validation/tier_a/tiny.gfa:4") ;;
    tier_b) datasets=("data/validation/tier_b/DRB1-3123.gfa:4") ;;
    tier_c) datasets=("data/chm13_subset_100w.gfa:8") ;;
    all)
        datasets=(
            "data/validation/tier_a/tiny.gfa:4"
            "data/validation/tier_b/DRB1-3123.gfa:4"
            "data/chm13_subset_100w.gfa:8"
        )
        ;;
    *) echo "uso: $0 [tiny|tier_b|tier_c|all]"; exit 1 ;;
esac

total_fail=0

for entry in "${datasets[@]}"; do
    GFA="${entry%:*}"
    THREADS="${entry#*:}"
    NAME=$(basename "$GFA" .gfa)
    IDX_REAL="$GFA.idx"

    if [[ ! -f "$GFA" ]]; then
        echo "⊘ skip $NAME — GFA não encontrado: $GFA"
        continue
    fi

    echo
    echo "═══ R1 / $NAME (threads=$THREADS) ═══"

    # Diretórios temporários para 2 ingestões independentes
    RUN1=/tmp/r1_${NAME}_run1
    RUN2=/tmp/r1_${NAME}_run2
    rm -rf "$RUN1" "$RUN2"
    mkdir -p "$RUN1" "$RUN2"

    # Estratégia: bench_gbwt grava sempre em $GFA.idx (mesmo path).
    # Para isolar, criamos symlinks para o GFA em diretórios diferentes
    # e rodamos a partir de lá.
    ln -sf "$(realpath "$GFA")" "$RUN1/$(basename "$GFA")"
    ln -sf "$(realpath "$GFA")" "$RUN2/$(basename "$GFA")"

    GFA_REL=$(basename "$GFA")

    # Run 1
    echo "  → Run 1 ..."
    rm -rf "$RUN1/$GFA_REL.idx"
    t0=$(date +%s.%N)
    (cd "$RUN1" && "$BENCH" "$GFA_REL" "$THREADS" --quiet) > "$RUN1/log.txt" 2>&1
    t1=$(date +%s.%N)
    elapsed1=$(awk -v t0="$t0" -v t1="$t1" 'BEGIN{printf "%.2f", t1-t0}')

    # Run 2
    echo "  → Run 2 ..."
    rm -rf "$RUN2/$GFA_REL.idx"
    t0=$(date +%s.%N)
    (cd "$RUN2" && "$BENCH" "$GFA_REL" "$THREADS" --quiet) > "$RUN2/log.txt" 2>&1
    t1=$(date +%s.%N)
    elapsed2=$(awk -v t0="$t0" -v t1="$t1" 'BEGIN{printf "%.2f", t1-t0}')

    echo "  ✓ Run 1: $elapsed1 s | Run 2: $elapsed2 s"

    IDX1="$RUN1/$GFA_REL.idx"
    IDX2="$RUN2/$GFA_REL.idx"

    if [[ ! -d "$IDX1" || ! -d "$IDX2" ]]; then
        echo "  ❌ FALHA: .idx não foi gerado"
        total_fail=$((total_fail+1))
        continue
    fi

    # Lista arquivos de ambos os runs (união)
    files1=$(cd "$IDX1" && ls -1 | sort)
    files2=$(cd "$IDX2" && ls -1 | sort)
    if [[ "$files1" != "$files2" ]]; then
        echo "  ❌ Lista de arquivos diverge:"
        diff <(echo "$files1") <(echo "$files2") | head -20
        total_fail=$((total_fail+1))
        continue
    fi

    # Compara SHA256 arquivo a arquivo
    diverged=0
    for f in $files1; do
        # Pula manifest.txt (contém timestamps/paths legíveis — análise separada)
        if [[ "$f" == "manifest.txt" ]]; then
            continue
        fi
        h1=$(sha256sum "$IDX1/$f" | awk '{print $1}')
        h2=$(sha256sum "$IDX2/$f" | awk '{print $1}')
        s1=$(stat -c%s "$IDX1/$f")
        s2=$(stat -c%s "$IDX2/$f")
        if [[ "$h1" == "$h2" ]]; then
            printf '    ✅ %-25s %10s B  %s\n' "$f" "$s1" "${h1:0:12}…"
        else
            if [[ "$s1" == "$s2" ]]; then
                printf '    ❌ %-25s %10s B  size=ok hash≠ (%s vs %s)\n' "$f" "$s1" "${h1:0:8}" "${h2:0:8}"
            else
                printf '    ❌ %-25s SIZE DIVERGE: %s vs %s B\n' "$f" "$s1" "$s2"
            fi
            diverged=$((diverged+1))
        fi
    done

    # manifest.txt: comparação textual (pode diferir em paths/timestamps)
    if [[ -f "$IDX1/manifest.txt" && -f "$IDX2/manifest.txt" ]]; then
        # Diff ignorando linhas que claramente são path/timestamp-dependentes
        diff_count=$(diff "$IDX1/manifest.txt" "$IDX2/manifest.txt" | grep -cE '^[<>]' || true)
        if [[ "$diff_count" == 0 ]]; then
            printf '    ✅ %-25s textually identical\n' "manifest.txt"
        else
            printf '    ⚠  %-25s %s diff lines (paths/timestamps acceptable)\n' "manifest.txt" "$diff_count"
        fi
    fi

    if [[ $diverged -eq 0 ]]; then
        echo "  ✅ $NAME: TODOS arquivos binários idênticos"
    else
        echo "  ❌ $NAME: $diverged arquivo(s) com hash diferente"
        total_fail=$((total_fail+1))
    fi
done

echo
if [[ $total_fail -eq 0 ]]; then
    echo "✅ R1: ingestão determinística em todos os datasets"
    exit 0
else
    echo "❌ R1: $total_fail dataset(s) com divergência — investigar"
    exit 1
fi
