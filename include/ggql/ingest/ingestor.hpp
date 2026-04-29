#pragma once

// ingestor.hpp — GfaIngestorMmapParallel (vg-equivalent).
//
// Diferença em relação ao original:
//   Após topology_->build(), remapeia os IDs MPHF para IDs sequenciais
//   contíguos por componente via make_component_remap() + remap_node_ids(),
//   e reescreve os arquivos temporários do gBWT com os novos IDs.
//   Preenche GbwtGroup::sigma = max_node_in_group + 1 para cada grupo.
//
// Isso reduz sigma do DynamicGBWT de ~85M (global) para ~M_i (componente),
// habilitando paralelismo efetivo igual ao do vg.
//
// Otimizações implementadas:
//   1. Paralelização com OpenMP (Passada 1 e 2)
//   2. posix_fadvise(POSIX_FADV_SEQUENTIAL) antes do mmap
//   3. MADV_HUGEPAGE para reduzir TLB misses (requer huge pages habilitadas)
//   4. MADV_SEQUENTIAL mantido para readahead (MADV_RANDOM antes da pass 2)
//   5. Chunking do arquivo por thread com alinhamento em '\n'
//   6. Fusão Pass 1 + Pass 1b: coleta S-keys e L-pairs em um único scan
//   7. Union-Find lock-free com std::atomic_ref (C++23)
//   8. Bin-packing de componentes → ~32 bins (elimina 2443 tmp files)
//   9. In-RAM threshold per-bin por sigma
//  10. fmt::format para nomes de paths (C++23)

// #include "ggql/cli/console.hpp"
#include "ggql/cli/progress_tracker.hpp"
#include "ggql/ingest/gfa_metadata_store.hpp"
#include "ggql/ingest/gbwt_group.hpp"
#include "ggql/storage/gfa_topology.hpp"

// #include <algorithm>
#include <array>
// #include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fmt/color.h>
#include <fmt/core.h>
#include <memory>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
// #include <utility>
#include <vector>

#include "mphf/BooPHF.h"
#include "mphf/xxh64.hpp"

#ifdef _OPENMP
    #include <omp.h>
#endif

namespace ggql {

// ── RAII wrapper para mmap com otimizações
// ─────────────────────────────────────
struct MmapFileParallel {
    const char* data = nullptr;
    size_t size = 0;

    MmapFileParallel() = default;

    // Fábrica retorna expected em vez de throw no construtor (C++23)
    static std::expected<MmapFileParallel, std::string> open(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return std::unexpected("mmap: nao foi possivel abrir: " + path);
        }

        struct stat st{};
        if (::fstat(fd, &st) < 0) {
            ::close(fd);
            return std::unexpected("mmap: fstat falhou: " + path);
        }

        size_t new_size = static_cast<size_t>(st.st_size);
        if (new_size == 0) {
            ::close(fd);
            return MmapFileParallel{};
        }

        // posix_fadvise ANTES do mmap — dica ao kernel de acesso sequencial
        ::posix_fadvise(fd, 0, st.st_size, POSIX_FADV_SEQUENTIAL);

        void* ptr = ::mmap(nullptr, new_size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (ptr == MAP_FAILED) {
            return std::unexpected("mmap: mmap() falhou: " + path);
        }

        MmapFileParallel m;
        m.data = static_cast<const char*>(ptr);
        m.size = new_size;

        // MADV_SEQUENTIAL para readahead otimizado
        ::madvise(ptr, new_size, MADV_SEQUENTIAL);

        // MADV_HUGEPAGE para reduzir TLB misses (requer huge pages habilitadas)
#ifdef MADV_HUGEPAGE
        ::madvise(ptr, new_size, MADV_HUGEPAGE);
#endif
        return m;
    }

    ~MmapFileParallel() {
        if (data && size) {
            ::munmap(const_cast<char*>(data), size);
        }
    }

    // Não copiável — recurso único
    MmapFileParallel(const MmapFileParallel&) = delete;
    MmapFileParallel& operator=(const MmapFileParallel&) = delete;
    MmapFileParallel(MmapFileParallel&& o) noexcept : data(o.data), size(o.size) {
        o.data = nullptr;
        o.size = 0;
    }
};

// ── Hasher
// ─────────────────────────────────────────────────────────────────────
struct StringViewHasherParallel {
    uint64_t operator()(std::string_view key, uint64_t seed = 0) const {
        return XXH64(key.data(), key.size(), seed);
    }
};

using boophf_parallel_t = boomphf::mphf<std::string_view, StringViewHasherParallel>;

// ── Resultado
// ─────────────────────────────────────────────────────────────────
struct IngestStatsParallel {
    uint64_t segment_count = 0;
    uint64_t link_count = 0;
    uint64_t node_count = 0;
    uint64_t path_count = 0; // W-lines/P-lines ingeridas
    double elapsed_io1_s = 0.0;
    double elapsed_build_s = 0.0;
    double elapsed_ingest_s = 0.0;
    double elapsed_metadata_s = 0.0;      // 0 se save_metadata=false
    double elapsed_topology_s = 0.0;      // 0 se build_topology=false
    double elapsed_gbwt_s = 0.0;          // Tempo real total do gBWT (wall_s)
    double elapsed_gbwt_insert_s = 0.0;   // Tempo acumulado em DynamicGBWT::insert()
    double elapsed_gbwt_compress_s = 0.0; // Tempo acumulado em GBWT(dynamic) compress
    double elapsed_fm_index_s = 0.0;      // 0 se build_fm_index=false
    double gbwt_cpu_avg_s = 0.0;          // Média CPU/thread
    double gbwt_cpu_max_s = 0.0;          // Máximo CPU (thread mais lenta)
    uint64_t gbwt_total_nodes = 0;        // Total de nós processados no gBWT
    size_t gbwt_peak_rss_kb = 0;          // Pico de memória isolado do build
    size_t gbwt_index_size_b = 0;         // Tamanho final em bytes
    double elapsed_total_s = 0.0;

    // Campos opcionais — preenchidos apenas quando keep_gbwt_tmp=true em ingest().
    // Permitem que o chamador re-execute build_path_index() com diferentes batch sizes
    // (e.g., Fase 10 do bench_topology). O chamador é responsável por excluir o arquivo.
    std::string gbwt_tmp_path;                // caminho do arquivo temporário em disco
    std::vector<std::string> gbwt_path_names; // nomes dos paths (cópia salva antes do move)
    uint8_t gbwt_node_width = 0;              // bits/símbolo usado no text_buffer_type
};

// ── Helper de Progresso / ETA ─────────────────────────────────────────────
// PERF-FM-6: ProgressTracker movido para ggql/cli/progress_tracker.hpp.
using ggql::ProgressTracker;

// ── GfaIngestorMmapParallel
// ───────────────────────────────────────────────────
// C++23: constexpr node_width calculation
//
// Encoding GBWT-bound: gbwt_encode_node(internal_id, orient) =
//   ((internal_id + 1) << 1) | orient. Para nkeys nós (IDs em [0, nkeys-1]),
// o maior valor encoded é ((nkeys-1+1)*2) | 1 = 2*nkeys + 1.
// Bits necessários = bit_width(2*nkeys + 1).
constexpr uint8_t calc_node_width(uint64_t nkeys) noexcept {
    return nkeys > 0 ? static_cast<uint8_t>(std::bit_width(nkeys * 2ull + 1ull)) : uint8_t(1);
}

class GfaIngestorMmapParallel {
  public:
    GfaIngestorMmapParallel() = default;
    ~GfaIngestorMmapParallel() = default;

    // Ingere um arquivo GFA e constrói os índices solicitados.
    //
    // Parâmetros:
    //   filepath        — caminho absoluto ou relativo para o arquivo .gfa.
    //   num_threads     — número de threads OpenMP usadas no parsing e na
    //                     construção do MPHF, SA (libsais64_omp) e gBWT.
    //   gamma           — fator de carga do BBHash MPHF (padrão 2.0).
    //                     Valores menores → menos RAM, mais colisões e tempo.
    //   verbose         — se true, imprime mensagens de progresso em stderr.
    //   save_metadata   — se true, persiste os índices no diretório cache_dir
    //                     (MPHF, CSR topologia, FM-Index, gBWT).
    //   build_topology  — se true, constrói GfaCsrTopology e a disponibiliza
    //                     via topology(). Eleva o pico de RAM em ~3N bytes
    //                     (adj_fwd + adj_bwd + offsets).
    //   build_fm_index  — se true, constrói o FM-Index de sequências de
    //                     segmentos (GfaSequenceIndex). Requer espaço pico de
    //                     10N bytes durante a construção do SA (N = bytes totais
    //                     das sequências concatenadas).
    //   max_gbwt_paths  — > 0 → coleta até este número de haplótipos (linhas
    //                     W/P) e constrói o gBWT com construct_im.
    //                     0 (padrão) → ignora W/P-lines; evita OOM em grafos
    //                     muito grandes. construct_im precisa de 5–9× |paths|
    //                     de RAM — use com cautela.
    //   keep_gbwt_tmp   — se true, não exclui o arquivo temporário do gBWT
    //                     após a construção; preenche gbwt_tmp_path,
    //                     gbwt_path_names e gbwt_node_width em
    //                     IngestStatsParallel. Útil para benchmarks de
    //                     batch_size. O CHAMADOR deve excluir o arquivo com
    //                     ::unlink(st.gbwt_tmp_path.c_str()).
    //   cache_dir       — subdiretório (relativo ao pai de filepath) onde os
    //                     arquivos de cache são lidos/gravados (padrão: ".idx/").
    //
    // Retorno:
    //   std::expected com IngestStatsParallel em caso de sucesso, ou uma
    //   std::string descrevendo o erro.
    std::expected<IngestStatsParallel, std::string>
    ingest(const std::string& filepath, int num_threads = 1, double gamma = 2.0,
           bool verbose = true, bool save_metadata = true, bool build_topology = false,
           bool build_fm_index = false, size_t max_gbwt_paths = 0, bool keep_gbwt_tmp = false,
           const std::string& cache_dir = ".idx/");

    // Retorna ponteiro para a topologia CSR construída (nullptr se build_topology=false)
    GfaCsrTopology* topology() noexcept { return res_.topology.get(); }

    // Libera na ordem correta: topology → mphf → mmap (backing das string_views).
    void reset() noexcept { res_ = {}; }

  private:
    // BUG-3 (fix): Resources agrupa os três recursos com ordem de destruição
    // enforçada pela estrutura — membros são destruídos na ordem INVERSA de
    // declaração: topology (1º), mphf (2º), mmap (3º, backing das string_views).
    // Sem comentário frágil "NÃO reordene" — a estrutura documenta a dependência.
    struct Resources {
        std::unique_ptr<GfaCsrTopology>     topology; // destruído 1º (usa mphf_)
        std::unique_ptr<boophf_parallel_t>  mphf;     // destruído 2º (usa mmap views)
        std::unique_ptr<MmapFileParallel>   mmap;     // destruído 3º (backing)
    } res_;

    // Option A: remap e componente calculados na passada 1 (após MPHF + L-lines).
    // remap_[mphf_id] = sequential_id contíguo por componente (maior primeiro).
    // comp_for_node_[mphf_id] = component_rank (0..K-1).
    // comp_sigma_[comp_rank] = nós no componente.
    // comp_to_bin_[comp_rank] = bin_id (after bin-packing of K comps → ~32 bins).
    // Preenchidos por build_remap_from_links() e liberados após passada 2.
    std::vector<uint32_t> remap_;
    std::vector<uint32_t> comp_for_node_;
    std::vector<uint64_t> comp_sigma_;
    std::vector<uint32_t> comp_to_bin_; // bin-packing: comp → bin
    std::vector<uint64_t> bin_sigma_;   // sigma acumulado por bin
    uint32_t num_components_ = 0;
    uint32_t num_bins_packed_ = 0;

    // ── Utilitários de parsing ────────────────────────────────────────────────

    static const char* skip_line(const char* p, const char* end) noexcept {
        const void* found = ::memchr(p, '\n', static_cast<size_t>(end - p));
        return found ? static_cast<const char*>(found) + 1 : end;
    }

    static std::string_view next_token(const char*& p, const char* end) noexcept {
        const char* start = p;
        // Com -O3 -march=native o GCC vetoriza este loop com AVX2 (32 B/ciclo).
        // Substituir por dois memchr é mais lento: aumenta o custo de chamadas e
        // escaneia os dados duas vezes. A vetorização automática já é ótima aqui.
        while (p < end && *p != '\t' && *p != '\n' && *p != '\r') {
            ++p;
        }

        std::string_view sv(start, static_cast<size_t>(p - start));
        if (p < end && *p == '\t') {
            ++p;
        }

        return sv;
    }

    static size_t tokenize(const char*& p, const char* end, std::array<std::string_view, 8>& out,
                           size_t max_fields) noexcept {
        size_t n = 0;
#ifdef __GNUC__
    #pragma GCC unroll 8
#endif
        while (n < max_fields && p < end && *p != '\n') {
            out[n++] = next_token(p, end);
        }

        p = skip_line(p, end);
        return n;
    }

    // Encontra início de linha mais próximo (evita dividir linha no meio).
    // Usa memrchr (POSIX) — busca reversa com SIMD via libc, ordens de magnitude
    // mais rápido que varredura byte a byte para contigs longos (50k+ bases).
    static const char* align_to_line_start(const char* p, const char* data_start) noexcept {
        if (p <= data_start) {
            return data_start;
        }

        ptrdiff_t len = p - data_start;
        const void* found = ::memrchr(data_start, '\n', static_cast<size_t>(len));
        if (!found) {
            return data_start;
        }

        return static_cast<const char*>(found) + 1;
    }

    // Passada 1 FUSIONADA: coleta S-line keys e L-line pairs em um único scan.
    // Retorna keys para MPHF. Se collect_links_for_uf=true, preenche
    // uf_pairs_ com pares (from_name, to_name) para Union-Find após MPHF.
    struct Pass1Result {
        std::vector<std::string_view> keys;
        // L-line pairs como string_views (resolvidos via MPHF depois)
        struct LinkNamePair {
            std::string_view from, to;
        };
        std::vector<std::vector<LinkNamePair>> thread_pairs;
    };

    Pass1Result collect_keys_and_links_parallel(int num_threads, bool collect_links_for_uf,
                                                bool verbose = true);

    // Passada 1b: executa Union-Find sobre pares já resolvidos via MPHF.
    // Preenche remap_, comp_for_node_, comp_sigma_, comp_to_bin_, bin_sigma_,
    // num_components_, num_bins_packed_.
    void build_remap_from_pairs(Pass1Result::LinkNamePair* /*unused*/, int num_threads,
                                std::vector<std::vector<Pass1Result::LinkNamePair>>& thread_pairs);

    // Passada 2 PARALELA: cada thread processa seu chunk.
    // collect_records=true → preenche o vetor de SegmentRecord para a GfaMetadataStore.
    // collect_links=true   → preenche o vetor de LinkRecord para topologia CSR.
    // collect_paths=true   → preenche o vetor de PathRecord (W/P-lines) para gBWT.
    struct PassadaDoisResult {
        IngestStatsParallel stats;
        std::vector<SegmentRecord> records;
        std::vector<LinkRecord> links;
        // Paths para gBWT: UM ARQUIVO TEMPORÁRIO POR THREAD — sem mutex.
        // Tipo unificado com GfaCsrTopology::GbwtGroup — elimina loop de conversão.
        using GbwtGroup = ::ggql::GbwtGroup;
        std::vector<GbwtGroup> gbwt_groups; // um por thread
        uint8_t node_width = 1;             // bits por símbolo (igual para todos)
    };
    PassadaDoisResult process_data_parallel(int num_threads, bool collect_records,
                                            bool collect_links, bool collect_paths = false,
                                            size_t max_paths = 0, bool verbose = true);

    // Parseia tags opcionais GFA (formato TAG:TYPE:VALUE) a partir de start_idx.
    // Preenche out_rc (RC:i), out_dp (DP:f) e out_ln (LN:i) se presentes.
    static void parse_optional_tags(const std::string_view* toks, size_t ntok, size_t start_idx,
                                    int32_t& out_rc, float& out_dp, uint32_t& out_ln) noexcept {
        for (size_t i = start_idx; i < ntok; ++i) {
            std::string_view sv = toks[i];
            // Formato mínimo: XX:Y:V (5 chars)
            if (sv.size() < 5 || sv[2] != ':' || sv[4] != ':') {
                continue;
            }

            // Normaliza para maiúsculas para comparação (GFA spec não padroniza caixa)
            const char c0   = static_cast<char>(sv[0] & ~0x20);
            const char c1   = static_cast<char>(sv[1] & ~0x20);
            const char type = sv[3];
            std::string_view val = sv.substr(5);

            // RC:i: / rc:i: — read count (Bandage, vg, hifiasm)
            if (c0 == 'R' && c1 == 'C' && type == 'i') {
                int32_t v{};
                if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc{}) {
                    out_rc = v;
                }
            // DP:f: / dp:f: — depth float (minigraph, minimap2)
            } else if (c0 == 'D' && c1 == 'P' && type == 'f') {
                float v{};
                if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc{}) {
                    out_dp = v;
                }
            // DP:i: / dp:i: — depth int (algumas ferramentas emitem inteiro)
            } else if (c0 == 'D' && c1 == 'P' && type == 'i') {
                int32_t v{};
                if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc{}) {
                    out_dp = static_cast<float>(v);
                }
            // LN:i: / ln:i: — comprimento explícito (presente quando SEQ é '*')
            } else if (c0 == 'L' && c1 == 'N' && type == 'i') {
                uint32_t v{};
                if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc{}) {
                    out_ln = v;
                }
            }
        }
    }

    // ── Persistência do Índice ─────────────────────────────────────────────────────────────────────────

    // Retorna mtime do arquivo em nanossegundos (usado como pré-filtro de cache).
    static int64_t get_mtime_ns(const std::string& path) noexcept {
        try {
            auto ft = std::filesystem::last_write_time(path);
            return static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ft.time_since_epoch())
                    .count());
        } catch (...) {
            return 0;
        }
    }

    // Salva MPHF e remap — deve ser chamado ANTES de mphf_.reset() e swap-clears.
    // Retorna false em qualquer falha de I/O — o chamador NÃO deve escrever o manifest.
    [[nodiscard]] bool save_mphf_and_remap(const std::string& dir);

    // Grava manifest de forma atômica: escreve em .tmp primeiro, depois rename() atômico.
    void write_manifest_atomic(const std::string& dir, uint64_t hash, uint64_t file_sz,
                               int64_t file_mtime_ns_val, uint8_t node_width,
                               const IngestStatsParallel& stats);

    // Tenta carregar o índice do cache.
    //   expected_hash    : hash XXH64 calculado pelo chamador (nullopt = não verificar).
    //   file_sz          : tamanho atual do GFA (0 = ignorar pré-filtro).
    //   file_mtime_ns_val: mtime em ns (0 = ignorar pré-filtro).
    std::optional<IngestStatsParallel>
    load_index(const std::string& dir, std::optional<uint64_t> expected_hash = std::nullopt,
               uint64_t file_sz = 0, int64_t file_mtime_ns_val = 0);

    // ── ARCH-4: parsers W/P extraídos de process_data_parallel ───────────────
    // Funções estáticas para possibilitar teste unitário isolado (ARCH-5).
    // Retornam false se a linha for inválida ou vazia (sem nós).
    static bool parse_w_line(const char*& p, const char* end,
                             std::array<std::string_view, 8>& tokens,
                             std::string& path_name, std::vector<uint64_t>& local_nodes,
                             uint64_t& first_mphf_id,
                             boophf_parallel_t& mphf, bool has_remap,
                             const std::vector<uint32_t>& remap);

    static bool parse_p_line(const char*& p, const char* end,
                             std::array<std::string_view, 8>& tokens,
                             std::string& path_name, std::vector<uint64_t>& local_nodes,
                             uint64_t& first_mphf_id,
                             boophf_parallel_t& mphf, bool has_remap,
                             const std::vector<uint32_t>& remap);

    // ── ARCH-3: orquestrador ingest() — métodos auxiliares ────────────────────

    // Tenta cache via mtime+size (O(1)). Retorna stats em hit completo; nullopt em miss ou
    // hit parcial (MPHF ok, mas algum índice ausente). Define out_skip=true em hit parcial.
    std::optional<IngestStatsParallel>
    try_cache_fast(const std::string& cache_dir,
                   uint64_t file_sz, int64_t file_mtime_ns,
                   bool save_metadata, bool build_fm_index,
                   bool verbose, bool& out_skip);

    // Computa hash XXH64 de mmap_data_ (reseta mmap_data_) e verifica cache por hash.
    // Retorna stats em hit completo; nullopt em miss ou hit parcial.
    // Define out_hash e out_skip=true em hit parcial. mmap_data_ fica fechado ao retornar.
    std::optional<IngestStatsParallel>
    try_cache_hash(const std::string& cache_dir,
                   bool save_metadata, bool verbose,
                   uint64_t& out_hash, bool& out_skip);

    // Ingestão completa (Pass1 + MPHF + Union-Find + Pass2 + topologia + save).
    // Pré-condição: mmap_data_ está aberto.
    // skip_pass1=true → salta Pass1 (MPHF já no cache), executa apenas Pass2.
    std::expected<IngestStatsParallel, std::string>
    run_full_ingest(int num_threads, double gamma, bool verbose,
                    bool save_metadata, bool build_topology,
                    bool build_fm_index, size_t max_gbwt_paths,
                    bool keep_gbwt_tmp, const std::string& cache_dir,
                    uint64_t file_sz, int64_t file_mtime_ns,
                    uint64_t gfa_hash, bool skip_pass1,
                    std::chrono::high_resolution_clock::time_point t_start);
};

} // namespace ggql
