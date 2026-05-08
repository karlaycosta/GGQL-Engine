// gbwt_backend.cpp — Implementacao do adapter GbwtBackend.

#include "ggql/gbwt/gbwt_backend.hpp"

#include "ggql/ingest/gbwt_group.hpp"  // gbwt_decode_node, kEndmarker

#include <gbwt/gbwt.h>
#include <gbwt/utils.h>  // gbwt::ENDMARKER
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

GbwtBackend::GbwtBackend(std::unique_ptr<::gbwt::GBWT> gbwt) noexcept
    : gbwt_(std::move(gbwt)) {}

// ── Interface usada pela topology ─────────────────────────────────────────

void GbwtBackend::extract_into(std::size_t                 path_id,
                               std::vector<std::uint64_t>& out) const {
    out.clear();
    if (!gbwt_) { return; }

    // ::gbwt::GBWT::extract retorna sequencia ENCODED (gbwt-bound: shift+1 +
    // bit de orientacao). Aplicamos gbwt_decode_node aqui para que o caller
    // (path_nodes em GfaCsrTopology) receba IDs internos globais — espelha
    // exatamente o comportamento de cb905db:src/storage/graph_algorithms.cpp.
    //
    // ENDMARKER (=0) e filtrado: nao representa um no real, e gbwt_decode_node
    // sobre 0 e UB (wrap para UINT64_MAX).
    const auto seq = gbwt_->extract(static_cast<::gbwt::size_type>(path_id));
    out.reserve(seq.size());
    for (const auto enc : seq) {
        if (enc == ::gbwt::ENDMARKER) { continue; }
        out.push_back(ggql::gbwt_decode_node(static_cast<std::uint64_t>(enc)));
    }
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
//   [12..]   ::gbwt::GBWT::simple_sds_serialize payload (cabecalho proprio)
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
        ::gbwt::GBWT empty;
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

    auto gbwt = std::make_unique<::gbwt::GBWT>();
    gbwt->simple_sds_load(in);

    if (!in.good()) {
        throw std::runtime_error("GbwtBackend::load: simple_sds_load failed");
    }

    return GbwtBackend{std::move(gbwt)};
}

}  // namespace ggql
