#!/bin/bash
# run_sprint4.sh — Sprint 4: filtros WHERE com cobertura ampliada.
#
# Cobre 4 lacunas críticas identificadas em queries_validation_plan.md §6 (bisect):
#
#   L-V1: WHERE depth em Tier A (bench_depth.gfa, GT float32 via Python)
#   L-V3: WHERE contains_kmer em Tier B (DRB1-3123, GT via awk grep)
#   L-V4: FIND EDGE WHERE filtros em Tier B (GT via awk sobre L-lines)
#   L-V5: WHERE name em Tier B (DRB1) e Tier C (chm13_100w)

set -eo pipefail
cd "$(dirname "$0")/../.."

RUNQ=build/tests/run_queries
[[ -x "$RUNQ" ]] || { echo "FALTA: $RUNQ"; exit 1; }

pass=0
fail=0
check() {
    local label=$1 ggql=$2 gt=$3 ext=${4:-}
    if [[ "$ggql" == "$gt" ]]; then
        printf '  %-58s %10s == %-10s ✅ %s\n' "$label" "$ggql" "$gt" "$ext"
        pass=$((pass+1))
    else
        printf '  %-58s %10s != %-10s ❌ Δ=%-5s %s\n' "$label" "$ggql" "$gt" "$((ggql-gt))" "$ext"
        fail=$((fail+1))
    fi
}

# ════════════════════════════════════════════════════════════════════════════
# L-V1: WHERE depth (Tier A — bench_depth.gfa)
# ════════════════════════════════════════════════════════════════════════════
echo "═══ L-V1: WHERE depth em bench_depth.gfa (Tier A) ═══"

GFA=data/bench_depth.gfa
IDX=$GFA.idx
[[ -d "$IDX" ]] || build/bench_gbwt "$GFA" 4 --quiet >/dev/null 2>&1

# GT em float32 (precisão usada pelo GGQL — float32 ≠ double)
gt_depth() {
    python3 -c "
import struct, re
def f32(d): return struct.unpack('f', struct.pack('f', d))[0]
op, val = '$1', $2
c = 0
with open('$GFA') as fh:
    for line in fh:
        if not line.startswith('S\t'): continue
        m = re.search(r'dp:f:(\S+)', line)
        if not m: continue
        d = f32(float(m.group(1)))
        v = f32(val)
        if op == '>=' and d >= v: c += 1
        elif op == '<'  and d <  v: c += 1
        elif op == '>'  and d >  v: c += 1
        elif op == '<=' and d <= v: c += 1
        elif op == '='  and d == v: c += 1
print(c)
"
}

for case in '>= 2.0' '< 1.0' '> 5.0' '<= 0.5'; do
    op=${case% *}; val=${case#* }
    ggql=$("$RUNQ" "$IDX" "FIND SEGMENT WHERE depth $op $val" 2>/dev/null | tail -1)
    gt=$(gt_depth "$op" "$val")
    check "depth $op $val" "$ggql" "$gt" "(float32)"
done

# L-V8 RESOLVIDO (commit pós-Sprint 4): eval_condition agora compara
# depth em float-precision (em vez de double-promoted). Range fechado
# e igualdade direta funcionam exatamente.
ggql=$("$RUNQ" "$IDX" 'FIND SEGMENT WHERE depth >= 1.95 AND depth <= 1.97' 2>/dev/null | tail -1)
gt=$(python3 -c "
import struct, re
def f32(d): return struct.unpack('f', struct.pack('f', d))[0]
lo, hi = f32(1.95), f32(1.97); c = 0
with open('$GFA') as fh:
    for line in fh:
        if line.startswith('S\t'):
            m = re.search(r'dp:f:(\S+)', line)
            if m:
                d = f32(float(m.group(1)))
                if lo <= d <= hi: c += 1
print(c)
")
check "depth in [1.95, 1.97] (range)" "$ggql" "$gt" "(float32 strict)"

# Igualdade direta (antes do fix L-V8 retornava 0)
ggql=$("$RUNQ" "$IDX" 'FIND SEGMENT WHERE depth = 1.96' 2>/dev/null | tail -1)
gt=$(python3 -c "
import struct, re
def f32(d): return struct.unpack('f', struct.pack('f', d))[0]
v = f32(1.96); c = 0
with open('$GFA') as fh:
    for line in fh:
        if line.startswith('S\t'):
            m = re.search(r'dp:f:(\S+)', line)
            if m and f32(float(m.group(1))) == v: c += 1
print(c)
")
check "depth = 1.96 (igualdade exata, regression guard L-V8)" "$ggql" "$gt" "(L-V8 fixed)"

# ════════════════════════════════════════════════════════════════════════════
# L-V3, L-V4, L-V5: filtros em DRB1-3123 (Tier B)
# ════════════════════════════════════════════════════════════════════════════
echo
echo "═══ L-V3/L-V4/L-V5: filtros em DRB1-3123 (Tier B) ═══"

GFA_B=data/validation/tier_b/DRB1-3123.gfa
IDX_B=$GFA_B.idx
if [[ ! -d "$IDX_B" ]]; then
    echo "  FALTA $IDX_B — rodar Sprint 2 primeiro"; exit 1
fi

# L-V3: contains_kmer (GT: segments cujo S-line contém o k-mer)
echo "  --- L-V3: contains_kmer ---"
for kmer in 'ACGT' 'TTTT' 'CCCCCC' 'XYZZ'; do
    ggql=$("$RUNQ" "$IDX_B" "FIND SEGMENT WHERE contains_kmer(\"$kmer\")" 2>/dev/null | tail -1)
    gt=$(awk -v k="$kmer" '$1=="S" && index($3,k)>0 {c++} END {print c+0}' "$GFA_B")
    check "contains_kmer(\"$kmer\")" "$ggql" "$gt" "(awk index)"
done

# L-V4: FIND EDGE WHERE
# Filtros válidos: to_orient (atributo direto, sem mapping). from_id requer
# mapping nome→internal_id (MPHF remap) — não é validável trivialmente por
# awk em segment names. Cobertura de from_id está em test_query_planner_idx
# (regression guard) que compara FIND EDGE conteúdo vs topo.out_neighbors.
echo "  --- L-V4: FIND EDGE WHERE to_orient ---"
ggql=$("$RUNQ" "$IDX_B" 'FIND EDGE WHERE to_orient = "+"' 2>/dev/null | tail -1)
gt=$(awk '$1=="L" && $5=="+" {c++} END {print c+0}' "$GFA_B")
check 'EDGE WHERE to_orient = "+"' "$ggql" "$gt" "(awk col 5)"

ggql=$("$RUNQ" "$IDX_B" 'FIND EDGE WHERE to_orient = "-"' 2>/dev/null | tail -1)
gt=$(awk '$1=="L" && $5=="-" {c++} END {print c+0}' "$GFA_B")
check 'EDGE WHERE to_orient = "-"' "$ggql" "$gt" "(awk col 5)"

# L-V5: WHERE name = "..."
echo "  --- L-V5: WHERE name ---"
first_seg=$(awk '$1=="S" {print $2; exit}' "$GFA_B")
ggql=$("$RUNQ" "$IDX_B" "FIND SEGMENT WHERE name = \"$first_seg\"" 2>/dev/null | tail -1)
gt=$(awk -v n="$first_seg" '$1=="S" && $2==n {c++} END {print c+0}' "$GFA_B")
check "SEGMENT WHERE name = \"$first_seg\"" "$ggql" "$gt" "(awk col 2)"

ggql=$("$RUNQ" "$IDX_B" 'FIND SEGMENT WHERE name = "_inexistente_"' 2>/dev/null | tail -1)
check 'SEGMENT WHERE name = "_inexistente_"' "$ggql" "0" "(miss)"

# ════════════════════════════════════════════════════════════════════════════
# L-V5 escala: WHERE name em Tier C (chm13_100w)
# ════════════════════════════════════════════════════════════════════════════
echo
echo "═══ L-V5 (escala): WHERE name em chm13_100w (Tier C) ═══"

GFA_C=data/chm13_subset_100w.gfa
IDX_C=$GFA_C.idx
if [[ -d "$IDX_C" ]]; then
    first_seg_c=$(awk '$1=="S" {print $2; exit}' "$GFA_C")
    ggql=$(GGQL_MAX_TOTAL_HITS=10000000 "$RUNQ" "$IDX_C" "FIND SEGMENT WHERE name = \"$first_seg_c\"" 2>/dev/null | tail -1)
    check "SEGMENT WHERE name = \"$first_seg_c\"" "$ggql" "1" "(exact match)"

    ggql=$(GGQL_MAX_TOTAL_HITS=10000000 "$RUNQ" "$IDX_C" 'FIND SEGMENT WHERE name = "ID_INEXISTENTE_XYZ"' 2>/dev/null | tail -1)
    check 'SEGMENT WHERE name = "ID_INEXISTENTE_XYZ"' "$ggql" "0" "(miss em 4.3M)"
else
    echo "  SKIP (sem chm13_100w.gfa.idx)"
fi

# ════════════════════════════════════════════════════════════════════════════
echo
total=$((pass + fail))
if [[ $fail -eq 0 ]]; then
    echo "✅ Sprint 4: $pass/$total casos passaram"
    exit 0
else
    echo "❌ Sprint 4: $fail/$total divergências (ver acima)"
    exit 1
fi
