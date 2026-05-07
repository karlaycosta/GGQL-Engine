// test_ingestor_profiles.cpp — testa os perfis de ingestão (build_gbwt /
// build_snarls separados de build_topology) e a invalidação de cache quando
// um flag é elevado entre execuções.

#include "ggql/ingest/ingestor.hpp"
#include "ggql/ingest/profile.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// GFA simples com 1 path P, suficiente para coletar W/P-line e exercitar
// build_gbwt e build_snarls de forma realista.
static std::string write_gfa(const std::string& name) {
    const std::string path = "/tmp/ggql_profiles_" + name + ".gfa";
    std::ofstream f(path);
    f << "S\tA\tACGT\n"
      << "S\tB\tGGGG\n"
      << "S\tC\tTTTT\n"
      << "L\tA\t+\tB\t+\t0M\n"
      << "L\tB\t+\tC\t+\t0M\n"
      << "P\tp1\tA+,B+,C+\t*\n";
    return path;
}

static void cleanup(const std::string& gfa, const std::string& idx) {
    ::unlink(gfa.c_str());
    fs::remove_all(idx);
}

// ── 1. Validação: build_gbwt sem build_topology deve falhar ───────────────────
static void test_validation_gbwt_requires_topology() {
    auto gfa = write_gfa("val_gbwt");
    const std::string idx = "/tmp/ggql_profiles_val_gbwt_idx/";

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false,
                          /*save_metadata=*/false,
                          /*build_topology=*/false,
                          /*build_fm_index=*/false,
                          /*max_gbwt_paths=*/1000,
                          /*keep_gbwt_tmp=*/false, idx,
                          /*build_gbwt=*/true,
                          /*build_snarls=*/false);
    assert(!res.has_value() && "build_gbwt sem build_topology deve falhar");

    cleanup(gfa, idx);
}

static void test_validation_snarls_requires_topology() {
    auto gfa = write_gfa("val_snarls");
    const std::string idx = "/tmp/ggql_profiles_val_snarls_idx/";

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false,
                          false, /*build_topology=*/false, false,
                          0, false, idx,
                          /*build_gbwt=*/false,
                          /*build_snarls=*/true);
    assert(!res.has_value() && "build_snarls sem build_topology deve falhar");

    cleanup(gfa, idx);
}

// ── 2. Perfil metagenomics: sem gBWT, sem snarls ──────────────────────────────
static void test_profile_metagenomics() {
    auto gfa = write_gfa("meta");
    const std::string idx = "/tmp/ggql_profiles_meta_idx/";

    const auto pf = ggql::flags_for(ggql::IngestProfile::Metagenomics);
    assert(!pf.build_gbwt);
    assert(!pf.build_snarls);

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false,
                          pf.save_metadata, pf.build_topology, pf.build_fm_index,
                          /*max_gbwt_paths=*/1000, false, idx,
                          pf.build_gbwt, pf.build_snarls);
    assert(res.has_value());
    assert(res->node_count == 3);
    // Sem gBWT: path_count e gbwt_index_size_b ficam zero.
    assert(res->path_count == 0);
    assert(res->gbwt_index_size_b == 0);
    // Sem snarls: arquivo não deve existir no cache.
    assert(!fs::exists(idx + "/path_stores.bin"));
    assert(!fs::exists(idx + "/snarls.bin"));
    // Topology sim, foi construída.
    assert(fs::exists(idx + "/E_fwd.bin"));

    cleanup(gfa, idx);
}

// ── 3. Perfil pangenome: gBWT e snarls construídos ────────────────────────────
static void test_profile_pangenome() {
    auto gfa = write_gfa("pan");
    const std::string idx = "/tmp/ggql_profiles_pan_idx/";

    const auto pf = ggql::flags_for(ggql::IngestProfile::Pangenome);
    assert(pf.build_gbwt);
    assert(pf.build_snarls);

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false,
                          pf.save_metadata, pf.build_topology, pf.build_fm_index,
                          /*max_gbwt_paths=*/1000, false, idx,
                          pf.build_gbwt, pf.build_snarls);
    assert(res.has_value());
    assert(res->path_count == 1);
    assert(res->gbwt_index_size_b > 0);
    assert(fs::exists(idx + "/path_stores.bin"));
    // snarls.bin só é gravado se snarls foram detectadas; em GFA linear
    // (sem bolhas) o arquivo pode não existir mesmo com build_snarls=true.
    // O importante: path_stores.bin existe.

    cleanup(gfa, idx);
}

// ── 4. Cache regenera ao elevar build_gbwt entre execuções ────────────────────
// 1ª execução: build_gbwt=false → não grava path_stores.bin.
// 2ª execução: build_gbwt=true → cache hit não pode ser silencioso; tem que
// regenerar para popular o índice de paths.
static void test_cache_invalidates_on_gbwt_upgrade() {
    auto gfa = write_gfa("cache_up");
    const std::string idx = "/tmp/ggql_profiles_cache_up_idx/";

    // 1ª: sem gBWT
    {
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false, true, true, false,
                              0, false, idx,
                              /*build_gbwt=*/false,
                              /*build_snarls=*/false);
        assert(res.has_value());
        assert(!fs::exists(idx + "/path_stores.bin"));
    }

    // 2ª: pede gBWT — deve regenerar (não cache hit silencioso)
    {
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false, true, true, false,
                              /*max_gbwt_paths=*/1000, false, idx,
                              /*build_gbwt=*/true,
                              /*build_snarls=*/false);
        assert(res.has_value());
        assert(fs::exists(idx + "/path_stores.bin"));
        assert(res->path_count == 1);
    }

    cleanup(gfa, idx);
}

// ── 5. Cache hit válido quando os flags casam ─────────────────────────────────
// Reuso do mesmo cache deve ser instantâneo (elapsed_total_s pequeno).
static void test_cache_hit_when_flags_match() {
    auto gfa = write_gfa("cache_hit");
    const std::string idx = "/tmp/ggql_profiles_cache_hit_idx/";

    const auto pf = ggql::flags_for(ggql::IngestProfile::Metagenomics);

    // 1ª: cria cache
    {
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              0, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
    }

    // 2ª: mesmos flags — deve dar cache hit (rápido, sem re-parse)
    {
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              0, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
        // elapsed_io1_s == 0 indica que não fez Pass1 (cache hit limpo).
        assert(res->elapsed_io1_s == 0.0 && "cache hit deve evitar Pass1");
    }

    cleanup(gfa, idx);
}

// ── 6. parse_profile + name_of round-trip ─────────────────────────────────────
static void test_profile_parsing() {
    using ggql::IngestProfile;
    using ggql::parse_profile;
    using ggql::name_of;

    assert(parse_profile("pangenome").value()    == IngestProfile::Pangenome);
    assert(parse_profile("pan").value()          == IngestProfile::Pangenome);
    assert(parse_profile("metagenomics").value() == IngestProfile::Metagenomics);
    assert(parse_profile("meta").value()         == IngestProfile::Metagenomics);
    assert(parse_profile("assembly").value()     == IngestProfile::Assembly);
    assert(parse_profile("asm").value()          == IngestProfile::Assembly);
    assert(parse_profile("comparative").value()  == IngestProfile::Comparative);
    assert(parse_profile("comp").value()         == IngestProfile::Comparative);
    assert(!parse_profile("xyz").has_value());
    assert(!parse_profile("").has_value());

    assert(name_of(IngestProfile::Pangenome) == "pangenome");
    assert(name_of(IngestProfile::Metagenomics) == "metagenomics");
}

// ── 7. Upgrade incremental: comparative → pangenome só roda snarls ─────────
// Cache de comparative tem gBWT mas não snarls. Subir para pangenome deve
// completar APENAS snarls sem reler o GFA (Fase 1 do upgrade incremental).
// Validação: elapsed_io1_s == 0 indica que Pass1 foi pulado.
static std::string write_bubble_gfa(const std::string& name) {
    const std::string path = "/tmp/ggql_profiles_" + name + ".gfa";
    std::ofstream f(path);
    // Bolha simples: A → {B,C} → D, mais um path A,B,D
    f << "S\tA\tACGT\n"
      << "S\tB\tGGGG\n"
      << "S\tC\tTTTT\n"
      << "S\tD\tCCCC\n"
      << "L\tA\t+\tB\t+\t0M\n"
      << "L\tA\t+\tC\t+\t0M\n"
      << "L\tB\t+\tD\t+\t0M\n"
      << "L\tC\t+\tD\t+\t0M\n"
      << "P\tp1\tA+,B+,D+\t*\n";
    return path;
}

static void test_incremental_snarls_upgrade() {
    auto gfa = write_bubble_gfa("incr");
    const std::string idx = "/tmp/ggql_profiles_incr_idx/";
    fs::remove_all(idx);

    // 1ª: comparative (gBWT sim, snarls não)
    {
        const auto pf = ggql::flags_for(ggql::IngestProfile::Comparative);
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              1000, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
        assert(fs::exists(idx + "/path_stores.bin"));
        assert(!fs::exists(idx + "/snarls.bin"));
    }

    // 2ª: pangenome — só snarls falta. Deve completar incrementalmente.
    {
        const auto pf = ggql::flags_for(ggql::IngestProfile::Pangenome);
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              1000, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
        assert(fs::exists(idx + "/snarls.bin"));
        // Pass1/Pass2 não devem ter rodado — elapsed_io1_s == 0 prova que o
        // path foi cache + complete_with_snarls, não reingestão completa.
        assert(res->elapsed_io1_s == 0.0
               && "upgrade comparative→pangenome não deve reler GFA");
    }

    cleanup(gfa, idx);
}

// ── 8. Upgrade incremental Fase 2: metagenomics → comparative ──────────────
// Cache de metagenomics não tem gBWT. Subir para comparative deve fazer Pass2
// paths-only (sem reconstruir MPHF/topology/metadata/fm_index).
// Validação: elapsed_io1_s == 0 e elapsed_topology_s == 0 indicam que MPHF e
// CSR não foram reconstruídos.
static void test_incremental_gbwt_upgrade() {
    auto gfa = write_bubble_gfa("incr_gbwt");
    const std::string idx = "/tmp/ggql_profiles_incr_gbwt_idx/";
    fs::remove_all(idx);

    // 1ª: metagenomics (sem gBWT, sem snarls)
    {
        const auto pf = ggql::flags_for(ggql::IngestProfile::Metagenomics);
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              0, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
        assert(!fs::exists(idx + "/path_stores.bin"));
    }

    // 2ª: comparative (gBWT sim, snarls não) — Fase 2.
    {
        const auto pf = ggql::flags_for(ggql::IngestProfile::Comparative);
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              1000, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
        assert(fs::exists(idx + "/path_stores.bin"));
        assert(res->path_count == 1);
        // Pass1 NÃO rodou — elapsed_io1_s e elapsed_topology_s zerados.
        assert(res->elapsed_io1_s == 0.0
               && "Pass2 paths-only não deve reconstruir MPHF");
        assert(res->elapsed_topology_s == 0.0
               && "Pass2 paths-only não deve reconstruir CSR");
        // Pass2 rodou — elapsed_ingest_s > 0.
        assert(res->elapsed_ingest_s > 0.0
               && "Pass2 paths-only deve ser executado");
    }

    cleanup(gfa, idx);
}

// ── 9. Upgrade incremental: metagenomics → pangenome ───────────────────────
// Caso composto: precisa Fase 2 (gBWT) seguida de Fase 1 (snarls), ambos
// derivados da topology em cache, sem reler segmentos/links do GFA.
static void test_incremental_meta_to_pangenome() {
    auto gfa = write_bubble_gfa("incr_full");
    const std::string idx = "/tmp/ggql_profiles_incr_full_idx/";
    fs::remove_all(idx);

    // 1ª: metagenomics
    {
        const auto pf = ggql::flags_for(ggql::IngestProfile::Metagenomics);
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              0, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
    }

    // 2ª: pangenome — Fase 2 + Fase 1.
    {
        const auto pf = ggql::flags_for(ggql::IngestProfile::Pangenome);
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              1000, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
        assert(fs::exists(idx + "/path_stores.bin"));
        assert(fs::exists(idx + "/snarls.bin"));
        assert(res->elapsed_io1_s == 0.0);
        assert(res->elapsed_topology_s == 0.0);
    }

    cleanup(gfa, idx);
}

// ── 10. Downgrade pangenome → metagenomics apaga path_stores.bin/snarls.bin
// Manifest deve refletir o novo perfil e os arquivos órfãos devem ser
// removidos do disco (não basta ignorá-los — o load() carregaria do disco).
static void test_downgrade_pangenome_to_metagenomics() {
    auto gfa = write_bubble_gfa("downgrade");
    const std::string idx = "/tmp/ggql_profiles_downgrade_idx/";
    fs::remove_all(idx);

    // 1ª: pangenome — cria path_stores.bin + snarls.bin
    {
        const auto pf = ggql::flags_for(ggql::IngestProfile::Pangenome);
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              1000, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
        assert(fs::exists(idx + "/path_stores.bin"));
        assert(fs::exists(idx + "/snarls.bin"));
    }

    // 2ª: metagenomics no MESMO dir — deve apagar path_stores.bin + snarls.bin
    {
        const auto pf = ggql::flags_for(ggql::IngestProfile::Metagenomics);
        ggql::GfaIngestorMmapParallel ing;
        auto res = ing.ingest(gfa, 1, 2.0, false,
                              pf.save_metadata, pf.build_topology, pf.build_fm_index,
                              0, false, idx, pf.build_gbwt, pf.build_snarls);
        assert(res.has_value());
        assert(!fs::exists(idx + "/path_stores.bin")
               && "downgrade deve remover path_stores.bin");
        assert(!fs::exists(idx + "/gbwts.bin")
               && "downgrade deve remover gbwts.bin legacy");
        assert(!fs::exists(idx + "/snarls.bin")
               && "downgrade deve remover snarls.bin");
        // FM-Index continua (metagenomics inclui FM-Index)
        assert(fs::exists(idx + "/fm_index.bin"));
    }

    // 3ª: leitura do manifest deve refletir os novos flags
    {
        ggql::GfaIngestorMmapParallel ing;
        auto cf = ing.read_cached_flags(idx);
        assert(cf.has_value());
        assert(cf->save_metadata);
        assert(cf->build_fm_index);
        assert(cf->build_topology);
        assert(!cf->build_gbwt && "manifest deve refletir gBWT desabilitado");
        assert(!cf->build_snarls && "manifest deve refletir snarls desabilitado");
    }

    cleanup(gfa, idx);
}

// ── 11. Manifest contém profile + indexed_at ──────────────────────────────────
static void test_manifest_has_profile_and_timestamp() {
    auto gfa = write_bubble_gfa("manifest");
    const std::string idx = "/tmp/ggql_profiles_manifest_idx/";
    fs::remove_all(idx);

    const auto pf = ggql::flags_for(ggql::IngestProfile::Pangenome);
    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false,
                          pf.save_metadata, pf.build_topology, pf.build_fm_index,
                          1000, false, idx, pf.build_gbwt, pf.build_snarls);
    assert(res.has_value());

    // Lê manifest cru
    std::ifstream m(idx + "/manifest.txt");
    std::string line;
    bool has_profile = false, has_indexed = false, has_v4 = false;
    while (std::getline(m, line)) {
        if (line == "profile=pangenome") { has_profile = true; }
        else if (line.starts_with("indexed_at=")) {
            has_indexed = true;
            // Formato: indexed_at=YYYY-MM-DDTHH:MM:SSZ
            assert(line.size() == std::string("indexed_at=YYYY-MM-DDTHH:MM:SSZ").size());
        }
        else if (line == "version=4") { has_v4 = true; }
    }
    assert(has_v4 && "manifest deve estar em version=4");
    assert(has_profile && "manifest deve ter profile=pangenome");
    assert(has_indexed && "manifest deve ter indexed_at em ISO-8601");

    cleanup(gfa, idx);
}

int main() {
    test_validation_gbwt_requires_topology();
    test_validation_snarls_requires_topology();
    test_profile_metagenomics();
    test_profile_pangenome();
    test_cache_invalidates_on_gbwt_upgrade();
    test_cache_hit_when_flags_match();
    test_profile_parsing();
    test_incremental_snarls_upgrade();
    test_incremental_gbwt_upgrade();
    test_incremental_meta_to_pangenome();
    test_downgrade_pangenome_to_metagenomics();
    test_manifest_has_profile_and_timestamp();
    return EXIT_SUCCESS;
}
