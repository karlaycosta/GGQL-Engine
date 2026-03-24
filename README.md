# GGQL Engine

Motor de consulta de grafos de pangenoma em **C++23**, projetado para escala 
de dezenas de milhões de nós (GFA HPRC). Expõe a linguagem **GGQL** — uma 
sintaxe declarativa inspirada em SQL com operadores específicos de grafos 
genômicos — através de um REPL interativo.

```
ggql> FIND TIP WHERE in_degree = 0 RETURN id
ggql> FIND SEGMENT WHERE length > 50000 AND depth >= 10 RETURN id, name, length
ggql> FIND BUBBLE WHERE branch_count >= 2 RETURN source, sink, branch_count
ggql> FIND REPEAT WHERE out_degree >= 3 RETURN id, out_degree
```

---

## Funcionalidades

| Recurso | Detalhes |
|---|---|
| **Ingest paralelo** | mmap + OpenMP + LSD Radix Sort; lê GFA de 4M nós em &lt; 2s |
| **Topologia CSR** | `sdsl::int_vector<64>`, orientações empacotadas, ~2.57 GB para 85M nós |
| **MPHF lookup** | BooPHF — mapeamento nome → ID em O(1) |
| **Metadados SQLite** | `length`, `read_count`, `depth` com pushdown de predicados |
| **Gramática PEGTL** | Parser PEG tipado, AST C++23 em `std::variant` |
| **QueryPlanner** | TIP, REPEAT, BUBBLE → topologia; SEGMENT → SQL |
| **REPL** | Prompt interativo com multi-linha, meta-comandos, `.load` |

---

## Linguagem GGQL

```
FIND <entity> [WHERE <cond> [AND|OR <cond>]...] [RETURN <field>, ...]
SEARCH KMER "<seq>"                              [RETURN <field>, ...]

entity  = SEGMENT | BUBBLE | TIP | REPEAT | EDGE | PATH
cond    = <field> <op> <value>
        | crosses(<path_name>)
        | contains_kmer("<seq>")
op      = =  !=  <  >  <=  >=
```

**Campos disponíveis por entidade:**

| Entidade | Campos |
|---|---|
| `TIP`, `REPEAT` | `id`, `in_degree`, `out_degree` |
| `BUBBLE` | `source`, `sink`, `branch_count` |
| `SEGMENT` | `id`, `name`, `length`, `read_count`, `depth` |

Consulte [docs/grammar.md](docs/grammar.md) para a gramática formal completa e 
[docs/user_guide.md](docs/user_guide.md) para exemplos de uso.

---

## Requisitos

| Dependência | Versão mínima | Instalação (Debian/Ubuntu) |
|---|---|---|
| g++ com C++23 | GCC 14 | `sudo apt install g++-14` |
| CMake | 3.16 | `sudo apt install cmake` |
| SDSL-lite | 2.1+ | `sudo apt install libsdsl-dev` |
| SQLite3 | 3.x | `sudo apt install libsqlite3-dev` |
| TBB | 2021+ | `sudo apt install libtbb-dev` |
| PEGTL | main | git submodule (incluso em `third_party/`) |

---

## Build

```bash
# Clone com submodules
git clone --recurse-submodules https://github.com/<usuario>/ggql_engine.git
cd ggql_engine

# Configurar (informe o compilador explicitamente se necessário)
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release

# Compilar todos os targets
cmake --build build -j$(nproc)
```

### Targets disponíveis

| Target | Descrição |
|---|---|
| `ggql_repl` | **REPL interativo** (uso principal) |
| `ggql_engine_dev` | Driver de integração (27 testes inline) |
| `ggql_parser_dev` | Testes unitários da gramática (23 casos) |
| `bench_ingest` | Benchmark de ingest com breakdown de tempos |

---

## Uso

### Ingest + consulta imediata

```bash
# Ingere um arquivo GFA e abre o REPL
./build/ggql_repl --gfa caminho/para/grafo.gfa --db /tmp/meta.sqlite --threads 8

# Abre banco SQLite já existente (apenas FIND SEGMENT disponível)
./build/ggql_repl --db /tmp/meta.sqlite
```

### Modo batch (sem REPL)

```bash
# Pipe de queries — executa e sai
printf 'FIND TIP RETURN id\n.quit\n' | ./build/ggql_repl --gfa grafo.gfa --db /tmp/m.sqlite
```

### Gerar GFA sintético para testes

```bash
python3 data/gen_gfa.py 10000 /tmp/test_10k.gfa
```

---

## Estrutura do projeto

```
ggql_engine/
├── include/
│   ├── ggql/
│   │   ├── ingest/          ← GfaIngestorMmapParallel, GfaMetadataStore
│   │   ├── storage/         ← GfaCsrTopology (CSR + algoritmos de grafo)
│   │   └── query/           ← ggql_ast.hpp, ggql_grammar.hpp, query_planner.hpp
│   └── mphf/                ← BooPHF + xxHash (header-only)
├── src/
│   ├── repl/                ← ggql_repl.cpp
│   ├── ingest/              ← bench_ingest.cpp
│   └── parser/              ← ggql_parser_dev.cpp
├── docs/                    ← Gramática formal e guia do usuário
├── data/                    ← gen_gfa.py (GFAs grandes são .gitignored)
├── tests/                   ← Placeholder (Phase 2)
└── third_party/
    └── pegtl/               ← TAO PEGTL (git submodule)
```

---

## Roadmap

- [x] GfaCsrTopology — CSR bidirecional com sdsl + algoritmos (tip, repeat, bubble)
- [x] GfaIngestorMmapParallel — mmap + OpenMP + BooPHF MPHF
- [x] GfaMetadataStore — SQLite com pushdown de predicados
- [x] GGQL Grammar (PEGTL) — AST tipado C++23
- [x] QueryPlanner — TIP / REPEAT / BUBBLE / SEGMENT
- [x] REPL interativo
- [ ] FIND EDGE, FIND PATH
- [ ] SEARCH KMER com FM-Index
- [ ] Testes formais com CTest
- [ ] CI/CD (GitHub Actions)

---

## Licença

MIT — veja [LICENSE](LICENSE).
