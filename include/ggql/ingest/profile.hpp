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

struct ProfileFlags {
    bool save_metadata  = false;
    bool build_fm_index = false;
    bool build_topology = false;
    bool build_gbwt     = false;
    bool build_snarls   = false;
};

[[nodiscard]] constexpr ProfileFlags flags_for(IngestProfile p) noexcept {
    switch (p) {
    case IngestProfile::Pangenome:
        return {true, true,  true, true,  true};
    case IngestProfile::Metagenomics:
        return {true, true,  true, false, false};
    case IngestProfile::Assembly:
        return {true, false, true, false, true};
    case IngestProfile::Comparative:
        return {true, false, true, true,  false};
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
