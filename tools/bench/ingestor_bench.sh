#!/usr/bin/env bash
# ingestor_bench.sh — baseline reproduzível p/ os fixes do ingestor (H1/H2).
#
# Mede dois regimes complementares:
#   • WARM  (bench_survey, iters>1): isola CPU de parsing — baseline do H2
#     (custo do scan escalar token-a-token). Sem ruído de I/O.
#   • COLD  (bench_gbwt, cache evicted): isola I/O multi-passada — baseline do
#     H1 (survey+hash+Pass1+Pass2 relendo o arquivo). Conta MAJOR page faults
#     (= leituras de disco) via /usr/bin/time -v.
#
# Não precisa root: cache frio via tools/bench/drop_file_cache (posix_fadvise).
#
# Uso: tools/bench/ingestor_bench.sh [build_dir] [threads] [reps]
set -euo pipefail
export LC_ALL=C   # awk/printf com '.' decimal (locale pt_BR lê "0.064" como 0)

BUILD="${1:-build}"
THREADS="${2:-8}"
REPS="${3:-3}"          # repetições COLD por arquivo (mediana)
SURVEY_ITERS=6          # 1ª iter descartada (cold), média das demais = warm
PROFILE=metagenomics    # mais leve (sem gBWT/snarls) → maximiza fração de scan

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# Arquivos-alvo: ajuste conforme disponível. Variados em tamanho.
FILES=(
  "data/bench_1M.gfa"
  "data/chm13_subset_10w.gfa"
  "data/chm13_subset_100w.gfa"
)

EVICT="$ROOT/tools/bench/drop_file_cache"
SURVEY="$BUILD/bench_survey"
GBWT="$BUILD/bench_gbwt"
TIME=/usr/bin/time

STAMP="$(date +%Y-%m-%d)"
OUT="docs/bench/ingestor_baseline_${STAMP}.md"
CSV="docs/bench/ingestor_baseline_${STAMP}.csv"

# ── pré-requisitos ──────────────────────────────────────────────────────────
[[ -x "$EVICT" ]]  || { echo "compile o evictor: cc -O2 -o $EVICT tools/bench/drop_file_cache.c"; exit 1; }
[[ -x "$SURVEY" ]] || { echo "falta $SURVEY (cmake --build $BUILD --target bench_survey)"; exit 1; }
[[ -x "$GBWT" ]]   || { echo "falta $GBWT (cmake --build $BUILD --target bench_gbwt)"; exit 1; }

COMMIT="$(git rev-parse --short HEAD)"
DIRTY="$(git diff --quiet -- include/ggql/ingest/ingestor.hpp src/ingest/ingestor.cpp src/ingest/survey.cpp && echo clean || echo DIRTY)"
KERNEL="$(uname -r)"
RAMGB="$(awk '/MemTotal/{printf "%.0f", $2/1048576}' /proc/meminfo)"

median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print (NR%2)? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }

{
  echo "# Baseline ingestor — ${STAMP}"
  echo
  echo "- Commit: \`${COMMIT}\` (ingestor: **${DIRTY}**)"
  echo "- Kernel: ${KERNEL} · RAM: ${RAMGB} GiB · Threads: ${THREADS} · Profile: ${PROFILE}"
  echo "- COLD reps: ${REPS} (mediana) · survey iters: ${SURVEY_ITERS}"
  echo "- Evicção de cache: posix_fadvise(DONTNEED) por arquivo (sem root)"
  echo
  echo "## WARM — bench_survey (isola CPU de parsing · baseline H2)"
  echo
  echo "| Arquivo | Tamanho | Scan médio warm (s) | Throughput (MB/s) |"
  echo "|---|---:|---:|---:|"
} > "$OUT"
echo "regime,file,size_bytes,metric,value" > "$CSV"

for f in "${FILES[@]}"; do
  [[ -f "$f" ]] || { echo "  (pulando ausente: $f)"; continue; }
  SZ=$(stat -c %s "$f")
  echo ">> WARM survey: $f"
  # iters>1; descarta iter 0 (cold), média das demais
  mapfile -t lines < <("$SURVEY" "$f" "$THREADS" "$SURVEY_ITERS" 2>/dev/null | grep -E '^iter [1-9]')
  warm_mean=$(printf '%s\n' "${lines[@]}" | sed -E 's/.*: ([0-9.]+)s.*/\1/' | awk '{s+=$1;n++} END{printf "%.4f", s/n}')
  mbps=$(awk -v b="$SZ" -v t="$warm_mean" 'BEGIN{printf "%.1f", (b/1048576)/t}')
  printf "| %s | %s | %s | %s |\n" "$(basename "$f")" "$(numfmt --to=iec "$SZ")" "$warm_mean" "$mbps" >> "$OUT"
  echo "warm,$f,$SZ,scan_mean_s,$warm_mean" >> "$CSV"
  echo "warm,$f,$SZ,throughput_mbps,$mbps" >> "$CSV"
done

{
  echo
  echo "## COLD — bench_gbwt cache-evicted (isola I/O multi-passada · baseline H1)"
  echo
  echo "Mediana de ${REPS} reps. \`fs_in\` = File system inputs (blocos de 512 B"
  echo "lidos do disco, via getrusage ru_inblock). \`fs_in×512 / tamanho\` ≈ nº de"
  echo "leituras do arquivo: ~1.0 = page cache persiste entre passadas (sem re-read)."
  echo
  echo "| Arquivo | Tamanho | Wall total (s) | fs_in (blocos) | fs_in/arquivo | Peak RSS (MB) |"
  echo "|---|---:|---:|---:|---:|---:|"
} >> "$OUT"

for f in "${FILES[@]}"; do
  [[ -f "$f" ]] || continue
  SZ=$(stat -c %s "$f")
  echo ">> COLD ingest: $f"
  walls=(); fsins=(); rsss=()
  for ((r=1; r<=REPS; r++)); do
    rm -rf "${f}.idx"                      # força ingest completo (sem cache hit)
    "$EVICT" "$f" >/dev/null 2>&1 || true  # cache frio
    TLOG=$(mktemp)
    "$TIME" -v "$GBWT" "$f" "$THREADS" --profile "$PROFILE" --quiet \
        >/dev/null 2>"$TLOG.err" || { echo "    rep $r FALHOU"; tail "$TLOG.err"; continue; }
    wall=$(grep -oP 'Elapsed \(wall clock\).*?\K[0-9:.]+$' "$TLOG.err" | awk -F: '{if(NF==3)print $1*3600+$2*60+$3; else if(NF==2)print $1*60+$2; else print $1}')
    fsin=$(grep -oP 'File system inputs: \K[0-9]+' "$TLOG.err")
    rss_kb=$(grep -oP 'Maximum resident set size \(kbytes\): \K[0-9]+' "$TLOG.err")
    walls+=("$wall"); fsins+=("$fsin"); rsss+=("$((rss_kb/1024))")
    rm -f "$TLOG.err"
  done
  [[ ${#walls[@]} -eq 0 ]] && continue
  mw=$(median "${walls[@]}"); mfs=$(median "${fsins[@]}"); mr=$(median "${rsss[@]}")
  ratio=$(awk -v b="$mfs" -v s="$SZ" 'BEGIN{printf "%.2f", (b*512)/s}')
  printf "| %s | %s | %s | %s | %s | %s |\n" \
    "$(basename "$f")" "$(numfmt --to=iec "$SZ")" "$mw" "$mfs" "$ratio" "$mr" >> "$OUT"
  for kv in "wall_s=$mw" "fs_inputs_blocks=$mfs" "fs_in_ratio=$ratio" "peak_rss_mb=$mr"; do
    echo "cold,$f,$SZ,${kv%%=*},${kv##*=}" >> "$CSV"
  done
done

echo >> "$OUT"
echo "_Gerado por \`tools/bench/ingestor_bench.sh\`. Re-rode após os fixes e compare a mesma tabela._" >> "$OUT"
echo
echo "OK — resultados em $OUT e $CSV"
