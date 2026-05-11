# R9-R15 — Roadmap para itens de alto impacto / gold standard

## Status: PENDENTES — requerem sessões dedicadas

Os itens R1-R8 foram mitigados em 1 sessão (~6h) gerando rigor científico mínimo
para peer review. Os itens R9-R15 envolvem trabalho substancial (dias a semanas),
e foram **explicitamente fora do escopo desta campanha**. Este documento mapeia
o que cada um exige.

---

## R9 — Tier D: dataset não-humano

**Esforço estimado**: 1-2 dias

**Por quê**: garantir generalização para bactéria/planta/fungo. Atualmente só
testamos pangenoma humano (HLA, chm13).

**Plano**:

1. Download de dataset E. coli PGGB:
   - Fonte: https://github.com/pangenome/pggb
   - Exemplo: `ecoli_simulated_uniform_distribution_4haplotypes.gfa.gz` (~50 MB)
   - Ou yeast Sce-pan: https://github.com/AndreaGuarracino/scerevisiae8.pangenome
2. Pipeline padrão (igual Sprint 2):
   - Ingest via bench_gbwt
   - Cross-check `vg snarls` + `gfatools stat`
   - Validar 6 queries canônicas: SEGMENT, EDGE, TIP, REPEAT, BUBBLE, PATH
3. Comparar topologia vs humano (bactéria tem mais cycles, plant tem mais paths)

**Critério de sucesso**: Δ=0 em 5/6 counts (BUBBLE pode ter Δ≤5 como em HLA).

**Arquivo a criar**: `tests/validation/run_tier_d.sh`

---

## R10 — HPRC full (88M nós) cross-check Liao 2023

**Esforço estimado**: 1 dia (+ ~40 GB disk + 32 GB RAM)

**Por quê**: HPRC year-1 pangenome é o benchmark de facto. Liao et al. 2023
(Nature 617:312-324) reporta ~85 mil snarls totais. Validar contra esse número
fecha o gap de "Tier C com N=1 dataset".

**Plano**:

1. Download: `https://github.com/human-pangenomics/HPP_Year1_Assemblies`
   - chm13 v2.0 + 47 haplotypes (88M nodes total)
   - Sirén tools / minigraph-cactus output GFA
2. Ingest com `bench_gbwt graph.gfa 16` (16 threads, esperado ~1h)
3. `FIND BUBBLE` → comparar contra ~85k de Liao 2023
4. Performance bench: ingest + queries vs vg integrated (esperado >>1h vg)
5. Documentar HW: probably ARM/x64, 32+ GB RAM

**Critério de sucesso**: |GGQL_bubbles - 85000| / 85000 < 10%

**Arquivo a criar**: `tests/validation/run_tier_c_hprc_full.sh`

---

## R11 — Profile coverage (metagenomics, assembly, comparative)

**Esforço estimado**: 2-3 dias

**Por quê**: GGQL declara 4 perfis (`pangenome`, `metagenomics`, `assembly`,
`comparative`) em `include/ggql/ingest/ingest_profile.hpp` mas só validamos
o perfil `pangenome` em todos os sprints.

**Plano**:

1. **metagenomics**: dataset com >1k species
   - Fonte: ATCC MSA (mock community) ou CAMI-2 challenge data
   - Workflow: usar pggb com `-X meta` flag
2. **assembly**: WGS contigs
   - Fonte: Bandage example data ou GFA do hifiasm output
3. **comparative**: synteny graph multi-species
   - Fonte: cactus output (alignment graph mammal/primate)

Para cada profile:
   - Ingest com `--profile X`
   - Verificar que metadata parsing extrai campos profile-specific
   - 3-5 queries representativas do uso típico

**Critério de sucesso**: para cada profile, todas as queries documentadas no
respectivo `*_profile.hpp` executam sem erro.

**Arquivos a criar**: `tests/validation/run_profile_{metag,assembly,comp}.sh`

---

## R12 — ✅ CI público + Docker

Concluído. Ver:
  - `Dockerfile` (multi-stage build + validation)
  - `.github/workflows/ci.yml` (build, validation, coverage jobs)

---

## R13 — Mutation testing

**Esforço estimado**: 1 semana

**Por quê**: coverage line não basta — branches "cobertas" podem não exercitar
o comportamento. Mutation testing força mutações no código (NaN ↔ 0,
`<` ↔ `<=`, etc.) e verifica que testes detectam.

**Plano**:

1. Instalar `mutate-cpp` (Python tool) ou `mull` (LLVM-based)
2. Configurar: apenas mutar `src/query/`, `src/storage/` (não third_party)
3. Rodar mutation campaign em build-coverage
4. Identificar mutações que sobrevivem (testes não detectam)
5. Adicionar testes para mutations não-cobertos

**Critério de sucesso**: mutation score > 80% (mutações detectadas / total).

**Arquivos a criar**:
  - `tests/validation/r13_mutate_config.yml`
  - `tests/validation/r13_mutate_run.sh`

**Observação**: alguns mutants são equivalentes (e.g., `a + 0 → a` sem efeito);
estes contam como FN mas não devem ser perseguidos.

---

## R14 — Cross-compiler + cross-platform

**Esforço estimado**: 1 semana

**Por quê**: tudo testado em Linux x86_64 / gcc-14 / glibc / Ubuntu. ARM (Apple
Silicon, AWS Graviton), macOS, clang, MSVC, musl libc não estão validados.
CSR layout, snarls.bin, .idx files assumem little-endian sem documentar.

**Plano**:

1. **Cross-compiler**: clang-17 no Linux x64
   - Adicionar config CMake para clang
   - Rodar ctest + validation suite
   - Fix unportable: `__builtin_*`, `[[likely]]`, GCC pragmas
2. **Cross-platform ARM**: Docker buildx multi-arch (linux/arm64)
   - Adicionar emulação via qemu-user-static
   - Rodar ctest dentro do container ARM
3. **macOS**: GitHub Actions runner macos-14 (M2)
   - apt-get → brew install (gcc, fmt, jemalloc)
   - Lidar com `libomp` no Homebrew (não-default)

**Critério de sucesso**: ctest 14/14 + validation suite básica passa em:
  - Linux x64 / gcc-14 (baseline) ✅
  - Linux x64 / clang-17
  - Linux ARM64 / gcc-14
  - macOS arm64 / clang+brew-gcc

**Arquivos a criar**:
  - `.github/workflows/ci-cross-compiler.yml`
  - `.github/workflows/ci-arm64.yml`
  - `.github/workflows/ci-macos.yml`

---

## R15 — Property-based testing (rapidcheck)

**Esforço estimado**: 1 semana

**Por quê**: testes atuais usam exemplos fixos. Property-based gera GFAs
aleatórios e verifica invariantes (e.g., `count(LIMIT k) ≤ count(LIMIT 2k)`,
`SEGMENT.count = SUM_over_bins(count_per_bin)`).

**Plano**:

1. Adicionar rapidcheck como third_party (já existe `git submodule`)
2. Definir generators: `RandomGfa(n_nodes, n_edges, n_paths)` produz GFA válido
3. Definir 5-10 propriedades testáveis:
   - Idempotência: 2x ingest produz mesmo .idx (R1 generalizado)
   - Monotonia: LIMIT k → result_size ≤ k
   - Cobertura: union(OFFSET 0, N, 2N, ...) == full result (R4 generalizado)
   - Comutatividade: `WHERE A AND B` ≡ `WHERE B AND A`
   - Associatividade: `(A AND B) AND C` ≡ `A AND (B AND C)`
   - Distributividade: `A AND (B OR C)` ≡ `(A AND B) OR (A AND C)`
4. Rodar 1000+ iterações; shrinking automático em falha

**Critério de sucesso**: 0 contraexemplos encontrados em 10000 iters
para cada propriedade.

**Arquivos a criar**:
  - `tests/test_properties.cpp` (rapidcheck binding)
  - `tests/validation/r15_property_run.sh`

---

## Resumo do esforço total restante

| Item | Esforço | Crítico? | Bloqueador? |
|---|---|:-:|:-:|
| R9 (Tier D) | 1-2 dias | ⭐ | Não |
| R10 (HPRC full) | 1 dia | ⭐⭐ | Não (já temos chm13_100w) |
| R11 (profiles) | 2-3 dias | ⭐ | Não |
| R13 (mutation) | 1 semana | ⭐ | Não |
| R14 (cross-platform) | 1 semana | ⭐⭐ | Não (target Linux apenas) |
| R15 (property-based) | 1 semana | ⭐⭐⭐ | Não |
| **TOTAL** | **~4 semanas** | | |

Para publicação científica:
  - **Aceitável**: R1-R8 + R12 (já feito) → submeter
  - **Forte**: + R9 + R10 (3 dias adicionais)
  - **Top-tier**: + R11 + R15 (1 mês adicional)
  - **Gold standard**: + R13 + R14 (1.5 meses adicionais)
