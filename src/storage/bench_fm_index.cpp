#include "ggql/ingest/gfa_ingestor.hpp"
#include "ggql/ingest/gfa_metadata_store.hpp"
#include "ggql/storage/gfa_fm_index.hpp"

#include <chrono>
#include <fmt/core.h>
#include <fmt/color.h>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using hrclock = std::chrono::high_resolution_clock;
using sec_t = std::chrono::duration<double>;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fmt::println(stderr,
                     "Uso: bench_fm_index <arquivo.gfa> [threads] [kmer_size] [num_searches]");
        return 1;
    }

    const std::string path = argv[1];
    const int threads = (argc >= 3) ? std::stoi(argv[2]) : 8;
    const size_t kmer_size = (argc >= 4) ? std::stoul(argv[3]) : 31;
    const size_t num_searches = (argc >= 5) ? std::stoul(argv[4]) : 10000;

    fmt::println("Iniciando benchmark...");
    std::fflush(stdout);

    ggql::GfaIngestorMmapParallel ingestor;

    fmt::println("Iniciando ingest...");
    std::fflush(stdout);

    auto res =
        ingestor.ingest(path, threads, 2.0, true, true, false, true, 0, false, ".idx_bench/");

    if (!res) {
        fmt::println(stderr, "Ingest falhou: {}", res.error());
        return 1;
    }

    fmt::println("Ingest concluido. Carregando metadados...");
    std::fflush(stdout);

    ggql::GfaMetadataStore metadata;
    metadata.load(".idx_bench/");

    fmt::println("Carregando index...");
    std::fflush(stdout);

    ggql::GfaSequenceIndex index;
    index.load(".idx_bench/");

    if (!index.is_loaded()) {
        fmt::println(stderr, "Index nao carregado.");
        return 1;
    }

    const auto& all_seqs = metadata.all_sequences();
    fmt::println("Sequencias carregadas: {} bases.", all_seqs.size());
    std::fflush(stdout);

    if (all_seqs.size() < kmer_size + 1) {
        fmt::println(stderr, "Texto muito curto.");
        return 1;
    }

    fmt::println("Gerando k-mers (iterando por segmentos)...");
    std::fflush(stdout);

    // Gera k-mers por amostragem de segmentos elegíveis (length >= kmer_size).
    // Evita o problema de janelas aleatórias no texto concatenado caírem em
    // '$' quando os segmentos são pequenos (bench_1M tem ~1 base/segmento).
    // Bônus: segment_id é conhecido sem passar por segment_id_from_offset().
    std::vector<uint64_t> eligible;
    eligible.reserve(metadata.size());
    for (uint64_t id = 0; id < metadata.size(); ++id) {
        if (metadata.sequence(id).size() >= kmer_size) {
            eligible.push_back(id);
        }
    }
    fmt::println("Segmentos eleg\u00edveis (seq >= {} bases): {} / {}",
                 kmer_size, eligible.size(), metadata.size());
    std::fflush(stdout);

    if (eligible.empty()) {
        fmt::println(stderr,
                     "Nenhum segmento tem length >= {}; use kmer_size menor.",
                     kmer_size);
        return 1;
    }

    std::vector<std::string> query_kmers;
    std::vector<uint64_t> expected_ids;       // segment_id de onde o k-mer foi extraído
    std::vector<uint64_t> expected_offsets;   // offset global (seq_offsets_[sid]+off)
    query_kmers.reserve(num_searches);
    expected_ids.reserve(num_searches);
    expected_offsets.reserve(num_searches);

    std::mt19937 gen(42);
    std::uniform_int_distribution<size_t> seg_dist(0, eligible.size() - 1);
    const auto& seg_offsets = metadata.all_seq_offsets();

    for (size_t i = 0; i < num_searches; ++i) {
        const uint64_t sid = eligible[seg_dist(gen)];
        const auto seq = metadata.sequence(sid);
        if (seq.size() < kmer_size) {
            continue; // defensivo
        }
        std::uniform_int_distribution<size_t> off_dist(0, seq.size() - kmer_size);
        const size_t off = off_dist(gen);
        query_kmers.emplace_back(seq.substr(off, kmer_size));
        expected_ids.push_back(sid);
        expected_offsets.push_back(seg_offsets[sid] + off);
    }

    fmt::println("Iniciando buscas...");
    std::fflush(stdout);

    size_t total_hits = 0;
    auto t0 = hrclock::now();
    for (const auto& kmer : query_kmers) {
        auto hits = index.search_kmer(kmer);
        total_hits += hits.size();
    }
    auto t1 = hrclock::now();

    double s = std::chrono::duration<double>(t1 - t0).count();
    fmt::println("Buscas concluidas em {:.4f} s. Hits: {}", s, total_hits);
    if (s > 0) {
        fmt::println("Throughput: {:.2f} k-mers/s", num_searches / s);
    }

    // ── Oráculo: FM-Index → segment_id_from_offset() → QueryPlanner contract ──
    // Para cada k-mer gerado na posição `pos` (segmento esperado conhecido),
    // valida que search_kmer() retorna pelo menos um offset que mapeia para
    // o mesmo segmento via metadata.segment_id_from_offset(). Isto cobre o
    // caminho exato usado por QueryPlanner::execute_search_kmer() e pela
    // visita ContainsKmerCondition.
    fmt::println("Validando or\u00e1culo FM-Index \u2194 MetadataStore...");
    std::fflush(stdout);

    size_t validated = 0;
    size_t failed = 0;
    size_t missing_own_offset = 0;    // FM-Index não retornou o próprio offset (bug de busca)
    size_t wrong_segment_mapping = 0; // Retornou offset correto mas segment_id errado
    auto tv0 = hrclock::now();
    for (size_t i = 0; i < query_kmers.size(); ++i) {
        const uint64_t expected_sid = expected_ids[i];
        const uint64_t expected_off = expected_offsets[i];
        const auto hits = index.search_kmer(query_kmers[i]);

        bool found_own_offset = false;
        bool found_expected_sid = false;
        for (uint64_t off : hits) {
            if (off == expected_off) {
                found_own_offset = true;
            }
            auto sid = metadata.segment_id_from_offset(off);
            if (sid && *sid == expected_sid) {
                found_expected_sid = true;
            }
        }

        if (found_expected_sid) {
            ++validated;
        } else {
            ++failed;
            if (found_own_offset) {
                ++wrong_segment_mapping;
            } else {
                ++missing_own_offset;
            }
            if (failed <= 3) {
                std::string at_expected(&all_seqs[expected_off], kmer_size);
                fmt::println(stderr,
                             "  [fail] kmer='{}' sid esperado={} off esperado={} "
                             "hits={} found_own_offset={}",
                             query_kmers[i], expected_sid, expected_off,
                             hits.size(), found_own_offset);
                fmt::println(stderr, "     all_seqs[{}..+{}]='{}' match={}",
                             expected_off, kmer_size, at_expected,
                             at_expected == query_kmers[i]);
                for (size_t h = 0; h < hits.size() && h < 5; ++h) {
                    auto sid = metadata.segment_id_from_offset(hits[h]);
                    std::string at_hit(&all_seqs[hits[h]], kmer_size);
                    fmt::println(stderr, "     hit[{}]={} \u2192 sid={} content='{}'",
                                 h, hits[h],
                                 sid ? std::to_string(*sid) : std::string("?"),
                                 at_hit);
                }
            }
        }
    }
    auto tv1 = hrclock::now();
    double vs = std::chrono::duration<double>(tv1 - tv0).count();

    fmt::println("Oraculo: {}/{} OK, {} falhas ({:.4f} s)",
                 validated, query_kmers.size(), failed, vs);
    fmt::println("  \u2937 missing own offset : {} (FM-Index n\u00e3o retornou a pr\u00f3pria posi\u00e7\u00e3o)",
                 missing_own_offset);
    fmt::println("  \u2937 wrong segment     : {} (segment_id_from_offset errado)",
                 wrong_segment_mapping);

    return failed == 0 ? 0 : 2;
}
