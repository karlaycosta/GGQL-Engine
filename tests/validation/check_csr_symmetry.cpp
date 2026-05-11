// check_csr_symmetry.cpp — Verifica simetria E_fwd↔E_bwd em um .idx.
//
// Bisect tool para B-V2. Carrega CSR do .idx, percorre todos os nós,
// e para cada (u, v) em out_neighbors(u) verifica se u aparece em
// in_neighbors(v). Reporta:
//   - total_links             : edges totais via out
//   - symmetric               : pares (u,v) com u em in_neighbors(v)
//   - asymmetric              : violações
//   - first_asymmetric_node   : menor u que tem violação
//   - last_asymmetric_node    : maior u que tem violação
//   - histogram por bucket de N nós : reveler distribuição espacial
//
// Uso: check_csr_symmetry <cache_dir.idx>

#include "ggql/storage/topology.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Uso: %s <cache_dir.idx>\n", argv[0]);
        return 2;
    }
    const std::string idx = argv[1];

    ggql::GfaCsrTopology topo;
    try {
        topo.load(idx);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Falha carregando %s: %s\n", idx.c_str(), e.what());
        return 1;
    }

    const uint64_t N = topo.num_nodes();
    std::printf("Loaded: %lu nodes, ~%lu edges\n",
                (unsigned long)N, (unsigned long)topo.num_edges());

    size_t total_links = 0, asymmetric = 0;
    uint64_t first_asym = UINT64_MAX, last_asym = 0;

    // Histograma: 32 buckets ao longo do node-id space
    constexpr int kBuckets = 32;
    std::array<size_t, kBuckets> bucket_total{};
    std::array<size_t, kBuckets> bucket_asym{};

    constexpr size_t kMaxSamples = 10;
    size_t samples_printed = 0;

    for (uint64_t u = 0; u < N; ++u) {
        const auto out_u = topo.out_neighbors(u);
        if (out_u.empty()) { continue; }
        const int bucket = static_cast<int>((u * kBuckets) / N);
        for (uint32_t enc_v : out_u) {
            ++total_links;
            ++bucket_total[bucket];
            const uint64_t v = ggql::GfaCsrTopology::decode(enc_v).node_id;
            const auto in_v = topo.in_neighbors(v);
            bool found = false;
            for (uint32_t enc_u : in_v) {
                if (ggql::GfaCsrTopology::decode(enc_u).node_id == u) {
                    found = true; break;
                }
            }
            if (!found) {
                ++asymmetric;
                ++bucket_asym[bucket];
                first_asym = std::min(first_asym, u);
                last_asym = std::max(last_asym, u);
                if (samples_printed < kMaxSamples) {
                    std::printf("  asym sample %zu: u=%lu → v=%lu, in_neighbors(v)=[",
                                samples_printed, (unsigned long)u, (unsigned long)v);
                    for (uint32_t e : in_v) {
                        std::printf("%lu, ",
                                    (unsigned long)ggql::GfaCsrTopology::decode(e).node_id);
                    }
                    std::printf("]\n");
                    ++samples_printed;
                }
            }
        }
    }

    std::printf("\n=== Resumo ===\n");
    std::printf("  total_links: %zu\n", total_links);
    std::printf("  symmetric:   %zu  (%.4f%%)\n",
                total_links - asymmetric,
                100.0 * (total_links - asymmetric) / std::max<size_t>(1, total_links));
    std::printf("  asymmetric:  %zu  (%.4f%%)\n",
                asymmetric, 100.0 * asymmetric / std::max<size_t>(1, total_links));
    if (asymmetric > 0) {
        std::printf("  first_asym_node: %lu\n", (unsigned long)first_asym);
        std::printf("  last_asym_node:  %lu\n", (unsigned long)last_asym);
        std::printf("\n  Histograma (32 buckets):\n");
        for (int b = 0; b < kBuckets; ++b) {
            const size_t tot = bucket_total[b], asy = bucket_asym[b];
            const double pct = tot > 0 ? 100.0 * asy / tot : 0.0;
            std::printf("    [%2d] node_range=[%lu..%lu)  total=%zu  asym=%zu (%.1f%%)\n",
                        b,
                        (unsigned long)((b * N) / kBuckets),
                        (unsigned long)(((b + 1) * N) / kBuckets),
                        tot, asy, pct);
        }
    }

    return asymmetric == 0 ? 0 : 1;
}
