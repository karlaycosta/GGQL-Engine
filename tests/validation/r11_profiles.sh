#!/bin/bash
# r11_profiles.sh — R11: validação dos 4 perfis de ingestão.
#
# Auditoria sistemática:
#   1) Cada profile gera os arquivos esperados (e omite os opcionais)
#   2) Queries com componentes habilitados retornam count correto vs GT
#   3) Queries com componentes desabilitados retornam 0 (degradação graciosa)
#   4) Nenhuma combinação produz crash
#
# Matriz Profile × ProfileFlags (from include/ggql/ingest/profile.hpp):
#
#                 │ metadata │ fm_index │ topology │  gbwt   │ snarls │
#   Pangenome    │    ✅    │    ✅    │    ✅    │   ✅    │   ✅   │
#   Metagenomics │    ✅    │    ✅    │    ✅    │   ❌    │   ❌   │
#   Assembly     │    ✅    │    ❌    │    ✅    │   ❌    │   ✅   │
#   Comparative  │    ✅    │    ❌    │    ✅    │   ✅    │   ❌   │
#
# Query × Profile (count esperado em DRB1-3123):
#
#                 │ Pangenome │ Metag │ Assembly │ Comp  │
#   FIND SEGMENT │   5581    │ 5581  │   5581   │ 5581  │  (topology)
#   FIND EDGE    │   7448    │ 7448  │   7448   │ 7448  │  (topology)
#   FIND TIP     │      5    │    5  │      5   │    5  │  (topology)
#   FIND REPEAT  │   1761    │ 1761  │   1761   │ 1761  │  (topology)
#   FIND PATH    │     12    │    0  │      0   │   12  │  (gbwt)
#   FIND BUBBLE  │   1516    │    0  │   1516   │    0  │  (snarls)
#   SEARCH KMER  │    111    │  111  │      0   │    0  │  (fm_index)

set -eo pipefail
cd "$(dirname "$0")/../.."

GFA_SRC=data/validation/tier_b/DRB1-3123.gfa
BENCH=build/bench_gbwt
RUNQ=build/tests/run_queries

[[ -x "$BENCH" && -x "$RUNQ" && -f "$GFA_SRC" ]] || { echo "FALTA tool/gfa"; exit 1; }

OUT=tests/validation/r11_results
mkdir -p "$OUT"

# Mapeia (profile, query) → count esperado em DRB1-3123
declare -A EXPECTED=(
    ["pangenome:FIND SEGMENT"]=5581
    ["pangenome:FIND EDGE"]=7448
    ["pangenome:FIND TIP"]=5
    ["pangenome:FIND REPEAT"]=1761
    ["pangenome:FIND PATH"]=12
    ["pangenome:FIND BUBBLE"]=1516
    ["pangenome:SEARCH KMER"]=111
    ["metagenomics:FIND SEGMENT"]=5581
    ["metagenomics:FIND EDGE"]=7448
    ["metagenomics:FIND TIP"]=5
    ["metagenomics:FIND REPEAT"]=1761
    ["metagenomics:FIND PATH"]=0
    ["metagenomics:FIND BUBBLE"]=0
    ["metagenomics:SEARCH KMER"]=111
    ["assembly:FIND SEGMENT"]=5581
    ["assembly:FIND EDGE"]=7448
    ["assembly:FIND TIP"]=5
    ["assembly:FIND REPEAT"]=1761
    ["assembly:FIND PATH"]=0
    ["assembly:FIND BUBBLE"]=1516
    ["assembly:SEARCH KMER"]=0
    ["comparative:FIND SEGMENT"]=5581
    ["comparative:FIND EDGE"]=7448
    ["comparative:FIND TIP"]=5
    ["comparative:FIND REPEAT"]=1761
    ["comparative:FIND PATH"]=12
    ["comparative:FIND BUBBLE"]=0
    ["comparative:SEARCH KMER"]=0
)

# Arquivos esperados por profile (presença ✅ / ausência ❌)
# Formato: "presente1,presente2:ausente1,ausente2"
declare -A FILES=(
    ["pangenome"]="fm_index.bin,path_stores.bin,snarls.bin,path_stats.bin,remap.bin:"
    ["metagenomics"]="fm_index.bin:path_stores.bin,snarls.bin,path_stats.bin,remap.bin"
    ["assembly"]="snarls.bin:fm_index.bin,path_stores.bin,path_stats.bin,remap.bin"
    ["comparative"]="path_stores.bin,path_stats.bin,remap.bin:fm_index.bin,snarls.bin"
)

total_pass=0
total_fail=0

echo "═══ R11: validação dos 4 perfis em DRB1-3123 ═══"

for profile in pangenome metagenomics assembly comparative; do
    echo
    echo "── Profile: $profile ──"

    # Ingere com cache limpo
    GFA=/tmp/r11_${profile}.gfa
    IDX=$GFA.idx
    rm -rf "$IDX"
    cp "$GFA_SRC" "$GFA"

    if ! "$BENCH" "$GFA" 4 --profile "$profile" --quiet > "$OUT/${profile}_ingest.log" 2>&1; then
        printf '  ❌ INGEST FALHOU (exit=%s)\n' "$?"
        total_fail=$((total_fail+1))
        continue
    fi

    # ── Check 1: arquivos esperados estão presentes ────────────────────────
    IFS=':' read -r present absent <<< "${FILES[$profile]}"
    file_check_pass=0
    file_check_fail=0
    IFS=',' read -ra present_arr <<< "$present"
    for f in "${present_arr[@]}"; do
        [[ -z "$f" ]] && continue
        if [[ -e "$IDX/$f" ]]; then
            file_check_pass=$((file_check_pass+1))
        else
            printf '  ❌ FILE ausente quando esperado: %s\n' "$f"
            file_check_fail=$((file_check_fail+1))
        fi
    done
    IFS=',' read -ra absent_arr <<< "$absent"
    for f in "${absent_arr[@]}"; do
        [[ -z "$f" ]] && continue
        if [[ ! -e "$IDX/$f" ]]; then
            file_check_pass=$((file_check_pass+1))
        else
            printf '  ❌ FILE presente quando NÃO esperado: %s\n' "$f"
            file_check_fail=$((file_check_fail+1))
        fi
    done
    if [[ $file_check_fail -eq 0 ]]; then
        printf '  ✅ Arquivos: %s checks OK\n' "$file_check_pass"
        total_pass=$((total_pass+1))
    else
        printf '  ❌ Arquivos: %s checks falharam\n' "$file_check_fail"
        total_fail=$((total_fail+1))
    fi

    # ── Check 2: queries retornam counts esperados ─────────────────────────
    for q in "FIND SEGMENT" "FIND EDGE" "FIND TIP" "FIND REPEAT" "FIND PATH" "FIND BUBBLE"; do
        expected="${EXPECTED[${profile}:${q}]}"
        got=$("$RUNQ" "$IDX" "$q" 2>/dev/null | tail -1)
        if [[ "$got" == "$expected" ]]; then
            printf '  ✅ %-15s = %-5s\n' "$q" "$got"
            total_pass=$((total_pass+1))
        else
            printf '  ❌ %-15s = %-5s (esperado %s)\n' "$q" "$got" "$expected"
            total_fail=$((total_fail+1))
        fi
    done

    # SEARCH KMER tratado separado (sintaxe diferente)
    expected="${EXPECTED[${profile}:SEARCH KMER]}"
    got=$("$RUNQ" "$IDX" 'SEARCH KMER "AAAA"' 2>/dev/null | tail -1)
    if [[ "$got" == "$expected" ]]; then
        printf '  ✅ %-15s = %-5s\n' "SEARCH KMER" "$got"
        total_pass=$((total_pass+1))
    else
        printf '  ❌ %-15s = %-5s (esperado %s)\n' "SEARCH KMER" "$got" "$expected"
        total_fail=$((total_fail+1))
    fi
done

# ── Análise de espaço em disco ────────────────────────────────────────────────
echo
echo "── Tamanho do .idx por profile ──"
for profile in pangenome metagenomics assembly comparative; do
    if [[ -d /tmp/r11_${profile}.gfa.idx ]]; then
        sz=$(du -sh /tmp/r11_${profile}.gfa.idx | awk '{print $1}')
        n=$(ls /tmp/r11_${profile}.gfa.idx/ | wc -l)
        printf '  %-15s %6s  (%s arquivos)\n' "$profile" "$sz" "$n"
    fi
done

echo
echo "═══ Resumo R11 ═══"
echo "  ✅ Pass: $total_pass"
echo "  ❌ Fail: $total_fail"
if [[ $total_fail -eq 0 ]]; then
    echo "✅ R11: TODOS os 4 perfis validados (arquivos + queries + degradação)"
    exit 0
else
    echo "❌ R11: $total_fail divergências"
    exit 1
fi
