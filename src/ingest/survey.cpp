// survey.cpp — Varredura leve do GFA (light Pass1).
//
// Conta apenas S/L/W/P lines via mmap + OpenMP, sem alocar keys/links.
// Usado por suggest_profile() antes de decidir os flags do ingest real.

#include "ggql/ingest/profile.hpp"
#include "ggql/ingest/ingestor.hpp"  // MmapFileParallel

#include <algorithm>  // std::ranges::count
#include <chrono>
#include <cstring>    // memchr
#include <numeric>    // std::reduce
#include <span>
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

// memchr usa SIMD (pcmpeqb/AVX2) na libc — 16-32 B/iter contra char-a-char.
inline const char* skip_line_local(const char* p, const char* end) {
    const auto* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    return nl ? nl + 1 : end;
}

// Encontra próximo TAB em [p, end); retorna end se não achar.
inline const char* find_tab(const char* p, const char* end) {
    const auto* t = static_cast<const char*>(std::memchr(p, '\t', static_cast<size_t>(end - p)));
    return t ? t : end;
}

} // namespace

std::expected<IngestSurvey, std::string>
survey_gfa_light(const std::string& filepath, int num_threads) {
    // steady_clock (monotônico) — high_resolution_clock pode ser alias de
    // system_clock em algumas libs e dar elapsed negativo sob ajuste de relógio.
    using clock = std::chrono::steady_clock;
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
                && p + 1 < chunk_end && *(p + 1) == '\t') [[likely]] {
                // Encontra fim da linha uma vez (reusado em S/W/P) — memchr SIMD.
                const auto* nl = static_cast<const char*>(
                    std::memchr(p, '\n', static_cast<size_t>(chunk_end - p)));
                const char* line_end = nl ? nl : chunk_end;
                switch (first) {
                    case 'S': {
                        ++s;
                        // S\t<name>\t<seq>\t[tags] — pular ao 2º TAB, contar até o 3º (ou fim).
                        const char* c = find_tab(p + 2, line_end); // fim do <name>
                        if (c < line_end) {
                            ++c; // skip TAB
                            const char* seq_end = find_tab(c, line_end);
                            seq_bytes += static_cast<uint64_t>(seq_end - c);
                        }
                        break;
                    }
                    case 'L': ++l; break;
                    case 'W':
                    case 'P': {
                        ++p_cnt;
                        uint64_t nodes = 0;
                        if (first == 'P') {
                            // P\t<nome>\t<seg1,seg2,...>\t<overlaps> — nós =
                            // vírgulas no campo de SEGMENTOS + 1. Restringir ao
                            // 2º→3º TAB evita contar as vírgulas da lista de
                            // overlaps CIGAR (que dobrariam a contagem). Aninhado
                            // (como o case 'S') p/ o compilador provar seg <= line_end.
                            const char* seg = find_tab(p + 2, line_end); // fim do <nome>
                            if (seg < line_end) {
                                ++seg;                                   // início dos <segmentos>
                                const char* seg_end = find_tab(seg, line_end);
                                nodes = static_cast<uint64_t>(
                                            std::ranges::count(std::span(seg, seg_end), ',')) + 1;
                            }
                        } else {
                            has_w = true;
                            // W\t…6 campos…\t<walk> — nós = nº de marcas de
                            // orientação no walk. '<'=0x3C e '>'=0x3E diferem só
                            // no bit 1, então (b|0x02)==0x3E casa os dois (e só
                            // eles) num único compare aritmético — auto-vetorizável
                            // (compare+popcount SIMD) ao contrário do OR escalar.
                            for (const char* c = p; c < line_end; ++c) {
                                nodes += (static_cast<unsigned char>(*c) | 0x02u) == 0x3Eu;
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
    out.file_size_bytes      = total_size;
    out.segment_count        = std::reduce(t_segs.begin(),      t_segs.end());
    out.link_count           = std::reduce(t_links.begin(),     t_links.end());
    out.path_count           = std::reduce(t_paths.begin(),     t_paths.end());
    out.total_sequence_bytes = std::reduce(t_seq_bytes.begin(), t_seq_bytes.end());
    out.has_w = std::ranges::any_of(t_has_w, [](uint8_t v) { return v != 0; });
    const uint64_t total_path_nodes =
        std::reduce(t_path_nodes.begin(), t_path_nodes.end());
    out.avg_path_length = (out.path_count > 0)
        ? static_cast<double>(total_path_nodes) / static_cast<double>(out.path_count)
        : 0.0;
    out.elapsed_s = sec(clock::now() - t0).count();
    return out;
}

} // namespace ggql
