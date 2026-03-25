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

  // Detecção de superbubbles locais por BFS conjunto por nível.
  //
  // Algoritmo (corrigido em relação à implementação anterior):
  //
  //   Para cada source com out_degree >= 2:
  //     1. Executa BFS independente por ramo, nível a nível, com cap
  //        max_nodes_per_branch (padrão 1024) para evitar explosão em grafos
  //        densos. Cada nó visitado recebe profundidade mínima por ramo.
  //     2. Um nó n é candidato a sink se:
  //          a. cnt_bitmap_[n] == num_branches  (alcançado por todos os ramos)
  //          b. depth(n) <= max_depth           (localidade: sink próximo)
  //          c. Não é a própria source
  //     3. Do conjunto de candidatos, seleciona o de menor profundidade máxima
  //        entre os ramos (sink mais "justo" em todos os caminhos).
  //     4. Valida disjunção: nenhum nó interno ao subgrafo
  //        (source, branches, sink) deve aparecer em mais de um ramo
  //        entre source e sink — se aparecer é repeat/cycle, não bolha.
  //     5. Valida fechamento: nenhum nó interno deve ter aresta de saída para
  //        fora do subgrafo {internos ∪ {sink}} (sem "escapes").
  //
  // Correções em relação à versão anterior:
  //   - Elimina segundo BFS (find_nearest_bfs) — sink é determinado na fase 2.
  //   - Interseção simétrica sobre todos os ramos (não apenas reachable[0]).
  //   - max_nodes_per_branch com default 1024 (em vez de 0=ilimitado).
  //   - Critério de localidade: depth(sink) <= max_depth.
  //   - Verificação de disjunção de ramos (condição de superbubble real).
  //   - Verificação de fechamento (sem "escapes" do subgrafo).
  //
  // Limitação: num_branches por nó <= 254 (grafos reais: tipicamente < 10).
  //
  // num_threads > 1: ativa caminho paralelo OpenMP.
  //   Custo extra: num_threads × 2 × num_nodes bytes de bitmaps por thread.
  std::vector<Bubble> find_bubbles(int max_depth = 5,
                                   size_t max_nodes_per_branch = 1024,
                                   int num_threads = 1) const {
    std::vector<Bubble> bubbles;
    if (num_nodes_ == 0) return bubbles;

#ifndef _OPENMP
    num_threads = 1;
#endif

    if (num_threads > 1) {
      // ── Caminho paralelo ─────────────────────────────────────────────────
      const int nth = num_threads;
      std::vector<std::vector<uint8_t>> t_cnt(static_cast<size_t>(nth));
      std::vector<std::vector<uint8_t>> t_vis(static_cast<size_t>(nth));
      std::vector<std::vector<uint8_t>> t_tmp(static_cast<size_t>(nth));
      std::vector<std::vector<Bubble>>  t_bubs(static_cast<size_t>(nth));

#pragma omp parallel num_threads(nth)
      {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        auto& cnt  = t_cnt [static_cast<size_t>(tid)];
        auto& vis  = t_vis [static_cast<size_t>(tid)];
        auto& tmp  = t_tmp [static_cast<size_t>(tid)];
        auto& bubs = t_bubs[static_cast<size_t>(tid)];
        if (cnt.size() < num_nodes_) cnt.assign(num_nodes_, 0);
        if (vis.size() < num_nodes_) vis.assign(num_nodes_, 0);
        if (tmp.size() < num_nodes_) tmp.assign(num_nodes_, 0);

#pragma omp for schedule(dynamic, 64) nowait
        for (uint64_t source = 0; source < num_nodes_; ++source) {
          find_bubble_at(*this, source, max_depth, max_nodes_per_branch,
                         cnt, vis, tmp, bubs);
        }
      }

      size_t total = 0;
      for (auto& v : t_bubs) total += v.size();
      bubbles.reserve(total);
      for (auto& v : t_bubs)
        bubbles.insert(bubbles.end(),
                       std::make_move_iterator(v.begin()),
                       std::make_move_iterator(v.end()));
      return bubbles;
    }

    // ── Caminho serial — usa bitmaps membros ─────────────────────────────
    if (cnt_bitmap_.size() < num_nodes_) cnt_bitmap_.assign(num_nodes_, 0);
    if (bfs_bitmap_.size() < num_nodes_) bfs_bitmap_.assign(num_nodes_, 0);
    if (tmp_bitmap_.size() < num_nodes_) tmp_bitmap_.assign(num_nodes_, 0);

    for (uint64_t source = 0; source < num_nodes_; ++source) {
      find_bubble_at(*this, source, max_depth, max_nodes_per_branch,
                     cnt_bitmap_, bfs_bitmap_, tmp_bitmap_, bubbles);
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

  // Bitmaps membros — usados pelo caminho serial de find_bubbles (num_threads=1).
  // INVARIANTE: sempre zerados antes e após cada uso — reset O(|visitados|).
  // O caminho paralelo (num_threads>1) usa bitmaps locais por thread; estes
  // membros ficam intocados durante execução paralela.
  mutable std::vector<uint8_t> bfs_bitmap_; // marcador de visita no BFS
  mutable std::vector<uint8_t> cnt_bitmap_; // contagem de ramos (interseção)
  mutable std::vector<uint8_t> tmp_bitmap_; // scratch BFS para find_bubble_at serial

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

  // ── BFS com captura de profundidade — passagem única ────────────────────
  //
  // Expande BFS a partir de `start` até max_depth hops.
  // out_nodes recebe os nós visitados em ordem BFS (inclui start com depth=0).
  // out_depths[i] = profundidade de out_nodes[i].
  // vis_bitmap: zerado na entrada, zerado na saída (reset O(|visit|)).
  // Retorna false se explodiu max_cap (vis_bitmap zerado mesmo assim).
  static bool bfs_with_depth(const GfaCsrTopology& g,
                              uint64_t start, int max_depth,
                              std::vector<uint64_t>& out_nodes,
                              std::vector<uint8_t>&  out_depths,
                              std::vector<uint8_t>&  vis_bitmap,
                              size_t max_cap) {
    out_nodes.clear();
    out_depths.clear();
    if (start >= g.num_nodes_) return true;
    if (vis_bitmap.size() < g.num_nodes_) vis_bitmap.assign(g.num_nodes_, 0);

    // Fila inline: pares (node, depth) — sem alocação dinâmica por nó
    std::vector<std::pair<uint64_t, uint8_t>> q;
    q.reserve(64);
    q.push_back({start, 0});
    vis_bitmap[start] = 1;
    out_nodes.push_back(start);
    out_depths.push_back(0);

    for (size_t qi = 0; qi < q.size(); ++qi) {
      auto [node, d] = q[qi];
      if (d >= static_cast<uint8_t>(max_depth)) continue;
      for (uint64_t enc : g.out_neighbors(node)) {
        auto [nb, _o] = GfaCsrTopology::decode(enc);
        if (!vis_bitmap[nb]) {
          vis_bitmap[nb] = 1;
          const uint8_t nd = d + 1;
          out_nodes.push_back(nb);
          out_depths.push_back(nd);
          if (max_cap > 0 && out_nodes.size() >= max_cap) {
            for (uint64_t n : out_nodes) vis_bitmap[n] = 0;
            return false;
          }
          q.push_back({nb, nd});
        }
      }
    }
    for (uint64_t n : out_nodes) vis_bitmap[n] = 0;
    return true;
  }

  // ── Núcleo do algoritmo de superbubble para um source ────────────────────
  //
  // BFS por ramo com interseção progressiva para aborte precoce:
  //   - Ramo 0: BFS livre, popula cnt_bitmap[n] = 1 para nós visitados.
  //   - Ramos b >= 1: BFS "guiado" — só se expande a partir de nós com
  //     cnt_bitmap[n] == b (alcançados por todos os b ramos anteriores).
  //     Isso reduz drasticamente o tamanho do BFS para ramos tardios.
  //   - Candidato a sink: cnt_bitmap[n] == B ao final.
  //   - vis_bitmap: guarda max(depth_b(n)+1) para achar o sink mais próximo.
  //
  // Bitmaps na entrada: cnt_bitmap, vis_bitmap, tmp_bitmap zerados.
  // Bitmaps na saída: todos zerados.
  // tmp_bitmap: bitmap auxiliar de visita para BFS de cada ramo.
  static void find_bubble_at(const GfaCsrTopology& g,
                              uint64_t source,
                              int max_depth,
                              size_t max_nodes_per_branch,
                              std::vector<uint8_t>& cnt_bitmap,  // conta ramos (zerado)
                              std::vector<uint8_t>& vis_bitmap,  // max depth+1 por nó (zerado)
                              std::vector<uint8_t>& tmp_bitmap,  // scratch BFS (zerado)
                              std::vector<Bubble>& out) {
    auto out_span = g.out_neighbors(source);
    const size_t B = out_span.size();
    if (B < 2 || B > 254) return;

    if (cnt_bitmap.size() < g.num_nodes_) cnt_bitmap.assign(g.num_nodes_, 0);
    if (vis_bitmap.size() < g.num_nodes_) vis_bitmap.assign(g.num_nodes_, 0);
    if (tmp_bitmap.size() < g.num_nodes_) tmp_bitmap.assign(g.num_nodes_, 0);

    // ── BFS unificado com interseção progressiva ─────────────────────────
    // Acumula todos os nós visitados em touched[] para limpeza final.
    // Para cada ramo b: faz BFS a partir do vizinho direto do source.
    //   - Se b == 0: aceita qualquer nó não visitado.
    //   - Se b  > 0: só aceita nós onde cnt_bitmap[n] == b (na fronteira
    //     da interseção progressiva dos ramos 0..b-1).
    //
    // Após o ramo b, cnt_bitmap[n] é incrementado para nós alcançados.
    // vis_bitmap[n] armazena max(depth_b(n)+1) como no algoritmo original.

    std::vector<uint64_t> touched; // todos os nós marcados (para limpeza)
    touched.reserve(64);

    // Fila BFS reutilizada entre ramos
    std::vector<std::pair<uint64_t, uint8_t>> q;
    q.reserve(64);

    // rdepth_sink[b] = profundidade do sink no ramo b (preenchida on-the-fly)
    // reachable_b[b] + rdepth_b[b] = nós internos (para validação de disjunção)
    // Só armazenamos nós que são candidatos internos (depth < depth_sink, != source/sink)
    std::vector<std::vector<uint64_t>> reachable(B);
    std::vector<std::vector<uint8_t>>  rdepth(B);

    bool all_have_candidate = false;

    for (size_t b = 0; b < B; ++b) {
      auto [sn, _o] = GfaCsrTopology::decode(out_span[b]);
      if (sn >= g.num_nodes_) goto cleanup;

      q.clear();
      q.push_back({sn, 1}); // depth=1 (source está no nível 0 implicitamente)
      tmp_bitmap[sn] = 1;

      for (size_t qi = 0; qi < q.size(); ++qi) {
        auto [node, d] = q[qi];
        // Visita este nó:
        if (b == 0) {
          // Primeiro ramo: marca tudo
          if (cnt_bitmap[node] == 0) touched.push_back(node);
          ++cnt_bitmap[node]; // agora cnt = 1
          if (d > vis_bitmap[node]) vis_bitmap[node] = d; // max depth+1
        } else {
          // Ramos subsequentes: só processa nós onde cnt == b (fronteira progressiva)
          // Nós com cnt < b ainda não foram vistos por todos os ramos anteriores — skip
          if (cnt_bitmap[node] == static_cast<uint8_t>(b)) {
            ++cnt_bitmap[node]; // agora cnt = b+1
            if (d > vis_bitmap[node]) vis_bitmap[node] = d;
          }
          // Independente de incrementar, expandimos o BFS a partir deste nó
          // (para que nós mais distantes possam ser alcançados via este)
        }

        if (d >= static_cast<uint8_t>(max_depth)) continue;
        for (uint64_t enc : g.out_neighbors(node)) {
          auto [nb, _o2] = GfaCsrTopology::decode(enc);
          if (nb >= g.num_nodes_) continue;
          if (!tmp_bitmap[nb]) {
            // Para ramos b > 0, só enfileira nós que têm cnt_bitmap >= b
            // (só eles podem chegar à interseção)
            if (b > 0 && cnt_bitmap[nb] < static_cast<uint8_t>(b)) continue;
            tmp_bitmap[nb] = 1;
            uint8_t nd = d + 1;
            q.push_back({nb, nd});
            if (max_nodes_per_branch > 0 && q.size() >= max_nodes_per_branch)
              goto abort_branch;
          }
        }
      }
      abort_branch:;
      // Reset tmp_bitmap para este ramo
      for (auto [n, _d] : q) tmp_bitmap[n] = 0;

      // Armazena reachable[b] = nós visitados por este ramo com seu depth
      // (Necessário para validações de disjunção/fechamento)
      reachable[b].clear();
      rdepth[b].clear();
      for (auto [n, d] : q) {
        if (n == source) continue;
        reachable[b].push_back(n);
        rdepth[b].push_back(d - 1); // d era depth+1 (por causa do encoding)
      }
    }

    {
      // ── Fase 2: seleciona melhor sink ────────────────────────────────────
      const uint8_t need      = static_cast<uint8_t>(B);
      const uint8_t depth_lim = static_cast<uint8_t>(max_depth) + 1;
      uint64_t best_sink  = UINT64_MAX;
      uint8_t  best_maxd1 = depth_lim + 1;

      for (uint64_t n : touched) {
        if (cnt_bitmap[n] != need || n == source) continue;
        uint8_t md1 = vis_bitmap[n];
        if (md1 > depth_lim) continue;
        if (md1 < best_maxd1) { best_maxd1 = md1; best_sink = n; }
      }

      if (best_sink == UINT64_MAX) goto cleanup;

      // ── Fase 3: coleta nós internos + validação de disjunção ─────────────
      std::vector<std::vector<uint64_t>> branch_internals(B);
      bool valid = true;

      // Reutiliza cnt_bitmap (já limpo na saída) → usa vis_bitmap como "membro do subgrafo"
      // mas vis_bitmap está com depths — usa tmp_bitmap como subgraph marker
      tmp_bitmap[source]    = 1;
      tmp_bitmap[best_sink] = 1;
      std::vector<uint64_t> subgraph_nodes = {source, best_sink};

      for (size_t b = 0; b < B && valid; ++b) {
        uint8_t sinked = 0;
        for (size_t i = 0; i < reachable[b].size(); ++i) {
          if (reachable[b][i] == best_sink) { sinked = rdepth[b][i]; break; }
        }
        auto& internals = branch_internals[b];
        for (size_t i = 0; i < reachable[b].size(); ++i) {
          uint64_t n = reachable[b][i];
          if (n == source || n == best_sink) continue;
          if (rdepth[b][i] >= sinked) continue;
          if (tmp_bitmap[n]) { valid = false; break; } // disjunção violada
          tmp_bitmap[n] = 1;
          subgraph_nodes.push_back(n);
          internals.push_back(n);
        }
      }

      // ── Fase 4: validação de fechamento ──────────────────────────────────
      if (valid) {
        for (size_t b = 0; b < B && valid; ++b) {
          for (uint64_t n : branch_internals[b]) {
            for (uint64_t enc : g.out_neighbors(n)) {
              auto [nb, _o] = GfaCsrTopology::decode(enc);
              if (!tmp_bitmap[nb]) { valid = false; break; }
            }
            if (!valid) break;
          }
        }
      }

      // Limpeza tmp_bitmap subgraph
      for (uint64_t n : subgraph_nodes) tmp_bitmap[n] = 0;

      if (valid) {
        std::vector<uint64_t> branches;
        for (auto& v : branch_internals)
          branches.insert(branches.end(), v.begin(), v.end());
        out.push_back({source, best_sink, std::move(branches)});
      }
    }

    cleanup:
    for (uint64_t n : touched) { cnt_bitmap[n] = 0; vis_bitmap[n] = 0; }
  }

  // Wrapper serial para bfs_collect (compatibilidade externa).
  void bfs_collect(uint64_t start, int max_depth,
                   std::vector<uint64_t>& out_nodes,
                   size_t max_cap = 0) const {
    std::vector<uint8_t> depths;
    bfs_with_depth(*this, start, max_depth, out_nodes, depths, bfs_bitmap_, max_cap);
  }

}; // class GfaCsrTopology

} // namespace ggql
