// ggql_repl.cpp — REPL interativo para GGQL
//
// Uso:
//   ggql_repl                                    — abre sem dados carregados
//   ggql_repl --gfa <arquivo.gfa>                — ingere GFA e constrói índices
//   ggql_repl --gfa <arquivo.gfa> --idx <path>   — especifica diretório de cache
//   ggql_repl --threads 8 --gfa <arquivo.gfa>    — usa 8 threads no ingest
//   ggql_repl --idx <dir>                        — abre cache existente (sem GFA nativo)
//
// Dentro do REPL:
//   FIND TIP WHERE length < 200 RETURN id;
//   .help    — mostra ajuda
//   .stats   — exibe estatísticas do grafo / banco carregado
//   .load <arquivo.gfa>  — carrega um GFA (descarta dados anteriores)
//   .quit    — sai
//
// Terminador de query: ';' (pode ser omitido em linhas que começam com FIND/SEARCH)
// Multi-linha: a acumulação de linhas termina quando ';' for encontrado.

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fmt/color.h>
#include <fmt/core.h>
#include <ggql/cli/console.hpp>
#include <ggql/ingest/ingestor.hpp>
#include <ggql/ingest/metadata_store.hpp>
#include <ggql/ingest/profile.hpp>
#include <ggql/query/grammar.hpp>
#include <ggql/query/query_planner.hpp>
#include <ggql/storage/fm_index.hpp>
#include <ggql/utils.hpp>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

using namespace ggql;
using namespace ggql::grammar;

// ─── Utilidades ───────────────────────────────────────────────────────────────

[[nodiscard]] static std::string_view trim(std::string_view s) noexcept {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string to_upper(std::string_view s) {
    return s | std::views::transform([](unsigned char c) { return (char)std::toupper(c); }) |
           std::ranges::to<std::string>();
}

// ─── Sessão de dados ──────────────────────────────────────────────────────────

struct Session {
    GfaIngestorMmapParallel ingestor;
    std::optional<IngestStatsParallel> stats;
    std::unique_ptr<QueryPlanner> planner;

    GfaMetadataStore metadata;
    GfaSequenceIndex fm_index;

    std::string idx_path;
    std::string gfa_path;
    ggql::IngestProfile profile = ggql::IngestProfile::Pangenome;

    [[nodiscard]] bool has_data() const noexcept { return planner != nullptr; }
};

// ─── Exibição de ajuda ────────────────────────────────────────────────────────

static void print_help() {
    const auto accent = fg(fmt::color::orange);
    const auto white = fg(fmt::color::white);
    const auto gray = fg(fmt::color::dim_gray);

    fmt::print(accent, "  ┌{:─^65}┐\n", "");
    fmt::print(accent, "  │");
    fmt::print(fmt::emphasis::bold, "{:^65}", "GGQL — Ajuda rápida");
    fmt::print(accent, "│\n");
    fmt::print(accent, "  ├{:─^65}┤\n", "");

    fmt::print(accent, "  │ ");
    fmt::print(white | fmt::emphasis::bold, "{:<64}", "Queries");
    fmt::print(accent, "│\n");

    fmt::print(accent, "  │ ");
    fmt::print(gray, "   FIND TIP / BUBBLE / REPEAT [WHERE <cond>]                    ");
    fmt::print(accent, "│\n");

    fmt::print(accent, "  │ ");
    fmt::print(gray, "   SEARCH KMER \"<seq>\" [RETURN <fields>]                        ");
    fmt::print(accent, "│\n");

    fmt::print(accent, "  │");
    fmt::print("{:65}", "");
    fmt::print(accent, "│\n");

    fmt::print(accent, "  │ ");
    fmt::print(white | fmt::emphasis::bold, "{:<64}", "Comandos internos");
    fmt::print(accent, "│\n");

    fmt::print(accent, "  │ ");
    fmt::print(gray, "   .help                — esta mensagem                         ");
    fmt::print(accent, "│\n");

    fmt::print(accent, "  │ ");
    fmt::print(gray, "   .stats               — estatísticas do grafo/banco carregado ");
    fmt::print(accent, "│\n");

    fmt::print(accent, "  │ ");
    fmt::print(gray, "   .load <file.gfa>     — carrega/recarrega um arquivo GFA      ");
    fmt::print(accent, "│\n");

    fmt::print(accent, "  │ ");
    fmt::print(gray, "   .quit / .exit        — sai do REPL                           ");
    fmt::print(accent, "│\n");

    fmt::print(accent, "  └{:─^65}┘\n\n", "");
}

// ─── Exibição de estatísticas ─────────────────────────────────────────────────

static void print_stats(const Session& sess) {
    if (!sess.has_data()) {
        fmt::println("  Nenhum dado carregado. Use .load <arquivo.gfa>");
        return;
    }

    fmt::println("");
    if (!sess.gfa_path.empty()) {
        fmt::println("  GFA      : {}", sess.gfa_path);
    }
    if (!sess.idx_path.empty()) {
        fmt::println("  CacheIdx : {}", sess.idx_path);
    }

    if (sess.stats) {
        const auto& s = *sess.stats;
        fmt::println("  Nós      : {}", s.node_count);
        fmt::println("  Segmentos: {}", s.segment_count);
        fmt::println("  Links    : {}", s.link_count);
        fmt::println("  Tempo total ingest: {}", ggql::format_seconds(s.elapsed_total_s));
        fmt::println("    passada1   : {}", ggql::format_seconds(s.elapsed_io1_s));
        fmt::println("    build MPHF : {}", ggql::format_seconds(s.elapsed_build_s));
        fmt::println("    passada2   : {}", ggql::format_seconds(s.elapsed_ingest_s));
        fmt::println("    metadados  : {}", ggql::format_seconds(s.elapsed_metadata_s));
        fmt::println("    topologia  : {}", ggql::format_seconds(s.elapsed_topology_s));
        if (s.elapsed_fm_index_s > 0) {
            fmt::println("    fm-index   : {}", ggql::format_seconds(s.elapsed_fm_index_s));
        }
    }
    fmt::println("");
}

// ─── Carregamento de GFA ──────────────────────────────────────────────────────

static void load_gfa(Session& sess, const std::string& gfa_path, int threads,
                     ggql::IngestProfile profile = ggql::IngestProfile::Pangenome,
                     const std::string& idx_path_override = "") {
    namespace fs = std::filesystem;

    if (!fs::exists(gfa_path)) {
        fmt::println(stderr, "  Erro: arquivo não encontrado: {}", gfa_path);
        return;
    }

    // Determina caminho do idx_path
    std::string idx_path = idx_path_override;
    if (idx_path.empty()) {
        idx_path = gfa_path + ".idx";
        // Garante que o diretório existe
        std::filesystem::create_directories(std::filesystem::path(idx_path).parent_path());
    }

    const auto pf = ggql::flags_for(profile);

    fmt::println("  Carregando {} ...", gfa_path);
    fmt::println("  MetadadosIdx : {}", idx_path);
    fmt::println("  Threads      : {}", threads);
    fmt::println("  Perfil       : {}", ggql::name_of(profile));

    sess.ingestor.reset();

    const auto result = sess.ingestor.ingest(gfa_path, threads,
                                             /*gamma=*/2.0,
                                             /*verbose=*/false,
                                             pf.save_metadata,
                                             pf.build_topology,
                                             pf.build_fm_index,
                                             /*max_gbwt_paths=*/1'000'000,
                                             /*keep_gbwt_tmp=*/false, idx_path,
                                             pf.build_gbwt,
                                             pf.build_snarls);

    if (!result) {
        fmt::println(stderr, "  Erro no ingest: {}", result.error());
        sess.planner.reset();
        return;
    }

    sess.stats = *result;
    sess.gfa_path = gfa_path;
    sess.idx_path = idx_path;

    // Carrega para acesso na memória
    sess.metadata.load(idx_path);
    if (auto r = sess.fm_index.load(idx_path); !r) {
        cli::note(fmt::format("FM-Index ignorado: {}", r.error()));
    }

    sess.planner =
        std::make_unique<QueryPlanner>(sess.ingestor.topology(), &sess.metadata,
                                       sess.fm_index.is_loaded() ? &sess.fm_index : nullptr);

    fmt::print(fg(fmt::color::green), "  ✔");
    fmt::print(" Pronto: {} nós, {} links (", result->node_count, result->link_count);
    fmt::print(fg(fmt::color::yellow), "{}", ggql::format_seconds(result->elapsed_total_s));
    fmt::print(")\n");
}

// ─── Execução de uma query ────────────────────────────────────────────────────

static void execute_query(const Session& sess, std::string_view query_str) {
    if (!sess.has_data()) {
        fmt::println(stderr, "  Nenhum dado carregado. Use .load <arquivo.gfa> ou --gfa.");
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();

    const auto parsed = parse_ggql(query_str);
    if (!parsed) {
        fmt::println(stderr, "  Erro de sintaxe: {}", parsed.error());
        return;
    }

    const auto result = sess.planner->execute(*parsed);
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    if (!result) {
        fmt::println(stderr, "  Erro: {}", result.error());
        return;
    }

    fmt::print("{}", result->format_table());

    fmt::print("  Tempo: {}\n\n", ggql::format_seconds(ns / 1e9));
}

// ─── Loop principal ───────────────────────────────────────────────────────────

static void repl_loop(Session& sess, int threads) {
    const bool tty = ::isatty(STDIN_FILENO);
    std::string buffer; // acumulador multi-linha

    auto prompt = [&]() {
        if (!tty) {
            return;
        }
        fmt::print(fg(fmt::color::orange) | fmt::emphasis::bold, "{}",
                   buffer.empty() ? "ggql> " : "   -> ");
        std::fflush(stdout);
    };

    prompt();
    for (std::string line; std::getline(std::cin, line);) {
        const std::string_view trimmed = trim(line);

        // ── Ignora linhas vazias (exceto se em acumulação multi-linha) ────────
        if (trimmed.empty()) {
            if (!buffer.empty()) {
                // Tenta executar o buffer acumulado como query implicitamente
                // completa (sem ';'). Só tenta se começa com FIND/SEARCH.
                const std::string up = to_upper(trim(buffer));
                if (up.starts_with("FIND") || up.starts_with("SEARCH")) {
                    execute_query(sess, trim(buffer));
                    buffer.clear();
                }
            }
            prompt();
            continue;
        }

        // ── Meta-comandos (sempre com buffer vazio) ───────────────────────────
        if (trimmed[0] == '.') {
            if (!buffer.empty()) {
                fmt::println(stderr, "  Buffer pendente descartado.");
                buffer.clear();
            }

            if (trimmed == ".quit" || trimmed == ".exit") {
                fmt::println("  bye.");
                return;
            } else if (trimmed == ".help") {
                print_help();
            } else if (trimmed == ".stats") {
                print_stats(sess);
            } else if (trimmed.starts_with(".load ")) {
                const std::string path{trim(trimmed.substr(6))};
                if (path.empty()) {
                    fmt::println(stderr, "  Uso: .load <arquivo.gfa>");
                } else {
                    load_gfa(sess, path, threads, sess.profile);
                }
            } else {
                fmt::println(stderr, "  Comando desconhecido: {}  (tente .help)", trimmed);
            }

            prompt();
            continue;
        }

        // ── Acumula linha no buffer ────────────────────────────────────────────
        if (!buffer.empty()) {
            buffer += '\n';
        }
        buffer += trimmed;

        // ── Verifica se a query está completa (termina em ';') ────────────────
        if (buffer.back() == ';') {
            buffer.pop_back(); // remove o ';'
            const std::string q{trim(buffer)};
            buffer.clear();
            if (!q.empty()) {
                execute_query(sess, q);
            }
        } else {
            // Sem ';': se a linha começa com FIND ou SEARCH, executa imediatamente
            const std::string up = to_upper(trim(buffer));
            if (up.starts_with("FIND") || up.starts_with("SEARCH")) {
                // Verifica se o conteúdo atual parece completo (não termina
                // com uma palavra incompleta que exigiria continuação).
                // Heurística: tenta parsear — se falhar, continua acumulando.
                const auto test = parse_ggql(trim(buffer));
                if (test.has_value()) {
                    // Query completa — executa
                    execute_query(sess, trim(buffer));
                    buffer.clear();
                }
                // Se falhou, mantém buffer e pede mais input
            }
        }

        prompt();
    }

    // EOF: executa buffer pendente se houver
    if (!buffer.empty()) {
        const std::string q{trim(buffer)};
        if (!q.empty()) {
            execute_query(sess, q);
        }
    }
}

// ─── Parsing de argumentos ────────────────────────────────────────────────────

struct Args {
    std::string gfa_path;
    std::string idx_path;
    int threads = 4;
    bool help = false;
    ggql::IngestProfile profile = ggql::IngestProfile::Pangenome;
    bool profile_err = false;
    std::string profile_err_val;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if ((arg == "--gfa" || arg == "-g") && i + 1 < argc) {
            a.gfa_path = argv[++i];
        } else if ((arg == "--idx" || arg == "-i") && i + 1 < argc) {
            a.idx_path = argv[++i];
        } else if ((arg == "--threads" || arg == "-t") && i + 1 < argc) {
            const std::string_view sv{argv[++i]};
            const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), a.threads);
            if (ec != std::errc{}) {
                a.threads = 4; // fallback: padrão
            }
        } else if ((arg == "--profile" || arg == "-p") && i + 1 < argc) {
            const std::string_view pv{argv[++i]};
            auto parsed = ggql::parse_profile(pv);
            if (!parsed) {
                a.profile_err = true;
                a.profile_err_val = pv;
            } else {
                a.profile = *parsed;
            }
        } else if (arg == "--help" || arg == "-h") {
            a.help = true;
        } else {
            fmt::println(stderr, "  Argumento desconhecido ignorado: {}", arg);
        }
    }
    return a;
}

static void usage(const char* prog) {
    fmt::print("Uso: {} [opções]\n"
               "  -g, --gfa <arquivo.gfa>    carrega e indexa o arquivo GFA\n"
               "  -i, --idx <dir>            abre diretório de cache/metadata binário\n"
               "  -t, --threads <N>          threads para ingest (padrão: 4)\n"
               "  -p, --profile <name>       perfil de ingestão (padrão: pangenome)\n"
               "                              pangenome     — tudo habilitado\n"
               "                              metagenomics  — sem gBWT, sem snarls\n"
               "                              assembly      — sem gBWT, com snarls\n"
               "                              comparative   — com gBWT, sem snarls\n"
               "  -h, --help                 esta mensagem\n",
               prog);
}

// ─── Banner ───────────────────────────────────────────────────────────────────

static void print_banner() {
    fmt::print(fg(fmt::color::orange) | fmt::emphasis::bold, R"(
   ██████╗  ██████╗  ██████╗ ██╗
  ██╔════╝ ██╔════╝ ██╔═══██╗██║
  ██║  ███╗██║  ███╗██║   ██║██║
  ██║   ██║██║   ██║██║▄▄ ██║██║
  ╚██████╔╝╚██████╔╝╚██████╔╝███████╗
   ╚═════╝  ╚═════╝  ╚══▀▀═╝ ╚══════╝
)");
    fmt::print(fmt::emphasis::bold, "\n Genomic Graph Query Language");
    fmt::print(fg(fmt::color::dim_gray), " — REPL interativo\n");
    fmt::print(fg(fmt::color::dim_gray), " Digite ");
    fmt::print(fmt::emphasis::bold, ".help");
    fmt::print(fg(fmt::color::dim_gray), " para ajuda ou ");
    fmt::print(fmt::emphasis::bold, ".quit");
    fmt::print(fg(fmt::color::dim_gray), " para sair.\n\n");
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    if (args.help) {
        usage(argv[0]);
        return 0;
    }
    if (args.profile_err) {
        fmt::println(stderr,
                     "Perfil desconhecido: '{}'  (use: pangenome|metagenomics|assembly|comparative)",
                     args.profile_err_val);
        return 1;
    }

    print_banner();

    Session sess;
    sess.profile = args.profile;

    // ── Carrega GFA se fornecido pela linha de comando ─────────────────────────
    if (!args.gfa_path.empty()) {
        load_gfa(sess, args.gfa_path, args.threads, args.profile, args.idx_path);
    } else if (!args.idx_path.empty()) {
        // Apenas IDX em cache — sem topologia CSR no momento a não ser que lida num save state
        sess.idx_path = args.idx_path;
        sess.metadata.load(args.idx_path);
        if (auto r = sess.fm_index.load(args.idx_path); !r) {
            cli::note(fmt::format("FM-Index ignorado: {}", r.error()));
        }
        sess.planner = std::make_unique<QueryPlanner>(
            nullptr, &sess.metadata, sess.fm_index.is_loaded() ? &sess.fm_index : nullptr);
        fmt::println("  Banco de Metadados carregado: {}", args.idx_path);
        if (sess.fm_index.is_loaded()) {
            fmt::println("  FM-Index carregado: SEARCH KMER disponível");
        }
        fmt::println("  (FIND TIP / BUBBLE / REPEAT requerem --gfa)\n");
    } else {
        fmt::print(fg(fmt::color::orange_red) | fmt::emphasis::bold, "  Nenhum dado carregado.\n");
        fmt::print("  Use ");
        fmt::print(fmt::emphasis::bold, ".load <arquivo.gfa>");
        fmt::print(" ou passe ");
        fmt::print(fmt::emphasis::bold, "--gfa");
        fmt::print(" na linha de comando.\n\n");
    }

    repl_loop(sess, args.threads);
    return 0;
}
