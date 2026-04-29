<div align="center">

# GGQL Engine

**Pangenome Graph Query Engine — C++23 · HPRC scale**

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg?logo=cplusplus)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake)](CMakeLists.txt)
[![GCC](https://img.shields.io/badge/GCC-14%2B-A42E2B?logo=gnu)](https://gcc.gnu.org)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey?logo=linux)](https://kernel.org)

A declarative query engine for pangenome graphs in **GFA format**, built for  
**85 M+ node** assemblies (HPRC / CHM13). Exposes **GGQL** — a SQL-inspired  
language with graph-native operators — through an interactive REPL.

</div>

---

```
ggql> FIND TIP   WHERE in_degree = 0          RETURN id
ggql> FIND REPEAT WHERE out_degree >= 3       RETURN id, out_degree
ggql> FIND BUBBLE WHERE branch_count >= 2    RETURN source, sink, branch_count
ggql> FIND SEGMENT WHERE length > 50000 AND depth >= 10  RETURN id, name, length
ggql> SEARCH KMER "ACGTACGT"                 RETURN id, position
```

---

## Table of Contents

- [Architecture](#architecture)
- [Features](#features)
- [GGQL Language](#ggql-language)
- [Performance](#performance)
- [Requirements](#requirements)
- [Building](#building)
- [Usage](#usage)
- [Project Structure](#project-structure)
- [Roadmap](#roadmap)
- [License](#license)

---

## Architecture

GGQL Engine is organized in **four independent static libraries**. Each layer recompiles only when its own code or direct dependencies change.

```
┌─────────────────────────────────────────────────────────────────────┐
│  REPL  ggql>                  (src/repl/ggql_repl.cpp)              │
├─────────────────────────────────────────────────────────────────────┤
│  ggql_query                                                         │
│    QueryPlanner · GGQL Grammar (PEGTL) · AST (std::variant C++23)  │
├─────────────────────────────────────────────────────────────────────┤
│  ggql_ingest                                                        │
│    GfaIngestorMmapParallel · GfaMetadataStore (binary SoA)          │
│    BooPHF MPHF · LSD Radix Sort · gBWT streaming writer            │
├─────────────────────────────────────────────────────────────────────┤
│  ggql_storage                                                       │
│    Layer 1 — CSR + Elias-Fano  (topology, algorithms)              │
│    Layer 2 — gBWT              (haplotype / path index)            │
│    Layer 3 — FM-Index          (k-mer search)                      │
├─────────────────────────────────────────────────────────────────────┤
│  ggql_core                                                          │
│    ANSI console · thread-safe ProgressTracker (CAS) · utils        │
└─────────────────────────────────────────────────────────────────────┘
        │
        ▼  third-party (git submodules)
   fmt · pegtl · sdsl-lite · gbwt · libsais
```

### Storage layers at a glance

| Layer | Structure | Purpose |
|---|---|---|
| **CSR** | `sdsl::sd_vector<>` (Elias-Fano) offsets + `uint32_t` edge arrays | Bidirectional adjacency; O(1) degree and neighbors |
| **gBWT** | `gbwt::GBWT` — r-index (Sirén 2020) | Haplotype / path index; O(r) space where r = BWT runs |
| **FM-Index** | BWT + OccBlock AoS + bitmap-sampled locate | k-mer search over segment sequences |
| **Metadata** | Binary Struct-of-Arrays + local index (`metadata.bin`) | Segment attributes: length, depth, read_count, name, sequence |

---

## Features

| Feature | Details |
|---|---|
| **Parallel ingest** | `mmap` + `posix_fadvise` + OpenMP chunked on `\n`; 4 M-node GFA in < 2 s |
| **LSD Radix Sort** | 4 passes × 8 bits for `LinkRecord` arrays — cache-friendly link sort |
| **CSR topology** | `uint32_t` edges with packed orientation bit; 85 M nodes / 118 M edges ≈ 0.94 GB |
| **Elias-Fano offsets** | `sdsl::sd_vector<>` replaces flat `uint32_t` arrays; ~10× space reduction for offset vectors |
| **Parallel offsets cache** | OMP `schedule(static)` on both EF selectors; ~75× speedup for N = 85 M |
| **gBWT streaming** | Nodes written to disk during parse via `text_buffer_type`; construction never materializes paths in RAM |
| **MPHF lookup** | BooPHF — name → internal ID in O(1), ~2.5 bits/key |
| **FM-Index k-mer search** | BWT + OccBlock; `search_kmer()` + sampled `locate()` |
| **Local binary index** | Metadata persisted as `metadata.bin` / `metadata_strings.bin` / `metadata_seqs.bin`; loaded once, queried in RAM |
| **PEGTL grammar** | Typed PEG parser; `std::variant`-based AST; zero dynamic dispatch in hot path |
| **Interactive REPL** | Multi-line input, `.load`, `.stats`, `.help`, batch mode via pipe |
| **Dart FFI** | `extern "C"` bindings in `src/ffi/ggql_ffi.cpp` |
| **15 unit tests** | CTest: 5 ingestor · 3 storage (CSR + FM-Index) · 7 query planner |

---

## GGQL Language

### Syntax

```
query       ::= find_stmt | search_stmt

find_stmt   ::= FIND <entity> [WHERE <cond> [AND|OR <cond>]...] [RETURN <field>, ...]
search_stmt ::= SEARCH KMER "<seq>"                              [RETURN <field>, ...]

entity      ::= SEGMENT | EDGE | PATH | BUBBLE | TIP | REPEAT
cond        ::= <field> <op> <value>
              | crosses(<path_name>)
              | contains_kmer("<seq>")
op          ::= =  !=  <  >  <=  >=
value       ::= <integer> | "<string>"
```

### Fields by entity

| Entity | Available fields | Backend |
|---|---|---|
| `TIP` | `id`, `in_degree`, `out_degree` | CSR topology |
| `REPEAT` | `id`, `in_degree`, `out_degree` | CSR topology |
| `BUBBLE` | `source`, `sink`, `branch_count` | CSR topology |
| `SEGMENT` | `id`, `name`, `length`, `read_count`, `depth` | In-memory metadata (binary index) |
| `EDGE` / `PATH` | — | Phase 2 |

### Examples

```sql
-- Topology queries
FIND TIP WHERE in_degree = 0 RETURN id
FIND REPEAT WHERE out_degree >= 3 RETURN id, out_degree
FIND BUBBLE WHERE branch_count >= 2 RETURN source, sink, branch_count

-- Metadata queries (predicate pushed down to SQLite)
FIND SEGMENT WHERE length > 50000 RETURN id, name, length
FIND SEGMENT WHERE depth >= 10 AND length > 1000 RETURN id, name, depth
FIND SEGMENT WHERE name = "ctg001" RETURN id, length

-- K-mer search
SEARCH KMER "ACGTACGT" RETURN id, position
```

---

## Performance

Benchmarked on **CHM13 HPRC GFA** · N ≈ 85 M nodes · 118 M edges · 5 haplotype paths · 8 threads.

### Ingest pipeline

| Phase | Time |
|---|---|
| Pass 1 — tokenize + MPHF build | 30.2 s |
| Pass 2 — links + path streaming | 132.4 s |
| Build CSR + Elias-Fano | 7.2 s |
| Build gBWT (streaming, 5 paths) | 16.1 s |
| **Total** | **≈ 186 s** |

### Memory footprint

| Component | Size |
|---|---|
| `E_fwd` + `E_bwd` — 118 M edges × 4 B × 2 | 0.94 GB |
| `ef_fwd` + `ef_bwd` — Elias-Fano offsets | ~64 MB |
| `offsets_cache` — lazy, alloc on first use | 680 MB |
| **Total `memory_bytes()`** | **≈ 2.76 GB** |

### Hot-path latencies

| Operation | ns/op |
|---|---|
| `out_degree(id)` | 8.4 |
| `in_degree(id)` | 7.5 |
| `out_neighbors(id)` | 7.8 |

---

## Requirements

| Dependency | Version | Install (Ubuntu 24.04) |
|---|---|---|
| g++ (C++23) | GCC 14+ | `sudo apt install g++-14` |
| CMake | 3.16+ | `sudo apt install cmake` |
| Intel TBB | 2021+ | `sudo apt install libtbb-dev` |
| divsufsort | any | `sudo apt install libdivsufsort-dev` |
| SQLite3 | 3.x | `sudo apt install libsqlite3-dev` |
| **fmt, pegtl, sdsl-lite, gbwt, libsais** | pinned tags | `git submodule update --init --recursive` |

---

## Building

```bash
# 1 — Clone with all submodules
git clone --recurse-submodules https://github.com/<user>/ggql_engine.git
cd ggql_engine

# 2 — Configure (specify compiler explicitly if needed)
cmake -B build \
      -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_BUILD_TYPE=Release

# 3 — Build all targets
cmake --build build -j$(nproc)

# 4 — Run tests
ctest --test-dir build --output-on-failure
```

### Available targets

| Target | Description |
|---|---|
| `ggql_repl` | **Interactive REPL** — main entry point |
| `ggql_engine_dev` | Integration driver (development) |
| `bench_gbwt` | gBWT construction benchmark |
| `bench_fm_index` | FM-Index benchmark |
| `test_fm_index_minimal` | Standalone FM-Index reproducer |

---

## Usage

### Ingest a GFA and open the REPL

```bash
./build/ggql_repl --gfa path/to/graph.gfa --db /tmp/meta.sqlite --threads 8
```

### Open an existing database (SEGMENT queries only)

```bash
./build/ggql_repl --db /tmp/meta.sqlite
```

### Batch mode (pipe)

```bash
printf 'FIND TIP RETURN id\n.quit\n' \
  | ./build/ggql_repl --gfa graph.gfa --db /tmp/meta.sqlite
```

### REPL meta-commands

| Command | Action |
|---|---|
| `.help` | Quick language reference |
| `.stats` | Node/edge counts and ingest timing breakdown |
| `.load <file.gfa>` | Re-ingest a GFA at runtime |
| `.quit` / `.exit` | Exit the REPL |

---

## Project Structure

```
ggql_engine/
├── include/
│   ├── ggql/
│   │   ├── cli/            ─ ANSI console, thread-safe ProgressTracker
│   │   ├── ingest/         ─ GfaIngestorMmapParallel, GfaMetadataStore, gBWT group
│   │   ├── storage/        ─ GfaCsrTopology (CSR+EF+gBWT), FM-Index, cactus finder
│   │   └── query/          ─ GGQL AST, PEGTL grammar, QueryPlanner
│   ├── ffi/                ─ C header for Dart FFI
│   └── mphf/               ─ BooPHF + xxHash (header-only)
├── src/
│   ├── cli/                ─ console.cpp
│   ├── ingest/             ─ gfa_ingestor.cpp (1 436 LOC)
│   ├── query/              ─ query_planner.cpp, query_evaluator.cpp
│   ├── storage/            ─ CSR builder, graph algorithms, path index, FM-Index
│   ├── ffi/                ─ Dart FFI bindings (extern "C")
│   └── repl/               ─ ggql_repl.cpp
├── tests/                  ─ 15 unit tests (CTest, 3 layers)
├── third_party/            ─ fmt · pegtl · sdsl-lite · gbwt · libsais
├── docs/                   ─ GFA format diagrams
└── CMakeLists.txt          ─ 4-layer static library architecture
```

---

## Roadmap

- [x] Bidirectional CSR topology (Elias-Fano offsets)
- [x] Parallel GFA ingest — mmap + OpenMP + BooPHF MPHF
- [x] SQLite metadata store with predicate pushdown
- [x] gBWT path index — streaming construction, O(r) space
- [x] FM-Index — BWT + OccBlock + sampled locate
- [x] GGQL grammar — PEGTL typed PEG + `std::variant` AST
- [x] QueryPlanner — TIP / REPEAT / BUBBLE / SEGMENT
- [x] Interactive REPL with multi-line input
- [x] Dart FFI bindings
- [x] 15 CTest unit tests (storage · ingest · query)
- [ ] `FIND EDGE` — edge model in SQLite
- [ ] `FIND PATH` — path result type
- [ ] `SEARCH KMER` — FM-Index integration in QueryPlanner
- [ ] `crosses()` predicate — requires PathIndex query
- [ ] GitHub Actions CI (`ubuntu-24.04`, `g++-14`)

---

## License

MIT — see [LICENSE](LICENSE).

---

<div align="center">
<sub>Built with C++23 · sdsl-lite · gbwt · PEGTL · BooPHF · fmt · libsais · SQLite</sub>
</div>
