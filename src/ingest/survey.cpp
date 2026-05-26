// survey.cpp — Varredura leve do GFA (light Pass1).
//
// Conta apenas S/L/W/P lines via mmap + OpenMP, sem alocar keys/links.
// Usado por suggest_profile() antes de decidir os flags do ingest real.

#include "ggql/ingest/profile.hpp"
#include "ggql/ingest/ingestor.hpp"  // MmapFileParallel

#include <chrono>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace ggql {

namespace {

// Mesma rotina de alinhamento usada em pass1 — encontra início de linha.
inline const char* align_to_line_start_local(const char* p, const char* start) {
    while (p > start && *(p - 1) != '\n') {
        --p;
    }
    return p;
}

inline const char* skip_line_local(const char* p, const char* end) {
    while (p < end && *p != '\n') {
        ++p;
    }
    if (p < end) {
        ++p;
    }
    return p;
}

} // namespace

std::expected<IngestSurvey, std::string>
survey_gfa_light(const std::string& filepath, int num_threads) {
    using clock = std::chrono::high_resolution_clock;
    using sec = std::chrono::duration<double>;

    const auto t0 = clock::now();

    auto mmap_res = MmapFileParallel::open(filepath);
    if (!mmap_res) {
        return std::unexpected(mmap_res.error());
    }
    const char* data_start = mmap_res->data;
    const size_t total_size = mmap_res->size;
    const char* data_end = data_start + total_size;

    if (num_threads < 1) { num_threads = 1; }

    std::vector<uint64_t> t_segs(static_cast<size_t>(num_threads), 0);
    std::vector<uint64_t> t_links(static_cast<size_t>(num_threads), 0);
    std::vector<uint64_t> t_paths(static_cast<size_t>(num_threads), 0);
    std::vector<uint64_t> t_seq_bytes(static_cast<size_t>(num_threads), 0);
    std::vector<uint64_t> t_path_nodes(static_cast<size_t>(num_threads), 0);
    std::vector<uint8_t>  t_has_w(static_cast<size_t>(num_threads), 0);

    const size_t chunk_size = total_size / static_cast<size_t>(num_threads);

#pragma omp parallel num_threads(num_threads)
    {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        const char* chunk_start = data_start + tid * chunk_size;
        const char* chunk_end =
            (tid == num_threads - 1) ? data_end : (chunk_start + chunk_size);

        if (tid > 0) {
            chunk_start = align_to_line_start_local(chunk_start, data_start);
        }
        if (tid < num_threads - 1) {
            const char* next_chunk = data_start + (tid + 1) * chunk_size;
            chunk_end = align_to_line_start_local(next_chunk, data_start);
        }

        uint64_t s = 0, l = 0, p_cnt = 0;
        uint64_t seq_bytes = 0, path_nodes = 0;
        bool has_w = false;
        const char* p = chunk_start;
        while (p < chunk_end) {
            const char first = *p;
            // Comentários, headers (H), etc. são puladas (default).
            if ((first == 'S' || first == 'L' || first == 'W' || first == 'P')
                && p + 1 < chunk_end && *(p + 1) == '\t') {
                // Encontra fim da linha uma vez (reusado em S/W/P).
                const char* line_end = p;
                while (line_end < chunk_end && *line_end != '\n') { ++line_end; }
                switch (first) {
                    case 'S': {
                        ++s;
                        // S\t<name>\t<seq>\t[tags] — pular ao 2º TAB, contar até o 3º (ou fim).
                        const char* c = p + 2; // pós "S\t"
                        while (c < line_end && *c != '\t') { ++c; } // pula <name>
                        if (c < line_end) {
                            ++c; // skip TAB
                            const char* seq_start = c;
                            while (c < line_end && *c != '\t') { ++c; }
                            seq_bytes += static_cast<uint64_t>(c - seq_start);
                        }
                        break;
                    }
                    case 'L': ++l; break;
                    case 'W':
                    case 'P': {
                        ++p_cnt;
                        if (first == 'W') { has_w = true; }
                        // Conta nós (separadores) — mesma lógica do Pass1.
                        uint64_t nodes = 0;
                        if (first == 'P') {
                            for (const char* c = p; c < line_end; ++c) {
                                if (*c == ',') { ++nodes; }
                            }
                            if (line_end > p) { ++nodes; }
                        } else {
                            for (const char* c = p; c < line_end; ++c) {
                                if (*c == '>' || *c == '<') { ++nodes; }
                            }
                        }
                        path_nodes += nodes;
                        break;
                    }
                }
                p = (line_end < chunk_end) ? (line_end + 1) : line_end;
            } else {
                p = skip_line_local(p, chunk_end);
            }
        }
        t_segs[tid]       = s;
        t_links[tid]      = l;
        t_paths[tid]      = p_cnt;
        t_seq_bytes[tid]  = seq_bytes;
        t_path_nodes[tid] = path_nodes;
        t_has_w[tid]      = has_w ? 1 : 0;
    }

    IngestSurvey out;
    out.file_size_bytes = total_size;
    for (uint64_t v : t_segs)       { out.segment_count        += v; }
    for (uint64_t v : t_links)      { out.link_count           += v; }
    for (uint64_t v : t_paths)      { out.path_count           += v; }
    for (uint64_t v : t_seq_bytes)  { out.total_sequence_bytes += v; }
    uint64_t total_path_nodes = 0;
    for (uint64_t v : t_path_nodes) { total_path_nodes         += v; }
    for (uint8_t v : t_has_w)       { if (v) { out.has_w = true; } }
    out.avg_path_length = (out.path_count > 0)
        ? static_cast<double>(total_path_nodes) / static_cast<double>(out.path_count)
        : 0.0;
    out.elapsed_s = sec(clock::now() - t0).count();
    return out;
}

} // namespace ggql
