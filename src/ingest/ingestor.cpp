// ingestor.cpp — Implementações de GfaIngestorMmapParallel.
//
// Mantido separado do header para que alterações neste arquivo não
// disparem recompilação de todos os binários que incluem ingestor.hpp.
// Compilado uma única vez em ggql_topology_vg.

#include "ggql/ingest/ingestor.hpp"

#include "ggql/gbwt/node_buffer.hpp"
#include "ggql/storage/fm_index.hpp"
#include "ggql/storage/snarl_decomposition.hpp"
#include "ggql/utils.hpp"

#include <cstring>
#include <ctime>
#include <fstream>

#ifdef _OPENMP
    #include <omp.h>
#endif

namespace ggql {

// ══════════════════════════════════════════════════════════════════════════════
// Passada 1 FUSIONADA
// ══════════════════════════════════════════════════════════════════════════════

GfaIngestorMmapParallel::Pass1Result
GfaIngestorMmapParallel::collect_keys_and_links_parallel(int num_threads, bool collect_links_for_uf,
                                                         bool verbose) {
    const size_t total_size = res_.mmap->size;
    const char* data_start = res_.mmap->data;
    const char* data_end = data_start + total_size;

    ProgressTracker tracker(total_size, "Passada 1", verbose);

    Pass1Result result;
    result.keys.reserve(std::min(total_size / 100, size_t(1 << 27)));

    std::vector<std::vector<std::string_view>> thread_keys(num_threads);
    if (collect_links_for_uf) {
        result.thread_pairs.resize(num_threads);
    }

    const size_t chunk_size = total_size / static_cast<size_t>(num_threads);

#pragma omp parallel num_threads(num_threads)
    {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        const char* chunk_start = data_start + tid * chunk_size;
        const char* chunk_end = (tid == num_threads - 1) ? data_end : (chunk_start + chunk_size);

        if (tid > 0) {
            chunk_start = align_to_line_start(chunk_start, data_start);
        }
        if (tid < num_threads - 1) {
            const char* next_chunk = data_start + (tid + 1) * chunk_size;
            chunk_end = align_to_line_start(next_chunk, data_start);
        }

        thread_keys[tid].reserve(std::min(chunk_size / 100, size_t(15'000'000)));
        if (collect_links_for_uf) {
            result.thread_pairs[tid].reserve(std::min(chunk_size / 50, size_t(20'000'000)));
        }

        const char* p = chunk_start;
        std::array<std::string_view, 8> tokens;
        size_t local_bytes = 0;
        const char* last_p = p;

        while (p < chunk_end) {
            char first = *p;
            if (first == 'S') {
                ++p;
                if (p >= chunk_end || *p != '\t') {
                    p = skip_line(p, chunk_end);
                    continue;
                }
                ++p;
                size_t ntok = tokenize(p, chunk_end, tokens, 2);
                if (ntok >= 1 && !tokens[0].empty()) {
                    thread_keys[tid].push_back(tokens[0]);
                }
            } else if (collect_links_for_uf && first == 'L') {
                ++p;
                if (p >= chunk_end || *p != '\t') {
                    p = skip_line(p, chunk_end);
                    continue;
                }
                ++p;
                size_t ntok = tokenize(p, chunk_end, tokens, 4);
                if (ntok >= 4 && !tokens[0].empty() && !tokens[2].empty()) {
                    result.thread_pairs[tid].push_back({tokens[0], tokens[2]});
                }
            } else {
                p = skip_line(p, chunk_end);
            }

            local_bytes += static_cast<size_t>(p - last_p);
            last_p = p;
            if (local_bytes > 32 * 1024 * 1024) {
                tracker.add_progress(local_bytes);
                local_bytes = 0;
            }
        }
        if (local_bytes > 0) {
            tracker.add_progress(local_bytes);
        }
    }

    tracker.finish();

    // Merge prefix-sum
    std::vector<size_t> key_off(static_cast<size_t>(num_threads) + 1, 0);
    for (int i = 0; i < num_threads; ++i) {
        key_off[i + 1] = key_off[i] + thread_keys[i].size();
    }
    result.keys.resize(key_off.back());
#pragma omp parallel for schedule(static) num_threads(num_threads)
    for (int i = 0; i < num_threads; ++i) {
        auto& src = thread_keys[static_cast<size_t>(i)];
        std::move(src.begin(), src.end(),
                  result.keys.begin() + static_cast<std::ptrdiff_t>(key_off[i]));
        src = {};
    }

    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// Passada 1b — Union-Find lock-free + bin-packing
// ══════════════════════════════════════════════════════════════════════════════

void GfaIngestorMmapParallel::build_remap_from_pairs(
    int num_threads, std::vector<std::vector<Pass1Result::LinkNamePair>>& thread_pairs) {

    const size_t N = res_.mphf->nbKeys();

    std::vector<uint32_t> parent(N);
    std::ranges::iota(parent, uint32_t(0));
    std::vector<uint8_t> uf_rank(N, 0);

    // acquire nos loads: necessário em ARM/RISC-V; no-op em x86 (TSO).
    auto find = [&](uint32_t x) -> uint32_t {
        while (true) {
            uint32_t p = std::atomic_ref(parent[x]).load(std::memory_order_acquire);
            if (p == x) {
                return x;
            }
            uint32_t gp = std::atomic_ref(parent[p]).load(std::memory_order_acquire);
            std::atomic_ref(parent[x]).compare_exchange_weak(p, gp, std::memory_order_relaxed);
            x = gp;
        }
    };

    auto unite = [&](uint32_t a, uint32_t b) {
        while (true) {
            a = find(a);
            b = find(b);
            if (a == b) {
                return;
            }
            uint8_t ra = std::atomic_ref(uf_rank[a]).load(std::memory_order_relaxed);
            uint8_t rb = std::atomic_ref(uf_rank[b]).load(std::memory_order_relaxed);
            if (ra < rb) {
                std::swap(a, b);
            }
            uint32_t expected = b;
            if (std::atomic_ref(parent[b]).compare_exchange_strong(
                    expected, a, std::memory_order_release, std::memory_order_acquire)) {
                if (ra == rb) {
                    std::atomic_ref(uf_rank[a])
                        .compare_exchange_strong(ra, static_cast<uint8_t>(ra + 1),
                                                 std::memory_order_relaxed);
                }
                return;
            }
        }
    };

#pragma omp parallel for schedule(static) num_threads(num_threads)
    for (int t = 0; t < static_cast<int>(thread_pairs.size()); ++t) {
        for (const auto& [from_sv, to_sv] : thread_pairs[t]) {
            uint64_t fid = res_.mphf->lookup(from_sv);
            uint64_t tid_val = res_.mphf->lookup(to_sv);
            if (fid == ULLONG_MAX || tid_val == ULLONG_MAX) {
                continue;
            }
            unite(static_cast<uint32_t>(fid), static_cast<uint32_t>(tid_val));
        }
    }

    thread_pairs = {};
    uf_rank = {};

    // Path compression total (read-only após uniões)
    auto find_readonly = [&](uint32_t x) -> uint32_t {
        while (parent[x] != x) {
            x = parent[x];
        }
        return x;
    };
#pragma omp parallel for schedule(static) num_threads(num_threads)
    for (size_t x = 0; x < N; ++x) {
        parent[x] = find_readonly(static_cast<uint32_t>(x));
    }

    // Compacta IDs de componentes
    std::vector<uint32_t> comp_of_root(N, UINT32_MAX);
    std::vector<uint32_t> comp_id_old(N);
    uint32_t K = 0;
    for (size_t x = 0; x < N; ++x) {
        uint32_t root = parent[x];
        if (comp_of_root[root] == UINT32_MAX) {
            comp_of_root[root] = K++;
        }
        comp_id_old[x] = comp_of_root[root];
    }

    parent = {};
    comp_of_root = {};

    std::vector<uint64_t> comp_size(K, 0);
    for (size_t x = 0; x < N; ++x) {
        ++comp_size[comp_id_old[x]];
    }

    std::vector<uint32_t> comp_order(K);
    std::ranges::iota(comp_order, uint32_t(0));
    std::ranges::sort(comp_order,
                      [&](uint32_t a, uint32_t b) { return comp_size[a] > comp_size[b]; });

    std::vector<uint32_t> comp_rank(K);
    for (uint32_t r = 0; r < K; ++r) {
        comp_rank[comp_order[r]] = r;
    }

    std::vector<uint32_t> new_base(K);
    uint32_t cursor = 0;
    for (uint32_t ci : comp_order) {
        new_base[ci] = cursor;
        cursor += static_cast<uint32_t>(comp_size[ci]);
    }

    remap_.resize(N);
    comp_for_node_.resize(N);
    std::vector<uint32_t> cc(K, 0);
    for (size_t x = 0; x < N; ++x) {
        const uint32_t c = comp_id_old[x];
        remap_[x] = new_base[c] + cc[c];
        comp_for_node_[x] = comp_rank[c];
        ++cc[c];
    }

    num_components_ = K;

    // Bin-packing: K componentes → max_bins bins com min-heap O(K log B)
    const uint32_t max_bins = static_cast<uint32_t>(
        std::min(static_cast<size_t>(K), static_cast<size_t>(num_threads) * 4));
    comp_to_bin_.resize(K);
    bin_sigma_.assign(max_bins, 0);

    using BinLoad = std::pair<uint64_t, uint32_t>;
    std::priority_queue<BinLoad, std::vector<BinLoad>, std::greater<BinLoad>> bin_heap;
    for (uint32_t b = 0; b < max_bins; ++b) {
        bin_heap.push({0ULL, b});
    }

    for (uint32_t ci : comp_order) {
        auto [load, bin_id] = bin_heap.top();
        bin_heap.pop();
        comp_to_bin_[comp_rank[ci]] = bin_id;
        bin_sigma_[bin_id] = load + comp_size[ci];
        bin_heap.push({bin_sigma_[bin_id], bin_id});
    }
    num_bins_packed_ = max_bins;

    cli::step_info(
        fmt::format("{} nós → {} componentes → {} bins (maior {}, menor {})", cli::format_count(N),
                    K, max_bins, cli::format_count(static_cast<size_t>(comp_size[comp_order[0]])),
                    cli::format_count(static_cast<size_t>(comp_size[comp_order[K - 1]]))));
}

// ══════════════════════════════════════════════════════════════════════════════
// parse_w_line / parse_p_line — extraídos de process_data_parallel
// ══════════════════════════════════════════════════════════════════════════════

bool GfaIngestorMmapParallel::parse_w_line(const char*& p, const char* end,
                                           std::array<std::string_view, 8>& tokens,
                                           std::string& path_name,
                                           std::vector<uint64_t>& local_nodes,
                                           uint64_t& first_mphf_id, boophf_parallel_t& mphf,
                                           bool has_remap, const std::vector<uint32_t>& remap) {
    size_t ntok = tokenize(p, end, tokens, 6);
    if (ntok < 6 || tokens[0].empty() || tokens[5].empty()) [[unlikely]] {
        return false;
    }
    path_name.clear();
    fmt::format_to(std::back_inserter(path_name), "{}#{}#{}:{}-{}",
                   tokens[0], tokens[1], tokens[2], tokens[3], tokens[4]);
    std::string_view walk = tokens[5];
    const char* wp = walk.data();
    const char* wend = wp + walk.size();
    while (wp < wend) {
        char orient_c = *wp++;
        if (orient_c != '>' && orient_c != '<') {
            break;
        }
        const char* seg_start = wp;
        while (wp < wend && *wp != '>' && *wp != '<') {
            ++wp;
        }
        std::string_view seg_name(seg_start, static_cast<size_t>(wp - seg_start));
        while (!seg_name.empty() &&
               (seg_name.back() == '\n' || seg_name.back() == '\r' || seg_name.back() == ' ')) {
            seg_name.remove_suffix(1);
        }
        if (seg_name.empty()) {
            continue;
        }
        uint64_t nid = mphf.lookup(seg_name);
        if (nid == ULLONG_MAX) [[unlikely]] {
            continue;
        }
        if (first_mphf_id == ULLONG_MAX) {
            first_mphf_id = nid;
        }
        uint64_t remapped = has_remap ? static_cast<uint64_t>(remap[nid]) : nid;
        uint64_t orient_bit = (orient_c == '<') ? 1u : 0u;
        local_nodes.push_back(gbwt_encode_node(remapped, orient_bit));
    }
    return true;
}

bool GfaIngestorMmapParallel::parse_p_line(const char*& p, const char* end,
                                           std::array<std::string_view, 8>& tokens,
                                           std::string& path_name,
                                           std::vector<uint64_t>& local_nodes,
                                           uint64_t& first_mphf_id, boophf_parallel_t& mphf,
                                           bool has_remap, const std::vector<uint32_t>& remap) {
    size_t ntok = tokenize(p, end, tokens, 3);
    if (ntok < 2 || tokens[0].empty() || tokens[1].empty()) [[unlikely]] {
        return false;
    }
    path_name = std::string(tokens[0]);
    std::string_view path_str = tokens[1];
    const char* pp = path_str.data();
    const char* pend = pp + path_str.size();
    while (pp < pend) {
        const char* seg_start = pp;
        while (pp < pend && *pp != ',' && *pp != '\t' && *pp != '\n' && *pp != '\r') {
            ++pp;
        }
        std::string_view entry(seg_start, static_cast<size_t>(pp - seg_start));
        if (pp < pend && *pp == ',') {
            ++pp;
        }
        if (entry.empty()) {
            continue;
        }
        char orient_c = entry.back();
        if (orient_c == '+' || orient_c == '-') {
            entry.remove_suffix(1);
        }
        if (entry.empty()) {
            continue;
        }
        uint64_t nid = mphf.lookup(entry);
        if (nid == ULLONG_MAX) [[unlikely]] {
            continue;
        }
        if (first_mphf_id == ULLONG_MAX) {
            first_mphf_id = nid;
        }
        uint64_t remapped = has_remap ? static_cast<uint64_t>(remap[nid]) : nid;
        uint64_t orient_bit = (orient_c == '-') ? 1u : 0u;
        local_nodes.push_back(gbwt_encode_node(remapped, orient_bit));
    }
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Passada 2 PARALELA
// ══════════════════════════════════════════════════════════════════════════════

GfaIngestorMmapParallel::PassadaDoisResult
GfaIngestorMmapParallel::process_data_parallel(int num_threads, bool collect_records,
                                               bool collect_links, bool collect_paths,
                                               size_t max_paths, bool verbose) {
    const size_t total_size = res_.mmap->size;
    const char* data_start = res_.mmap->data;
    const char* data_end = data_start + total_size;

    ProgressTracker tracker(total_size, "Passada 2", verbose);

    struct alignas(64) Counter {
        uint64_t value = 0;
    };
    std::vector<Counter> local_segments(num_threads);
    std::vector<Counter> local_links(num_threads);

    std::vector<std::vector<SegmentRecord>> thread_recs(
        collect_records ? static_cast<size_t>(num_threads) : 0);
    if (collect_records) {
        const size_t est_per_thread =
            std::min(total_size / 500 / static_cast<size_t>(num_threads), size_t(15'000'000));
        for (auto& v : thread_recs) {
            v.reserve(est_per_thread);
        }
    }

    std::vector<std::vector<LinkRecord>> thread_links(
        collect_links ? static_cast<size_t>(num_threads) : 0);
    if (collect_links) {
        const size_t est_links =
            std::min(total_size / 200 / static_cast<size_t>(num_threads), size_t(20'000'000));
        for (auto& v : thread_links) {
            v.reserve(est_links);
        }
    }

    const bool has_remap = !remap_.empty();
    const size_t num_bins = (collect_paths && has_remap && num_bins_packed_ > 0)
                                ? static_cast<size_t>(num_bins_packed_)
                                : static_cast<size_t>(num_threads);

    std::atomic<size_t> paths_written_global{0};
    std::vector<std::string> bin_gbwt_paths(collect_paths ? num_bins : 0);
    std::vector<std::vector<std::string>> bin_path_names(collect_paths ? num_bins : 0);
    std::vector<std::unique_ptr<ggql::NodeBuffer>> bin_gbwt_bufs;

    constexpr uint64_t IN_RAM_SIGMA_THRESHOLD = 10'000;
    std::vector<bool> bin_use_in_ram(collect_paths ? num_bins : 0, false);
    std::vector<std::vector<ggql::gbwt::vector_type>> bin_in_ram_seqs(collect_paths ? num_bins : 0);

    constexpr size_t SMALL_FILE_THRESHOLD = size_t(64) << 20;
    if (collect_paths) {
        if (total_size < SMALL_FILE_THRESHOLD) {
            std::fill(bin_use_in_ram.begin(), bin_use_in_ram.end(), true);
        } else if (has_remap) {
            for (size_t b = 0; b < num_bins; ++b) {
                bin_use_in_ram[b] =
                    (b < bin_sigma_.size() && bin_sigma_[b] < IN_RAM_SIGMA_THRESHOLD);
            }
        }
    }

    uint8_t node_width_out = 1;
    struct TmpFileGuard {
        std::string path;
        bool released = false;
        ~TmpFileGuard() {
            if (!released && !path.empty()) {
                ::unlink(path.c_str());
            }
        }
        std::string release() noexcept {
            released = true;
            return std::move(path);
        }
    };
    std::vector<TmpFileGuard> tmp_guards(collect_paths ? num_bins : 0);

    if (collect_paths) {
        const uint64_t nkeys = res_.mphf->nbKeys();
        node_width_out = calc_node_width(nkeys);

        bin_gbwt_bufs.resize(num_bins);
        for (size_t t = 0; t < num_bins; ++t) {
            if (bin_use_in_ram[t]) {
                continue;
            }
            std::string tpl = "/tmp/ggql_gbwt_XXXXXX";
            int tfd = ::mkstemp(tpl.data());
            if (tfd < 0) {
                collect_paths = false;
                break;
            }
            ::close(tfd);
            tmp_guards[t].path = tpl;
            bin_gbwt_paths[t] = tpl;
            bin_gbwt_bufs[t] = std::make_unique<ggql::NodeBuffer>(
                tpl, std::ios::out, /*buffer_size=*/1024 * 1024, node_width_out);
        }
    }

    const size_t chunk_size_p2 = total_size / static_cast<size_t>(num_threads);

    // Per-thread accumulators para bins em RAM — sem mutex no loop paralelo.
    // Bins em disco continuam usando disk_mutexes (I/O é o gargalo, não a contenção).
    std::vector<std::mutex> disk_mutexes(collect_paths ? num_bins : 0);
    using SeqVec = std::vector<ggql::gbwt::vector_type>;
    using NameVec = std::vector<std::string>;
    std::vector<std::vector<SeqVec>> thr_in_ram(num_threads,
                                                std::vector<SeqVec>(collect_paths ? num_bins : 0));
    std::vector<std::vector<NameVec>> thr_names(num_threads,
                                                std::vector<NameVec>(collect_paths ? num_bins : 0));

#pragma omp parallel num_threads(num_threads)
    {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        const char* chunk_start = data_start + tid * chunk_size_p2;
        const char* chunk_end = (tid == num_threads - 1) ? data_end : (chunk_start + chunk_size_p2);

        if (tid > 0) {
            chunk_start = align_to_line_start(chunk_start, data_start);
        }
        if (tid < num_threads - 1) {
            const char* next_chunk = data_start + (tid + 1) * chunk_size_p2;
            chunk_end = align_to_line_start(next_chunk, data_start);
        }

        {
            size_t adv_len = static_cast<size_t>(chunk_end - chunk_start);
            if (adv_len > 0) {
                ::madvise(const_cast<char*>(chunk_start), adv_len, MADV_SEQUENTIAL);
            }
        }

        const char* p = chunk_start;
        std::array<std::string_view, 8> tokens;

        auto lookup = [&](std::string_view sv) -> uint64_t { return res_.mphf->lookup(sv); };

        std::string path_name;
        std::vector<uint64_t> local_nodes;
        if (collect_paths) {
            local_nodes.reserve(1u << 20);
        }

        size_t local_bytes = 0;
        const char* last_p = p;

        while (p < chunk_end) {
            char first = *p;
            ++p;

            if (first == 'S') {
                if (p >= chunk_end || *p != '\t') {
                    p = skip_line(p, chunk_end);
                    continue;
                }
                ++p;
                size_t ntok = tokenize(p, chunk_end, tokens, 8);
                if (ntok < 2 || tokens[0].empty()) {
                    continue;
                }

                uint64_t id = lookup(tokens[0]);
                if (id == ULLONG_MAX) {
                    continue;
                }
                ++local_segments[tid].value;

                if (collect_records) {
                    SegmentRecord rec;
                    rec.internal_id = has_remap ? static_cast<uint64_t>(remap_[id]) : id;
                    rec.original_id = std::string(tokens[0]);
                    uint32_t ln_tag = 0;
                    parse_optional_tags(tokens.data(), ntok, 2, rec.read_count, rec.depth, ln_tag);
                    rec.length =
                        (tokens[1] == "*") ? ln_tag : static_cast<uint32_t>(tokens[1].size());
                    rec.sequence = (tokens[1] == "*") ? std::string_view{} : tokens[1];
                    thread_recs[static_cast<size_t>(tid)].push_back(std::move(rec));
                }

            } else if (first == 'L') {
                if (p >= chunk_end || *p != '\t') {
                    p = skip_line(p, chunk_end);
                    continue;
                }
                ++p;
                size_t ntok = tokenize(p, chunk_end, tokens, 4);
                if (ntok < 4 || tokens[0].empty() || tokens[2].empty()) {
                    continue;
                }

                uint64_t from_id = lookup(tokens[0]);
                uint64_t to_id = lookup(tokens[2]);
                if (from_id == ULLONG_MAX || to_id == ULLONG_MAX) {
                    continue;
                }
                ++local_links[tid].value;

                if (collect_links) {
                    uint64_t from_csr =
                        has_remap ? static_cast<uint64_t>(remap_[from_id]) : from_id;
                    uint64_t to_csr = has_remap ? static_cast<uint64_t>(remap_[to_id]) : to_id;
                    char from_orient = (!tokens[1].empty()) ? tokens[1][0] : '+';
                    char to_orient = (!tokens[3].empty()) ? tokens[3][0] : '+';
                    thread_links[static_cast<size_t>(tid)].push_back(
                        LinkRecord::make(from_csr, from_orient, to_csr, to_orient));
                }

            } else if (collect_paths && (first == 'W' || first == 'P')) {
                if (p >= chunk_end || *p != '\t') {
                    p = skip_line(p, chunk_end);
                    continue;
                }
                ++p;

                path_name.clear();
                local_nodes.clear();
                uint64_t first_mphf_id = ULLONG_MAX;

                // Parsing delegado para métodos estáticos extraídos.
                if (first == 'W') {
                    parse_w_line(p, chunk_end, tokens, path_name, local_nodes, first_mphf_id,
                                 *res_.mphf, has_remap, remap_);
                } else {
                    parse_p_line(p, chunk_end, tokens, path_name, local_nodes, first_mphf_id,
                                 *res_.mphf, has_remap, remap_);
                }

                if (!local_nodes.empty() && collect_paths) {
                    const size_t path_nr =
                        paths_written_global.fetch_add(1, std::memory_order_relaxed);
                    if (max_paths == 0 || path_nr < max_paths) {
                        size_t target;
                        if (has_remap && first_mphf_id != ULLONG_MAX) {
                            uint32_t comp = comp_for_node_[first_mphf_id];
                            target = (comp < comp_to_bin_.size())
                                         ? static_cast<size_t>(comp_to_bin_[comp])
                                         : 0;
                            if (target >= num_bins) {
                                target = 0;
                            }
                        } else {
                            target = path_nr % num_bins;
                        }
                        ggql::gbwt::vector_type seq(local_nodes.begin(), local_nodes.end());
                        if (bin_use_in_ram[target]) {
                            // Sem mutex — cada thread escreve no seu slot privado.
                            thr_in_ram[static_cast<size_t>(tid)][target].push_back(std::move(seq));
                            thr_names[static_cast<size_t>(tid)][target].push_back(
                                std::move(path_name));
                        } else if (bin_gbwt_bufs[target]) {
                            std::lock_guard<std::mutex> lk(disk_mutexes[target]);
                            for (uint64_t v : seq) {
                                bin_gbwt_bufs[target]->push_back(v);
                            }
                            bin_gbwt_bufs[target]->push_back(ggql::kEndmarker);
                            bin_path_names[target].push_back(std::move(path_name));
                        }
                    }
                }

            } else {
                p = skip_line(p, chunk_end);
            }

            local_bytes += static_cast<size_t>(p - last_p);
            last_p = p;
            if (local_bytes > 32 * 1024 * 1024) {
                tracker.add_progress(local_bytes);
                local_bytes = 0;
            }
        }
        if (local_bytes > 0) {
            tracker.add_progress(local_bytes);
        }
    }

    // Merge paralelo dos acumuladores por thread (cada bin é independente).
    if (collect_paths) {
#pragma omp parallel for schedule(static) num_threads(num_threads)
        for (size_t b = 0; b < num_bins; ++b) {
            for (int t = 0; t < num_threads; ++t) {
                auto& src_seqs = thr_in_ram[static_cast<size_t>(t)][b];
                auto& src_names = thr_names[static_cast<size_t>(t)][b];
                std::move(src_seqs.begin(), src_seqs.end(), std::back_inserter(bin_in_ram_seqs[b]));
                std::move(src_names.begin(), src_names.end(),
                          std::back_inserter(bin_path_names[b]));
            }
        }
    }

    tracker.finish();

    IngestStatsParallel stats;
    for (int i = 0; i < num_threads; ++i) {
        stats.segment_count += local_segments[i].value;
        stats.link_count += local_links[i].value;
    }

    // Merge paralelo via prefix-sum
    std::vector<SegmentRecord> all_records;
    if (collect_records) {
        std::vector<size_t> rec_off(static_cast<size_t>(num_threads) + 1, 0);
        for (int i = 0; i < num_threads; ++i) {
            rec_off[i + 1] = rec_off[i] + thread_recs[i].size();
        }
        all_records.resize(rec_off.back());
#pragma omp parallel for schedule(static) num_threads(num_threads)
        for (int i = 0; i < num_threads; ++i) {
            auto& src = thread_recs[static_cast<size_t>(i)];
            std::move(src.begin(), src.end(),
                      all_records.begin() + static_cast<std::ptrdiff_t>(rec_off[i]));
            src = {};
        }
    }

    std::vector<LinkRecord> all_links;
    if (collect_links) {
        std::vector<size_t> lnk_off(static_cast<size_t>(num_threads) + 1, 0);
        for (int i = 0; i < num_threads; ++i) {
            lnk_off[i + 1] = lnk_off[i] + thread_links[i].size();
        }
        all_links.resize(lnk_off.back());
#pragma omp parallel for schedule(static) num_threads(num_threads)
        for (int i = 0; i < num_threads; ++i) {
            auto& src = thread_links[static_cast<size_t>(i)];
            std::move(src.begin(), src.end(),
                      all_links.begin() + static_cast<std::ptrdiff_t>(lnk_off[i]));
            src = {};
        }
    }

    // Merge serial dos paths
    std::vector<PassadaDoisResult::GbwtGroup> gbwt_groups;
    if (collect_paths) {
        size_t total_paths = 0;
        for (size_t t = 0; t < num_bins; ++t) {
            if (bin_gbwt_bufs.size() > t && bin_gbwt_bufs[t]) {
                bin_gbwt_bufs[t]->close();
                bin_gbwt_bufs[t].reset();
            }
            total_paths += bin_path_names[t].size();
        }
        stats.path_count = total_paths;

        gbwt_groups.reserve(num_bins);
        for (size_t t = 0; t < num_bins; ++t) {
            if (!bin_path_names[t].empty()) {
                PassadaDoisResult::GbwtGroup g;
                if (bin_use_in_ram[t]) {
                    g.in_ram_seqs = std::move(bin_in_ram_seqs[t]);
                } else {
                    g.tmp_path = (t < tmp_guards.size()) ? tmp_guards[t].release()
                                                         : std::move(bin_gbwt_paths[t]);
                }
                g.path_names = std::move(bin_path_names[t]);
                if (has_remap && t < bin_sigma_.size()) {
                    g.sigma = bin_sigma_[t];
                }
                gbwt_groups.push_back(std::move(g));
            }
        }
    }

    return PassadaDoisResult{stats, std::move(all_records), std::move(all_links),
                             std::move(gbwt_groups), node_width_out};
}

// ══════════════════════════════════════════════════════════════════════════════
// Persistência
// ══════════════════════════════════════════════════════════════════════════════

bool GfaIngestorMmapParallel::save_mphf_and_remap(const std::string& dir) {
    {
        std::ofstream out(dir + "/mphf.bin", std::ios::binary);
        if (!out) {
            fmt::print(stderr, "  [cache] AVISO: falha ao criar mphf.bin\n");
            return false;
        }
        res_.mphf->save(out);
        if (!out) {
            fmt::print(stderr, "  [cache] AVISO: erro de I/O ao salvar mphf.bin\n");
            return false;
        }
    }
    if (!remap_.empty()) {
        std::ofstream out(dir + "/remap.bin", std::ios::binary);
        if (!out) {
            fmt::print(stderr, "  [cache] AVISO: falha ao criar remap.bin\n");
            return false;
        }
        uint64_t sz = static_cast<uint64_t>(remap_.size());
        out.write(reinterpret_cast<const char*>(&sz), sizeof(sz));
        out.write(reinterpret_cast<const char*>(remap_.data()),
                  static_cast<std::streamsize>(sz * sizeof(uint32_t)));
        sz = static_cast<uint64_t>(comp_for_node_.size());
        out.write(reinterpret_cast<const char*>(&sz), sizeof(sz));
        out.write(reinterpret_cast<const char*>(comp_for_node_.data()),
                  static_cast<std::streamsize>(sz * sizeof(uint32_t)));
        if (!out) {
            fmt::print(stderr, "  [cache] AVISO: erro de I/O ao salvar remap.bin\n");
            return false;
        }
    }
    return true;
}

// Deriva o nome canônico do perfil a partir dos flags. Retorna "custom"
// quando os flags não correspondem exatamente a nenhum perfil pré-definido.
[[nodiscard]] static const char*
profile_name_from_flags(const GfaIngestorMmapParallel::CachedFlags& f) noexcept {
    if (f.save_metadata && f.build_fm_index && f.build_topology && f.build_gbwt && f.build_snarls) {
        return "pangenome";
    }
    if (f.save_metadata && f.build_fm_index && f.build_topology && !f.build_gbwt &&
        !f.build_snarls) {
        return "metagenomics";
    }
    if (f.save_metadata && !f.build_fm_index && f.build_topology && !f.build_gbwt &&
        f.build_snarls) {
        return "assembly";
    }
    if (f.save_metadata && !f.build_fm_index && f.build_topology && f.build_gbwt &&
        !f.build_snarls) {
        return "comparative";
    }
    return "custom";
}

// ISO-8601 com fuso UTC ("2026-04-30T12:34:56Z"). Independente de locale.
[[nodiscard]] static std::string iso_utc_now() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

void GfaIngestorMmapParallel::write_manifest_atomic(const std::string& dir, uint64_t hash,
                                                    uint64_t file_sz, int64_t file_mtime_ns_val,
                                                    uint8_t node_width,
                                                    const IngestStatsParallel& stats,
                                                    CachedFlags flags) {
    const std::string tmp_path = dir + "/manifest.tmp";
    {
        std::ofstream out(tmp_path);
        if (!out) {
            fmt::print(stderr, "  [cache] AVISO: falha ao criar manifest.tmp\n");
            return;
        }
        out << "version=4\n"
            << "gfa_hash=" << hash << "\n"
            << "gfa_size=" << file_sz << "\n"
            << "gfa_mtime_ns=" << file_mtime_ns_val << "\n"
            << "node_width=" << static_cast<int>(node_width) << "\n"
            << "num_nodes=" << stats.node_count << "\n"
            << "link_count=" << stats.link_count << "\n"
            << "path_count=" << stats.path_count << "\n"
            << "num_components=" << num_components_ << "\n"
            << "num_bins=" << num_bins_packed_ << "\n"
            << "profile=" << profile_name_from_flags(flags) << "\n"
            << "indexed_at=" << iso_utc_now() << "\n"
            << "flag_save_metadata=" << (flags.save_metadata ? 1 : 0) << "\n"
            << "flag_build_fm_index=" << (flags.build_fm_index ? 1 : 0) << "\n"
            << "flag_build_topology=" << (flags.build_topology ? 1 : 0) << "\n"
            << "flag_build_gbwt=" << (flags.build_gbwt ? 1 : 0) << "\n"
            << "flag_build_snarls=" << (flags.build_snarls ? 1 : 0) << "\n";
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, dir + "/manifest.txt", ec);
    if (ec) {
        fmt::print(stderr, "  [cache] AVISO: rename manifest falhou: {}\n", ec.message());
    }
}

// Lê os flags do manifest. Retorna nullopt em manifest ausente ou v3-.
std::optional<GfaIngestorMmapParallel::CachedFlags>
GfaIngestorMmapParallel::read_cached_flags(const std::string& dir) const {
    namespace fs = std::filesystem;
    if (!fs::exists(dir + "/manifest.txt")) {
        return std::nullopt;
    }
    std::ifstream in(dir + "/manifest.txt");
    if (!in) {
        return std::nullopt;
    }

    int manifest_version = 1;
    CachedFlags f;
    bool any_flag_seen = false;
    auto fc_i = [](std::string_view sv) -> int {
        int v = 0;
        std::from_chars(sv.data(), sv.data() + sv.size(), v);
        return v;
    };

    std::string line;
    while (std::getline(in, line)) {
        std::string_view lv{line};
        if (lv.starts_with("version=")) {
            manifest_version = fc_i(lv.substr(8));
        } else if (lv.starts_with("flag_save_metadata=")) {
            f.save_metadata = fc_i(lv.substr(19)) != 0;
            any_flag_seen = true;
        } else if (lv.starts_with("flag_build_fm_index=")) {
            f.build_fm_index = fc_i(lv.substr(20)) != 0;
            any_flag_seen = true;
        } else if (lv.starts_with("flag_build_topology=")) {
            f.build_topology = fc_i(lv.substr(20)) != 0;
            any_flag_seen = true;
        } else if (lv.starts_with("flag_build_gbwt=")) {
            f.build_gbwt = fc_i(lv.substr(16)) != 0;
            any_flag_seen = true;
        } else if (lv.starts_with("flag_build_snarls=")) {
            f.build_snarls = fc_i(lv.substr(18)) != 0;
            any_flag_seen = true;
        }
    }
    if (manifest_version < 4 || !any_flag_seen) {
        return std::nullopt; // v3 ou anterior: sem flags persistidos
    }
    return f;
}

// Regrava manifest mantendo todas as linhas, exceto as de flags/profile/
// indexed_at que são substituídas pelos novos valores. Preserva ordem das
// linhas existentes; se algum campo de flag não estava no manifest (v3),
// é adicionado no final.
void GfaIngestorMmapParallel::rewrite_manifest_flags(const std::string& dir,
                                                     CachedFlags new_flags) const {
    namespace fs = std::filesystem;
    if (!fs::exists(dir + "/manifest.txt")) {
        return;
    }

    std::vector<std::string> lines;
    {
        std::ifstream in(dir + "/manifest.txt");
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(std::move(line));
        }
    }

    auto starts_with = [](const std::string& s, std::string_view pfx) {
        return s.size() >= pfx.size() && std::memcmp(s.data(), pfx.data(), pfx.size()) == 0;
    };
    bool seen_save_meta = false, seen_fm = false, seen_topo = false;
    bool seen_gbwt = false, seen_snarls = false;
    bool seen_profile = false, seen_indexed = false, seen_version = false;
    for (auto& l : lines) {
        if (starts_with(l, "version=")) {
            l = "version=4";
            seen_version = true;
        } else if (starts_with(l, "profile=")) {
            l = std::string("profile=") + profile_name_from_flags(new_flags);
            seen_profile = true;
        } else if (starts_with(l, "indexed_at=")) {
            l = "indexed_at=" + iso_utc_now();
            seen_indexed = true;
        } else if (starts_with(l, "flag_save_metadata=")) {
            l = std::string("flag_save_metadata=") + (new_flags.save_metadata ? "1" : "0");
            seen_save_meta = true;
        } else if (starts_with(l, "flag_build_fm_index=")) {
            l = std::string("flag_build_fm_index=") + (new_flags.build_fm_index ? "1" : "0");
            seen_fm = true;
        } else if (starts_with(l, "flag_build_topology=")) {
            l = std::string("flag_build_topology=") + (new_flags.build_topology ? "1" : "0");
            seen_topo = true;
        } else if (starts_with(l, "flag_build_gbwt=")) {
            l = std::string("flag_build_gbwt=") + (new_flags.build_gbwt ? "1" : "0");
            seen_gbwt = true;
        } else if (starts_with(l, "flag_build_snarls=")) {
            l = std::string("flag_build_snarls=") + (new_flags.build_snarls ? "1" : "0");
            seen_snarls = true;
        }
    }
    if (!seen_version) {
        lines.insert(lines.begin(), "version=4");
    }
    if (!seen_profile) {
        lines.push_back(std::string("profile=") + profile_name_from_flags(new_flags));
    }
    if (!seen_indexed) {
        lines.push_back("indexed_at=" + iso_utc_now());
    }
    if (!seen_save_meta) {
        lines.push_back(std::string("flag_save_metadata=") + (new_flags.save_metadata ? "1" : "0"));
    }
    if (!seen_fm) {
        lines.push_back(std::string("flag_build_fm_index=") +
                        (new_flags.build_fm_index ? "1" : "0"));
    }
    if (!seen_topo) {
        lines.push_back(std::string("flag_build_topology=") +
                        (new_flags.build_topology ? "1" : "0"));
    }
    if (!seen_gbwt) {
        lines.push_back(std::string("flag_build_gbwt=") + (new_flags.build_gbwt ? "1" : "0"));
    }
    if (!seen_snarls) {
        lines.push_back(std::string("flag_build_snarls=") + (new_flags.build_snarls ? "1" : "0"));
    }

    const std::string tmp = dir + "/manifest.tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            fmt::print(stderr, "  [cache] AVISO: falha ao criar manifest.tmp em rewrite\n");
            return;
        }
        for (const auto& l : lines) {
            out << l << '\n';
        }
    }
    std::error_code ec;
    fs::rename(tmp, dir + "/manifest.txt", ec);
    if (ec) {
        fmt::print(stderr, "  [cache] AVISO: rename manifest falhou em rewrite: {}\n",
                   ec.message());
    }
}

// Apaga arquivos órfãos quando o novo perfil é mais restrito que o antigo.
bool GfaIngestorMmapParallel::prune_cache_for_downgrade(const std::string& dir,
                                                        CachedFlags old_flags,
                                                        CachedFlags new_flags, bool verbose) const {
    namespace fs = std::filesystem;
    bool removed_any = false;
    auto try_remove = [&](const char* fname) {
        std::error_code ec;
        if (fs::remove(dir + "/" + fname, ec)) {
            if (verbose) {
                cli::step_info(fmt::format("downgrade: removeu {}", fname));
            }
            removed_any = true;
        }
    };

    // FM-Index: agora desabilitado; apaga fm_index.bin
    if (old_flags.build_fm_index && !new_flags.build_fm_index) {
        try_remove("fm_index.bin");
    }
    // Índice de paths: agora desabilitado; apaga path_stores.bin (e
    // gbwts.bin de caches legacy) + path_stats.bin.
    if (old_flags.build_gbwt && !new_flags.build_gbwt) {
        try_remove("path_stores.bin");
        try_remove("gbwts.bin");
        try_remove("path_stats.bin");
    }
    // Snarls: agora desabilitado; apaga snarls.bin
    if (old_flags.build_snarls && !new_flags.build_snarls) {
        try_remove("snarls.bin");
    }
    // Metadata: agora desabilitado; apaga arquivos do MetadataStore
    if (old_flags.save_metadata && !new_flags.save_metadata) {
        try_remove("metadata.bin");
        try_remove("metadata_seqs.bin");
        try_remove("metadata_strings.bin");
        try_remove("length_index.bin");
        try_remove("depth_index.bin");
    }
    return removed_any;
}

std::optional<IngestStatsParallel>
GfaIngestorMmapParallel::load_index(const std::string& dir, std::optional<uint64_t> expected_hash,
                                    uint64_t file_sz, int64_t file_mtime_ns_val) {
    namespace fs = std::filesystem;
    if (!fs::exists(dir + "/manifest.txt")) {
        return std::nullopt;
    }

    uint64_t hash = 0, cached_sz = 0;
    int64_t cached_mtime_ns = 0;
    int manifest_version = 1;
    uint64_t n_nodes = 0, n_links = 0, n_paths = 0;
    uint32_t n_comps = 0, n_bins = 0;
    int n_width = 0;

    auto fc_u64 = [](std::string_view sv, uint64_t& out) {
        std::from_chars(sv.data(), sv.data() + sv.size(), out);
    };
    auto fc_u32 = [](std::string_view sv, uint32_t& out) {
        std::from_chars(sv.data(), sv.data() + sv.size(), out);
    };
    auto fc_i32 = [](std::string_view sv, int& out) {
        std::from_chars(sv.data(), sv.data() + sv.size(), out);
    };

    std::ifstream in_m(dir + "/manifest.txt");
    std::string line;
    while (std::getline(in_m, line)) {
        std::string_view lv{line};
        if (lv.starts_with("version=")) {
            fc_i32(lv.substr(8), manifest_version);
        } else if (lv.starts_with("gfa_hash=")) {
            fc_u64(lv.substr(9), hash);
        } else if (lv.starts_with("gfa_size=")) {
            fc_u64(lv.substr(9), cached_sz);
        } else if (lv.starts_with("gfa_mtime_ns=")) {
            uint64_t tmp = 0;
            fc_u64(lv.substr(13), tmp);
            cached_mtime_ns = static_cast<int64_t>(tmp);
        } else if (lv.starts_with("num_nodes=")) {
            fc_u64(lv.substr(10), n_nodes);
        } else if (lv.starts_with("link_count=")) {
            fc_u64(lv.substr(11), n_links);
        } else if (lv.starts_with("path_count=")) {
            fc_u64(lv.substr(11), n_paths);
        } else if (lv.starts_with("node_width=")) {
            fc_i32(lv.substr(11), n_width);
        } else if (lv.starts_with("num_components=")) {
            fc_u32(lv.substr(15), n_comps);
        } else if (lv.starts_with("num_bins=")) {
            fc_u32(lv.substr(9), n_bins);
        }
    }

    if (manifest_version < 3) {
        // v3: encoding GBWT-bound corrigido (gbwt_encode_node com shift +1).
        // Caches v2 ou anteriores tinham paths divididos silenciosamente em
        // node_id=0 forward. Re-ingest obrigatório.
        cli::step_info(
            fmt::format("cache version={} desatualizado — será regenerado", manifest_version));
        return std::nullopt;
    }

    if (file_sz > 0 && file_sz == cached_sz && file_mtime_ns_val != 0 &&
        file_mtime_ns_val == cached_mtime_ns) {
        cli::step_info("cache válido (mtime+size) — carregando");
    } else if (expected_hash.has_value()) {
        if (*expected_hash != hash) {
            cli::step_info("cache inválido (hash mismatch) — re-ingerindo");
            return std::nullopt;
        }
        cli::step_info("cache válido (hash) — carregando");
    } else {
        return std::nullopt;
    }

    const auto t_start = std::chrono::steady_clock::now();

    std::ifstream in_mphf(dir + "/mphf.bin", std::ios::binary);
    if (!in_mphf) {
        cli::step_info("mphf.bin ausente — cache incompleto, regenerando");
        return std::nullopt;
    }
    res_.mphf = std::make_unique<boophf_parallel_t>();
    res_.mphf->load(in_mphf);

    res_.topology = std::make_unique<GfaCsrTopology>();
    try {
        res_.topology->load(dir);
    } catch (const std::exception& e) {
        cli::step_info(fmt::format("falha ao carregar topologia: {} — regenerando", e.what()));
        res_.topology.reset();
        res_.mphf.reset();
        return std::nullopt;
    }

    constexpr uint64_t MAX_SANE_NODES = 500'000'000ULL;
    if (fs::exists(dir + "/remap.bin")) {
        std::ifstream in_r(dir + "/remap.bin", std::ios::binary);
        bool remap_ok = static_cast<bool>(in_r);
        uint64_t sz = 0;
        if (remap_ok) {
            in_r.read(reinterpret_cast<char*>(&sz), sizeof(sz));
            remap_ok = in_r.good() && sz <= MAX_SANE_NODES;
        }
        if (remap_ok) {
            remap_.resize(static_cast<size_t>(sz));
            in_r.read(reinterpret_cast<char*>(remap_.data()),
                      static_cast<std::streamsize>(sz * sizeof(uint32_t)));
            remap_ok = in_r.good();
        }
        if (remap_ok) {
            in_r.read(reinterpret_cast<char*>(&sz), sizeof(sz));
            remap_ok = in_r.good() && sz <= MAX_SANE_NODES;
        }
        if (remap_ok) {
            comp_for_node_.resize(static_cast<size_t>(sz));
            in_r.read(reinterpret_cast<char*>(comp_for_node_.data()),
                      static_cast<std::streamsize>(sz * sizeof(uint32_t)));
            remap_ok = static_cast<bool>(in_r);
        }
        if (remap_ok) {
            num_components_ = n_comps;
            num_bins_packed_ = n_bins;
        } else {
            fmt::print(stderr, "  [cache] AVISO: remap.bin corrompido — ignorado\n");
            remap_.clear();
            comp_for_node_.clear();
        }
    }

    const auto t_end = std::chrono::steady_clock::now();
    IngestStatsParallel st;
    st.node_count = n_nodes;
    st.link_count = n_links;
    st.path_count = n_paths;
    st.gbwt_node_width = static_cast<uint8_t>(n_width);
    st.elapsed_total_s = std::chrono::duration<double>(t_end - t_start).count();

    cli::step_ok("cache carregado", ggql::format_seconds(st.elapsed_total_s));
    return st;
}

// ══════════════════════════════════════════════════════════════════════════════
// Auxiliares do orquestrador ingest()
// ══════════════════════════════════════════════════════════════════════════════

// FASE 1 — upgrade incremental de cache.
// Quando o cache em disco cobre tudo exceto snarls.bin, derivamos snarls
// diretamente da topology já carregada (snarl_decomposition::find_all_snarls opera
// apenas na CSR — não precisa do GFA original).
IngestStatsParallel GfaIngestorMmapParallel::complete_with_snarls(
    const std::string& cache_dir, IngestStatsParallel stats, bool verbose,
    std::chrono::high_resolution_clock::time_point t_start) {
    using clock = std::chrono::high_resolution_clock;
    using sec = std::chrono::duration<double>;

    if (!res_.topology) {
        return stats; // proteção; caller já validou
    }
    try {
        const auto t0 = clock::now();
        auto snarls = snarl_decomposition::find_all_snarls(*res_.topology, /*num_threads=*/1);
        const auto count = snarls.size();
        res_.topology->set_snarls(std::move(snarls));
        res_.topology->save_snarls_only(cache_dir);
        if (verbose) {
            cli::step_ok_end("Snarls (FIND BUBBLE)",
                             fmt::format("{} snarls em {}", cli::format_count(count),
                                         ggql::format_seconds(sec(clock::now() - t0).count())));
        }
    } catch (const std::exception& e) {
        fmt::print(stderr,
                   "  [snarls] AVISO: falha ao computar snarls — FIND BUBBLE indisponível: {}\n",
                   e.what());
    }
    stats.elapsed_total_s = sec(clock::now() - t_start).count();
    return stats;
}

// FASE 2 — upgrade incremental de cache.
// Cache cobre MPHF + topology + metadata + (fm_index opcional), mas falta
// gbwts.bin. Em vez de re-Pass1+Pass2 completos, fazemos um Pass2 paths-only
// (sem coletar segmentos/links — esses já estão em disco) e reconstruímos
// apenas o gBWT. Custo HPRC estimado: ~30 s vs ~165 s da reingestão completa.
//
// Pré-condição: res_.mmap, res_.mphf, res_.topology e remap_ já carregados.
std::expected<IngestStatsParallel, std::string> GfaIngestorMmapParallel::complete_with_gbwt(
    const std::string& /*filepath*/, const std::string& cache_dir, int num_threads,
    size_t max_gbwt_paths, bool build_snarls, bool verbose, IngestStatsParallel stats,
    std::chrono::high_resolution_clock::time_point t_start) {
    using clock = std::chrono::high_resolution_clock;
    using sec = std::chrono::duration<double>;

    if (!res_.mmap || !res_.mphf || !res_.topology) {
        return std::unexpected("complete_with_gbwt: pré-condição inválida (cache não carregado)");
    }

    // bin_packing não é persistido — reseta para que process_data_parallel
    // use num_threads bins simples (sem bin packing por componente).
    num_bins_packed_ = 0;
    bin_sigma_.clear();
    comp_to_bin_.clear();

    if (verbose) {
        cli::phase(2, "Pass2 paths-only — coletando W/P-lines do GFA");
    }
    const auto t_p2_0 = clock::now();
    const size_t mp = (max_gbwt_paths == 0) ? std::numeric_limits<size_t>::max() : max_gbwt_paths;
    auto pass2 = process_data_parallel(num_threads,
                                       /*collect_records=*/false,
                                       /*collect_links=*/false,
                                       /*collect_paths=*/true, mp, verbose);
    const auto t_p2_1 = clock::now();
    stats.elapsed_ingest_s = sec(t_p2_1 - t_p2_0).count();
    stats.path_count = pass2.stats.path_count;
    stats.node_count = res_.topology->num_nodes();

    if (pass2.gbwt_groups.empty()) {
        if (verbose) {
            cli::step_info("Sem W/P-lines no GFA — pulando construção de gBWT");
        }
        // Mesmo sem paths, snarls (se solicitado) ainda devem ser computados
        // a partir da topology em cache. Não retorna cedo aqui.
        if (build_snarls && !std::filesystem::exists(cache_dir + "/snarls.bin")) {
            stats = complete_with_snarls(cache_dir, std::move(stats), verbose, t_start);
        }
        stats.elapsed_total_s = sec(clock::now() - t_start).count();
        return stats;
    }

    // Build serial (1 thread) na Fase 2: paths podem atravessar componentes
    // (típico de metaFlye e similares — paths são walks pré-collapse), e o
    // repartition por componente não é seguro aqui. Trade-off: paralelismo
    // perdido na construção do índice de paths, mas Pass1/CSR/metadata já
    // foram pulados (que era o ganho real da Fase 2).
    if (verbose) {
        cli::phase(5, fmt::format("gBWT serial (Fase 2) · {} paths em {} grupos",
                                  cli::format_count(stats.path_count), pass2.gbwt_groups.size()));
    }
    const uint8_t node_width = pass2.node_width;
    const auto t_gb0 = clock::now();
    auto gbwt_t = res_.topology->build_path_storage_parallel(std::move(pass2.gbwt_groups),
                                                              node_width,
                                                              /*num_threads=*/1);
    const auto t_gb1 = clock::now();
    stats.elapsed_gbwt_s = gbwt_t.wall_s;
    stats.gbwt_cpu_avg_s = gbwt_t.cpu_avg_s;
    stats.gbwt_cpu_max_s = gbwt_t.cpu_max_s;
    stats.gbwt_total_nodes = gbwt_t.total_nodes;
    stats.gbwt_peak_rss_kb = gbwt_t.peak_rss_kb;
    stats.gbwt_index_size_b = gbwt_t.index_size_b;
    stats.path_count = res_.topology->path_count();
    if (verbose) {
        cli::step_ok_end("gBWT construído",
                         fmt::format("{} paths · {} · {}", cli::format_count(stats.path_count),
                                     ggql::format_size(stats.gbwt_index_size_b),
                                     ggql::format_seconds(sec(t_gb1 - t_gb0).count())));
    }

    // Path stats — usa metadata em cache para lengths.
    // Reusa o caminho "GBWT decompression" pois não temos mais as sequências.
    GfaMetadataStore meta;
    try {
        meta.load(cache_dir);
        if (meta.lengths_span().size() == res_.topology->num_nodes()) {
            res_.topology->build_path_stats(meta.lengths_span());
        }
    } catch (...) { /* path_stats fica vazio — ok */
    }

    // Salva apenas path_stores.bin + path_stats.bin + topology.bin (não toca
    // metadata.bin/E_*/ef_*/fm_index.bin).
    res_.topology->save_path_index_only(cache_dir);

    // Snarls (se solicitado e ainda não em cache).
    if (build_snarls && !std::filesystem::exists(cache_dir + "/snarls.bin")) {
        stats = complete_with_snarls(cache_dir, std::move(stats), verbose, t_start);
    }

    stats.elapsed_total_s = sec(clock::now() - t_start).count();
    return stats;
}

std::optional<IngestStatsParallel> GfaIngestorMmapParallel::try_cache_fast(
    const std::string& cache_dir, uint64_t file_sz, int64_t file_mtime_ns, bool save_metadata,
    bool build_fm_index, bool build_gbwt, bool build_snarls, bool verbose, bool& out_skip,
    bool& out_complete_snarls_only, bool& out_complete_gbwt_only) {
    auto res = load_index(cache_dir, std::nullopt, file_sz, file_mtime_ns);
    if (!res) {
        return std::nullopt;
    }

    const bool meta_ok = !save_metadata || std::filesystem::exists(cache_dir + "/metadata.bin");
    const bool fm_ok = !build_fm_index || std::filesystem::exists(cache_dir + "/fm_index.bin");
    // gbwt_ok aceita o novo path_stores.bin OU o legacy gbwts.bin (migrado
    // on-the-fly em load()).
    const bool gbwt_ok = !build_gbwt
                          || std::filesystem::exists(cache_dir + "/path_stores.bin")
                          || std::filesystem::exists(cache_dir + "/gbwts.bin");
    const bool snarls_ok = !build_snarls || std::filesystem::exists(cache_dir + "/snarls.bin");

    if (meta_ok && fm_ok && gbwt_ok && snarls_ok) {
        if (verbose) {
            cli::step_ok_end("cache hit (mtime+size) — carregamento instantâneo");
        }
        return *res;
    }
    // FASE 1: só snarls ausente, tudo o resto presente → derivar da CSR já
    // carregada sem reler o GFA. Caller completa com snarl_decomposition::find_all_snarls.
    if (meta_ok && fm_ok && gbwt_ok && !snarls_ok && res_.topology) {
        out_complete_snarls_only = true;
        if (verbose) {
            cli::step_info("cache parcial (só snarls falta) — completando incrementalmente");
        }
        return *res;
    }
    // FASE 2: só gbwt (e possivelmente snarls) ausente. MPHF + topology +
    // metadata em cache → caller faz Pass2 paths-only sobre o GFA, sem
    // reconstruir CSR/EF/metadata. Retornamos nullopt para o caller seguir
    // o fluxo de abrir mmap.
    if (meta_ok && fm_ok && !gbwt_ok && res_.topology && res_.mphf) {
        out_complete_gbwt_only = true;
        if (verbose) {
            cli::step_info("cache parcial (só gBWT falta) — Pass2 paths-only");
        }
        return std::nullopt;
    }
    out_skip = true;
    if (verbose) {
        cli::step_info("cache RAM ok (mtime) — prosseguindo para Passada 2 (metadata)");
    }
    return std::nullopt;
}

std::optional<IngestStatsParallel> GfaIngestorMmapParallel::try_cache_hash(
    const std::string& cache_dir, bool save_metadata, bool build_gbwt, bool build_snarls,
    bool verbose, uint64_t& out_hash, bool& out_skip, bool& out_complete_snarls_only,
    bool& out_complete_gbwt_only) {
    out_hash = XXH64(res_.mmap->data, res_.mmap->size, 0);
    // NÃO reseta mmap aqui: caller pode precisar para Pass2 paths-only.
    // O reset original (que existia para liberar RAM) acontecerá quando
    // load_index falhar e voltarmos para o caller reabrir.

    auto res = load_index(cache_dir, out_hash);
    if (!res) {
        // Cache miss real: fecha mmap (caller reabrirá) e retorna nullopt.
        res_.mmap.reset();
        return std::nullopt;
    }

    const bool meta_ok = !save_metadata || std::filesystem::exists(cache_dir + "/metadata.bin");
    const bool gbwt_ok = !build_gbwt
                          || std::filesystem::exists(cache_dir + "/path_stores.bin")
                          || std::filesystem::exists(cache_dir + "/gbwts.bin");
    const bool snarls_ok = !build_snarls || std::filesystem::exists(cache_dir + "/snarls.bin");

    if (meta_ok && gbwt_ok && snarls_ok) {
        res_.mmap.reset(); // hit completo, mmap não é mais necessário
        if (verbose) {
            cli::step_ok_end("cache hit (hash) — carregado");
        }
        return *res;
    }
    if (meta_ok && gbwt_ok && !snarls_ok && res_.topology) {
        res_.mmap.reset(); // Fase 1 não usa mmap
        out_complete_snarls_only = true;
        if (verbose) {
            cli::step_info("cache parcial (só snarls falta) — completando incrementalmente");
        }
        return *res;
    }
    if (meta_ok && !gbwt_ok && res_.topology && res_.mphf) {
        // FASE 2: mmap permanece aberto para Pass2 paths-only.
        out_complete_gbwt_only = true;
        if (verbose) {
            cli::step_info("cache parcial (só gBWT falta) — Pass2 paths-only");
        }
        return std::nullopt;
    }
    res_.mmap.reset();
    out_skip = true;
    if (verbose) {
        cli::step_info("cache RAM ok (hash) — prosseguindo para Passada 2 (metadata)");
    }
    return std::nullopt;
}

std::expected<IngestStatsParallel, std::string> GfaIngestorMmapParallel::run_full_ingest(
    int num_threads, double gamma, bool verbose, bool save_metadata, bool build_topology,
    bool build_fm_index, size_t max_gbwt_paths, bool keep_gbwt_tmp, const std::string& cache_dir,
    bool build_gbwt, bool build_snarls, uint64_t file_sz, int64_t file_mtime_ns, uint64_t gfa_hash,
    bool skip_pass1, std::chrono::high_resolution_clock::time_point t_start) {
    using clock = std::chrono::high_resolution_clock;
    using sec = std::chrono::duration<double>;

    const bool do_early_remap = build_topology && build_gbwt && (max_gbwt_paths > 0);

    Pass1Result pass1;
    auto t0 = clock::now();
    auto t_io1 = t0;
    auto t_built = t0;

    if (!skip_pass1) {
        if (verbose) {
            cli::phase(1, do_early_remap ? "Ingestão de IDs (Passada 1 + L-pairs)"
                                         : "Ingestão de IDs (Passada 1)");
        }
        pass1 = collect_keys_and_links_parallel(num_threads, do_early_remap, verbose);
        t_io1 = clock::now();
        if (pass1.keys.empty()) {
            if (verbose) {
                cli::step_fail_end("nenhum segmento encontrado");
            }
            return std::unexpected("Nenhum segmento encontrado");
        }
        if (verbose) {
            cli::step_ok("Passada 1",
                         fmt::format("{} nós · {}", cli::format_count(pass1.keys.size()),
                                     ggql::format_seconds(sec(t_io1 - t0).count())));
        }

        res_.mphf = std::make_unique<boophf_parallel_t>(pass1.keys.size(), pass1.keys, num_threads,
                                                        gamma, false, false);
        t_built = clock::now();
        if (verbose) {
            cli::step_ok("MPHF construído",
                         fmt::format("{:.2f} bits/chave · {} · {}",
                                     static_cast<double>(res_.mphf->totalBitSize()) /
                                         static_cast<double>(pass1.keys.size()),
                                     ggql::format_size(res_.mphf->totalBitSize() / 8),
                                     ggql::format_seconds(sec(t_built - t_io1).count())));
        }

        {
            std::vector<std::string_view> tmp;
            pass1.keys.swap(tmp);
        }

        if (do_early_remap) {
            auto t_uf0 = clock::now();
            build_remap_from_pairs(num_threads, pass1.thread_pairs);
            auto t_uf1 = clock::now();
            if (verbose) {
                cli::step_ok_end("Union-Find + bin-packing",
                                 fmt::format("{} componentes → {} bins · {}", num_components_,
                                             num_bins_packed_,
                                             ggql::format_seconds(sec(t_uf1 - t_uf0).count())));
            }
        }
    } else {
        t_io1 = clock::now();
        t_built = t_io1;
    }

    bool save_ok = true;
    if (!skip_pass1) {
        std::filesystem::create_directories(cache_dir);
        save_ok = save_mphf_and_remap(cache_dir);
    }

    const bool write_db = save_metadata;
    if (verbose) {
        std::string suffix = "processamento";
        if (write_db) {
            suffix += " + metadados";
        }
        if (build_topology) {
            suffix += " + links + paths";
        }
        cli::phase(2, fmt::format("Passada 2 paralela — {}", suffix));
    }
    const bool collect_paths = (max_gbwt_paths > 0) && build_topology && build_gbwt;
    auto pass2_ = process_data_parallel(num_threads, write_db, build_topology, collect_paths,
                                        max_gbwt_paths, verbose);
    auto& stats = pass2_.stats;
    auto& records = pass2_.records;
    auto& links = pass2_.links;
    auto& gbwt_groups = pass2_.gbwt_groups;
    uint8_t& node_width = pass2_.node_width;
    auto t1 = clock::now();

    stats.elapsed_io1_s = sec(t_io1 - t0).count();
    stats.elapsed_build_s = sec(t_built - t_io1).count();
    stats.elapsed_ingest_s = sec(t1 - t_built).count();
    stats.node_count = res_.mphf->nbKeys();

    std::unique_ptr<char[]> fm_seq_for_build;
    size_t fm_seq_for_build_size = 0;
    // Mantido vivo além do bloco write_db para build_path_stats após GBWT.
    std::unique_ptr<GfaMetadataStore> store_up;

    if (write_db) {
        if (verbose) {
            cli::step_info(fmt::format("gravando {} segmentos nos Metadados",
                                       cli::format_count(records.size())));
        }
        auto t_db0 = clock::now();
        store_up = std::make_unique<GfaMetadataStore>();
        store_up->init(stats.node_count);
        store_up->flush_segments(records);
        store_up->save(cache_dir);
        auto t_db1 = clock::now();
        stats.elapsed_metadata_s = sec(t_db1 - t_db0).count();
        if (verbose) {
            cli::step_ok_end("Metadata idx flush", ggql::format_seconds(stats.elapsed_metadata_s));
        }
        {
            std::vector<SegmentRecord> tmp;
            records.swap(tmp);
        }
        if (build_fm_index) {
            auto pair = store_up->take_sequences();
            fm_seq_for_build = std::move(pair.first);
            fm_seq_for_build_size = pair.second;
        }
    }

    res_.mphf.reset();
    {
        std::vector<uint32_t> tmp;
        remap_.swap(tmp);
    }
    {
        std::vector<uint32_t> tmp;
        comp_for_node_.swap(tmp);
    }
    {
        std::vector<uint32_t> tmp;
        comp_to_bin_.swap(tmp);
    }
    {
        std::vector<uint64_t> tmp;
        bin_sigma_.swap(tmp);
    }
    res_.mmap.reset();

    if (fm_seq_for_build_size > 0) {
        if (verbose) {
            cli::phase(3, "FM-Index");
        }
        auto t_fm0 = clock::now();
        GfaSequenceIndex fm_index;
        fm_index.build(std::span<const char>(fm_seq_for_build.get(),
                                             fm_seq_for_build_size),
                       num_threads);
        fm_seq_for_build.reset();
        fm_seq_for_build_size = 0;
        fm_index.save(cache_dir);
        auto t_fm1 = clock::now();
        stats.elapsed_fm_index_s = sec(t_fm1 - t_fm0).count();
    }

    if (build_topology && !skip_pass1) {
        if (verbose) {
            cli::phase(4, fmt::format("CSR Topology · {} links", cli::format_count(links.size())));
        }
        auto t_topo0 = clock::now();
        res_.topology = std::make_unique<GfaCsrTopology>();
        res_.topology->build(stats.node_count, std::move(links), num_threads);
        auto t_topo1 = clock::now();
        stats.elapsed_topology_s = sec(t_topo1 - t_topo0).count();
        if (verbose) {
            cli::step_info_end("CSR topology",
                               fmt::format("{} · {}",
                                           ggql::format_seconds(stats.elapsed_topology_s),
                                           ggql::format_size(res_.topology->memory_bytes())));
        }

        GfaCsrTopology::RemapResult remap_res;
        if (build_gbwt && !gbwt_groups.empty() && !do_early_remap) {
            auto t_remap0 = clock::now();
            remap_res = res_.topology->make_component_remap(num_threads);
            res_.topology->remap_node_ids(remap_res.remap);
            auto t_remap1 = clock::now();
            if (verbose) {
                cli::step_ok("Remap IDs por componente",
                             ggql::format_seconds(sec(t_remap1 - t_remap0).count()));
            }

            auto t_rw0 = clock::now();
            const uint64_t N_nodes = stats.node_count;
            const uint8_t new_node_width = static_cast<uint8_t>(
                N_nodes > 1 ? std::bit_width((N_nodes - 1) * 2ull + 1ull) : 1u);
            const auto& remap = remap_res.remap;
            const int n_groups = static_cast<int>(gbwt_groups.size());

#pragma omp parallel for schedule(dynamic) num_threads(num_threads)
            for (int gi = 0; gi < n_groups; ++gi) {
                auto& g = gbwt_groups[gi];
                if (g.tmp_path.empty() && g.in_ram_seqs.empty()) {
                    continue;
                }
                if (!g.in_ram_seqs.empty()) {
                    for (auto& seq : g.in_ram_seqs) {
                        for (auto& enc : seq) {
                            // enc já vem com shift +1 de parse_w/p_line.
                            const uint64_t old_node = gbwt_decode_node(enc);
                            const uint64_t new_node = (old_node < remap.size())
                                                          ? static_cast<uint64_t>(remap[old_node])
                                                          : old_node;
                            enc = gbwt_encode_node(new_node, gbwt_decode_orient(enc));
                        }
                    }
                } else {
                    const std::string old_path = g.tmp_path;
                    const std::string new_path = old_path + ".vg";
                    ggql::NodeBuffer in_buf(old_path, std::ios::in, 64u << 20, node_width);
                    ggql::NodeBuffer out_buf(new_path, std::ios::out, 64u << 20,
                                              new_node_width);
                    for (std::uint64_t val : in_buf) {
                        if (val == ggql::kEndmarker) {
                            out_buf.push_back(ggql::kEndmarker);
                        } else {
                            const uint64_t old_node = gbwt_decode_node(val);
                            const uint64_t orient = gbwt_decode_orient(val);
                            const uint64_t new_node = (old_node < remap.size())
                                                          ? static_cast<uint64_t>(remap[old_node])
                                                          : old_node;
                            out_buf.push_back(gbwt_encode_node(new_node, orient));
                        }
                    }
                    out_buf.close();
                    ::unlink(old_path.c_str());
                    g.tmp_path = new_path;
                }
            }
            node_width = new_node_width;
            auto t_rw1 = clock::now();
            if (verbose) {
                cli::step_ok("Reescrita de arquivos gBWT temporários",
                             ggql::format_seconds(sec(t_rw1 - t_rw0).count()));
            }
        }

        if (build_gbwt && !gbwt_groups.empty()) {
            const size_t total_paths = stats.path_count;
            if (verbose) {
                cli::phase(5, fmt::format("gBWT paralelo · {} paths em {} grupos",
                                          cli::format_count(total_paths), gbwt_groups.size()));
            }

            std::vector<GfaCsrTopology::GbwtGroup> raw_groups = std::move(gbwt_groups);

            std::vector<GfaCsrTopology::GbwtGroup> topo_groups;
            if (do_early_remap) {
                topo_groups = std::move(raw_groups);
            } else if (!remap_res.node_component.empty()) {
                auto t_rp0 = clock::now();
                topo_groups = res_.topology->repartition_groups_topological(
                    std::move(raw_groups), remap_res.node_component, remap_res.num_components,
                    node_width);
                auto t_rp1 = clock::now();
                if (verbose) {
                    cli::step_ok("repartição por componente",
                                 fmt::format("{} grupos → {} grupos · {}", topo_groups.size(),
                                             topo_groups.size(),
                                             ggql::format_seconds(sec(t_rp1 - t_rp0).count())));
                }
            } else {
                topo_groups = std::move(raw_groups);
            }

            if (keep_gbwt_tmp) {
                stats.gbwt_node_width = node_width;
                for (const auto& tg : topo_groups) {
                    stats.gbwt_path_names.insert(stats.gbwt_path_names.end(), tg.path_names.begin(),
                                                 tg.path_names.end());
                }
            }

            // ── Pré-computa path stats antes do GBWT build ────────────────────────
            // Lê sequências de in_ram_seqs (RAM) ou de tmp_path (disco sequencial).
            // Ambas as fontes são muito mais rápidas que build_path_stats() que
            // re-atravessa o GBWT comprimido (~15 min no HPRC: 4,46B traversais).
            //
            // RAM → O(total_nodes) a ~20 GB/s (~1-2 s HPRC).
            // Disco (tmp_path) → leitura sequencial a ~500 MB/s (~1-2 min HPRC)
            //                   vs ~15-24 min via GBWT decompression.
            //
            // Grupos vazios (in_ram_seqs e tmp_path ambos vazios) são ignorados.
            std::vector<uint32_t> precomp_nc;
            std::vector<int64_t> precomp_bp;
            bool stats_from_ram = false;
            if (store_up && res_.topology) {
                const auto lengths = store_up->lengths_span();
                precomp_nc.reserve(stats.path_count);
                precomp_bp.reserve(stats.path_count);
                for (const auto& g : topo_groups) {
                    if (!g.in_ram_seqs.empty()) {
                        // Grupo em RAM: iteração direta — O(nodes) a ~20 GB/s
                        for (const auto& seq : g.in_ram_seqs) {
                            if (seq.empty()) {
                                continue;
                            }
                            precomp_nc.push_back(static_cast<uint32_t>(seq.size()));
                            int64_t bp = 0;
                            for (const uint64_t v : seq) {
                                const uint64_t nid = gbwt_decode_node(v);
                                if (nid < lengths.size()) {
                                    bp += static_cast<int64_t>(lengths[nid]);
                                }
                            }
                            precomp_bp.push_back(bp);
                        }
                    } else if (!g.tmp_path.empty()) {
                        // Grupo em disco: leitura sequencial do tmp file (muito mais
                        // rápido que GBWT decompression). gbwt_encode_node garante que
                        // valores armazenados nunca colidem com ggql::kEndmarker (0).
                        ggql::NodeBuffer buf(g.tmp_path, std::ios::in, size_t(64) << 20,
                                              node_width);
                        uint32_t cur_nc = 0;
                        int64_t cur_bp = 0;
                        for (const std::uint64_t val : buf) {
                            if (val == ggql::kEndmarker) {
                                if (cur_nc > 0) {
                                    precomp_nc.push_back(cur_nc);
                                    precomp_bp.push_back(cur_bp);
                                }
                                cur_nc = 0;
                                cur_bp = 0;
                            } else {
                                const uint64_t nid = gbwt_decode_node(val);
                                ++cur_nc;
                                if (nid < lengths.size()) {
                                    cur_bp += static_cast<int64_t>(lengths[nid]);
                                }
                            }
                        }
                        // Defensivo: path sem ENDMARKER final (não deveria acontecer)
                        if (cur_nc > 0) {
                            precomp_nc.push_back(cur_nc);
                            precomp_bp.push_back(cur_bp);
                        }
                    }
                    // Grupo vazio: sem paths, sem ação
                }
                stats_from_ram = true;
            }

            auto t_gbwt0 = clock::now();
            auto gbwt_t = res_.topology->build_path_storage_parallel(std::move(topo_groups),
                                                                      node_width, num_threads);
            auto t_gbwt1 = clock::now();

            stats.elapsed_gbwt_s = gbwt_t.wall_s;
            stats.gbwt_cpu_avg_s = gbwt_t.cpu_avg_s;
            stats.gbwt_cpu_max_s = gbwt_t.cpu_max_s;
            stats.gbwt_total_nodes = gbwt_t.total_nodes;
            stats.gbwt_peak_rss_kb = gbwt_t.peak_rss_kb;
            stats.gbwt_index_size_b = gbwt_t.index_size_b;
            stats.path_count = res_.topology->path_count();
            if (verbose) {
                cli::step_ok_end("gBWT construído",
                                 fmt::format("{} paths · {} · {}",
                                             cli::format_count(stats.path_count),
                                             ggql::format_size(stats.gbwt_index_size_b),
                                             ggql::format_seconds(stats.elapsed_gbwt_s)));
            }

            // Aplica path stats: pré-computado (RAM/disco) ou GBWT decompression
            // (fallback usado apenas se store_up existir mas precomp falhou —
            // shouldn't happen with v3 encoding, mas mantido como defesa).
            if (store_up) {
                if (stats_from_ram && precomp_nc.size() != res_.topology->path_count()) {
                    // Não deveria ocorrer com gbwt_encode_node (v3); se ocorrer,
                    // fallback via GBWT decompression preserva correção.
                    if (verbose) {
                        cli::step_info(
                            "Path stats",
                            fmt::format("precomp count {} != path_count {} — usando fallback",
                                        precomp_nc.size(), res_.topology->path_count()));
                    }
                    stats_from_ram = false;
                    precomp_nc.clear();
                    precomp_bp.clear();
                }
                auto t_ps0 = clock::now();
                if (stats_from_ram) {
                    res_.topology->set_path_stats(std::move(precomp_nc), std::move(precomp_bp));
                } else {
                    res_.topology->build_path_stats(store_up->lengths_span());
                }
                auto t_ps1 = clock::now();
                if (verbose) {
                    const double ps_s = sec(t_ps1 - t_ps0).count();
                    const char* method = stats_from_ram ? "RAM+disco (pré-computado)"
                                                        : "GBWT decompression (fallback)";
                    cli::step_ok_end("Path stats",
                                     fmt::format("{} · {}", method, ggql::format_seconds(ps_s)));
                }
                store_up.reset();
            }
        }
    }

    stats.elapsed_total_s = sec(clock::now() - t_start).count();

    if (!skip_pass1) {
        // Snarls (FIND BUBBLE): pré-computa antes de salvar a topologia.
        // Usa snarl_decomposition::find_all_snarls (port nativo otimizado de
        // at-cg/billi): construção iterativa do biedged graph + SESE, paridade
        // exata com o binário Billi. Single-thread por enquanto.
        if (build_snarls && res_.topology) {
            try {
                auto t_snarls0 = clock::now();
                auto snarls = snarl_decomposition::find_all_snarls(*res_.topology, num_threads);
                const auto count = snarls.size();
                res_.topology->set_snarls(std::move(snarls));
                if (verbose) {
                    cli::step_info(
                        "Snarls (FIND BUBBLE)",
                        fmt::format("{} snarls em {}", cli::format_count(count),
                                    ggql::format_seconds(sec(clock::now() - t_snarls0).count())));
                }
            } catch (const std::exception& e) {
                fmt::print(
                    stderr,
                    "  [snarls] AVISO: falha ao computar snarls — FIND BUBBLE indisponível: {}\n",
                    e.what());
            }
        }
        try {
            if (res_.topology) {
                res_.topology->save(cache_dir);
            }
        } catch (const std::exception& e) {
            fmt::print(stderr, "  [cache] AVISO: falha ao salvar topologia: {}\n", e.what());
        }
        if (save_ok) {
            CachedFlags cf{save_metadata, build_fm_index, build_topology, build_gbwt, build_snarls};
            write_manifest_atomic(cache_dir, gfa_hash, file_sz, file_mtime_ns, node_width, stats,
                                  cf);
        }
    }

    if (verbose) {
        cli::ok("Ingestão concluída", ggql::format_seconds(stats.elapsed_total_s));
        cli::note(fmt::format(
            "  segmentos {} · links {} · nós únicos {}", cli::format_count(stats.segment_count),
            cli::format_count(stats.link_count), cli::format_count(stats.node_count)));
    }

    return stats;
}

// ══════════════════════════════════════════════════════════════════════════════
// ingest — orquestrador principal
// ══════════════════════════════════════════════════════════════════════════════

std::expected<IngestStatsParallel, std::string>
GfaIngestorMmapParallel::ingest(const std::string& filepath, int num_threads, double gamma,
                                bool verbose, bool save_metadata, bool build_topology,
                                bool build_fm_index, size_t max_gbwt_paths, bool keep_gbwt_tmp,
                                const std::string& cache_dir, bool build_gbwt, bool build_snarls) {
    using clock = std::chrono::high_resolution_clock;

    if (num_threads <= 0) {
        return std::unexpected("num_threads deve ser positivo");
    }
    if (!build_topology && (build_gbwt || build_snarls)) {
        return std::unexpected(
            "build_gbwt/build_snarls requerem build_topology=true (ambos dependem da CSR)");
    }

#ifdef _OPENMP
    if (verbose) {
        cli::note(fmt::format("OpenMP habilitado · {} threads", num_threads));
    }
#else
    if (verbose)
        cli::note("OpenMP NÃO habilitado — rodando serial");
    num_threads = 1;
#endif

    uint64_t file_sz = 0;
    int64_t file_mtime_ns = 0;
    try {
        file_sz = static_cast<uint64_t>(std::filesystem::file_size(filepath));
        file_mtime_ns = get_mtime_ns(filepath);
    } catch (...) {}

    const auto t_start = clock::now();
    bool skip_pass1 = false;
    uint64_t gfa_hash = 0;

    if (verbose) {
        cli::phase(0, "Verificando cache");
        cli::step_info("cache dir", cache_dir);
    }

    // Downgrade automático: se o cache existente foi indexado com flags
    // mais permissivos que os pedidos agora (ex: pangenome → metagenomics),
    // apaga arquivos órfãos antes de carregar. Reduz RAM no load() já que
    // topology->load() carrega tudo que está em disco.
    {
        const CachedFlags requested{save_metadata, build_fm_index, build_topology, build_gbwt,
                                    build_snarls};
        if (auto cached_flags = read_cached_flags(cache_dir); cached_flags.has_value()) {
            if (prune_cache_for_downgrade(cache_dir, *cached_flags, requested, verbose)) {
                rewrite_manifest_flags(cache_dir, requested);
            }
        }
    }

    bool complete_snarls_only = false;
    bool complete_gbwt_only = false;

    // ── fast path O(1): mtime + size ─────────────────────────────────────────
    if (auto cached = try_cache_fast(cache_dir, file_sz, file_mtime_ns, save_metadata,
                                     build_fm_index, build_gbwt, build_snarls, verbose, skip_pass1,
                                     complete_snarls_only, complete_gbwt_only)) {
        if (complete_snarls_only) {
            cached = complete_with_snarls(cache_dir, *cached, verbose, t_start);
        }
        return *cached;
    }

    // ── abrir mmap ────────────────────────────────────────────────────────────
    auto mmap_res = MmapFileParallel::open(filepath);
    if (!mmap_res) {
        return std::unexpected(mmap_res.error());
    }
    res_.mmap = std::make_unique<MmapFileParallel>(std::move(*mmap_res));
    if (verbose) {
        cli::step_ok_end("Arquivo mapeado", ggql::format_size(res_.mmap->size));
    }

    // FASE 2: try_cache_fast detectou só gBWT faltando — Pass2 paths-only.
    // try_cache_hash adiante seria redundante (mtime+size já casou).
    if (complete_gbwt_only) {
        return complete_with_gbwt(filepath, cache_dir, num_threads, max_gbwt_paths, build_snarls,
                                  verbose, IngestStatsParallel{}, t_start);
    }

    // ── verificação por hash XXH64 (só se mtime não casou) ───────────────────
    if (!skip_pass1) {
        if (auto cached =
                try_cache_hash(cache_dir, save_metadata, build_gbwt, build_snarls, verbose,
                               gfa_hash, skip_pass1, complete_snarls_only, complete_gbwt_only)) {
            if (complete_snarls_only) {
                cached = complete_with_snarls(cache_dir, *cached, verbose, t_start);
            }
            return *cached;
        }
        // Fase 2: mmap continua aberto, MPHF/topology em RAM. Pass2 paths-only.
        if (complete_gbwt_only) {
            // O try_cache_hash já consumiu o mmap (XXH64 lê todos os bytes);
            // reabre antes de Pass2.
            if (!res_.mmap) {
                auto mmap_res2 = MmapFileParallel::open(filepath);
                if (!mmap_res2) {
                    return std::unexpected(mmap_res2.error());
                }
                res_.mmap = std::make_unique<MmapFileParallel>(std::move(*mmap_res2));
            }
            return complete_with_gbwt(filepath, cache_dir, num_threads, max_gbwt_paths,
                                      build_snarls, verbose, IngestStatsParallel{}, t_start);
        }
        // try_cache_hash fechou mmap_data_; reabrir para pass1
        auto mmap_res2 = MmapFileParallel::open(filepath);
        if (!mmap_res2) {
            return std::unexpected(mmap_res2.error());
        }
        res_.mmap = std::make_unique<MmapFileParallel>(std::move(*mmap_res2));
    }

    // ── ingestão completa (pass1 + MPHF + pass2 + topologia + save) ──────────
    return run_full_ingest(num_threads, gamma, verbose, save_metadata, build_topology,
                           build_fm_index, max_gbwt_paths, keep_gbwt_tmp, cache_dir, build_gbwt,
                           build_snarls, file_sz, file_mtime_ns, gfa_hash, skip_pass1, t_start);
}

} // namespace ggql
