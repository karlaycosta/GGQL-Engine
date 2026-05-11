#!/bin/bash
# r2_perf_rigorous.sh — R2: performance comparativa GGQL vs vg/awk
# com rigor estatístico (5 warmup + 10 medidas, mediana ± IQR via hyperfine).
#
# Substitui run_sprint8_perf.sh que tinha N=1 medição. Inclui:
#   - 5 runs de warmup (cache OS) descartados
#   - 10 runs medidos, mediana e IQR via hyperfine
#   - taskset -c 0-7 para isolar CPU
#   - --shell=none para minimizar overhead de spawn
#   - --export-json para análise posterior
#
# Saída: tests/validation/r2_results.json + tabela CSV
#
# Pré-requisitos:
#   apt install hyperfine    (já disponível: /usr/bin/hyperfine)
#   data/chm13_subset_100w.gfa + .idx ingerido
#
# Hardware:
#   $(lscpu | grep "Model name" | head -1)

set -eo pipefail
cd "$(dirname "$0")/../.."

GFA=data/chm13_subset_100w.gfa
IDX=$GFA.idx
RUNQ=build/tests/run_queries
GFATOOLS=data/validation/tools/gfatools/gfatools
VG=data/validation/tools/vg

[[ -x "$RUNQ" && -x "$GFATOOLS" && -x "$VG" && -f "$GFA" ]] || {
    echo "FALTA ferramenta ou GFA"; exit 1;
}

export GGQL_MAX_TOTAL_HITS=10000000

OUTDIR=tests/validation/r2_results
mkdir -p "$OUTDIR"

# Garante .idx (cold ingest fora do timing das queries)
if [[ ! -d "$IDX" ]]; then
    echo "→ Ingestão inicial (fora das medidas)..."
    build/bench_gbwt "$GFA" 8 --quiet >/dev/null
fi

# Garante .pg do vg (cold)
if [[ ! -f /tmp/chm13_100w.pg ]]; then
    echo "→ Convertendo GFA → .pg (fora das medidas)..."
    "$VG" convert -g "$GFA" -p > /tmp/chm13_100w.pg 2>/dev/null
fi

# Captura hardware para o report
HW_INFO=$(lscpu | grep -E "Model name|CPU\(s\):" | head -2)
MEM_GB=$(free -h | awk 'NR==2 {print $2}')
KERNEL=$(uname -r)
GCC_VER=$(g++-14 --version | head -1)

cat > "$OUTDIR/hardware.txt" <<EOF
Hardware:
  $HW_INFO
  Memória:  $MEM_GB
  Kernel:   $KERNEL
  Compiler: $GCC_VER
  Data:     $(date -Iseconds)
  GGQL ver: $(git rev-parse --short HEAD)
EOF

echo "═════ R2: perf rigoroso (hyperfine 5w+10m, taskset 0-7) ═════"
cat "$OUTDIR/hardware.txt"
echo

# Helper hyperfine
HF_FLAGS=(
    --warmup 5
    --runs 10
    --shell=none
    --time-unit millisecond
    --style basic
)

# ── Bloco 1: Queries estruturais (warm) ──
echo
echo "── Bloco 1: Queries estruturais ──"
taskset -c 0-7 hyperfine "${HF_FLAGS[@]}" \
    --export-json "$OUTDIR/block1_struct.json" \
    -n "GGQL_FIND_SEGMENT"  "$RUNQ $IDX 'FIND SEGMENT'" \
    -n "GGQL_FIND_EDGE"     "$RUNQ $IDX 'FIND EDGE'" \
    -n "GGQL_FIND_TIP"      "$RUNQ $IDX 'FIND TIP'" \
    -n "GGQL_FIND_REPEAT"   "$RUNQ $IDX 'FIND REPEAT'" \
    -n "GGQL_FIND_PATH"     "$RUNQ $IDX 'FIND PATH'" \
    2>&1 | tail -40

# ── Bloco 2: BUBBLE / vg snarls ──
echo
echo "── Bloco 2: FIND BUBBLE / vg snarls ──"
taskset -c 0-7 hyperfine "${HF_FLAGS[@]}" \
    --export-json "$OUTDIR/block2_bubble.json" \
    -n "GGQL_FIND_BUBBLE"   "$RUNQ $IDX 'FIND BUBBLE'" \
    -n "vg_snarls"          "$VG stats -R /tmp/chm13_100w.pg" \
    2>&1 | tail -20

# ── Bloco 3: SEARCH KMER ──
# Cria scripts awk standalone para evitar problemas de escaping com hyperfine --shell=none.
cat > "$OUTDIR/awk_kmer_AAAAAAAA.sh" <<'EOF'
#!/bin/bash
awk -v k=AAAAAAAA '$1=="S" && index($3,k)>0 {c++} END {print c+0}' "$1"
EOF
cat > "$OUTDIR/awk_kmer_CGATCGATCG.sh" <<'EOF'
#!/bin/bash
awk -v k=CGATCGATCG '$1=="S" && index($3,k)>0 {c++} END {print c+0}' "$1"
EOF
chmod +x "$OUTDIR/awk_kmer_AAAAAAAA.sh" "$OUTDIR/awk_kmer_CGATCGATCG.sh"

echo
echo "── Bloco 3: SEARCH KMER (alta seletividade vs baixa) ──"
taskset -c 0-7 hyperfine "${HF_FLAGS[@]}" \
    --export-json "$OUTDIR/block3_kmer.json" \
    -n "GGQL_KMER_AAAAAAAA"  "$RUNQ $IDX 'SEARCH KMER \"AAAAAAAA\"'" \
    -n "GGQL_KMER_CGATCGATCG" "$RUNQ $IDX 'SEARCH KMER \"CGATCGATCG\"'" \
    -n "awk_kmer_AAAAAAAA"   "$OUTDIR/awk_kmer_AAAAAAAA.sh $GFA" \
    2>&1 | tail -25

# ── Bloco 4: WHERE filters ──
cat > "$OUTDIR/awk_length_gt_1000.sh" <<'EOF'
#!/bin/bash
awk '$1=="S" && length($3)>1000 {c++} END {print c+0}' "$1"
EOF
chmod +x "$OUTDIR/awk_length_gt_1000.sh"

echo
echo "── Bloco 4: WHERE filters ──"
taskset -c 0-7 hyperfine "${HF_FLAGS[@]}" \
    --export-json "$OUTDIR/block4_where.json" \
    -n "GGQL_length_gt_1000" "$RUNQ $IDX 'FIND SEGMENT WHERE length > 1000'" \
    -n "GGQL_name_eq_1"      "$RUNQ $IDX 'FIND SEGMENT WHERE name = \"1\"'" \
    -n "awk_length_gt_1000"  "$OUTDIR/awk_length_gt_1000.sh $GFA" \
    2>&1 | tail -25

# ── Análise final: tabela consolidada ──
echo
echo "═════ R2: tabela final (mediana e IQR) ═════"
python3 - <<'PYEOF'
import json
import os
import statistics

outdir = "tests/validation/r2_results"
files = sorted(f for f in os.listdir(outdir) if f.endswith(".json"))

print(f"{'Operação':<32} {'Mediana':>10} {'IQR':>10} {'Min':>10} {'Max':>10} {'Estável?':>10}")
print("-" * 84)

for f in files:
    with open(os.path.join(outdir, f)) as fh:
        data = json.load(fh)
    for r in data["results"]:
        name = r["command"]
        times_s = r["times"]
        times_ms = [t * 1000 for t in times_s]
        median_ms = statistics.median(times_ms)
        q1, q3 = statistics.quantiles(times_ms, n=4)[0], statistics.quantiles(times_ms, n=4)[2]
        iqr_ms = q3 - q1
        min_ms = min(times_ms)
        max_ms = max(times_ms)
        # Estável se IQR/mediana < 5%
        stable = "✅" if iqr_ms / median_ms < 0.05 else ("⚠" if iqr_ms / median_ms < 0.10 else "❌")
        print(f"{name:<32} {median_ms:>8.1f}ms {iqr_ms:>8.1f}ms {min_ms:>8.1f}ms {max_ms:>8.1f}ms {stable:>10}")

print()
print("Legenda:")
print("  ✅ IQR/mediana < 5%  (medida estável)")
print("  ⚠  IQR/mediana 5-10% (aceitável, variação normal)")
print("  ❌ IQR/mediana > 10% (instável — investigar)")
PYEOF

echo
echo "✅ R2: medições com rigor estatístico em $OUTDIR/"
echo "  - hardware.txt: HW/SW context"
echo "  - block*.json:  raw timings hyperfine"
