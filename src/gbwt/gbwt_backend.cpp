// gbwt_backend.cpp — Implementacao do adapter GbwtBackend.

#include "ggql/gbwt/gbwt_backend.hpp"

#include <gbwt/gbwt.h>
#include <sdsl/io.hpp>

#include <array>
#include <cstring>
#include <stdexcept>

namespace ggql {

namespace {

// Magic bytes do envelope GGQL para um shard GbwtBackend. Escrito pelo
// save(), validado pelo load(). Distinto do magic do PathStorage
// ("GGQLPS\0\0") para permitir auto-detect via dispatch em
// read_path_stores_payload (Task #6).
constexpr std::array<char, 8> kMagic = {'G', 'G', 'Q', 'L', 'G', 'B', '\0', '\0'};

// Versao do formato GGQL-envelope. Bump ao mudar header/payload (independente
// da versao interna de simple_sds, que e gerenciada pela libgbwt).
constexpr std::uint32_t kVersion = 1;

}  // namespace

GbwtBackend::GbwtBackend() = default;

GbwtBackend::~GbwtBackend() = default;

GbwtBackend::GbwtBackend(GbwtBackend&&) noexcept            = default;
GbwtBackend& GbwtBackend::operator=(GbwtBackend&&) noexcept = default;

GbwtBackend::GbwtBackend(std::unique_ptr<gbwt::GBWT> gbwt) noexcept
    : gbwt_(std::move(gbwt)) {}

// ── IPathBackend ───────────────────────────────────────────────────────────

void GbwtBackend::extract_into(std::size_t                 path_id,
                               std::vector<std::uint64_t>& out) const {
    if (!gbwt_) {
        out.clear();
        return;
    }
    // gbwt::GBWT::extract retorna vector_type (== vector<gbwt::node_type>,
    // que e uint64). Aqui copiamos para o buffer do caller substituindo o
    // conteudo — segue o contrato de extract_into em PathStorage.
    auto seq = gbwt_->extract(static_cast<gbwt::size_type>(path_id));
    out.assign(seq.begin(), seq.end());
}

std::size_t GbwtBackend::num_paths() const noexcept {
    return gbwt_ ? static_cast<std::size_t>(gbwt_->sequences()) : 0u;
}

std::size_t GbwtBackend::total_nodes() const noexcept {
    return gbwt_ ? static_cast<std::size_t>(gbwt_->size()) : 0u;
}

std::size_t GbwtBackend::memory_bytes() const noexcept {
    return gbwt_ ? sdsl::size_in_bytes(*gbwt_) : 0u;
}

// ── Persistencia ───────────────────────────────────────────────────────────
//
// Layout on-disk:
//   [0..7]   magic    "GGQLGB\0\0"  (8 B)
//   [8..11]  version  u32           (4 B)
//   [12..]   gbwt::GBWT::simple_sds_serialize payload (cabecalho proprio)
//
// O payload de simple_sds inclui sua propria estrutura/checksum gerenciados
// pela libgbwt — nao replicamos aqui. O envelope GGQL serve apenas para
// dispatch entre backends (PathStorage vs GbwtBackend) no reader.

void GbwtBackend::save(std::ostream& out) const {
    out.write(kMagic.data(), kMagic.size());
    out.write(reinterpret_cast<const char*>(&kVersion), sizeof(kVersion));

    if (!gbwt_) {
        // Shard vazio: escreve um header-only sem payload. O load detecta
        // ausencia de payload via num_paths/size = 0 do gbwt default-constr.
        gbwt::GBWT empty;
        empty.simple_sds_serialize(out);
    } else {
        gbwt_->simple_sds_serialize(out);
    }

    if (!out.good()) {
        throw std::runtime_error("GbwtBackend::save: stream write failed");
    }
}

GbwtBackend GbwtBackend::load(std::istream& in) {
    std::array<char, 8> magic{};
    in.read(magic.data(), magic.size());
    if (!in.good()) {
        throw std::runtime_error("GbwtBackend::load: failed to read magic");
    }
    if (magic != kMagic) {
        throw std::runtime_error("GbwtBackend::load: bad magic");
    }

    std::uint32_t version = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!in.good()) {
        throw std::runtime_error("GbwtBackend::load: failed to read version");
    }
    if (version != kVersion) {
        throw std::runtime_error(
            "GbwtBackend::load: versao " + std::to_string(version) +
            " nao suportada (esperado " + std::to_string(kVersion) +
            "). Cache antigo — regenere via `ggql ingest`.");
    }

    auto gbwt = std::make_unique<gbwt::GBWT>();
    gbwt->simple_sds_load(in);

    if (!in.good()) {
        throw std::runtime_error("GbwtBackend::load: simple_sds_load failed");
    }

    return GbwtBackend{std::move(gbwt)};
}

}  // namespace ggql
