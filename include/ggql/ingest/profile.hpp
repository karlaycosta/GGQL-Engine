#pragma once

// profile.hpp — Perfis de ingestão.
//
// Mapeia casos de uso biológicos (pangenoma, metagenômica, assembly graph,
// comparativa) ao subconjunto de estruturas a construir em ingest().
// Documentação completa: docs/ingest_profiles.md.

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility> // std::unreachable

namespace ggql {

enum class IngestProfile : uint8_t {
    Pangenome,    // tudo: MetadataStore, FM-Index, Topology, gBWT, Snarls
    Metagenomics, // sem gBWT e sem snarls (sem haplótipos significativos)
    Assembly,     // sem gBWT, com snarls (QC de montagem)
    Comparative,  // com gBWT, sem snarls (paths = espécies/strains)
};

// PathBackendKind — escolha de backend para path_stores_ (Camada 2 da
// GfaCsrTopology). Coexistencia controlada pelo IngestProfile/flags.
//
//   PathStorage  — uint32 cru concatenado; ratio 4 B/no; extract O(L) memcpy
//   Gbwt         — gbwt::GBWT comprimido; ratio ~1-2 bits/no; extract via LF
//   Auto         — heuristica futura (Task #6); por enquanto = PathStorage
//
// Ortogonal a build_gbwt: a flag aqui escolhe COMO os paths sao armazenados
// quando a Camada 2 e construida. build_gbwt apenas decide SE construi-la.
enum class PathBackendKind : uint8_t {
    PathStorage = 0,
    Gbwt        = 1,
    Auto        = 2,
};

[[nodiscard]] constexpr std::string_view name_of(PathBackendKind k) noexcept {
    switch (k) {
    case PathBackendKind::PathStorage: return "pathstorage";
    case PathBackendKind::Gbwt:        return "gbwt";
    case PathBackendKind::Auto:        return "auto";
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::optional<PathBackendKind>
parse_path_backend(std::string_view s) noexcept {
    if (s == "pathstorage" || s == "ps") { return PathBackendKind::PathStorage; }
    if (s == "gbwt"        || s == "gb") { return PathBackendKind::Gbwt; }
    if (s == "auto")                     { return PathBackendKind::Auto; }
    return std::nullopt;
}

struct ProfileFlags {
    bool             save_metadata  = false;
    bool             build_fm_index = false;
    bool             build_topology = false;
    bool             build_gbwt     = false;
    bool             build_snarls   = false;
    PathBackendKind  path_backend   = PathBackendKind::PathStorage;
};

[[nodiscard]] constexpr ProfileFlags flags_for(IngestProfile p) noexcept {
    switch (p) {
    case IngestProfile::Pangenome:
        // Default: PathStorage (preserva comportamento atual). Pangenoma
        // grande pode trocar para Gbwt via flag custom para reduzir RAM 16x.
        return {true, true,  true, true,  true,  PathBackendKind::PathStorage};
    case IngestProfile::Metagenomics:
        return {true, true,  true, false, false, PathBackendKind::PathStorage};
    case IngestProfile::Assembly:
        return {true, false, true, false, true,  PathBackendKind::PathStorage};
    case IngestProfile::Comparative:
        return {true, false, true, true,  false, PathBackendKind::PathStorage};
    }
    std::unreachable(); // todos os enum values cobertos acima
}

[[nodiscard]] constexpr std::string_view name_of(IngestProfile p) noexcept {
    switch (p) {
    case IngestProfile::Pangenome:    return "pangenome";
    case IngestProfile::Metagenomics: return "metagenomics";
    case IngestProfile::Assembly:     return "assembly";
    case IngestProfile::Comparative:  return "comparative";
    }
    std::unreachable(); // todos os enum values cobertos acima
}

[[nodiscard]] constexpr std::optional<IngestProfile>
parse_profile(std::string_view s) noexcept {
    if (s == "pangenome"    || s == "pan")  { return IngestProfile::Pangenome; }
    if (s == "metagenomics" || s == "meta") { return IngestProfile::Metagenomics; }
    if (s == "assembly"     || s == "asm")  { return IngestProfile::Assembly; }
    if (s == "comparative"  || s == "comp") { return IngestProfile::Comparative; }
    return std::nullopt;
}

} // namespace ggql
