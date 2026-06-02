<div align="center">

# GGQL Engine

**Motor de consulta declarativa para grafos genômicos — C++23 · escala HPRC**

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg?logo=cplusplus)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake)](CMakeLists.txt)
[![GCC](https://img.shields.io/badge/GCC-14%2B-A42E2B?logo=gnu)](https://gcc.gnu.org)
[![Plataforma](https://img.shields.io/badge/plataforma-Linux-lightgrey?logo=linux)](https://kernel.org)
[![Versão](https://img.shields.io/badge/vers%C3%A3o-0.1.0%20%C2%B7%20ABI%201-blue)](docs/plano_versionamento_engine.md)
[![FFI](https://img.shields.io/badge/FFI-43%20s%C3%ADmbolos-success)](docs/ffi_flutter_python_integration.md)
[![Tests](https://img.shields.io/badge/CTest-18%20suites-blue)](tests/CMakeLists.txt)

Um motor de consulta declarativa para grafos pangenômicos no **formato GFA**,
projetado para genomas de **85 milhões de nós** (HPRC / CHM13). Expõe a
**GGQL** — uma linguagem inspirada em SQL com operadores nativos para grafos —
através de uma REPL interativa e bindings C/Dart.

</div>

---

```sql
ggql> FIND TIP    WHERE in_degree = 0           RETURN id;
ggql> FIND REPEAT WHERE out_degree >= 3         RETURN id, out_degree;
ggql> FIND BUBBLE WHERE branch_count >= 2       RETURN source, sink, branch_count;
ggql> FIND SEGMENT WHERE length > 50000         RETURN id, name, length;
ggql> SEARCH KMER "ACGTACGT"                    RETURN id, position;
```

---

## Sumário

- [O que é a GGQL Engine?](#o-que-é-a-ggql-engine)
- [Casos de uso e perfis de ingestão](#casos-de-uso-e-perfis-de-ingestão)
- [Arquitetura](#arquitetura)
- [Linguagem GGQL](#linguagem-ggql)
- [Performance](#performance)
- [Requisitos](#requisitos)
- [Build](#build)
- [Uso](#uso)
- [Estrutura do projeto](#estrutura-do-projeto)
- [Roadmap](#roadmap)
- [Licença](#licença)

---

## O que é a GGQL Engine?

A bioinformática moderna abandonou a representação de genomas como sequências
lineares: hoje, **grafos pangenômicos** capturam variação populacional de
forma muito mais rica. Mas consultar esses grafos exige ferramentas
especializadas — varrer arquivos GFA com scripts Python ou awk não escala
para os 85 milhões de nós do **HPRC** (Human Pangenome Reference Consortium).

A **GGQL Engine** resolve esse gap. Você escreve consultas declarativas
inspiradas em SQL e o motor planeja e executa contra estruturas de dados
sucintas mantidas em memória:

```sql
FIND BUBBLE WHERE branch_count >= 2 AND crosses(HG002#chr1) RETURN source, sink;
```

A consulta acima identifica todas as bolhas (snarls) com pelo menos 2 ramos
que atravessam um haplótipo específico — uma operação que combinaria
pelo menos três ferramentas distintas em um pipeline tradicional.

### O que torna o motor diferente

- **Linguagem declarativa, não imperativa.** Você descreve *o quê* (`FIND PATH
  WHERE crosses(...)`), o motor decide *como* — escolhendo o índice
  apropriado (CSR, gBWT, FM-Index) e o plano de execução.
- **Escala para genomas reais.** Pipeline de ingestão paralelo (mmap +
  OpenMP + MPHF) processa um GFA de 4 milhões de nós em poucos segundos
  e o HPRC inteiro (85 M nós, 118 M arestas, 28 k haplótipos) em
  ~17,65 minutos com perfil `pangenome` completo (cold ingest, 8 threads).
- **Perfis de ingestão por caso de uso.** Metagenômica, pangenoma, grafo de
  montagem e genômica comparativa têm necessidades diferentes — você escolhe
  apenas as estruturas que vai consultar e economiza RAM/tempo.
- **Estruturas sucintas state-of-the-art.** CSR + Elias-Fano para topologia,
  r-index gBWT para haplótipos, FM-Index BWT para busca de k-mers — todos
  com construção paralela e custo de memória proporcional à informação real.
- **Cache inteligente em disco.** A primeira ingestão grava `.idx/`; chamadas
  subsequentes carregam em milissegundos com validação por hash XXH64 +
  mtime/size.
- **Integração multi-linguagem madura.** Além da REPL, expõe **43 símbolos
  C ABI** para bindings em Dart/Flutter, Python (via `ctypes`), Rust, etc.
  API endurecida no Round 4 (2026-05-22, 9 commits em 7 fases): opaque
  struct typedefs, `struct_size` para forward-compat, cursor stateful
  (true O(N) streaming via `std::generator` C++23), `ingestor_cancel`
  cooperativo, copy-out de erros, `event_callback` thread-local. Round 5
  (2026-05-29) adicionou pré-voo de ingestão (`ggql_survey_gfa` +
  `ggql_estimate_ingest`, sugestão de perfil) e subgrafo induzido para o
  GraphView (`ggql_engine_subgraph_from_result`) — tudo documentado em
  [`docs/ffi_flutter_python_integration.md`](docs/ffi_flutter_python_integration.md).
- **Versionamento legível em build e runtime.** SemVer (`0.1.0`) vinda do
  CMake + identidade de build (git hash + flag dirty + data) gerada em
  build-time, e um contrato de ABI explícito (`GGQL_FFI_ABI_VERSION`). O
  símbolo handle-free `ggql_ffi_version` deixa o consumer fazer um **gate de
  compatibilidade no boot** (ABI divergente vira erro acionável, não UB), e
  `ggql_repl --version` + `tools/deploy_ffi_to_ui.sh` garantem que a `.so`
  copiada é a esperada (sha256 verificado). Ver
  [`docs/plano_versionamento_engine.md`](docs/plano_versionamento_engine.md).
- **Allocator otimizado por target.** Binários standalone usam jemalloc
  (perf máxima); `libggql_ffi.so` usa mimalloc (dlopen-safe, sem
  `LD_PRELOAD` para Flutter/Python). Configurável via
  `-DGGQL_ALLOCATOR=jemalloc-system|jemalloc|mimalloc|glibc` e
  `-DGGQL_FFI_ALLOCATOR=mimalloc|jemalloc-system|glibc`.

---

## Casos de uso e perfis de ingestão

Diferentes domínios da bioinformática constroem grafos com características
muito diferentes. Forçar todos a pagar o custo das mesmas estruturas é
desperdício. A GGQL Engine define **4 perfis** que selecionam apenas as
estruturas relevantes:

| Perfil | Caso de uso típico | Estruturas | Queries habilitadas |
|--------|-------------------|------------|---------------------|
| **`pangenome`** *(default)* | HPRC, minigraph-cactus, pggb | Tudo: MPHF, Metadata, FM-Index, CSR, gBWT, Snarls | `FIND` em todas as entidades + `SEARCH KMER` |
| **`metagenomics`** | MetaFlye, MEGAHIT, metaSPAdes | MPHF, Metadata, FM-Index, CSR | `FIND SEGMENT/EDGE/TIP/REPEAT`, `SEARCH KMER` |
| **`assembly`** | hifiasm, verkko, Flye | MPHF, Metadata, CSR, Snarls | `FIND SEGMENT/EDGE/TIP/REPEAT/BUBBLE` |
| **`comparative`** | minigraph multi-espécies | MPHF, Metadata, CSR, gBWT | `FIND SEGMENT/EDGE/TIP/REPEAT/PATH` |

Documentação completa: [`docs/ingest_profiles.md`](docs/ingest_profiles.md).

```bash
# Ingestão metagenômica — sem gBWT, sem snarls (economiza ~5 GB de RAM no HPRC)
./build/bench_gbwt assembly.gfa 8 0 --profile metagenomics

# REPL com perfil de assembly (com snarls para QC, sem gBWT)
./build/ggql_repl --gfa hifiasm.gfa --profile assembly --threads 8
```

Subir um perfil entre execuções (ex: começou com `metagenomics` e agora
quer `pangenome`) regenera o cache automaticamente — a validação detecta
estruturas ausentes via verificação de arquivos no `.idx/`.

---

## Arquitetura

A GGQL Engine é organizada em **bibliotecas estáticas independentes**, com
recompilação incremental: alterar a query layer não reconstrói o ingest.

```
┌─────────────────────────────────────────────────────────────────────┐
│  REPL  ggql>                  (src/repl/ggql_repl.cpp)              │
├─────────────────────────────────────────────────────────────────────┤
│  ggql_query                                                         │
│    QueryPlanner · Gramática GGQL (PEGTL) · AST (std::variant C++23) │
├─────────────────────────────────────────────────────────────────────┤
│  ggql_ingest                                                        │
│    GfaIngestorMmapParallel · MetadataStore (SoA binário)            │
│    BBHash MPHF · LSD Radix Sort · escritor gBWT streaming           │
├─────────────────────────────────────────────────────────────────────┤
│  ggql_storage                                                       │
│    Camada 1 — CSR + Elias-Fano   (topologia, algoritmos de grafo)   │
│    Camada 2 — gBWT               (índice de paths/haplótipos)       │
│    Camada 3 — FM-Index           (busca de k-mers)                  │
│    Camada 4 — Snarls             (3-ECC + cactus para bolhas)       │
├─────────────────────────────────────────────────────────────────────┤
│  ggql_core                                                          │
│    Console ANSI · ProgressTracker thread-safe (CAS) · utilitários   │
└─────────────────────────────────────────────────────────────────────┘
        │
        ▼  third-party (submódulos git)
   fmt · pegtl · sdsl-lite · gbwt · libsais
```

### Estruturas de armazenamento em detalhe

| Camada | Estrutura | Propósito |
|--------|-----------|-----------|
| **CSR + Elias-Fano** | `sdsl::sd_vector<>` (offsets) + `uint32_t[]` (arestas) | Adjacência bidirecional; grau e vizinhos em O(1) |
| **gBWT** | `gbwt::GBWT` — r-index (Sirén 2020) | Índice de haplótipos; espaço O(r) onde r = runs do BWT |
| **FM-Index** | BWT + OccBlock AoS + bitmap-sampled locate | Busca de k-mers nas sequências dos segmentos |
| **MetadataStore** | Struct-of-Arrays binário + índice local | Atributos de segmento: length, depth, read_count, name, sequence |
| **Snarls** | 3-ECC + grafo cactus + BFS (~24 B/nó) | Detecção de superbolhas (Onodera et al. 2013) |

### Pipeline de ingestão

A ingestão é organizada em fases que podem ser independentemente
habilitadas conforme o perfil:

1. **Pass 1 — tokenização + MPHF.** mmap do arquivo, scan paralelo dos
   segmentos com OpenMP, construção da BBHash MPHF (lookup nome→ID em ~2.5
   bits/chave).
2. **Pass 2 — links + paths.** Parse paralelo das linhas L (links) e W/P
   (haplótipos), com escrita streaming dos paths para arquivos temporários.
3. **CSR + Elias-Fano.** LSD radix sort dos links em 4 passadas × 8 bits,
   construção da topologia bidirecional sucinta.
4. **FM-Index.** SA via libsais64 (paralelo), backward search com tabela Occ
   por blocos.
5. **gBWT.** Construção paralela com remap de IDs por componente (reduz
   sigma de ~85M para o tamanho do componente, viabilizando paralelização).
6. **Snarls.** Detecção de superbolhas via 3-edge-connectivity + grafo
   cactus + BFS multi-branch.
7. **Persistência.** Cache em disco (`.idx/`) com hash XXH64 para validação.

---

## Linguagem GGQL

### Sintaxe

```
query       ::= find_stmt | search_stmt

find_stmt   ::= FIND <entidade> [WHERE <cond> [AND|OR <cond>]...] [RETURN <campo>, ...]
search_stmt ::= SEARCH KMER "<seq>"                                [RETURN <campo>, ...]

entidade    ::= SEGMENT | EDGE | PATH | BUBBLE | TIP | REPEAT
cond        ::= <campo> <op> <valor>
              | crosses(<nome_path>)
              | contains_kmer("<seq>")
op          ::= =  !=  <  >  <=  >=
valor       ::= <inteiro> | "<string>"
```

### Campos por entidade

| Entidade | Campos disponíveis | Backend |
|----------|-------------------|---------|
| `TIP` | `id`, `in_degree`, `out_degree` | CSR |
| `REPEAT` | `id`, `in_degree`, `out_degree` | CSR |
| `BUBBLE` | `source`, `sink`, `branch_count` | CSR + Snarls |
| `SEGMENT` | `id`, `name`, `length`, `read_count`, `depth` | MetadataStore |
| `EDGE` | `from`, `to`, `from_orient`, `to_orient` | CSR |
| `PATH` | `id`, `name`, `node_count`, `bp_length` | gBWT |

### Exemplos

```sql
-- Topologia: identificar pontas e regiões repetitivas
FIND TIP    WHERE in_degree = 0      RETURN id;
FIND REPEAT WHERE out_degree >= 3    RETURN id, out_degree;

-- Bolhas (variação estrutural em pangenomas)
FIND BUBBLE WHERE branch_count >= 2  RETURN source, sink, branch_count;

-- Filtros de metadata (pushdown ao MetadataStore)
FIND SEGMENT WHERE length > 50000               RETURN id, name, length;
FIND SEGMENT WHERE depth >= 10 AND length > 1000 RETURN id, name, depth;
FIND SEGMENT WHERE name = "ctg001"              RETURN id, length;

-- Busca de k-mer com FM-Index
SEARCH KMER "ACGTACGT" RETURN id, position;

-- Combinação: paths que atravessam uma região via gBWT
FIND PATH WHERE crosses(HG002#chr1) RETURN id, name, bp_length;
```

---

## Performance

Benchmarks no **GFA HPRC v1.0 (mc-chm13)** · 85.282.047 nós · 118.135.187
arestas · 28.784 haplótipos · 8 threads · perfil `pangenome` (cold ingest
com `/usr/bin/time -v`, medido em 2026-05-12).

### Pipeline de ingestão (HPRC full, perfil `pangenome`)

| Fase | Tempo |
|------|-------|
| Pass 1 — tokenização + MPHF | 10,6 s |
| Pass 2 — links + paths streaming | 2,99 min |
| CSR + Elias-Fano | 6,2 s |
| FM-Index (libsais64 + Occ) | 2,51 min |
| gBWT (28.784 paths) | 7,18 min |
| Snarls (3-ECC + cactus) | 2,08 min |
| Persistência do `.idx/` | 1,1 s |
| **Total wall-clock** | **17,65 min** (1059 s) |

User CPU: 6.159 s (~5,8× wall, paralelismo efetivo ~6 threads no melhor
caso — bottleneck em fases single-thread como o stage de IDs do Pass 1).

### Footprint de memória e disco

| Métrica | Valor |
|---|---|
| **RSS pico (cold ingest HPRC)** | **50,7 GB** |
| Pico isolado da construção gBWT | 20,9 GB |
| Cache em disco `.idx/` | 22 GB (45% do GFA de 49 GB) |
| Page faults major / minor | 4.218 / 52,7 M |

### Latências de hot-path

| Operação | ns/op |
|----------|-------|
| `out_degree(id)` | 8,4 |
| `in_degree(id)` | 7,5 |
| `out_neighbors(id)` | 7,8 |

### Comparação contra ferramentas estabelecidas (HPRC, mesmo hardware)

| Ferramenta | Wall-clock | RSS pico | Threads efetivas |
|---|---:|---:|---:|
| **GGQL** (`bench_gbwt`, perfil `pangenome`) | **17,65 min** | **50,7 GB** | 8 (multi) |
| `gfatools stat` (só contagem, sem índice) | 18,98 min | 47,5 GB | 1 (single) |

Detalhe relevante: `gfatools stat` é estritamente uma contagem de segs/links
(sem índice persistente, sem haplótipos, sem FM, sem snarls) e ainda assim
gasta RAM equivalente e é mais lento que GGQL fazendo o pipeline completo.

### Perfis comparados (HPRC, estimativas)

| Perfil | RAM pico | Tempo total | Casos de uso |
|--------|----------|-------------|--------------|
| `metagenomics` | ~25 GB | ~8 min | sem haplótipos significativos |
| `assembly` | ~30 GB | ~10 min | QC de montagem (snarls, sem gBWT) |
| `comparative` | ~45 GB | ~15 min | paths = espécies (gBWT, sem snarls) |
| `pangenome` | **50,7 GB** | **17,65 min** | tudo habilitado *(medido)* |

---

## Requisitos

| Dependência | Versão | Instalação (Ubuntu 24.04) |
|-------------|--------|---------------------------|
| g++ (C++23) | GCC 14+ | `sudo apt install g++-14` |
| CMake | 3.16+ | `sudo apt install cmake` |
| Intel TBB | 2021+ | `sudo apt install libtbb-dev` |
| divsufsort | qualquer | `sudo apt install libdivsufsort-dev` |
| **fmt, pegtl, sdsl-lite, gbwt, libsais** | tags fixadas | `git submodule update --init --recursive` |

---

## Build

```bash
# 1 — Clone com todos os submódulos
git clone --recurse-submodules https://github.com/karlaycosta/GGQL-Engine.git
cd GGQL-Engine

# 2 — Configure (especifique o compilador se necessário)
cmake -B build \
      -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_BUILD_TYPE=Release

# 3 — Build de todos os alvos
cmake --build build -j$(nproc)

# 4 — Rode os testes
ctest --test-dir build --output-on-failure
```

### Opções CMake

| Variável | Padrão | Valores | Efeito |
|---|---|---|---|
| `GGQL_ALLOCATOR` | `jemalloc-system` | `jemalloc-system` \| `jemalloc` (vendor) \| `mimalloc` \| `glibc` | Allocator dos binários standalone (`ggql_repl`, `bench_*`). jemalloc-system dá throughput máximo no gBWT (~5.94 M nós/s vs 2.86 M do glibc). |
| `GGQL_FFI_ALLOCATOR` | `mimalloc` | `mimalloc` \| `jemalloc-system` \| `glibc` | Allocator da `libggql_ffi.so`. mimalloc é **dlopen-safe** — Flutter/Python carregam a `.so` sem precisar de `LD_PRELOAD`. |
| `CMAKE_BUILD_TYPE` | `Release` | `Debug` \| `RelWithDebInfo` \| `MinSizeRel` | Debug desabilita LTO + adiciona símbolos. |

### Alvos disponíveis

| Alvo | Descrição |
|------|-----------|
| `ggql_repl` | **REPL interativa** — entry point principal |
| `bench_gbwt` | Benchmark de ingestão + construção gBWT |
| `bench_{snarl,fm_index,metadata,topology,graph_algorithms,path_storage}` | Microbench por componente |
| `libggql_ffi.so` | Biblioteca compartilhada para bindings Dart/Python/FFI (**43 símbolos**) |
| `tests/test_*` | **18 testes CTest** (incluindo `test_ffi_query`, `test_ffi_ingest_cancel`, `test_ffi_cursor`, `test_ffi_subgraph`, `test_ffi_version` validando a API FFI) |

---

## Uso

### REPL interativa

```bash
# Pangenome (default) — tudo habilitado
./build/ggql_repl --gfa caminho/para/grafo.gfa --threads 8

# Metagenômica — sem gBWT, sem snarls
./build/ggql_repl --gfa metagenomic.gfa --profile metagenomics --threads 8

# Carregar índice já existente (sem re-ingestão)
./build/ggql_repl --idx caminho/para/grafo.gfa.idx

# Identidade do build (SemVer · ABI · git hash · data)
./build/ggql_repl --version
# → ggql 0.1.0 (43fab89) · ABI 1 · built 2026-06-02
```

### Modo batch (pipe)

```bash
printf 'FIND TIP RETURN id;\n.quit\n' \
  | ./build/ggql_repl --gfa grafo.gfa
```

### Comandos da REPL

| Comando | Ação |
|---------|------|
| `.help` | Referência rápida da linguagem |
| `.stats` | Contagens e timings da ingestão |
| `.load <arquivo.gfa>` | Re-ingere um GFA em runtime |
| `.quit` / `.exit` | Sai da REPL |

### Benchmark de ingestão

```bash
# Pangenome (default)
./build/bench_gbwt grafo.gfa 8 0

# Metagenomics, modo silencioso
./build/bench_gbwt grafo.gfa 8 0 -q --profile metagenomics

# Aliases curtos (pan, meta, asm, comp)
./build/bench_gbwt grafo.gfa 8 0 -p comp
```

### Bindings C/Dart/FFI/Python

A API C está em [`include/ffi/ffi.h`](include/ffi/ffi.h) — **43 símbolos
exportados**. Guia completo de integração com Flutter (ffigen) e Python
(ctypes/cffi) em
[`docs/ffi_flutter_python_integration.md`](docs/ffi_flutter_python_integration.md).

```c
// ── Versão / gate de ABI (handle-free — chamável no boot, sem engine_load) ──
// Consumer compara o GGQL_FFI_ABI_VERSION que compilou (via ffigen) contra o
// abi_version devolvido em runtime: divergência ⇒ .so incompatível com o header.
GgqlVersionInfo v = { .struct_size = sizeof(GgqlVersionInfo) };
if (ggql_ffi_version(&v) == 0 && v.abi_version != GGQL_FFI_ABI_VERSION) {
    /* erro acionável: "recompile/recopie o engine" — em vez de UB */
}
// v.version_str "0.1.0" · v.git_hash "43fab89" (+ "-dirty") · v.build_date ISO-8601
```

```c
// ── Ingestão (Round 4 Fase 5 — BREAKING vs versões anteriores) ──────────────
// Retorna int32_t status (0=ok, 1=fail, -1=NULL args, -2=struct_size
// insuficiente) e preenche stats via out param. Caller DEVE preencher
// out_stats->struct_size = sizeof(GgqlIngestStats) antes de chamar.
GgqlIngestStats stats = { .struct_size = sizeof(GgqlIngestStats) };
int32_t rc = ggql_ingestor_ingest_with_profile(
    handle, filepath, num_threads, gamma, verbose,
    GGQL_PROFILE_PANGENOME,  // ou METAGENOMICS, ASSEMBLY, COMPARATIVE
    max_gbwt_paths, keep_gbwt_tmp, cache_dir,
    event_cb, user_data,
    &stats);

// Cancel cooperativo do ingest (Fase 3, simétrico ao engine_cancel)
ggql_ingestor_cancel(handle);

// ── Engine de query (Rounds 1–3 + Round 4 refinements) ─────────────────────
GgqlEngineHandle eng = ggql_engine_load(cache_dir);  // lazy load do FM-Index
GgqlResultHandle r = ggql_engine_execute(eng, "FIND SEGMENT WHERE length > 1000", 100, 0);

// Variante com cap per-query (Fase 6 — paralelismo seguro)
GgqlResultHandle r2 = ggql_engine_execute_with_cap(eng, query, 100, 0, /*cap=*/50);

// Cursor stateful — true O(N) streaming (Fase 7 / C7.1, SEGMENT full-scan)
GgqlCursorHandle c = ggql_engine_execute_cursor(eng, "FIND SEGMENT RETURN id, name");
while (true) {
    GgqlResultHandle page = ggql_cursor_next(c, /*page_size=*/4096);
    if (ggql_result_row_count(page) == 0) { ggql_result_free(page); break; }
    /* processa page */
    ggql_result_free(page);
}
ggql_cursor_destroy(c);

// ── Subgrafo induzido para o GraphView (Round 5 / C12) ─────────────────────
// A partir dos segment ids de um resultado, devolve o subgrafo induzido +
// halo de 1-hop em índices LOCAIS densos, via two-pass (struct_size + copy-out):
//   - seg_ids_in: ids globais (= ggql_result_cell_int do FIND SEGMENT RETURN id)
//   - edges: pares achatados não-direcionados/dedupados em idx local
//   - node_cap: orçamento total (resultado + halo); estouro → truncated
int64_t ids[] = { /* ids do resultado */ };
GgqlSubgraphDims dims = { .struct_size = sizeof(GgqlSubgraphDims) };
ggql_engine_subgraph_from_result(eng, ids, n, /*halo_hops=*/1, /*node_cap=*/5000,
                                 &dims, NULL,0, NULL,0, NULL,0);   // 1ª passada: dims
if (!dims.truncated) {                                            // senão: "refine query"
    int32_t* edges     = malloc(dims.edge_count * 2 * sizeof(int32_t));
    int64_t* seg_ids   = malloc(dims.node_count * sizeof(int64_t));
    uint8_t* is_result = malloc(dims.node_count * sizeof(uint8_t));
    ggql_engine_subgraph_from_result(eng, ids, n, 1, 5000, &dims, // 2ª passada: preenche
        edges, dims.edge_count*2, seg_ids, dims.node_count, is_result, dims.node_count);
    /* edges → layout; seg_ids → labels/hit-test; is_result → resultado vs halo */
}
```

#### Cliente Flutter validado

O cliente `ggql_ui` (repo Flutter separado) usa bindings gerados via
`ffigen` + wrapper Dart idiomático (`GgqlEngine`, `GgqlResult`,
`GgqlCapabilities`, `GgqlVersion`). **48 testes FFI** passando contra
`libggql_ffi.so` atual — inclui o **gate de ABI fail-closed no startup**
(`GgqlLib.checkAbiCompatibility()`) e a versão real do engine na status bar.
Integração coordenada em
[`ggql_ui/docs/ffi_conflitos_coordenacao.md`](https://github.com/karlaycosta/ggql_ui)
e [`ggql_ui/docs/versionamento_ggql.md`](https://github.com/karlaycosta/ggql_ui).

---

## Estrutura do projeto

```
ggql_engine/
├── include/
│   ├── ggql/
│   │   ├── cli/          ─ Console ANSI, ProgressTracker thread-safe
│   │   ├── ingest/       ─ Ingestor, MetadataStore, GbwtGroup, Profile
│   │   ├── storage/      ─ Topology (CSR+EF), gBWT, FM-Index, CactusFinder
│   │   └── query/        ─ AST GGQL, gramática PEGTL, QueryPlanner
│   ├── ffi/              ─ Header C para bindings Dart/FFI
│   └── mphf/             ─ BBHash + xxHash (header-only)
├── src/
│   ├── cli/              ─ console.cpp
│   ├── ingest/           ─ ingestor.cpp
│   ├── query/            ─ query_planner.cpp, query_evaluator.cpp
│   ├── storage/          ─ csr_builder, graph_algorithms, path_index, fm_index
│   ├── ffi/              ─ ffi.cpp (bindings extern "C")
│   ├── repl/             ─ ggql_repl.cpp (inclui --version)
│   └── bench_gbwt.cpp    ─ Driver de benchmark/ingestão
├── include/ggql/version.h.in ─ template do header de versão (gerado em build-time)
├── tests/                ─ 18 testes do projeto (CTest)
│                           inclui test_ffi_query / _ingest_cancel / _cursor / _subgraph / _version
├── tools/                ─ deploy_ffi_to_ui.sh + cmake/GenerateVersionHeader.cmake
├── third_party/          ─ fmt · pegtl · sdsl-lite · gbwt · libsais
├── docs/                 ─ ingest_profiles.md + diagramas GFA
│                           ffi_flutter_python_integration.md (guia consumer)
│                           plano_versionamento_engine.md (sistema de versão)
└── CMakeLists.txt        ─ Arquitetura por bibliotecas estáticas
                            (+ vendor jemalloc opcional via ExternalProject_Add)
```

---

## Roadmap

### Core engine

- [x] Topologia CSR bidirecional (offsets Elias-Fano)
- [x] Ingestão paralela de GFA — mmap + OpenMP + BBHash MPHF
- [x] MetadataStore binário (SoA) com pushdown de predicados
- [x] Índice de paths gBWT — construção streaming, espaço O(r)
- [x] FM-Index — BWT + OccBlock + locate amostrado
- [x] Detecção de snarls — 3-ECC + grafo cactus + BFS
- [x] Gramática GGQL — PEG tipado (PEGTL) + AST `std::variant`
- [x] QueryPlanner — `FIND TIP/REPEAT/BUBBLE/SEGMENT/EDGE/PATH`, `SEARCH KMER`
- [x] REPL interativa com input multilinha
- [x] **Perfis de ingestão** — pangenome / metagenomics / assembly / comparative
- [x] **18 testes CTest** (do projeto) + 7 do gbwt vendorizado
- [x] `crosses()` integrado ao QueryPlanner (com `CrossesCache` para
  composição AND/OR sobre paths atravessados)
- [x] `contains_kmer()` integrado ao `FIND SEGMENT WHERE`

### FFI / Integração (Round 4 — entregue 2026-05-22)

- [x] Bindings Dart/FFI completos (engine + query + perfis + ingest events)
- [x] `last_error` thread-safe (C9) — race entre threads concorrentes eliminada
- [x] Arena per-event de 16 slots em `EventBridge` (C1) — janela de race
  Dart `NativeCallable.listener` reduzida 16×
- [x] Terminal Ok/Fail garantido (E3) com sentinela `__ggql_ingest_done__`
  — consumer fecha callbacks sem `Future.delayed` mágico
- [x] `ggql_ingestor_cancel` simétrico ao `engine_cancel` (C2/P10) com 7
  checkpoints entre fases de ingest
- [x] **Opaque struct typedef** (E1/P4) nos 3 handles — type confusion
  rejeitada pelo compilador
- [x] **`struct_size`** em `GgqlIngestStats`/`Capabilities`/`ParseResult`
  (C5/P3, BREAKING) + refactor das 3 `ingest*` para out param —
  forward-compat para evolução do ABI
- [x] `ggql_engine_execute_with_cap` (C8) — cap per-query inline
  para paralelismo seguro
- [x] `ggql_engine_last_error_into` (E2) — copy-out strlcpy elimina
  dependência do `thread_local`
- [x] `cli::set_event_callback_tl` (C3b) — ingests paralelos em threads
  distintas têm canais isolados
- [x] **Cursor stateful** via `std::generator` C++23 (C7.1) — true
  streaming O(N) para `FIND SEGMENT` full-scan
- [x] **Allocator split** — jemalloc nos binários + mimalloc na `.so`
  (dlopen-safe para Flutter/Python sem `LD_PRELOAD`)

### FFI / Integração (Round 5 — entregue 2026-05-29)

- [x] **Pré-voo de ingestão** (C11) — `ggql_survey_gfa` (survey estendido) +
  `ggql_estimate_ingest` (estimativa de RAM/tempo por perfil + sugestão de
  perfil) para a UI decidir antes de ingerir. O *light Pass1*
  (`survey_gfa_light`) é uma varredura `mmap` + OpenMP **SIMD** — `memchr`
  para fronteiras de linha + contagem de orientação de walk vetorizada
  (`(b|0x02)==0x3E` casa `<`/`>` num único compare) — com contagem de nós da
  P-line restrita ao campo de segmentos (ignora o overlaps CIGAR, mantendo
  `avg_path_length` correto para a sugestão de perfil)
- [x] **Subgrafo induzido para o GraphView** (C12) —
  `ggql_engine_subgraph_from_result`: a partir dos segment ids de um resultado,
  devolve o subgrafo induzido + halo de 1-hop em índices locais densos
  (two-pass `struct_size` + copy-out, arestas não-direcionadas/dedupadas, cap
  rígido com `truncated`). Habilita o GraphView result-driven do cliente Flutter

### Versionamento (entregue 2026-06-02)

- [x] **SemVer + identidade de build** — `project(VERSION 0.1.0)` no CMake +
  `version.h` gerado em build-time (git hash + flag dirty + data) via
  `GenerateVersionHeader.cmake` (custom target, `copy_if_different` para não
  recompilar à toa)
- [x] **Contrato de ABI explícito** — macro `GGQL_FFI_ABI_VERSION` + struct
  `GgqlVersionInfo` + símbolo handle-free `ggql_ffi_version` (padrão
  `struct_size`), habilitando o gate de compatibilidade no boot do consumer
- [x] **Ergonomia/deploy** — `ggql_repl --version` + `tools/deploy_ffi_to_ui.sh`
  (build + cópia + log de versão + verificação sha256)

### Próximos passos

- [ ] **C7.2+** — portar EDGE/PATH/TIP/REPEAT/BUBBLE/SEARCH KMER + fast-paths
  de SEGMENT (name/length/depth index) para cursor `std::generator`
- [ ] Persistência de queries pré-compiladas (cache de planos)
- [ ] CI no GitHub Actions (`ubuntu-24.04`, `g++-14`)
- [ ] Suporte a GFA 2.0 (linhas E/G/U)
- [ ] Indexação de alinhamentos GAF (reuso de GBWT/CrossesCache) — ver
  *future work* no paper
- [ ] Portabilidade macOS / ARM

---

## Licença

MIT — veja [LICENSE](LICENSE).

---

<div align="center">
<sub>Construído com C++23 · sdsl-lite · gbwt · PEGTL · BBHash · fmt · libsais</sub>
</div>
