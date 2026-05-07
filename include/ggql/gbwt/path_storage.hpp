#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <span>
#include <vector>

namespace ggql {

// PathStorage — substituto interno de gbwt::GBWT otimizado para extração
// massiva sequencial. Nós concatenados em vector<node_type>; cada path é
// localizado por offsets[path_id]..offsets[path_id+1]. Imutável após build.
//
// `node_type = uint32_t` (Fase 6 onda 4 — Opção B-lite). Limita o grafo a
// 2³² - 1 ≈ 4.3 bilhões de nós internos distintos. Cobre HPRC v1/v2 com
// margem >7×. Para grafos com mais nós, voltar para uint64_t (passo único:
// alterar o typedef abaixo + bump kVersion no save/load).
class PathStorage {
  public:
    using node_type = std::uint32_t;

    PathStorage() = default;

    // Rule-of-5 explícito: garante move noexcept (vector<uint32_t> e
    // vector<uint64_t> são noexcept-movable). Sem isso, `vector<PathStorage>`
    // poderia cair em copy fallback se o STL não conseguir provar a garantia.
    PathStorage(const PathStorage&)                = default;
    PathStorage& operator=(const PathStorage&)     = default;
    PathStorage(PathStorage&&) noexcept            = default;
    PathStorage& operator=(PathStorage&&) noexcept = default;
    ~PathStorage()                                  = default;

    [[nodiscard]] static PathStorage
    from_paths(std::vector<std::vector<node_type>>&& paths);

    // Cópia para buffer pré-alocado (substitui out por completo).
    void extract_into(std::size_t path_id, std::vector<node_type>& out) const;

    // Overload de conveniência para callers que mantêm buffer em uint64_t
    // (API pública `GfaCsrTopology::path_nodes(size_t, vector<uint64_t>&)`).
    // Faz cópia com promoção uint32 → uint64 elemento-a-elemento.
    void extract_into(std::size_t path_id, std::vector<std::uint64_t>& out) const;

    // Span zero-copy sobre o storage interno; válido enquanto *this viver.
    // Retorna `span<const uint32_t>` — em hot paths que iteram somando ou
    // comparando IDs, prefira este sobre extract_into.
    [[nodiscard]] std::span<const node_type>
    nodes_view(std::size_t path_id) const noexcept;

    [[nodiscard]] std::size_t path_size(std::size_t path_id) const noexcept;

    // Invariante: offsets_.size() == num_paths() + 1 (sentinel inicial 0 sempre presente).
    [[nodiscard]] std::size_t num_paths() const noexcept { return offsets_.size() - 1; }
    [[nodiscard]] std::size_t total_nodes() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t memory_bytes() const noexcept;

    void save(std::ostream& out) const;
    [[nodiscard]] static PathStorage load(std::istream& in);

  private:
    std::vector<node_type>     nodes_;
    std::vector<std::uint64_t> offsets_ = {0};

    friend class PathStorageBuilder;
};

class PathStorageBuilder {
  public:
    PathStorageBuilder() = default;

    // Rule-of-5: copy proibida (builder é estado mutável que não faz sentido
    // duplicar); move noexcept para permitir armazenar em containers.
    PathStorageBuilder(const PathStorageBuilder&)                = delete;
    PathStorageBuilder& operator=(const PathStorageBuilder&)     = delete;
    PathStorageBuilder(PathStorageBuilder&&) noexcept            = default;
    PathStorageBuilder& operator=(PathStorageBuilder&&) noexcept = default;
    ~PathStorageBuilder()                                         = default;

    void reserve(std::size_t num_paths_hint, std::size_t total_nodes_hint);

    void insert(std::span<const PathStorage::node_type> seq);
    void insert(const std::vector<PathStorage::node_type>& seq) {
        insert(std::span<const PathStorage::node_type>{seq});
    }

    [[nodiscard]] PathStorage finish();

    [[nodiscard]] std::size_t paths_inserted() const noexcept {
        return offsets_.size() - 1;
    }
    [[nodiscard]] std::size_t nodes_inserted() const noexcept { return nodes_.size(); }

  private:
    std::vector<PathStorage::node_type> nodes_;
    std::vector<std::uint64_t>          offsets_{0};
    bool                                finished_ = false;
};

}  // namespace ggql
