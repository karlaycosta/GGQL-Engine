#pragma once

// GfaCsrTopology — topologia GFA em CSR bidirecional.
//
// Layout de memória:
//   offsets_fwd_ : std::vector<uint32_t>  — offsets CSR de saída  (num_nodes+1 entradas)
//   E_fwd_       : sdsl::int_vector<64>   — arestas de saída (63-bit node_id | 1-bit orient)
//   offsets_bwd_ : std::vector<uint32_t>  — offsets CSR de entrada (num_nodes+1 entradas)
//   E_bwd_       : sdsl::int_vector<64>   — arestas de entrada (mesmo encoding)
//
// Encoding de orientação no bit 0 de cada valor em E_*:
//   valor = (neighbor_id << 1) | orient_bit
//   orient_bit = 0 → '+',  1 → '-'
//
// Consumo de memória típico para 118 M arestas / 85 M nós:
//   E_fwd/E_bwd        ≈ 2 × 118M × 8 B ≈ 1.89 GB
//   offsets_fwd/bwd    ≈ 2 × 85M  × 4 B ≈  680 MB
//   Total              ≈ 2.57 GB
//
// API principal:
//   build(num_nodes, links)              — constrói a partir de lista de links
//   out_degree(node) / in_degree(node)   — grau de saída / entrada
//   out_neighbors(node)                  — span dos valores de saída codificados
//   in_neighbors(node)                   — span dos valores de entrada codificados
//   decode(encoded_value)                — decodifica → EdgeRef{node_id, orient}
//   find_tips()                          — nós com grau_in=0 ou grau_out=0
//   find_repeats(min_degree)             — nós com grau > min_degree
//   find_bubbles(max_depth)              — detecção de bolhas (BFS + interseção)
//   memory_bytes()                       — uso total de memória

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <execution>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <sdsl/int_vector.hpp>

namespace ggql {

// ── LinkRecord: link bruto coletado no ingest — 16 bytes (sem padding) ────
//
// Orientações empacotadas no bit 0 de cada campo (mesmo encoding que E_fwd_):
//   from_packed = (from_id << 1) | (from_orient == '-' ? 1 : 0)
//   to_packed   = (to_id   << 1) | (to_orient   == '-' ? 1 : 0)
//
// 16 bytes vs 32 bytes do layout with chars = 50% menos tráfego de memória
// no radix sort (crítico: 4 passes × 2 CSRs = 8 varreduras completas do vetor).
struct LinkRecord {
  uint64_t from_packed; // (from_id << 1) | orient_bit
  uint64_t to_packed;   // (to_id   << 1) | orient_bit

  static LinkRecord make(uint64_t from_id, char from_orient,
                         uint64_t to_id,   char to_orient) noexcept {
    return { (from_id << 1) | (from_orient == '-' ? 1u : 0u),
             (to_id   << 1) | (to_orient   == '-' ? 1u : 0u) };
  }

  uint64_t from_id()     const noexcept { return from_packed >> 1; }
  char     from_orient() const noexcept { return (from_packed & 1u) ? '-' : '+'; }
  uint64_t to_id()       const noexcept { return to_packed >> 1; }
  char     to_orient()   const noexcept { return (to_packed & 1u) ? '-' : '+'; }

  // Inverte from↔to (para construção do CSR reverso)
  LinkRecord reversed() const noexcept { return { to_packed, from_packed }; }
};

// ── EdgeRef: arestas decodificadas para uso nos algoritmos ────────────────
struct EdgeRef {
  uint64_t node_id;
  char     orient; // '+' ou '-'
};

// ── Bubble: resultado de find_bubbles ─────────────────────────────────────
struct Bubble {
  uint64_t              source;
  uint64_t              sink;
  std::vector<uint64_t> branches; // nós intermediários em qualquer ramo
};

// ── GfaCsrTopology ────────────────────────────────────────────────────────
class GfaCsrTopology {
public:
  GfaCsrTopology() = default;

  // ── Construção ────────────────────────────────────────────────────────────
  //
  // links deve conter todos os links do GFA já com IDs MPHF resolvidos.
  // num_nodes = mphf_->nbKeys() (IDs válidos: [0, num_nodes)).
  //
  // A função realiza sort internamente — não requer entrada pré-ordenada.
  void build(uint64_t num_nodes, std::vector<LinkRecord> links, int num_threads = 1) {
    num_nodes_ = num_nodes;

    // ── Forward CSR ───────────────────────────────────────────────────────
    build_csr(links, E_fwd_, offsets_fwd_, num_threads);

    // ── Backward CSR (inverte from↔to via reversed()) ────────────────────
    for (auto& lk : links) lk = lk.reversed();
    build_csr(links, E_bwd_, offsets_bwd_, num_threads);
  }

  // ── Consultas de grau ─────────────────────────────────────────────────────
  uint64_t out_degree(uint64_t node) const noexcept {
    if (node >= num_nodes_ || offsets_fwd_.empty()) return 0;
    return static_cast<uint64_t>(offsets_fwd_[node + 1]) - offsets_fwd_[node];
  }

  uint64_t in_degree(uint64_t node) const noexcept {
    if (node >= num_nodes_ || offsets_bwd_.empty()) return 0;
    return static_cast<uint64_t>(offsets_bwd_[node + 1]) - offsets_bwd_[node];
  }

  // ── Iteração sobre vizinhos ───────────────────────────────────────────────
  // Retorna span de valores codificados: use decode() para obter EdgeRef.
  std::span<const uint64_t> out_neighbors(uint64_t node) const noexcept {
    auto [lo, hi] = edge_range_fwd(node);
    if (hi <= lo) return {};
    return { E_fwd_.data() + lo, hi - lo };
  }

  std::span<const uint64_t> in_neighbors(uint64_t node) const noexcept {
    auto [lo, hi] = edge_range_bwd(node);
    if (hi <= lo) return {};
    return { E_bwd_.data() + lo, hi - lo };
  }

  // ── Decodificação ─────────────────────────────────────────────────────────
  static EdgeRef decode(uint64_t encoded) noexcept {
    return { encoded >> 1, (encoded & 1u) ? '-' : '+' };
  }

  // ── Algoritmos de análise de grafo ────────────────────────────────────────

  // Retorna IDs de nós "ponta" (tip): in_degree=0 OU out_degree=0
  std::vector<uint64_t> find_tips() const {
    std::vector<uint64_t> tips;
    for (uint64_t i = 0; i < num_nodes_; ++i) {
      if (in_degree(i) == 0 || out_degree(i) == 0) {
        tips.push_back(i);
      }
    }
    return tips;
  }

  // Retorna IDs de nós "repeat": out_degree >= min_degree
  std::vector<uint64_t> find_repeats(uint64_t min_degree = 2) const {
    std::vector<uint64_t> repeats;
    for (uint64_t i = 0; i < num_nodes_; ++i) {
      if (out_degree(i) >= min_degree) {
        repeats.push_back(i);
      }
    }
    return repeats;
  }

  // Detecção de bolhas por BFS + interseção de conjuntos.
  //
  // Algoritmo (idêntico ao validado no Dart/C++ prototype):
  //   Para cada nó `source` com out_degree >= 2:
  //     Expande BFS a partir de cada vizinho direto até profundidade max_depth.
  //     Qualquer nó que apareça em TODOS os conjuntos de alcançabilidade é um
  //     `sink` candidato.
  //     O menor sink (por ordem de BFS) é selecionado como o sink da bolha.
  std::vector<Bubble> find_bubbles(int max_depth = 5) const {
    std::vector<Bubble> bubbles;

    for (uint64_t source = 0; source < num_nodes_; ++source) {
      auto out = out_neighbors(source);
      if (out.size() < 2) continue;

      // BFS a partir de cada vizinho direto
      std::vector<std::unordered_set<uint64_t>> reachable(out.size());
      for (size_t b = 0; b < out.size(); ++b) {
        auto [start, _orient] = decode(out[b]);
        bfs_reachable(start, max_depth, reachable[b]);
      }

      // Interseção de todos os conjuntos
      std::unordered_set<uint64_t> common = reachable[0];
      for (size_t b = 1; b < reachable.size(); ++b) {
        std::erase_if(common, [&](uint64_t n) {
          return !reachable[b].contains(n);
        });
      }

      if (common.empty()) continue;

      // Encontra o sink mais próximo via BFS a partir do source
      uint64_t sink = find_nearest_in_set(source, common, max_depth * 2);
      if (sink == UINT64_MAX) continue;

      // Coleta nós intermediários (union de todos os conjuntos, excluindo
      // source e sink)
      std::unordered_set<uint64_t> all_between;
      for (auto& rs : reachable) all_between.insert(rs.begin(), rs.end());
      all_between.erase(source);
      all_between.erase(sink);

      Bubble bub;
      bub.source   = source;
      bub.sink     = sink;
      bub.branches = std::vector<uint64_t>(all_between.begin(), all_between.end());
      bubbles.push_back(std::move(bub));
    }

    return bubbles;
  }

  // ── Estatísticas de memória ────────────────────────────────────────────────
  size_t memory_bytes() const noexcept {
    auto vec_bytes = [](const std::vector<uint32_t>& v) {
      return v.size() * sizeof(uint32_t);
    };
    return sdsl::size_in_bytes(E_fwd_) + vec_bytes(offsets_fwd_)
         + sdsl::size_in_bytes(E_bwd_) + vec_bytes(offsets_bwd_);
  }

  uint64_t num_nodes() const noexcept { return num_nodes_; }
  uint64_t num_edges() const noexcept { return E_fwd_.size(); }

private:
  uint64_t num_nodes_ = 0;

  // Arrays de arestas (encoding: bit0 = orientação, bits 1..63 = node_id)
  sdsl::int_vector<64> E_fwd_;  // arestas de saída
  sdsl::int_vector<64> E_bwd_;  // arestas de entrada

  // Offsets CSR: offsets_fwd_[i] = 1ª aresta de saída do nó i
  //              offsets_fwd_[num_nodes_] = total de arestas (sentinela)
  // uint32_t é suficiente: num_edges <= 118M << 2^32
  std::vector<uint32_t> offsets_fwd_;
  std::vector<uint32_t> offsets_bwd_;

  // ── Helpers ───────────────────────────────────────────────────────────────

  // LSD radix sort paralelo de LinkRecord por from_id — O(n/p) por passo.
  //
  // Algoritmo (4 passes de 8 bits, OpenMP):
  //   1. Cada thread calcula histograma local do seu chunk          O(n/p)
  //   2. Prefix-sum global: computa offset de scatter por thread    O(256 × p)
  //   3. Cada thread faz scatter independente usando seu offset     O(n/p)
  //
  // A fase de scatter é completamente paralela porque os offsets por thread
  // são pré-calculados antes do scatter (sem contenção de escrita).
  static void radix_sort_by_from(std::vector<LinkRecord>& links, int num_threads = 1) {
    const size_t n = links.size();
    if (n < 2) return;

    // Determina quantos passes de 8 bits são necessários (ordena por from_id)
    uint64_t max_val = 0;
    for (auto& lk : links) if (lk.from_id() > max_val) max_val = lk.from_id();
    int n_passes = 0;
    for (uint64_t v = max_val; v; v >>= 8) ++n_passes;
    if (n_passes == 0) n_passes = 1;

    std::vector<LinkRecord> buf(n);
    LinkRecord* src = links.data();
    LinkRecord* dst = buf.data();

    const size_t chunk = (n + static_cast<size_t>(num_threads) - 1)
                         / static_cast<size_t>(num_threads);

    // Histogramas por thread: lhist[tid][digit]
    std::vector<std::array<uint32_t, 256>> lhist(
        static_cast<size_t>(num_threads));

    for (int pass = 0; pass < n_passes; ++pass) {
      const int shift = pass * 8;

      // ── 1. Histograma paralelo ───────────────────────────────────────────
#pragma omp parallel num_threads(num_threads)
      {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        size_t lo = static_cast<size_t>(tid) * chunk;
        size_t hi = std::min(lo + chunk, n);
        auto& h = lhist[static_cast<size_t>(tid)];
        h.fill(0);
        for (size_t i = lo; i < hi; ++i)
          h[(src[i].from_id() >> shift) & 0xFFu]++;
      }

      // ── 2. Prefix-sum global: total por dígito + offset por (tid, digit) ─
      // starts[tid][digit] = posição inicial de scatter do thread tid no bucket digit
      std::vector<std::array<uint32_t, 256>> starts(
          static_cast<size_t>(num_threads));

      std::array<uint32_t, 256> global_pfx{};
      {
        std::array<uint32_t, 256> total{};
        total.fill(0);
        for (int t = 0; t < num_threads; ++t)
          for (int d = 0; d < 256; ++d)
            total[static_cast<size_t>(d)] +=
                lhist[static_cast<size_t>(t)][static_cast<size_t>(d)];

        uint32_t pfx = 0;
        for (int d = 0; d < 256; ++d) {
          global_pfx[static_cast<size_t>(d)] = pfx;
          pfx += total[static_cast<size_t>(d)];
        }
      }
      for (int d = 0; d < 256; ++d) {
        uint32_t acc = global_pfx[static_cast<size_t>(d)];
        for (int t = 0; t < num_threads; ++t) {
          starts[static_cast<size_t>(t)][static_cast<size_t>(d)] = acc;
          acc += lhist[static_cast<size_t>(t)][static_cast<size_t>(d)];
        }
      }

      // ── 3. Scatter paralelo ──────────────────────────────────────────────
#pragma omp parallel num_threads(num_threads)
      {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        size_t lo = static_cast<size_t>(tid) * chunk;
        size_t hi = std::min(lo + chunk, n);
        // Cópia local dos offsets — sem false sharing com outras threads
        auto offsets = starts[static_cast<size_t>(tid)];
        for (size_t i = lo; i < hi; ++i) {
          uint8_t digit = (src[i].from_id() >> shift) & 0xFFu;
          dst[offsets[digit]++] = src[i];
        }
      }

      std::swap(src, dst);
    }

    // Após número ímpar de passes, resultado em buf — copia de volta
    if (src != links.data())
      std::copy(src, src + n, links.data());
  }

  // Constrói E e offsets a partir de links (modifica in-place via sort).
  void build_csr(
      std::vector<LinkRecord>& links,
      sdsl::int_vector<64>& out_E,
      std::vector<uint32_t>& out_offsets,
      int num_threads = 1)
  {
    const uint64_t m = links.size();

    out_offsets.assign(num_nodes_ + 1, 0u);

    if (m == 0) {
      out_E = sdsl::int_vector<64>(0);
      return;
    }

    // Ordena por from_id para layout CSR — LSD radix sort paralelo O(n/p)
    radix_sort_by_from(links, num_threads);

    // Conta arestas por nó
    for (auto& lk : links) {
      out_offsets[lk.from_id() + 1]++;
    }

    // Prefix-sum
    for (uint64_t i = 1; i <= num_nodes_; ++i) {
      out_offsets[i] += out_offsets[i - 1];
    }

    // Preenche E — to_packed já tem o encoding (to_id<<1|orient_bit)
    out_E = sdsl::int_vector<64>(m);
    for (uint64_t i = 0; i < m; ++i) {
      out_E[i] = links[i].to_packed;
    }
  }

  std::pair<uint64_t, uint64_t> edge_range_fwd(uint64_t node) const noexcept {
    if (node >= num_nodes_ || offsets_fwd_.empty()) return {0, 0};
    return { static_cast<uint64_t>(offsets_fwd_[node]),
             static_cast<uint64_t>(offsets_fwd_[node + 1]) };
  }

  std::pair<uint64_t, uint64_t> edge_range_bwd(uint64_t node) const noexcept {
    if (node >= num_nodes_ || offsets_bwd_.empty()) return {0, 0};
    return { static_cast<uint64_t>(offsets_bwd_[node]),
             static_cast<uint64_t>(offsets_bwd_[node + 1]) };
  }

  // BFS limitado a max_depth a partir de `start`, preenche `visited`.
  void bfs_reachable(uint64_t start, int max_depth,
                     std::unordered_set<uint64_t>& visited) const {
    std::vector<std::pair<uint64_t, int>> queue;
    queue.reserve(64);
    queue.push_back({start, 0});
    visited.insert(start);

    for (size_t qi = 0; qi < queue.size(); ++qi) {
      auto [node, depth] = queue[qi];
      if (depth >= max_depth) continue;
      for (uint64_t enc : out_neighbors(node)) {
        auto [nb, _orient] = decode(enc);
        if (visited.insert(nb).second) {
          queue.push_back({nb, depth + 1});
        }
      }
    }
  }

  // Retorna o nó mais próximo de `source` (BFS) que esteja em `candidates`.
  uint64_t find_nearest_in_set(uint64_t source,
                                const std::unordered_set<uint64_t>& candidates,
                                int max_depth) const {
    std::vector<std::pair<uint64_t, int>> queue;
    std::unordered_set<uint64_t> visited;
    queue.push_back({source, 0});
    visited.insert(source);

    for (size_t qi = 0; qi < queue.size(); ++qi) {
      auto [node, depth] = queue[qi];
      if (depth > max_depth) continue;
      if (node != source && candidates.contains(node)) return node;
      for (uint64_t enc : out_neighbors(node)) {
        auto [nb, _orient] = decode(enc);
        if (visited.insert(nb).second) {
          queue.push_back({nb, depth + 1});
        }
      }
    }
    return UINT64_MAX;
  }
};

} // namespace ggql
