// bench_ingest — benchmark do ingestor GFA
//
// Uso: bench_ingest <arquivo.gfa> [threads] [gamma] [--topology]
//
// O banco SQLite é criado automaticamente em ./data/<nome_do_gfa>.sqlite
// (pasta data/ criada no diretório de trabalho atual se não existir).
// --topology : constrói GfaCsrTopology (CSR em RAM) e exibe stats.
//


#include <algorithm>
#include <filesystem>
#include <print>
#include <stdexcept>
#include <string>

#include "ggql/ingest/gfa_ingestor.hpp"

static void separator(const char* title) {
    std::println("\n══════════════════════════════════════════");
    std::println("  {}", title);
    std::println("══════════════════════════════════════════");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println(stderr, "Uso: bench_ingest <arquivo.gfa> [threads] [gamma]");
        return 1;
    }
    const std::string path = argv[1];

    // Detecta flags em qualquer posição após argv[1] e coleta posicionais
    bool do_topology = false;
    bool no_sqlite   = false;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--topology")  { do_topology = true; continue; }
        if (a == "--no-sqlite") { no_sqlite   = true; continue; }
        if ((a == "-t" || a == "--threads") && i + 1 < argc) {
            positional.push_back(argv[++i]); // threads como posicional[0]
            continue;
        }
        if (a.size() >= 2 && a[0] == '-') continue; // flags desconhecidas
        positional.push_back(a);
    }
    const int    threads = (positional.size() >= 1) ? std::stoi(positional[0]) : 1;
    const double gamma   = (positional.size() >= 2) ? std::stod(positional[1]) : 2.0;

    // Deriva o caminho do banco: ./data/<stem_do_gfa>.sqlite
    // (desabilitado por --no-sqlite)
    namespace fs = std::filesystem;
    std::string db_path;
    if (!no_sqlite) {
        const fs::path data_dir = fs::current_path() / "data";
        std::error_code ec;
        fs::create_directories(data_dir, ec);
        if (ec) {
            std::println(stderr, "Erro ao criar pasta data/: {}", ec.message());
            return 1;
        }
        db_path = (data_dir / (fs::path(path).stem().string() + ".sqlite")).string();
    }

    std::println("Arquivo : {}", path);
    std::println("Threads : {}", threads);
    std::println("Gamma   : {}", gamma);
    std::println("SQLite  : {}", no_sqlite ? "(desabilitado)" : db_path);
    std::println("Topology: {}", do_topology ? "sim" : "nao");
    separator("mmap paralelo (OpenMP)");
    try {
        ggql::GfaIngestorMmapParallel ingestor;
        auto result = ingestor.ingest(path, threads, gamma, /*verbose=*/true, db_path,
                                     do_topology);
        if (!result) {
            std::println(stderr, "[omp] Erro: {}", result.error());
            return 1;
        }
        const auto& stats = *result;
        std::println("\n  [resumo]");
        std::println("  passada1={:.0f} ms  build={:.0f} ms  passada2={:.0f} ms  sqlite={:.0f} ms  topology={:.0f} ms  TOTAL={:.0f} ms",
                     stats.elapsed_io1_s    * 1000,
                     stats.elapsed_build_s  * 1000,
                     stats.elapsed_ingest_s * 1000,
                     stats.elapsed_sqlite_s * 1000,
                     stats.elapsed_topology_s * 1000,
                     stats.elapsed_total_s  * 1000);

        if (do_topology && ingestor.topology()) {
            const auto* topo = ingestor.topology();
            std::println("  [topologia]");
            std::println("  nos    : {}", topo->num_nodes());
            std::println("  arestas: {}", topo->num_edges());
            std::println("  memoria: {:.1f} MB", topo->memory_bytes() / (1024.0 * 1024.0));
        }
    } catch (const std::exception& e) {
        std::println(stderr, "[omp] Excecao: {}", e.what());
        return 1;
    }

    std::println("\nBenchmark concluido.");
    return 0;
}
