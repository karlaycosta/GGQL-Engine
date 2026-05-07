#pragma once

#include "path_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

namespace ggql {

// PathStorage — backend de paths sem compressao otimizado para extracao
// massiva sequencial. Nos concatenados em vector<node_type>; cada path e
// localizado por offsets[path_id]..offsets[path_id+1]. Imutavel apos build.
//
// `node_type = uint32_t` (Fase 6 onda 4 — Opção B-lite). Limita o grafo a
// 2³² - 1 ≈ 4.3 bilhões de nós internos distintos. Cobre HPRC v1/v2 com
// margem >7×. Para grafos com mais nós, voltar para uint64_t (passo único:
// alterar o typedef abaixo + bump kVersion no save/load).
//
// Implementa IPathBackend para coexistir com GbwtBackend (libgbwt). APIs
// especificas (nodes_view zero-copy, extract_into uint32, path_size) ficam
// fora da interface — uso direto via PathStorage* quando necessario.
class PathStorage : public IPathBackend {
  public:
    using node_type = IPathBackend::node_type;

    PathStorage() = default;

    // Rule-of-5 explícito: garante move noexcept (vector<uint32_t> e
    // vector<uint64_t> são noexcept-movable). Sem isso, `vector<PathStorage>`
    // poderia cair em copy fallback se o STL não conseguir provar a garantia.
    PathStorage(const PathStorage&)                = default;
    PathStorage& operator=(const PathStorage&)     = default;
    PathStorage(PathStorage&&) noexcept            = default;
    PathStorage& operator=(PathStorage&&) noexcept = default;
    ~PathStorage() override                         = default;

    [[nodiscard]] static PathStorage
    from_paths(std::vector<std::vector<node_type>>&& paths);

    // ── Implementacao de IPathBackend ────────────────────────────────────────
    void extract_into(std::size_t path_id,
                      std::vector<std::uint64_t>& out) const override;

    [[nodiscard]] std::size_t num_paths() const noexcept override {
        return offsets_.size() - 1;  // invariante: offsets[0]=0 sentinel
    }
    [[nodiscard]] std::size_t total_nodes() const noexcept override {
        return nodes_.size();
    }
    [[nodiscard]] std::size_t memory_bytes() const noexcept override;

    void save(std::ostream& out) const override;

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "PathStorage";
    }

    // ── APIs especificas (fora da interface IPathBackend) ────────────────────

    // Copia para buffer uint32_t pre-alocado (substitui out por completo).
    // Caminho rapido — memcpy direto sem promocao.
    void extract_into(std::size_t path_id, std::vector<node_type>& out) const;

    // Span zero-copy sobre o storage interno; valido enquanto *this viver.
    // Hot paths que iteram somando ou comparando IDs devem preferir este
    // sobre extract_into.
    [[nodiscard]] std::span<const node_type>
    nodes_view(std::size_t path_id) const noexcept;

    [[nodiscard]] std::size_t path_size(std::size_t path_id) const noexcept;

    // Factory estatica — load() nao pode ser virtual. O dispatch entre
    // backends acontece em read_path_stores_payload() via magic bytes.
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
