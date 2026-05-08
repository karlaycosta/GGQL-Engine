#pragma once

// GbwtBackend — wrapper sobre ::gbwt::GBWT (libgbwt) que expoe a interface
// usada pela GfaCsrTopology (Camada 2: paths/haplotipos por shard).
//
// Apos a remocao do PathStorage (commit pre-removal:
// f630dc5 perf(allocator): jemalloc decay:100ms), GbwtBackend e o unico
// backend de armazenamento de paths. Sem heranca/interface — classe
// concreta, expoe metodos diretamente.
//
// O .hpp NAO inclui gbwt headers (pesados): usa pImpl com unique_ptr
// para ::gbwt::GBWT. Quem precisa do tipo completo deve incluir
// explicitamente <gbwt/gbwt.h> e usar raw().

#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <ostream>
#include <string_view>
#include <vector>

namespace gbwt {
class GBWT;
}  // namespace gbwt

namespace ggql {

class GbwtBackend {
  public:
    using node_type = std::uint32_t;

    GbwtBackend();
    ~GbwtBackend();

    // Move-only: ::gbwt::GBWT contem estruturas SDSL que nao sao
    // trivialmente copiaveis e cujas copias profundas seriam caras.
    GbwtBackend(const GbwtBackend&)            = delete;
    GbwtBackend& operator=(const GbwtBackend&) = delete;
    GbwtBackend(GbwtBackend&&) noexcept;
    GbwtBackend& operator=(GbwtBackend&&) noexcept;

    // Toma posse de um ::gbwt::GBWT ja construido. O caller transfere
    // ownership via unique_ptr — evita expor o tipo completo no header.
    explicit GbwtBackend(std::unique_ptr<::gbwt::GBWT> gbwt) noexcept;

    // ── Interface usada pela topology ───────────────────────────────────────

    // Hot path: copia a sequencia de nos do path para o buffer.
    // Substitui o conteudo de `out` por completo.
    void extract_into(std::size_t                 path_id,
                      std::vector<std::uint64_t>& out) const;

    [[nodiscard]] std::size_t num_paths()    const noexcept;
    [[nodiscard]] std::size_t total_nodes()  const noexcept;
    [[nodiscard]] std::size_t memory_bytes() const noexcept;

    void save(std::ostream& out) const;

    // ── Factory ─────────────────────────────────────────────────────────────
    [[nodiscard]] static GbwtBackend load(std::istream& in);

    // ── Acesso direto ao ::gbwt::GBWT subjacente ──────────────────────────────
    //
    // Para callers que precisam de operacoes especificas do gBWT (locate,
    // find, paths_through_region etc.). Retorna ptr nullptr se backend
    // foi construido vazio.
    [[nodiscard]] const ::gbwt::GBWT* raw() const noexcept { return gbwt_.get(); }

  private:
    std::unique_ptr<::gbwt::GBWT> gbwt_;
};

}  // namespace ggql
