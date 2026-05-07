#pragma once

// GbwtBackend — adapter sobre ::gbwt::GBWT (libgbwt) implementando
// IPathBackend.
//
// Comprime paths via BWT generalizado (~1-2 bits/no em pangenoma humano,
// vs 32 bits/no do PathStorage). Trade-off: extract via LF-stepping em vez
// de memcpy direto (mais lento, mas uma ordem de magnitude menos disco/RAM).
//
// Operacoes especificas do ::gbwt::GBWT (locate, find, paths_through_region,
// haplotype sampling) ficam acessiveis via raw() — fora da interface
// IPathBackend, que cobre apenas o contrato comum.
//
// O .hpp NAO inclui gbwt headers (pesados): usa pImpl com unique_ptr para
// ::gbwt::GBWT. Quem precisa do tipo completo deve incluir explicitamente
// <gbwt/gbwt.h> e usar raw().

#include "path_backend.hpp"

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

class GbwtBackend : public IPathBackend {
  public:
    GbwtBackend();
    ~GbwtBackend() override;

    // Move-only: ::gbwt::GBWT contem estruturas SDSL que nao sao trivialmente
    // copiaveis e cujas copias profundas seriam caras.
    GbwtBackend(const GbwtBackend&)            = delete;
    GbwtBackend& operator=(const GbwtBackend&) = delete;
    GbwtBackend(GbwtBackend&&) noexcept;
    GbwtBackend& operator=(GbwtBackend&&) noexcept;

    // Toma posse de um ::gbwt::GBWT ja construido. O caller transfere
    // ownership via unique_ptr — evita expor o tipo completo no header.
    explicit GbwtBackend(std::unique_ptr<::gbwt::GBWT> gbwt) noexcept;

    // ── IPathBackend ────────────────────────────────────────────────────────
    void extract_into(std::size_t path_id,
                      std::vector<std::uint64_t>& out) const override;

    [[nodiscard]] std::size_t num_paths()    const noexcept override;
    [[nodiscard]] std::size_t total_nodes()  const noexcept override;
    [[nodiscard]] std::size_t memory_bytes() const noexcept override;

    void save(std::ostream& out) const override;

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "GbwtBackend";
    }

    // ── Factory ─────────────────────────────────────────────────────────────
    //
    // load() le o magic bytes do GbwtBackend (escrito por save()), valida
    // e entao chama ::gbwt::GBWT::simple_sds_load. Se o magic for de outro
    // backend, lanca runtime_error — dispatch entre backends acontece em
    // read_path_stores_payload (Task #6).
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
