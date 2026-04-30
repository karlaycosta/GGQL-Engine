<div align="center">

# GGQL Engine

**Motor de consulta declarativa para grafos genômicos — C++23 · escala HPRC**

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg?logo=cplusplus)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake)](CMakeLists.txt)
[![GCC](https://img.shields.io/badge/GCC-14%2B-A42E2B?logo=gnu)](https://gcc.gnu.org)
[![Plataforma](https://img.shields.io/badge/plataforma-Linux-lightgrey?logo=linux)](https://kernel.org)

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
  OpenMP + MPHF) processa um GFA de 4 milhões de nós em menos de 2 segundos
  e o HPRC inteiro em ~3 minutos.
- **Perfis de ingestão por caso de uso.** Metagenômica, pangenoma, grafo de
  montagem e genômica comparativa têm necessidades diferentes — você escolhe
  apenas as estruturas que vai consultar e economiza RAM/tempo.
- **Estruturas sucintas state-of-the-art.** CSR + Elias-Fano para topologia,
  r-index gBWT para haplótipos, FM-Index BWT para busca de k-mers — todos
  com construção paralela e custo de memória proporcional à informação real.
- **Cache inteligente em disco.** A primeira ingestão grava `.idx/`; chamadas
  subsequentes carregam em milissegundos com validação por hash XXH64 +
  mtime/size.
- **Integração multi-linguagem.** Além da REPL, expõe API C completa para
  bindings em Dart/Flutter, Python (via `ctypes`), Rust, etc.

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

Benchmarks no **GFA HPRC CHM13** · N ≈ 85 M nós · 118 M arestas · 5
haplótipos · 8 threads.

### Pipeline de ingestão (perfil `pangenome`)

| Fase | Tempo |
|------|-------|
| Pass 1 — tokenização + MPHF | 30,2 s |
| Pass 2 — links + paths streaming | 132,4 s |
| CSR + Elias-Fano | 7,2 s |
| gBWT (5 paths) | 16,1 s |
| Snarls (3-ECC + cactus) | ~5 s |
| **Total** | **≈ 191 s** |

### Footprint de memória

| Componente | Tamanho |
|------------|---------|
| `E_fwd` + `E_bwd` — 118 M arestas × 4 B × 2 | 0,94 GB |
| `ef_fwd` + `ef_bwd` — offsets Elias-Fano | ~64 MB |
| `offsets_cache` — lazy, alocado sob demanda | 680 MB |
| **Total `memory_bytes()`** | **≈ 2,76 GB** |

### Latências de hot-path

| Operação | ns/op |
|----------|-------|
| `out_degree(id)` | 8,4 |
| `in_degree(id)` | 7,5 |
| `out_neighbors(id)` | 7,8 |

### Perfis comparados (HPRC)

| Perfil | RAM pico | Tempo total | Casos de uso |
|--------|----------|-------------|--------------|
| `metagenomics` | ~3 GB | ~165 s | sem haplótipos significativos |
| `assembly` | ~7 GB | ~175 s | QC de montagem (snarls, sem gBWT) |
| `comparative` | ~10 GB | ~185 s | paths = espécies (gBWT, sem snarls) |
| `pangenome` | ~12 GB | ~191 s | tudo habilitado |

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

### Alvos disponíveis

| Alvo | Descrição |
|------|-----------|
| `ggql_repl` | **REPL interativa** — entry point principal |
| `bench_gbwt` | Benchmark de ingestão + construção gBWT |
| `libggql_ffi.so` | Biblioteca compartilhada para bindings Dart/FFI |
| `tests/test_*` | 10 testes CTest do projeto |

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

### Bindings C/Dart/FFI

A API C está em [`include/ffi/ffi.h`](include/ffi/ffi.h). Funções principais:

```c
// Ingestão clássica (todos os flags individualmente)
GgqlIngestStats ggql_ingestor_ingest(...);

// Ingestão por perfil (recomendado para clientes)
GgqlIngestStats ggql_ingestor_ingest_with_profile(
    handle, filepath, num_threads, gamma, verbose,
    GGQL_PROFILE_PANGENOME,  // ou METAGENOMICS, ASSEMBLY, COMPARATIVE
    max_gbwt_paths, keep_gbwt_tmp, cache_dir,
    event_cb, user_data);

// Engine de query
GgqlEngineHandle eng = ggql_engine_load(cache_dir);
GgqlResultHandle r = ggql_engine_execute(eng, "FIND SEGMENT WHERE length > 1000", 100, 0);
```

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
│   ├── repl/             ─ ggql_repl.cpp
│   └── bench_gbwt.cpp    ─ Driver de benchmark/ingestão
├── tests/                ─ 10 testes do projeto (CTest)
├── third_party/          ─ fmt · pegtl · sdsl-lite · gbwt · libsais
├── docs/                 ─ ingest_profiles.md, diagramas GFA
└── CMakeLists.txt        ─ Arquitetura por bibliotecas estáticas
```

---

## Roadmap

- [x] Topologia CSR bidirecional (offsets Elias-Fano)
- [x] Ingestão paralela de GFA — mmap + OpenMP + BBHash MPHF
- [x] MetadataStore binário (SoA) com pushdown de predicados
- [x] Índice de paths gBWT — construção streaming, espaço O(r)
- [x] FM-Index — BWT + OccBlock + locate amostrado
- [x] Detecção de snarls — 3-ECC + grafo cactus + BFS
- [x] Gramática GGQL — PEG tipado (PEGTL) + AST `std::variant`
- [x] QueryPlanner — `FIND TIP/REPEAT/BUBBLE/SEGMENT/EDGE/PATH`, `SEARCH KMER`
- [x] REPL interativa com input multilinha
- [x] Bindings Dart/FFI completos (engine + query + perfis)
- [x] **Perfis de ingestão** — pangenome / metagenomics / assembly / comparative
- [x] **17 testes CTest** (10 do projeto + 7 do gbwt)
- [ ] `crosses()` integrado ao QueryPlanner — requer query gBWT por região
- [ ] Persistência de queries pré-compiladas (cache de planos)
- [ ] CI no GitHub Actions (`ubuntu-24.04`, `g++-14`)
- [ ] Suporte a GFA 2.0 (linhas E/G/U)

---

## Licença

MIT — veja [LICENSE](LICENSE).

---

<div align="center">
<sub>Construído com C++23 · sdsl-lite · gbwt · PEGTL · BBHash · fmt · libsais</sub>
</div>
