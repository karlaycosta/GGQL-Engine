# GGQL Engine — Histórico de Implementações

> **Projeto**: Motor de consulta de grafos genômicos (GFA) em C++23  
> **Repositório**: `ggql_engine`  
> **Linguagem**: C++23 · Compilador: g++-14  
> **Data do relatório**: 24 de março de 2026

---

## Visão Geral do Projeto

O GGQL Engine é um motor de consulta para grafos de pangenoma no formato GFA 
(Graphical Fragment Assembly), projetado para escala de 85M+ nós (HPRC). 
Expõe uma linguagem de consulta declarativa chamada **GGQL** (Genomic Graph 
Query Language) com sintaxe inspirada em SQL e operadores específicos de 
grafos genômicos.

### Arquitetura final (4 camadas)

```
┌────────────────────────────────────────────────────────┐
│  REPL  ggql>  (src/repl/ggql_repl.cpp)                 │
├────────────────────────────────────────────────────────┤
│  QueryPlanner  (include/ggql/query/query_planner.hpp)  │
│  GGQL Grammar  (include/ggql/query/ggql_grammar.hpp)   │
│  GGQL AST      (include/ggql/query/ggql_ast.hpp)       │
├────────────────────────────────────────────────────────┤
│  GfaCsrTopology  (include/ggql/storage/)               │
│  GfaMetadataStore + SQLite  (include/ggql/ingest/)     │
├────────────────────────────────────────────────────────┤
│  GfaIngestorMmapParallel  (include/ggql/ingest/)       │
│  BooPHF MPHF  (include/mphf/)                          │
└────────────────────────────────────────────────────────┘
```

---

## Componentes Pré-existentes

Os seguintes componentes já existiam antes desta sessão de desenvolvimento:

### `GfaIngestorMmapParallel` — `include/ggql/ingest/gfa_ingestor.hpp`

Ingestor paralelo de alta performance para arquivos GFA.

- **Estratégia de I/O**: `mmap` + `posix_fadvise(POSIX_FADV_SEQUENTIAL)` + `MADV_HUGEPAGE`
- **Paralelismo**: OpenMP com chunking alinhado em `\n` para evitar races
- **2 passadas**:
  1. Coleta de chaves (nomes de segmentos) → build do MPHF
  2. Processa segmentos → `SegmentRecord[]` → SQLite + topologia
- **LSD Radix Sort** paralelo (4 passes × 8 bits) para `LinkRecord`
- **Ausência de alocações hot-path**: `tokenize()` reutiliza buffer fixo
- `ingest(filepath, threads, gamma, verbose, db_path, build_topology)` → `std::expected<IngestStatsParallel, std::string>`

### `GfaCsrTopology` — `include/ggql/storage/gfa_csr_topology.hpp`

Topologia GFA em CSR (Compressed Sparse Row) bidirecional.

- **Layout**: `sdsl::int_vector<64>` para arestas + `std::vector<uint32_t>` para offsets
- **Encoding**: `valor = (node_id << 1) | orient_bit` (bit 0 = orientação)
- **Consumo** para 85M nós / 118M arestas: ~2.57 GB
- **Algoritmos**:
  - `find_tips()` — nós com `in_degree=0` ou `out_degree=0`
  - `find_repeats(min_degree)` — nós com `out_degree >= min_degree`
  - `find_bubbles(max_depth)` — BFS + interseção de conjuntos (`unordered_set`)
- **Tipos**: `LinkRecord` (16 bytes, orientações empacotadas), `EdgeRef`, `Bubble`

### `GfaMetadataStore` — `include/ggql/ingest/gfa_metadata_store.hpp`

Armazenamento de metadados de segmentos em SQLite.

- **PRAGMAs agressivos**: `journal_mode=OFF`, `synchronous=OFF`, `locking_mode=EXCLUSIVE`, `cache_size=-2097152`, `page_size=32768`
- **Mapeamento 1:1**: `internal_id = mphf_->lookup()` (O(1) RAM ↔ disco)
- **Gravação em batch**: transação única para todos os registros
- **Schema**: `segments(internal_id PK, original_id, length, read_count, depth)`
- **Índice secundário**: `idx_segments_orig` criado após o bulk insert

### `BooPHF MPHF` — `include/mphf/BooPHF.h` + `include/mphf/xxh64.hpp`

Função de hash perfeita mínima para mapeamento de nomes de segmentos → IDs inteiros.

---

## Sessão de Desenvolvimento — 24 de março de 2026

### Iteração 1 — GGQL Grammar (PEGTL)

**Situação inicial**: existia um protótipo funcional em `src/parser/pegtl_parser2.cpp`
com gramática PEGTL, mas sem tipos AST reais (usava `std::string` no estado),
sem header público, e sem integração com o resto do sistema.

---

#### `include/ggql/query/ggql_ast.hpp` — **NOVO**

Tipos AST tipados para representar queries GGQL parseadas.

```
namespace ggql {
  enum class EntityType  { SEGMENT, EDGE, PATH, BUBBLE, TIP, REPEAT }
  enum class Operator    { EQ, NEQ, GT, LT, GEQ, LEQ }
  enum class LogicalOp   { AND, OR }
  using Value = std::variant<int64_t, std::string>

  struct CompareCondition   { field, op, value }
  struct CrossesCondition   { path_name }
  struct ContainsKmerCondition { kmer }
  using ConditionExpr = std::variant<Compare, Crosses, ContainsKmer>

  struct WhereClause  { conditions[], connectors[] }
  struct FindQuery    { entity, where?, return_fields[] }
  struct SearchKmerQuery { kmer, return_fields[] }
  using GgqlQuery = std::variant<FindQuery, SearchKmerQuery>
}
```

Helpers utilitários: `entity_to_string()`, `op_to_string()`, `logical_op_to_string()`.

---

#### `include/ggql/query/ggql_grammar.hpp` — **NOVO**

Gramática PEG completa em templates PEGTL + API pública de parse.

**Regras PEGTL implementadas**:

| Regra | Descrição |
|---|---|
| `kw<S>` | Keyword com borda de palavra (`not_at<identifier_other>`) |
| `entity_type` | `SEGMENT \| EDGE \| PATH \| BUBBLE \| TIP \| REPEAT` |
| `op_sym` | `!= \| >= \| <= \| = \| > \| <` (ordem correta para PEG) |
| `where_field` / `return_field` | Identificador que não é keyword reservada |
| `int_value` / `str_value` | Valores numéricos e strings entre aspas |
| `fn_crosses` | `crosses(<identifier>)` |
| `fn_contains` | `contains_kmer("<string>")` |
| `condition_expr` | `<field> <op> <value>` |
| `condition` | `function_call \| condition_expr` |
| `where_clause` | `WHERE <cond> [AND\|OR <cond> ...]` |
| `return_clause` | `RETURN <field> [, <field> ...]` |
| `find_stmt` | `FIND <entity> [WHERE] [RETURN]` |
| `search_stmt` | `SEARCH KMER "<seq>" [RETURN]` |
| `query` | Regra raiz com `eof` |

**`ParseState`**: temporários por cláusula + acumuladores finais, sem `goto` ou estado global.

**Ações tipadas**: cada `action<Rule>` popula diretamente os tipos AST.

**API pública**:
```cpp
std::expected<GgqlQuery, std::string> parse_ggql(std::string_view input);
```

---

#### `src/parser/ggql_parser_dev.cpp` — **NOVO**

Driver de testes unitários do parser com 23 casos:

- 17 queries válidas (todas as entidades, todos os operadores, multi-linha, espaços extras)
- 6 rejeições esperadas (SQL, entidade inválida, query vazia, etc.)
- **Resultado**: 23/23 OK

---

#### CMakeLists.txt — **ATUALIZADO**

Adicionado target:
```cmake
add_executable(ggql_parser_dev src/parser/ggql_parser_dev.cpp)
target_link_libraries(ggql_parser_dev PRIVATE pegtl)
```

---

### Iteração 2 — QueryPlanner

**Situação inicial**: o `TODO.md` listava o QueryPlanner como próximo passo.
A gramática estava funcional mas nada executava contra os backends de dados.

---

#### `include/ggql/query/query_planner.hpp` — **NOVO**

Orquestrador que recebe `GgqlQuery` e a executa contra `GfaCsrTopology` e/ou SQLite.

**Tipos públicos**:
```cpp
using RowFields = std::unordered_map<std::string, Value>;
struct QueryResult { columns, rows, format_table() };
```

**Estratégias por entidade**:

| Entidade | Backend | Campos disponíveis |
|---|---|---|
| `TIP` | `topology_->find_tips()` | `id, in_degree, out_degree` |
| `REPEAT` | `topology_->find_repeats(2)` | `id, in_degree, out_degree` |
| `BUBBLE` | `topology_->find_bubbles()` | `source, sink, branch_count` |
| `SEGMENT` | SQLite (`segments`) | `id, name, length, read_count, depth` |
| `EDGE` / `PATH` | — | Erro "Phase 2" |
| `SEARCH KMER` | — | Erro "Phase 2 (FM-Index)" |

**Pushdown SQL para SEGMENT**:
- Se todas as condições WHERE são `CompareCondition` em campos conhecidos → tradução completa para SQL com parentesização correta preservando a semântica esquerda-para-direita.
- Se há `CrossesCondition`, `ContainsKmerCondition` ou campo desconhecido → full-scan + filtro em memória.

**Avaliação em memória** (`eval_where`):
- Esquerda-para-direita, sem precedência de operadores (igual à gramática GGQL)
- `compare_values()` suporta `int64_t × int64_t`, `string × string` e `int64_t × string` (conversão automática)

**`format_table()`**: tabela ASCII alinhada com `std::setw` para uso no REPL.

---

#### `src/ggql_engine_dev.cpp` — **NOVO**

Driver de integração completo: constrói topologia de teste em memória + banco SQLite temporário + executa queries reais.

**Topologia de teste** (5 nós, 5 arestas):
```
0 → 1 → 2 → 3    caminho principal
        ↘   ↑
         4 ─┘    ramo alternativo (cria bolha)

Tips   = {0, 3}
Repeat = {1}  (out_degree=2)
Bubble = source=1, sink=3, branches=[2, 4]
```

**Resultado**: **27/27 OK**

---

#### CMakeLists.txt — **ATUALIZADO**

```cmake
add_executable(ggql_engine_dev src/ggql_engine_dev.cpp)
target_link_libraries(ggql_engine_dev PRIVATE pegtl sdsl sqlite3 gomp pthread)
```

---

### Iteração 3 — REPL Interativo

**Situação inicial**: parser + planner funcionando, mas sem interface de usuário.

---

#### `src/repl/ggql_repl.cpp` — **NOVO**

Shell interativo completo para o GGQL Engine.

**Argumentos da linha de comando**:

| Argumento | Descrição |
|---|---|
| `--gfa <arquivo.gfa>` | Ingere GFA, constrói MPHF + topologia + SQLite |
| `--db  <arquivo.sqlite>` | Abre banco existente sem GFA (apenas SEGMENT) |
| `--threads N` | Threads para o ingest (padrão: 4) |
| `--help` | Exibe ajuda |

**Comandos internos**:

| Comando | Ação |
|---|---|
| `FIND … / SEARCH …` | Executa query GGQL e exibe resultado tabular |
| `.help` | Referência rápida da linguagem |
| `.stats` | Nós, segmentos, links, tempos de ingest detalhados |
| `.load <arquivo.gfa>` | Recarrega GFA em runtime (descarta dados anteriores) |
| `.quit` / `.exit` | Encerra o REPL |

**Comportamento de parse multi-linha**:
- Linha terminada com `;` → executa imediatamente
- `FIND`/`SEARCH` sem `;` → tenta parsear; se completo, executa; senão, acumula
- Linha em branco com buffer preenchido → tenta executar o buffer

**Exibição**: `format_table()` ASCII alinhado + tempo de execução em ms.

---

#### Correção: `gfa_metadata_store.hpp` — **BUG FIX**

**Problema**: `flush_segments()` usava `goto rollback` e chamava `sqlite3_errmsg()` 
**depois** do `ROLLBACK`, que reinicia o estado de erro no SQLite, resultando 
na mensagem enganosa `"not an error"`.

**Solução**: captura da mensagem de erro **antes** do `ROLLBACK`:
```cpp
// Antes (bug):
if (rc != SQLITE_DONE) goto rollback;
// ...
rollback:
  exec("ROLLBACK;");
  return std::unexpected("Erro: " + std::string(sqlite3_errmsg(db_)));  // já perdeu o erro

// Depois (correto):
if (rc != SQLITE_DONE) {
    const std::string err = sqlite3_errmsg(db_);  // captura ANTES
    exec("ROLLBACK;");
    return std::unexpected("Erro: " + err);
}
```

Adicionalmente: o REPL passa a remover o banco anterior antes de reingeri-lo
para evitar falha por violação de `PRIMARY KEY`.

---

#### Correção: `gfa_ingestor.hpp` — **FEATURE FIX**

**Problema**: arquivos GFA com `SEQ = *` (formato padrão em GFAs de pangenoma
como HPRC) armazenavam `length = 0` no SQLite, tornando qualquer query
`WHERE length > N` inútil.

A tag `LN:i:` contém o comprimento real nesses arquivos, mas não era lida.

**Solução**: extensão de `parse_optional_tags()` para capturar `LN:i:`:

```cpp
// Novo parâmetro out_ln adicionado à assinatura
static void parse_optional_tags(..., uint32_t& out_ln) noexcept {
    // ...
    } else if (sv[0] == 'L' && sv[1] == 'N' && type == 'i') {
        uint32_t v{};
        if (std::from_chars(...).ec == std::errc{}) out_ln = v;
    }
}

// Uso na passada de ingest:
uint32_t ln_tag = 0;
parse_optional_tags(tokens.data(), ntok, 2, rec.read_count, rec.depth, ln_tag);
rec.length = (tokens[1] == "*") ? ln_tag : static_cast<uint32_t>(tokens[1].size());
```

---

#### CMakeLists.txt — **ATUALIZADO**

```cmake
add_executable(ggql_repl src/repl/ggql_repl.cpp)
target_compile_options(ggql_repl PRIVATE -O2 -march=native -fopenmp)
target_link_libraries(ggql_repl PRIVATE pegtl sdsl sqlite3 gomp pthread tbb)
```

---

## Estrutura Final de Arquivos

```
ggql_engine/
├── CMakeLists.txt                          ← 4 targets: bench_ingest, ggql_parser_dev,
│                                              ggql_engine_dev, ggql_repl
├── include/
│   └── ggql/
│       ├── ingest/
│       │   ├── gfa_ingestor.hpp            ← MPHF + mmap parallel (LN:i fix)
│       │   ├── gfa_ingestor_serial.hpp     ← versão serial (referência)
│       │   └── gfa_metadata_store.hpp      ← SQLite bulk insert (goto fix)
│       ├── storage/
│       │   └── gfa_csr_topology.hpp        ← CSR bidirecional + algoritmos
│       └── query/                          ← NOVO (pasta criada nesta sessão)
│           ├── ggql_ast.hpp                ← tipos AST tipados
│           ├── ggql_grammar.hpp            ← gramática PEGTL + parse_ggql()
│           └── query_planner.hpp           ← execução contra os backends
├── src/
│   ├── ingest/
│   │   └── bench_ingest.cpp               ← benchmark multi-parâmetro
│   ├── parser/
│   │   ├── pegtl_parser.cpp               ← protótipo abandonado
│   │   ├── pegtl_parser2.cpp              ← protótipo original (referência)
│   │   └── ggql_parser_dev.cpp            ← NOVO: 23 testes do parser
│   ├── repl/
│   │   └── ggql_repl.cpp                  ← NOVO: REPL interativo completo
│   └── ggql_engine_dev.cpp                ← NOVO: 27 testes de integração
└── third_party/
    └── pegtl/                             ← PEGTL header-only
```

---

## Sintaxe GGQL Suportada

```
query ::= find_stmt | search_stmt

find_stmt   ::= FIND <entity> [WHERE <cond> [AND|OR <cond>]...] [RETURN <field>, ...]
search_stmt ::= SEARCH KMER "<seq>"           [RETURN <field>, ...]

entity  ::= SEGMENT | EDGE | PATH | BUBBLE | TIP | REPEAT
cond    ::= <field> <op> <value>
          | crosses(<path_name>)
          | contains_kmer("<seq>")
op      ::= = | != | < | > | <= | >=
value   ::= <integer> | "<string>"
```

**Campos por entidade**:

| Entidade | Campos |
|---|---|
| `TIP`, `REPEAT` | `id`, `in_degree`, `out_degree` |
| `BUBBLE` | `source`, `sink`, `branch_count` |
| `SEGMENT` | `id`, `name`, `length`, `read_count`, `depth` |

**Exemplos**:
```sql
FIND TIP WHERE in_degree = 0 RETURN id
FIND REPEAT WHERE out_degree >= 3 RETURN id, out_degree
FIND BUBBLE WHERE branch_count >= 2 RETURN source, sink
FIND SEGMENT WHERE length > 1000 AND depth >= 5 RETURN id, name, length
FIND SEGMENT WHERE name = "ctg001" RETURN id, length
SEARCH KMER "ACGTACGT" RETURN id, position
```

---

## Resultados de Testes

| Suite | Testes | Resultado |
|---|---|---|
| `ggql_parser_dev` | 23 (17 válidos + 6 rejeições) | ✅ 23/23 OK |
| `ggql_engine_dev` | 27 (TIP, REPEAT, BUBBLE, SEGMENT) | ✅ 27/27 OK |
| REPL com GFA 10k nós | Ingest + 5 queries ao vivo | ✅ OK |

---

## Pendências (Phase 2)

| Item | Arquivo alvo | Impacto |
|---|---|---|
| `FIND EDGE` | `query_planner.hpp` | Requer modelo de arestas no SQLite |
| `FIND PATH` | `query_planner.hpp` | Requer PathStore |
| `SEARCH KMER` | `query_planner.hpp` | Requer FM-Index |
| `crosses()` em memória | `query_planner.hpp` | Requer PathIndex |
| Paralelizar `find_tips()` / `find_repeats()` | `gfa_csr_topology.hpp` | `std::execution::par_unseq` |
| Substituir `unordered_set` em `find_bubbles()` | `gfa_csr_topology.hpp` | Bitmap `vector<bool>` |
| Pushdown parcial AND/OR misto em SEGMENT | `query_planner.hpp` | Performance em queries mistas |
| Guarda de ciclos em `find_bubbles()` | `gfa_csr_topology.hpp` | Correção de resultados incompletos |

---

## Sessão de Desenvolvimento — 27 de março de 2026

### Contexto

Integração do índice de haplótipos **gBWT** (libgbwt, Sirén et al. 2020) ao pipeline de ingest.
Objetivo: indexar W-lines / P-lines do GFA CHM13 (5 haplótipos × ~85 M nós) sem estouro de RAM.

---

### Iteração 1 — Migração para vgteam/sdsl-lite

**Problema:** libgbwt requer `sdsl/simple_sds.hpp` de `vgteam/sdsl-lite`, ausente no sdsl-lite padrão (sistema).

#### `third_party/sdsl-lite/` — **NOVO SUBMODULE**

Fork vgteam do SDSL adicionado como Git submodule.  
Fornece: `sdsl/simple_sds.hpp`, `sdsl/int_vector_buffer.hpp`, `sdsl/sd_vector.hpp` com `simple_sds_serialize`.

#### `third_party/sdsl-cmake/CMakeLists.txt` — **NOVO**

Wrapper CMake que compila `third_party/sdsl-lite/lib/*.cpp` como biblioteca estática `sdsl`:

```cmake
configure_file(${SDSL_DIR}/lib/structure_tree.cpp.cmake
               ${CMAKE_CURRENT_BINARY_DIR}/structure_tree.cpp @ONLY)
add_library(sdsl STATIC ${SDSL_SOURCES} ${...structure_tree.cpp})
target_include_directories(sdsl PUBLIC ${SDSL_DIR}/include)
target_link_libraries(sdsl PUBLIC divsufsort divsufsort64)
```

#### `CMakeLists.txt` — **ATUALIZADO**

```cmake
add_subdirectory(third_party/sdsl-cmake)  # antes do gbwt
add_subdirectory(third_party/gbwt)
```

---

### Iteração 2 — `GfaCsrGbwTopology`: Camada 2 (gBWT)

**Arquivo:** `include/ggql/storage/gfa_csr_gbw_topology.hpp`

#### 2.1 — Estrutura de dados Camada 2

Adicionados ao header:
```cpp
#include <gbwt/dynamic_gbwt.h>
#include <gbwt/gbwt.h>
#include <gbwt/algorithms.h>
#include <sdsl/int_vector_buffer.hpp>

// Membros privados:
std::unique_ptr<gbwt::GBWT>  gbwt_;
std::vector<std::string>     path_names_;
```

#### 2.2 — `build_path_index` — Overload original (GBWTBuilder)

Método `build_path_index(std::vector<PathRecord>)` usando `GBWTBuilder` com `batch_size` adaptado ao total real de símbolos. Descartado em produção após diagnóstico de OOM (ver 3.1), mantido como referência.

#### 2.3 — `build_path_index` — Overload streaming — **PRINCIPAL**

```cpp
void build_path_index(const std::string& text_path,
                      std::vector<std::string> names,
                      uint8_t node_width);
```

Lê nós de arquivo file-backed (`text_buffer_type = int_vector_buffer<0>`) em batches de 10 M nós. `PathRecord.nodes` nunca é materializado em RAM.

```cpp
gbwt::text_buffer_type text(text_path, std::ios::in, 1u<<20, node_width);
gbwt::DynamicGBWT dynamic;
dynamic.insert(text, /*batch=*/10'000'000, false);
gbwt_ = std::make_unique<gbwt::GBWT>(dynamic);
```

**Guard de overflow** (`GBWT_SAVE_MEMORY`):  
`GBWT_SAVE_MEMORY` usa `uint32_t` para `edge_type`/`run_type`. Para `num_nodes_ > 2^30` o `node_type` máximo excederia `2^31`. Adicionado:
```cpp
if (num_nodes_ > (size_t(1) << 30))
    throw std::overflow_error("num_nodes excede o limite de GBWT_SAVE_MEMORY...");
```

#### 2.4 — `sd_vector_builder(multiset=true)`

**Bug fix:** `build_csr()` usava `sd_vector_builder(universe, n)` sem `multiset=true`. Para nós sem arestas, o offset é zero em sequências consecutivas — o construtor do Elias-Fano standard não aceita valores repetidos. Adicionado o terceiro parâmetro `true`.

#### 2.5 — Consultas ao índice de paths

| Método | Descrição |
|---|---|
| `path_count()` | Número de paths indexados |
| `path_name(id)` | Nome do path `id` |
| `path_nodes(id)` | Reconstrói path via `gbwt::extract()` |
| `count_paths_through(span)` | GBWT forward search; retorna `state.size()` |
| `find_paths_through(span)` | GBWT `locate(state)`; retorna IDs das sequências |

---

### Iteração 3 — Diagnóstico e Correção de OOM

#### 3.1 — Root cause

Construção com `std::vector<PathRecord>` para CHM13:

```
5 paths × 85 M nós × 8 bytes = ~3,4 GB  ← PathRecord.nodes em RAM
DynamicGBWT durante construção BWT       ← ~8–15 GB adicionais
```
RAM disponível: 54 GB livres → sistema entrou em swap → OOM killer.

#### 3.2 — Solução: streaming via `text_buffer_type`

Nós escritos **diretamente para disco** durante o parsing (mutex-protected). DynamicGBWT lê em batches de 10 M nós → nunca mais de ~35 MB de buffer de leitura em RAM simultaneamente.

---

### Iteração 4 — `GfaIngestorMmapParallel`: Streaming de Paths

**Arquivo:** `include/ggql/ingest/gfa_ingestor.hpp`

#### 4.1 — `PassadaDoisResult` — Struct refatorada

```cpp
// ANTES:
struct PassadaDoisResult {
    IngestStatsParallel stats;
    std::vector<SegmentRecord> records;
    std::vector<LinkRecord> links;
    std::vector<PathRecord> paths;   // ← até 3,4 GB em RAM para CHM13
};

// DEPOIS:
struct PassadaDoisResult {
    IngestStatsParallel stats;
    std::vector<SegmentRecord> records;
    std::vector<LinkRecord> links;
    std::string              gbwt_tmp_path;   // arquivo temporário em /tmp
    std::vector<std::string> path_names;      // só nomes; nós em disco
    uint8_t                  gbwt_node_width; // bits/símbolo para o buffer
};
```

#### 4.2 — Parser W/P-line: escrita streaming

Durante o parsing paralelo, cada thread acumula os nós do path corrente num `vector<uint64_t>` local (descartado após a escrita). A escrita no arquivo compartilhado é atômica via mutex:

```cpp
if (!local_nodes.empty()) {
    std::lock_guard<std::mutex> lk(path_mutex);
    if (max_paths == 0 || paths_written < max_paths) {
        for (uint64_t v : local_nodes) gbwt_text_buf->push_back(v);
        gbwt_text_buf->push_back(gbwt::ENDMARKER);  // = 0
        path_names_collected.push_back(std::move(path_name));
        ++paths_written;
    }
}
```

O mutex é tomado raramente (≪1 % das linhas do GFA), não é gargalo.

#### 4.3 — Arquivo temporário via `mkstemp`

```cpp
char tpl[] = "/tmp/ggql_gbwt_XXXXXX";
int tfd = ::mkstemp(tpl);
::close(tfd);
gbwt_text_buf = std::make_unique<gbwt::text_buffer_type>(
    tpl, std::ios::out, 1024*1024, node_width);
```

`node_width` calculado como `bit_width((num_nodes-1)*2+1)` — idêntico ao encoding `(nid<<1)|orient_bit`.

#### 4.4 — `ingest()`: chamada à nova API

```cpp
// ANTES:
topology_->build_path_index(std::move(paths));

// DEPOIS:
topology_->build_path_index(gbwt_tmp, std::move(path_names), node_width);
::unlink(gbwt_tmp.c_str());  // remove arquivo tmp após uso
```

#### 4.5 — `process_data_parallel`: nova assinatura

Adicionado parâmetro `size_t max_paths = 0` para limitar a escrita no arquivo durante o parsing (evita escrever paths que seriam descartados depois).

---

### Iteração 5 — Análise da libgbwt + Correções pós-análise

#### 5.1 — Análise da implementação libgbwt

Resultado da leitura do código-fonte de `jltsiren/gbwt`:

| Componente | Detalhe relevante |
|---|---|
| `text_buffer_type` | `sdsl::int_vector_buffer<0>` — arquivo com header SDSL; `push_back` é a API correta de append |
| `GBWTBuilder::finish()` | Chama `index.recode()` internamente — **não** é necessário chamar explicitamente |
| `insert(text_buffer_type&)` | Cria `GBWTBuilder` internamente; lê o arquivo em lotes de `batch_size` nós |
| `GBWT_SAVE_MEMORY` | Ativo por padrão; usa `uint32_t` para `edge_type`, `run_type`, `sample_type`; limite: 2^30 nós |
| `DASamples` | DA sampling a cada 1024 posições; usa `sdsl::sd_vector<>` (Elias-Fano) |
| `FastLocate` | r-index alternativo; `r` amostras (início de cada run BWT) em vez de `n/1024` |
| Encoding de nó | `Node::encode(id, rev) = (id << 1) \| rev` — **idêntico** ao nosso `(nid<<1)\|orient_bit` |

#### 5.2 — Correções aplicadas

**C1 — `push_back` em vez de `operator[]`** (`gfa_ingestor.hpp`)

`(*buf)[buf->size()] = v` funcionava (o `write()` interno do SDSL estende `m_size` quando `idx == m_size`), porém era semanticamente opaco. Substituído por `buf->push_back(v)`.

**C2 — Guard de overflow `GBWT_SAVE_MEMORY`** (`gfa_csr_gbw_topology.hpp`)

Adicionado `throw std::overflow_error` quando `num_nodes_ > 2^30`, prevenindo corrupção silenciosa do GBWT em grafos futuros de maior escala.

**C3 — Truncação redundante em `ingest()` — removida**

O check `if (max_gbwt_paths < pnames.size()) pnames.resize(...)` não existia na versão final — a limitação já ocorre durante o parsing via `paths_written < max_paths`.

**C4 — Nomes únicos para W-lines** (`gfa_ingestor.hpp`)

O CHM13 contém múltiplos W-lines com o mesmo trio `(sample, haplotype, seqid)` cobrindo regiões distintas da contig. O nome anterior (`sample#haplotype#seqid`) era ambíguo.

```cpp
// ANTES — pode duplicar:
path_name = tokens[0] + '#' + tokens[1] + '#' + tokens[2];

// DEPOIS — canônico conforme GFA 1.1 spec:
path_name = tokens[0] + '#' + tokens[1] + '#' + tokens[2]
          + ':' + tokens[3] + '-' + tokens[4];  // tokens[3]=start, [4]=end
```

---

### Run 7 — Primeiro benchmark completo com gBWT streaming

**Data:** 27 de março de 2026  
**Comando:** `./bench_topology hprc-v1.0-mc-chm13.gfa 8 2.0 3`  
**Hardware:** 62 GB RAM, 8 threads

#### Pipeline

| Fase | Tempo |
|---|---|
| Passada 1 + MPHF | 30 170,5 ms |
| Passada 2 (links) | 132 407,0 ms |
| Build CSR + Elias-Fano | 7 217,8 ms |
| **Build gBWT streaming** | **16 068,7 ms** |
| **TOTAL** | **186 926,9 ms** |

#### Footprint de memória

| Componente | Tamanho |
|---|---|
| E_fwd + E_bwd (118 M × 8 B × 2) | 1 802,6 MB |
| offsets_fwd + offsets_bwd (85 M × 4 B × 2) | 650,6 MB |
| ef_fwd + ef_bwd (Elias-Fano) | ~65,0 MB |
| **TOTAL `memory_bytes()`** | **2 763,4 MB** |
| ΔRSS processo | 3 055,9 MB |

#### Fase 9 — gBWT

| Métrica | Valor |
|---|---|
| Paths indexados | 5 |
| `count_paths_through` (5 nós quentes) | < 0,01 ms avg |
| `find_paths_through` (5 nós quentes) | < 0,01 ms avg |

> **Nota:** nomes dos 5 paths aparecem duplicados na saída do Run 7 porque o campo `:start-end` ainda não estava presente naquela execução (corrigido em C4 após o run).

#### Latências por operação

| Operação | ns/op |
|---|---|
| `out_degree()` | 8,36 |
| `in_degree()` | 7,49 |
| `out_neighbors()` | 7,79 |

---

### Estrutura de arquivos alterados

| Arquivo | Tipo de alteração |
|---|---|
| `third_party/sdsl-cmake/CMakeLists.txt` | **NOVO** — wrapper build vgteam sdsl |
| `third_party/sdsl-lite/` | **NOVO** — submodule vgteam/sdsl-lite |
| `CMakeLists.txt` | Atualizado — `add_subdirectory` sdsl-cmake + gbwt |
| `include/ggql/storage/gfa_csr_gbw_topology.hpp` | Adicionados Camada 2 gBWT, `PathRecord`, `build_path_index` (dois overloads), consultas, guard overflow |
| `include/ggql/ingest/gfa_ingestor.hpp` | `PassadaDoisResult` refatorado, parser streaming, `mkstemp`, `push_back`, nomes W-line corrigidos |
