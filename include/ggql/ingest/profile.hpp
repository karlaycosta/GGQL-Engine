#pragma once

// profile.hpp — Perfis de ingestão.
//
// Mapeia casos de uso biológicos (pangenoma, metagenômica, assembly graph,
// comparativa) ao subconjunto de estruturas a construir em ingest().
// Documentação completa: docs/ingest_profiles.md.

#include <optional>
#include <string_view>

namespace ggql {

enum class IngestProfile {
    Pangenome,    // tudo: MetadataStore, FM-Index, Topology, gBWT, Snarls
    Metagenomics, // sem gBWT e sem snarls (sem haplótipos significativos)
    Assembly,     // sem gBWT, com snarls (QC de montagem)
    Comparative,  // com gBWT, sem snarls (paths = espécies/strains)
};

struct ProfileFlags {
    bool save_metadata;
    bool build_fm_index;
    bool build_topology;
    bool build_gbwt;
    bool build_snarls;
};

constexpr ProfileFlags flags_for(IngestProfile p) noexcept {
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
    return {true, true, true, true, true}; // unreachable, default = pangenome
}

constexpr std::string_view name_of(IngestProfile p) noexcept {
    switch (p) {
    case IngestProfile::Pangenome:    return "pangenome";
    case IngestProfile::Metagenomics: return "metagenomics";
    case IngestProfile::Assembly:     return "assembly";
    case IngestProfile::Comparative:  return "comparative";
    }
    return "pangenome";
}

inline std::optional<IngestProfile> parse_profile(std::string_view s) noexcept {
    if (s == "pangenome"    || s == "pan")  { return IngestProfile::Pangenome; }
    if (s == "metagenomics" || s == "meta") { return IngestProfile::Metagenomics; }
    if (s == "assembly"     || s == "asm")  { return IngestProfile::Assembly; }
    if (s == "comparative"  || s == "comp") { return IngestProfile::Comparative; }
    return std::nullopt;
}

} // namespace ggql
