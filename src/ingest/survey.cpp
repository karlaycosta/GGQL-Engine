// survey.cpp — Varredura leve do GFA (light Pass1).
//
// Conta apenas S/L/W/P lines via mmap + OpenMP, sem alocar keys/links.
// Usado por suggest_profile() antes de decidir os flags do ingest real.

#include "ggql/ingest/profile.hpp"
#include "ggql/ingest/ingestor.hpp"  // MmapFileParallel

#include <algorithm>  // std::ranges::count, std::min, std::max
#include <chrono>
#include <cstring>    // memchr
#include <span>

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

    // Particionamento em MUITOS chunks finos line-aligned + schedule(dynamic):
    // os tipos de linha clusterizam no GFA (bloco S → bloco L → bloco W/P) e o
    // custo por byte é desigual (L trivial vs W/P com contagem por byte). Com
    // 1 chunk/thread estático, a thread que pega o bloco W/P vira o caminho
    // crítico. Com ~8 chunks/thread e agendamento dinâmico, os blocos caros se
    // distribuem entre as threads. Chunk mínimo de 1 MiB p/ o overhead de
    // align/agendamento não dominar. A invariante de fronteira é mantida: o fim
    // do chunk k (align((k+1)·cs)) é idêntico ao início do chunk k+1 → sem
    // gap/overlap e determinístico em qualquer nº de threads/chunks.
    constexpr size_t kMinChunk = 1u << 20;  // 1 MiB
    int n_chunks = static_cast<int>(std::min<size_t>(
        static_cast<size_t>(num_threads) * 8,
        std::max<size_t>(1, total_size / kMinChunk)));
    if (n_chunks < 1) { n_chunks = 1; }
    const size_t cs = total_size / static_cast<size_t>(n_chunks);

    uint64_t segs = 0, links = 0, paths = 0, seq_total = 0, path_nodes_total = 0;
    int has_w_i = 0;

#pragma omp parallel for schedule(dynamic) num_threads(num_threads)               \
    reduction(+ : segs, links, paths, seq_total, path_nodes_total)                \
    reduction(| : has_w_i)
    for (int k = 0; k < n_chunks; ++k) {
        const char* chunk_start =
            (k == 0) ? data_start
                     : align_to_line_start_local(
                           data_start + static_cast<size_t>(k) * cs, data_start);
        const char* chunk_end =
            (k == n_chunks - 1) ? data_end
                                : align_to_line_start_local(
                                      data_start + static_cast<size_t>(k + 1) * cs, data_start);

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
        // Acumula os parciais do chunk nas variáveis de reduction OpenMP.
        segs             += s;
        links            += l;
        paths            += p_cnt;
        seq_total        += seq_bytes;
        path_nodes_total += path_nodes;
        has_w_i          |= (has_w ? 1 : 0);
    }

    IngestSurvey out;
    out.file_size_bytes      = total_size;
    out.segment_count        = segs;
    out.link_count           = links;
    out.path_count           = paths;
    out.total_sequence_bytes = seq_total;
    out.has_w                = (has_w_i != 0);
    out.avg_path_length = (out.path_count > 0)
        ? static_cast<double>(path_nodes_total) / static_cast<double>(out.path_count)
        : 0.0;
    out.elapsed_s = sec(clock::now() - t0).count();
    return out;
}

} // namespace ggql
