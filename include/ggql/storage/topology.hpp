#pragma once

// topology.hpp — Declaração de GfaCsrTopology.
//
// Variante vg-equivalent: adiciona remap_node_ids() e make_component_remap()
// para converter os IDs MPHF aleatórios em IDs sequenciais contíguos por
// componente, reduzindo sigma de ~85 M para ~M_i por job.
//
// ═══════════════════════════════════════════════════════════════════════════
// CAMADA 1 — CSR + Elias-Fano  ("o mapa das ruas")
// ═══════════════════════════════════════════════════════════════════════════
//
// Armazena a topologia pura do grafo de sobreposição (quem se conecta a quem).
//
// Layout de memória:
//   E_fwd_       : std::vector<uint32_t>    — arestas de saída
//                    encoding: (neighbor_id << 1) | orient_bit
//   E_bwd_       : std::vector<uint32_t>    — arestas de entrada (mesmo encoding)
//   ef_fwd_      : sdsl::sd_vector<>        — Elias-Fano dos offsets de saída
//   ef_bwd_      : sdsl::sd_vector<>        — Elias-Fano dos offsets de entrada
//
// Consumo de memória típico para 118 M arestas / 85 M nós:
//   E_fwd/E_bwd  ≈ 2 × 118M × 4 B ≈ 0.94 GB  (uint32_t; era 1.89 GB com uint64_t)
//   ef_fwd/bwd   ≈ 2 × 85M  × 3 b ≈  64 MB    (vs 680 MB de vector<uint32_t>)
//   offsets_cache ≈ 680 MB  LAZY — alocado só na 1ª chamada a find_bubbles
//
// ═══════════════════════════════════════════════════════════════════════════
// CAMADA 2 — gBWT  ("o mapa dos haplótipos")
// ═══════════════════════════════════════════════════════════════════════════
//
// Indexa paths/haplótipos com o GBWT (r-index, Sirén 2020).
// Construção incremental path-a-path; espaço O(r) onde r = runs BWT.
//
// API de paths:
//   path_count()                         — número de paths indexados
//   path_name(path_id)                   — nome do path
//   build_path_index_parallel(groups,w,t)— construção paralela do índice
//
// ═══════════════════════════════════════════════════════════════════════════
// NOTA DE COMPILAÇÃO
// ═══════════════════════════════════════════════════════════════════════════
//
// As implementações pesadas (find_bubbles, build_path_index_parallel,
// repartition_groups_topological, make_component_remap, remap_node_ids,
// radix_sort_by_from, build_csr, ensure_offsets_cache)
// estão em src/storage/gfa_topology.cpp e são compiladas UMA ÚNICA VEZ
// na biblioteca estática ggql_topology_vg.  Este header inclui apenas:
//   • Declarações de classe (sem corpo de métodos pesados)
//   • Métodos inline triviais (< 5 LOC, hot paths de benchmark)
//   • find_tips e find_repeats (inline por performance — hot paths medidos)

#include "ggql/ingest/gbwt_group.hpp"
#include "ggql/storage/topology_types.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _OPENMP
    #include <omp.h>
#endif

#include <sdsl/int_vector.hpp>
#include <sdsl/int_vector_buffer.hpp>
#include <sdsl/sd_vector.hpp>

// libgbwt — gbwt::GBWT precisa de tipo completo para unique_ptr destrutor.
// gbwt/utils.h é leve (sem algoritmos): fornece gbwt::vector_type, node_type e ENDMARKER.
// Os headers pesados (algorithms.h, dynamic_gbwt.h) são incluídos APENAS no .cpp.
#include <gbwt/gbwt.h>
#include <gbwt/utils.h>

// ── Garante C++23 em tempo de compilação ────────────────────────────────────
static_assert(__cplusplus >= 202302L, "topology.hpp requer C++23 — compile com -std=c++23.");

namespace ggql {

class GfaCsrTopology {
  public:
    GfaCsrTopology() = default;

    // Destrutor declarado aqui; definido no .cpp onde gbwt::GBWT é completo.
    // (O compilador precisa do destrutor de gbwt::GBWT para destruir unique_ptr<GBWT>.)
    ~GfaCsrTopology();

    // Sem cópia — contém unique_ptr e recursos grandes
    GfaCsrTopology(const GfaCsrTopology&) = delete;
    GfaCsrTopology& operator=(const GfaCsrTopology&) = delete;
    GfaCsrTopology(GfaCsrTopology&& other) noexcept;
    GfaCsrTopology& operator=(GfaCsrTopology&& other) noexcept;

    // ── Timings do gBWT ──────────────────────────────────────────────────────
    struct GbwtTimings {
        double wall_s = 0.0;      // Tempo real total (wall-clock)
        double cpu_avg_s = 0.0;   // Média de tempo de CPU por thread
        double cpu_max_s = 0.0;   // Máximo de tempo de CPU em uma única thread
        double insert_s = 0.0;    // Tempo acumulado em DynamicGBWT::insert()
        double compress_s = 0.0;  // Tempo acumulado em GBWT(dynamic) compress
        uint64_t total_nodes = 0; // Total de nós processados
        size_t peak_rss_kb = 0;   // Pico isolado de memória (RSS) em KB
        size_t index_size_b = 0;  // Tamanho final do índice em bytes

        // Helpers de performance
        double nodes_per_s() const { return wall_s > 0 ? (double)total_nodes / wall_s : 0; }
        double bits_per_node() const {
            return total_nodes > 0 ? (double)index_size_b * 8 / total_nodes : 0;
        }
    };

    // ── Grupo de paths para build paralelo ───────────────────────────────────
    // Tipo unificado definido em ggql/ingest/gbwt_group.hpp.
    using GbwtGroup = ::ggql::GbwtGroup;

    // ── Construção CSR + Elias-Fano ──────────────────────────────────────────
    //
    // links: todos os links do GFA com IDs MPHF resolvidos.
    // num_nodes = mphf_->nbKeys() (IDs válidos: [0, num_nodes)).
    // Sort realizado internamente — não requer entrada pré-ordenada.
    void build(uint64_t num_nodes, std::vector<LinkRecord> links, int num_threads = 1) {
        num_nodes_ = num_nodes;
        build_csr(links, E_fwd_, ef_fwd_, ef_fwd_sel_, /*is_fwd=*/true, num_threads);
        for (auto& lk : links) {
            lk = lk.reversed();
        }
        build_csr(links, E_bwd_, ef_bwd_, ef_bwd_sel_, /*is_fwd=*/false, num_threads);
    }

    // ── Consultas de grau (inline — hot path, ~27 ns/op no benchmark) ────────
    uint64_t out_degree(uint64_t node) const noexcept {
        if (node >= num_nodes_ || E_fwd_.empty()) {
            return 0;
        }

        auto [lo, hi] = edge_range_fwd(node);
        return hi - lo;
    }

    uint64_t in_degree(uint64_t node) const noexcept {
        if (node >= num_nodes_ || E_bwd_.empty()) {
            return 0;
        }

        auto [lo, hi] = edge_range_bwd(node);
        return hi - lo;
    }

    // ── Iteração sobre vizinhos (inline — hot path, ~27 ns/op) ───────────────
    //
    // Retorna span dos valores codificados em E_fwd_/E_bwd_ (uint32_t):
    //   bit  0       → orientação (0='+', 1='-')
    //   bits [31:1]  → node_id    (suporta até 2^31 ≈ 2.1 B nós)
    // Use decode(encoded) → EdgeRef{uint64_t, char} para tipos semânticos.
    std::span<const uint32_t> out_neighbors(uint64_t node) const noexcept {
        auto [lo, hi] = edge_range_fwd(node);
        if (hi <= lo) {
            return {};
        }
        return {E_fwd_.data() + lo, hi - lo};
    }

    std::span<const uint32_t> in_neighbors(uint64_t node) const noexcept {
        auto [lo, hi] = edge_range_bwd(node);
        if (hi <= lo) {
            return {};
        }

        return {E_bwd_.data() + lo, hi - lo};
    }

    // ── Decodificação ─────────────────────────────────────────────────────────
    static EdgeRef decode(uint32_t encoded) noexcept {
        return {static_cast<uint64_t>(encoded >> 1), (encoded & 1u) ? '-' : '+'};
    }

    // ── ID Remapping (vg-equivalent) ────────────────────────────────────────
    //
    // Resultado de make_component_remap(): contém tanto o remap quanto
    // node_component necessário para repartition_groups_topological().
    struct RemapResult {
        std::vector<uint32_t> remap;          // remap[old_mphf_id] = new_sequential_id
        std::vector<uint32_t> node_component; // node_component[new_id] = comp_id (0..K-1)
        uint32_t num_components = 0;          // número total de componentes K
    };

    // Constrói remap[old_mphf_id] = new_sequential_id de forma que nós do mesmo
    // componente recebem IDs contíguos. O componente maior recebe [0, M_0), etc.
    // Retorna RemapResult com remap + node_component[new_id] = comp_id.
    // node_component é necessário para repartition_groups_topological().
    // Complexidade: O(N × alpha(N)) + O(N log K).
    [[nodiscard]] RemapResult make_component_remap(int num_threads = 1) const;

    // Aplica remap em E_fwd_, E_bwd_ e reconstrói ef_fwd_, ef_bwd_.
    // Invalida offsets_cache_fwd_/bwd_ (reconstrução lazy na próxima chamada).
    void remap_node_ids(const std::vector<uint32_t>& remap);

    // Redistribui grupos existentes por componente topológico, balanceando em
    // target_n jobs (um por componente). Recebe o mapa node_component[new_id]=comp_id
    // de make_component_remap() e o node_width.
    // Cada job resultante contém paths exclusivamente de um componente → merge OK.
    [[nodiscard]] std::vector<GbwtGroup>
    repartition_groups_topological(std::vector<GbwtGroup> input_groups,
                                   const std::vector<uint32_t>& node_component, size_t target_n,
                                   uint8_t node_width, const std::string& data_dir = "/tmp") const;

    // ── Persistência (sdsl/simple-sds) ───────────────────────────────────────
    // Salva/carrega todos os componentes (CSR, EF, GBWT) em um diretório.
    void save(const std::string& dir) const;
    void load(const std::string& dir);

    // ── Estatísticas de memória ───────────────────────────────────────────────
    [[nodiscard]] size_t memory_bytes() const noexcept;

    // ── Dimensões do grafo ────────────────────────────────────────────────────
    uint64_t num_nodes() const noexcept { return num_nodes_; }
    uint64_t num_edges() const noexcept { return E_fwd_.size(); }

    // ── Algoritmos de análise de grafo ────────────────────────────────────────
    //
    // find_tips e find_repeats mantidos INLINE: são hot paths medidos (94–209 ms
    // por chamada serial no benchmark).  Inline preserva a especialização do
    // compilador no kParallelNodeThreshold_ e elimina o overhead de call site.

    // Retorna IDs de nós "ponta" (tip): in_degree=0 OU out_degree=0.
    std::vector<uint64_t> find_tips(int num_threads = 1) const {
        if (offsets_cache_fwd_.empty() || offsets_cache_bwd_.empty()) {
            std::vector<uint64_t> tips;
            tips.reserve(num_nodes_ / 64);

            for (uint64_t i = 0; i < num_nodes_; ++i) {
                if (in_degree(i) == 0 || out_degree(i) == 0) {
                    tips.push_back(i);
                }
            }

            return tips;
        }

#ifndef _OPENMP
        num_threads = 1;
#endif
        const uint64_t N = num_nodes_;

        // Só entra no caminho paralelo se N superar o limiar: grafos <20M nós
        // têm overhead de fork/join maior que o ganho (puro BW-bound, ~2 reads/nó).
        if (num_threads > 1 && N >= kParallelNodeThreshold_) {
            std::vector<std::vector<uint64_t>> t_tips(static_cast<size_t>(num_threads));
#pragma omp parallel num_threads(num_threads)
            {
                int tid = 0;
#ifdef _OPENMP
                tid = omp_get_thread_num();
#endif
                auto& local = t_tips[static_cast<size_t>(tid)];
                // /4 em vez de /16: CHM13 tem ~0.41% de tips (17k/4.3M).
                local.reserve(N / static_cast<uint64_t>(num_threads * 4));
#pragma omp for schedule(static)
                for (uint64_t i = 0; i < N; ++i) {
                    if (offsets_cache_fwd_[i] == offsets_cache_fwd_[i + 1] ||
                        offsets_cache_bwd_[i] == offsets_cache_bwd_[i + 1]) {
                        local.push_back(i);
                    }
                }
            }
            size_t total = 0;
            for (const auto& v : t_tips) {
                total += v.size();
            }

            std::vector<uint64_t> tips;
            tips.reserve(total);
            for (auto& v : t_tips) {
                tips.insert(tips.end(), v.begin(), v.end());
            }

            return tips;
        }

        std::vector<uint64_t> tips;
        tips.reserve(N / 64);
        for (uint64_t i = 0; i < N; ++i) {
            if (offsets_cache_fwd_[i] == offsets_cache_fwd_[i + 1] ||
                offsets_cache_bwd_[i] == offsets_cache_bwd_[i + 1]) {
                tips.push_back(i);
            }
        }

        return tips;
    }

    // Retorna IDs de nós "repeat": out_degree >= min_degree.
    std::vector<uint64_t> find_repeats(uint64_t min_degree = 2, int num_threads = 1) const {
        if (offsets_cache_fwd_.empty()) {
            std::vector<uint64_t> repeats;
            repeats.reserve(num_nodes_ / 4);
            for (uint64_t i = 0; i < num_nodes_; ++i) {
                if (out_degree(i) >= min_degree) {
                    repeats.push_back(i);
                }
            }

            return repeats;
        }

#ifndef _OPENMP
        num_threads = 1;
#endif
        const uint64_t N = num_nodes_;

        // Threshold idêntico ao de find_tips: BW-bound, sem ganho real para N<20M.
        if (num_threads > 1 && N >= kParallelNodeThreshold_) {
            std::vector<std::vector<uint64_t>> t_rep(static_cast<size_t>(num_threads));
            std::atomic<size_t> total{0};
#pragma omp parallel num_threads(num_threads)
            {
                int tid = 0;
#ifdef _OPENMP
                tid = omp_get_thread_num();
#endif
                auto& local = t_rep[static_cast<size_t>(tid)];
                local.reserve(N / static_cast<uint64_t>(num_threads * 4));
#pragma omp for schedule(static)
                for (uint64_t i = 0; i < N; ++i) {
                    if (static_cast<uint64_t>(offsets_cache_fwd_[i + 1] - offsets_cache_fwd_[i]) >=
                        min_degree) {
                        local.push_back(i);
                    }
                }
                total.fetch_add(local.size(), std::memory_order_relaxed);
            }
            std::vector<uint64_t> repeats;
            if (total.load(std::memory_order_relaxed) > 1'000'000) {
                repeats.reserve(total.load(std::memory_order_relaxed));
            }

            for (auto& v : t_rep) {
                repeats.insert(repeats.end(), v.begin(), v.end());
            }

            return repeats;
        }

        std::vector<uint64_t> repeats;
        repeats.reserve(N / 4);
        for (uint64_t i = 0; i < N; ++i) {
            if (static_cast<uint64_t>(offsets_cache_fwd_[i + 1] - offsets_cache_fwd_[i]) >=
                min_degree) {
                repeats.push_back(i);
            }
        }

        return repeats;
    }

    // Detecção de superbubbles — algoritmo Onodera et al. (2013), O(V+E).
    // Implementação em graph_algorithms.cpp.
    [[nodiscard]] std::vector<Bubble> find_bubbles(int num_threads = 1) const;

    // ── Camada 2 — Construção do índice gBWT ─────────────────────────────────

    // Constrói em paralelo a partir de N GbwtGroups.
    GbwtTimings build_path_index_parallel(std::vector<GbwtGroup> groups, uint8_t node_width,
                                          int num_threads = 1);

    // ── Consultas ao índice de paths (inline triviais) ────────────────────────
    size_t path_count() const noexcept { return path_names_.size(); }

    std::string_view path_name(size_t path_id) const noexcept {
        if (path_id >= path_names_.size()) {
            return {};
        }

        return path_names_[path_id];
    }

    // Retorna IDs internos de nós (sem bit de orientação) na ordem do path.
    // Retorna vetor vazio se path_id está fora do intervalo ou gBWT não disponível.
    // Complexidade: O(L) onde L = número de nós no path.
    [[nodiscard]] std::vector<uint64_t> path_nodes(size_t path_id) const;
    // Overload com out-param: reusa buffer entre chamadas. Preferir em hot
    // loops (build_path_stats, etc.) para evitar malloc por chamada.
    void path_nodes(size_t path_id, std::vector<uint64_t>& out) const;

    // ── Cache de estatísticas de path (node_count + bp_length) ───────────────
    //
    // Pré-computado no ingest e persistido em path_stats.bin.
    // Elimina o path_nodes() no loop de plan_paths — O(1) por walk.
    //
    // lengths: span de GfaMetadataStore::lengths_[] (mesmos IDs internos).
    // Usa extração GBWT pós-build. Para datasets grandes prefira
    // set_path_stats() com stats pré-computados das sequências em RAM.
    void build_path_stats(std::span<const uint32_t> lengths);

    // Atribui stats pré-computados diretamente, sem traversal GBWT.
    // Chamado pelo ingestor quando as sequências ainda estão em RAM
    // (antes de build_path_index_parallel consumir os GbwtGroups).
    void set_path_stats(std::vector<uint32_t> nc, std::vector<int64_t> bp) noexcept {
        path_node_counts_ = std::move(nc);
        path_bp_lengths_  = std::move(bp);
    }

    bool has_path_stats() const noexcept {
        return !path_node_counts_.empty() &&
               path_node_counts_.size() == path_names_.size();
    }

    uint32_t path_node_count(size_t path_id) const noexcept {
        return path_id < path_node_counts_.size() ? path_node_counts_[path_id] : 0;
    }

    int64_t path_bp_length(size_t path_id) const noexcept {
        return path_id < path_bp_lengths_.size() ? path_bp_lengths_[path_id] : 0;
    }

    // ── Snarls (FIND BUBBLE) — persistido em snarls.bin ───────────────────────
    //
    // Computado uma única vez durante o ingest via find_snarls_streaming().
    // Carregado em load() com fallback gracioso (vazio se snarls.bin ausente).
    bool has_snarls() const noexcept { return !snarls_.empty(); }

    std::span<const SnarlRecord> snarls() const noexcept {
        return std::span<const SnarlRecord>(snarls_.data(), snarls_.size());
    }

    void set_snarls(std::vector<SnarlRecord> s) noexcept { snarls_ = std::move(s); }

    // Persiste apenas snarls.bin no diretório (sem reescrever os outros
    // arquivos de cache). Usado pelo upgrade incremental: quando o usuário
    // sobe de um perfil sem snarls (metagenomics/comparative) para um com
    // snarls (assembly/pangenome), evita reescrever CSR/EF/gBWT já em disco.
    void save_snarls_only(const std::string& dir) const;

    // Persiste apenas gbwts.bin + path_stats.bin + offsets relacionados.
    // Usado pelo upgrade incremental Fase 2 (metagenomics/assembly →
    // comparative/pangenome): após reconstruir gBWT a partir de Pass2
    // paths-only, grava só os arquivos do gBWT sem reescrever CSR/metadata.
    void save_gbwt_only(const std::string& dir) const;

  private:
    // ── Threshold de paralelismo ─────────────────────────────────────────────
    // Abaixo deste valor o overhead do fork/join OpenMP (~0.5 ms) supera o
    // ganho para operações com trabalho trivial por nó (BW-bound, 2 reads/nó).
    // Evidência: find_repeats(≥2) serial=99ms vs 4t=100ms vs 8t=102ms (CHM13).
    static constexpr uint64_t kParallelNodeThreshold_ = 20'000'000ULL;

    uint64_t num_nodes_ = 0;

    // ── Camada 1: CSR + Elias-Fano ───────────────────────────────────────────
    std::vector<uint32_t> E_fwd_; // arestas de saída
    std::vector<uint32_t> E_bwd_; // arestas de entrada

    sdsl::sd_vector<> ef_fwd_; // EF dos offsets de saída
    sdsl::sd_vector<>::select_1_type ef_fwd_sel_;
    sdsl::sd_vector<> ef_bwd_; // EF dos offsets de entrada
    sdsl::sd_vector<>::select_1_type ef_bwd_sel_;

    // Cache LAZY dos offsets CSR — populado na 1ª chamada a find_bubbles().
    mutable std::vector<uint32_t> offsets_cache_fwd_;
    mutable std::vector<uint32_t> offsets_cache_bwd_;

    // ── Camada 2: gBWT ───────────────────────────────────────────────────────
    std::vector<std::unique_ptr<gbwt::GBWT>> gbwts_;
    std::vector<size_t> gbwt_offsets_;
    std::vector<std::string> path_names_;

    // Cache de estatísticas de path (persistido em path_stats.bin)
    std::vector<uint32_t> path_node_counts_;
    std::vector<int64_t>  path_bp_lengths_;

    // Snarls (persistido em snarls.bin) — vazio se nunca computado.
    std::vector<SnarlRecord> snarls_;

    // ── Helpers privados (implementados no .cpp) ─────────────────────────────

    // Popula offsets_cache_fwd_/bwd_ com select_1 paralelo (OMP).
    void ensure_offsets_cache(int num_threads = 1) const;

    // Constrói E_*, ef_*, ef_*_sel_ a partir de links ordenados.
    void build_csr(std::vector<LinkRecord>& links, std::vector<uint32_t>& out_E,
                   sdsl::sd_vector<>& out_ef, sdsl::sd_vector<>::select_1_type& out_ef_sel,
                   bool is_fwd, int num_threads = 1);

    // LSD radix sort paralelo por from_id.
    static void radix_sort_by_from(std::vector<LinkRecord>& links, uint64_t num_nodes,
                                   int num_threads = 1);

    // Helpers de acesso ao cache/EF (inline — hot path em find_bubbles DFS).
    std::pair<uint64_t, uint64_t> edge_range_fwd(uint64_t node) const noexcept {
        if (node >= num_nodes_ || E_fwd_.empty()) {
            return {0, 0};
        }

        if (!offsets_cache_fwd_.empty()) [[likely]] {
            return {offsets_cache_fwd_[node], offsets_cache_fwd_[node + 1]};
        }

        return {ef_fwd_sel_(node + 1), ef_fwd_sel_(node + 2)};
    }

    std::pair<uint64_t, uint64_t> edge_range_bwd(uint64_t node) const noexcept {
        if (node >= num_nodes_ || E_bwd_.empty()) {
            return {0, 0};
        }

        if (!offsets_cache_bwd_.empty()) [[likely]] {
            return {offsets_cache_bwd_[node], offsets_cache_bwd_[node + 1]};
        }

        return {ef_bwd_sel_(node + 1), ef_bwd_sel_(node + 2)};
    }

    // Reconstrói gbwt_offsets_ após modificações em gbwts_.
    // Não noexcept: vector::resize pode lançar bad_alloc.
    void rebuild_offsets_();


}; // class GfaCsrTopology

} // namespace ggql