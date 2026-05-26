#pragma once

// profile.hpp — Perfis de ingestão.
//
// Mapeia casos de uso biológicos (pangenoma, metagenômica, assembly graph,
// comparativa) ao subconjunto de estruturas a construir em ingest().
// Documentação completa: docs/ingest_profiles.md.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
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

// Sumário estrutural do GFA usado para inferir o perfil de ingestão.
// Pode ser preenchido por uma varredura leve (survey_gfa_light) ou
// reaproveitado de um Pass1 completo já executado no ingestor.
struct IngestSurvey {
    uint64_t segment_count        = 0;
    uint64_t link_count           = 0;
    uint64_t path_count           = 0;  // W-lines + P-lines (agregado)
    uint64_t total_sequence_bytes = 0;  // soma do campo seq das S-lines (≠ file_size)
    uint64_t file_size_bytes      = 0;
    double   avg_path_length      = 0.0;  // média de nós por path (0 se path_count == 0)
    double   elapsed_s            = 0.0;
    bool     has_w                = false; // ao menos 1 W-line vista
};

// Varredura leve do GFA: mmap + paralelismo OpenMP + apenas contagem
// de S/L/W/P. Não aloca keys nem links; não constrói MPHF nem UF.
// Custo dominante: leitura sequencial de file_size bytes do mmap (cold
// cache) ou ~constante per-byte (warm cache). Pra GRCh38-464 (~3.3 GB):
// cold ~1.5 s em NVMe rápido, warm ~0.5 s.
//
// Retorna IngestSurvey ou string de erro (arquivo não encontrado, mmap
// falhou). Standalone — não precisa de instância de GfaIngestorMmapParallel.
[[nodiscard]] std::expected<IngestSurvey, std::string>
survey_gfa_light(const std::string& filepath, int num_threads = 4);

// Heurística baseada em densidade de links e presença de paths.
// Threshold 1.5 (links/segs) escolhido empiricamente — pangenomas HPRC
// típicos ficam em ~1.4–1.5; assembly graphs ramificados sobem rapidamente.
[[nodiscard]] inline IngestProfile suggest_profile(const IngestSurvey& s) noexcept {
    const double density = (s.segment_count > 0)
        ? static_cast<double>(s.link_count) / static_cast<double>(s.segment_count)
        : 0.0;
    if (s.path_count == 0) {
        return density > 1.5 ? IngestProfile::Assembly : IngestProfile::Metagenomics;
    }
    return density > 1.5 ? IngestProfile::Pangenome : IngestProfile::Comparative;
}

} // namespace ggql
