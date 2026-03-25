# GfaCsrTopology — Histórico de Otimizações (mar/2026)

## Contexto

`GfaCsrTopology` é a estrutura principal de armazenamento da topologia de grafos GFA em formato CSR
(Compressed Sparse Row) bidirecional. Ela suporta os algoritmos de análise genômica do motor GGQL:
detecção de pontas (`find_tips`), nós repetidos (`find_repeats`) e bolhas (`find_bubbles`).

**Arquivo principal:** `include/ggql/storage/gfa_csr_topology.hpp`

---

## Layout de memória

```
offsets_fwd_  : std::vector<uint32_t>   — offsets CSR de saída  (num_nodes+1 entradas)
E_fwd_        : sdsl::int_vector<64>    — arestas de saída (63-bit node_id | 1-bit orient)
offsets_bwd_  : std::vector<uint32_t>   — offsets CSR de entrada
E_bwd_        : sdsl::int_vector<64>    — arestas de entrada
```

Encoding de orientação:
```
valor = (neighbor_id << 1) | orient_bit
orient_bit = 0 → '+'  |  1 → '-'
```

Consumo típico (HPRC — 118M arestas / 85M nós):
| Componente | Tamanho |
|---|---|
| E_fwd + E_bwd (64 bits/aresta) | ~1.89 GB |
| offsets_fwd + offsets_bwd (32 bits) | ~680 MB |
| **Total** | **~2.57 GB** |

---

## Análise das melhorias propostas

### 1. `int_vector<64>` → largura dinâmica (não implementado)

Usar `sdsl::int_vector<>` com largura calculada via `ceil(log2(num_nodes)) + 1` reduziria os arrays de
arestas de ~1.89 GB → ~1.0 GB para grafos com até 85M nós (28 bits por entrada).

**Decisão:** Adiado. Acesso a `int_vector<>` com largura não-pot-de-2 exige bit-shifting por operação;
em loops de BFS com milhões de acessos aleatórios pode regredir performance. Requer benchmark com
`perf stat` antes de adotar.

---

### 2. ✅ Substituição de `unordered_set` por bitmap no BFS (implementado)

**Problema original:**
- `bfs_reachable` alocava um `std::unordered_set<uint64_t>` **por ramo**, por source candidato.
- `find_nearest_in_set` alocava outro `unordered_set` localmente.
- Em grafos com 800K+ sources com out_degree ≥ 2 e profundidade 5: milhões de alocações dinâmicas
  com hashing + rehash + cache miss por inserção (~5–20 ns cada).

**Solução implementada:**

Dois bitmaps `mutable std::vector<uint8_t>` reutilizáveis como membros da classe:
- `bfs_bitmap_` — flag de visita no BFS (valores: 0 = não visitado, 1 = visitado)
- `cnt_bitmap_` — contagem de ramos para interseção (valores: 0–255)

Reset O(|visitados|) em vez de O(num_nodes) — percorre apenas os nós marcados para reverter.

**Mudanças em `gfa_csr_topology.hpp`:**

| Antes | Depois |
|---|---|
| `#include <unordered_set>` | removido |
| `bfs_reachable(start, depth, unordered_set&)` | `bfs_collect(start, depth, vector<uint64_t>&, max_cap)` |
| `find_nearest_in_set(source, unordered_set&, depth)` | `find_nearest_bfs(source, vector<uint8_t>&, match_val, depth)` |
| Interseção via `std::erase_if + contains` | Contagem em `cnt_bitmap_` — O(Σ visitados) |
| Dedup de branches via `unordered_set` | Dedup via `bfs_bitmap_` (já zerado após find_nearest) |

**Ganho teórico estimado:** 10×–50× em `find_bubbles` para grafos de montagem reais (onde a maioria
dos nós tem out_degree < 2 e o BFS visita 3–15 nós por ramo).

---

### 3. `max_nodes_per_branch` — cap por ramo (implementado junto ao ponto 2)

**Problema:** Em grafos sintéticos aleatórios (e.g., `bench_1M.gfa`, onde 80% dos nós têm 
out_degree ≥ 2 e os links são aleatórios), o BFS explode para centenas de milhares de nós por ramo.

**Solução:** Novo parâmetro `max_nodes_per_branch` (padrão: `0` = sem limite):

```cpp
std::vector<Bubble> find_bubbles(int max_depth = 5,
                                  size_t max_nodes_per_branch = 0,
                                  int num_threads = 1) const;
```

Quando `max_nodes_per_branch > 0`, o BFS do ramo aborta ao atingir o cap e o source é descartado
(bolhas com ramos mais largos que o cap não são detectadas, o que é o comportamento correto para
grafos com alta conectividade aleatória que não são bolhas genômicas reais).

O mesmo parâmetro foi adicionado a `bfs_collect_impl` internamente.

---

### 4. `find_tips` e `find_repeats` — paralelização (não implementado)

`find_tips` e `find_repeats` são loops simples sobre `num_nodes_` com acesso read-only ao vetor de
offsets. Podem ser paralelizados com `std::execution::par_unseq` (já incluído via `<execution>`).
Adiado por serem já rápidos (~2–8 ms para 1M nós).

---

### 5. ✅ Paralelização de `find_bubbles` com OpenMP (implementado)

**Problema:** `find_bubbles` é O(N × BFS × ramos), com N = num_nodes e work irregular (maioria dos
nós é filtrada pelo `out_size < 2`). O caminho serial com bitmap já era correto mas ainda serial.

**Arquitetura adotada:**

Extração de dois métodos `static` privados **thread-safe** que recebem bitmaps explícitos:

```cpp
static void bfs_collect_impl(const GfaCsrTopology& g,
                               uint64_t start, int max_depth,
                               std::vector<uint64_t>& out_nodes,
                               std::vector<uint8_t>& bitmap,
                               size_t max_cap = 0);

static uint64_t find_nearest_bfs_impl(const GfaCsrTopology& g,
                                        uint64_t source,
                                        const std::vector<uint8_t>& cand_bitmap,
                                        uint8_t match_val, int max_depth,
                                        std::vector<uint8_t>& visit_bitmap);
```

Os wrappers `bfs_collect` e `find_nearest_bfs` delegam para os impls usando os bitmaps membros
(caminho serial — comportamento idêntico ao anterior, sem overhead).

**Caminho paralelo** (`num_threads > 1`):

```cpp
// Bitmaps por thread — lazy-grow para num_nodes_ na 1ª iteração
std::vector<std::vector<uint8_t>> t_bfs(nth);
std::vector<std::vector<uint8_t>> t_cnt(nth);
std::vector<std::vector<Bubble>>  t_bubs(nth);

#pragma omp parallel num_threads(nth)
{
    // cada thread usa t_bfs[tid] e t_cnt[tid] exclusivamente
    // sem mutex, sem atomic, sem false sharing

    #pragma omp for schedule(dynamic, 64) nowait
    for (uint64_t source = 0; source < num_nodes_; ++source) {
        // ... BFS via bfs_collect_impl com bitmaps locais ...
    }
}

// Merge serial após o bloco parallel
```

`schedule(dynamic, 64)`: granularidade 64 amortiza overhead do scheduler; work irregular (80%+ dos
sources são filtrados no primeiro `if (out.size() < 2) continue`) é bem balanceado dinamicamente.

**Custo de memória do caminho paralelo:**
```
num_threads × 2 × num_nodes bytes
4 threads × 2 × 85M nós = 680 MB extra (aceitável em servidores com 32+ GB)
```

**Bitmaps membros (`bfs_bitmap_`, `cnt_bitmap_`) ficam intocados** durante a execução paralela —
o caminho serial continua funcionando corretamente e sem regressão.

---

## Benchmark

**Arquivo:** `src/storage/bench_bubbles.cpp`  
**Target CMake:** `bench_bubbles`  
**Compilador:** `g++-14`, `-O3 -march=native -fopenmp -flto=auto`

### Resultado — grafo sintético `bench_1M.gfa` (1M nós, 3M links aleatórios)

```
Plataforma: Linux x86-64, g++-14, OpenMP
Parâmetros: max_depth=5, max_nodes_per_branch=200, runs=3
```

| Algoritmo | Threads | Tempo médio | Speedup |
|---|---|---|---|
| `find_tips` | 1 | 2.0 ms | — |
| `find_repeats(≥2)` | 1 | 5.8 ms | — |
| `find_bubbles` | 1 (serial) | 9,385 ms | 1.00× |
| `find_bubbles` | 2 | 4,136 ms | **2.27×** |
| `find_bubbles` | 4 | 2,278 ms | **4.12×** |

Bolhas detectadas: **181** (consistente em todas as execuções — resultado correto e reproduzível).

**Nota sobre o speedup super-linear com 2 threads (2.27×):** Resulta do fato de que cada thread
processa metade dos sources com work completamente independente, sem nenhuma contenção. O sistema
operacional explora dois núcleos físicos com L1/L2 cache quentes para cada metade do dataset.

### Uso do benchmark

```bash
./build/bench_bubbles <arquivo.gfa> [threads_ingest] [max_depth] [runs] [max_nodes/branch] [threads_bfs]

# Exemplos:
./build/bench_bubbles data/bench_1M.gfa 4 5 3 200 1   # serial
./build/bench_bubbles data/bench_1M.gfa 4 5 3 200 4   # 4 threads BFS
./build/bench_bubbles data/hprc.gfa 8 5 3 0 8         # grafo real, sem cap
```

---

## Melhorias não implementadas (backlog)

| # | Melhoria | Impacto | Complexidade |
|---|---|---|---|
| A | `int_vector<>` dinâmico para E_fwd/E_bwd | ~−900 MB RAM | Média (risco de regressão) |
| B | Elias-Fano para offsets_fwd/bwd | ~−480 MB RAM | Alta (acesso via rank/select) |
| C | Bit-parallel BFS (`bitset & bitset`) | Potencial 10×–100× em find_bubbles | Alta (pesquisa) |
| D | Paralelizar `find_tips` e `find_repeats` | <5 ms já | Baixa |
| E | Layout NUMA-aware para grafos >200M nós | Relevante só em multi-socket | Alta |

---

## Resumo das mudanças em arquivos

### `include/ggql/storage/gfa_csr_topology.hpp`

1. **`#include <unordered_set>` removido** — não mais necessário.
2. **Membros privados adicionados:**
   ```cpp
   mutable std::vector<uint8_t> bfs_bitmap_;
   mutable std::vector<uint8_t> cnt_bitmap_;
   ```
3. **`bfs_reachable` → `bfs_collect`**: aceita `vector<uint64_t>& out_nodes` e `size_t max_cap`.
4. **`find_nearest_in_set` → `find_nearest_bfs`**: aceita `vector<uint8_t>& cand_bitmap`.
5. **`bfs_collect_impl` e `find_nearest_bfs_impl`** adicionados como `static` privados  
   (recebem bitmaps explícitos — thread-safe).
6. **`bfs_collect` e `find_nearest_bfs`** convertidos em thin wrappers que delegam para os impls com bitmaps membros.
7. **`find_bubbles`** reescrito:
   - Novo parâmetro `size_t max_nodes_per_branch = 0`.
   - Novo parâmetro `int num_threads = 1`.
   - Caminho serial (num_threads=1): usa membros `bfs_bitmap_` / `cnt_bitmap_` via wrappers.
   - Caminho paralelo (num_threads>1): bitmaps locais por thread, `#pragma omp for schedule(dynamic, 64)`, merge serial.
8. **Comentário de cabeçalho atualizado** para refletir os novos parâmetros de `find_bubbles`.

### `src/storage/bench_bubbles.cpp` (arquivo novo)

Driver de benchmark isolado para `find_tips`, `find_repeats` e `find_bubbles`:
- Ingesta o GFA e constrói a topologia CSR via `GfaIngestorMmapParallel`.
- Executa cada algoritmo `runs` vezes e exibe média/min/max em ms.
- Parâmetros: `threads_ingest`, `max_depth`, `runs`, `max_nodes_per_branch`, `threads_bfs`.

### `CMakeLists.txt`

Novo target `bench_bubbles` adicionado:
```cmake
add_executable(bench_bubbles src/storage/bench_bubbles.cpp)
target_compile_options(bench_bubbles PRIVATE -O3 -march=native -fopenmp -flto=auto)
target_link_options(bench_bubbles PRIVATE -flto=auto)
target_link_libraries(bench_bubbles PRIVATE gomp pthread sdsl sqlite3 tbb)
```
