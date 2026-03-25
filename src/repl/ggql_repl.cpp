// ggql_repl.cpp — REPL interativo para GGQL
//
// Uso:
//   ggql_repl                                    — abre sem dados carregados
//   ggql_repl --gfa <arquivo.gfa>                — ingere GFA e constrói índices
//   ggql_repl --gfa <arquivo.gfa> --db <path>    — especifica caminho do SQLite
//   ggql_repl --gfa <arquivo.gfa> --threads 8    — usa 8 threads no ingest
//   ggql_repl --db  <arquivo.sqlite>             — abre banco existente (sem GFA)
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

#include <chrono>
#include <filesystem>
#include <ggql/ingest/gfa_ingestor.hpp>
#include <ggql/query/ggql_grammar.hpp>
#include <ggql/query/query_planner.hpp>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <print>
#include <ranges>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

using namespace ggql;
using namespace ggql::grammar;

// ─── Utilidades ───────────────────────────────────────────────────────────────

static std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return std::string{s.substr(b, e - b + 1)};
}

static std::string to_upper(std::string_view s) {
    std::string u{s};
    std::ranges::transform(u, u.begin(), [](unsigned char c) { return std::toupper(c); });
    return u;
}

// ─── Sessão de dados ──────────────────────────────────────────────────────────

struct Session {
    GfaIngestorMmapParallel ingestor;
    std::optional<IngestStatsParallel> stats;
    std::unique_ptr<QueryPlanner> planner;
    std::string db_path;
    std::string gfa_path;

    bool has_data() const noexcept { return planner != nullptr; }
};

// ─── Exibição de ajuda ────────────────────────────────────────────────────────

static void print_help() {
    std::print(R"(
┌─────────────────────────────────────────────────────────────────┐
│                   GGQL  —  Ajuda rápida                         │
├─────────────────────────────────────────────────────────────────┤
│ Queries                                                         │
│   FIND TIP   [WHERE <cond>] [RETURN <fields>]                   │
│   FIND BUBBLE [WHERE <cond>] [RETURN <fields>]                  │
│   FIND REPEAT [WHERE <cond>] [RETURN <fields>]                  │
│   FIND SEGMENT [WHERE <cond>] [RETURN <fields>]                 │
│   SEARCH KMER "<seq>" [RETURN <fields>]                         │
│                                                                 │
│ Entidades e campos disponíveis:                                 │
│   TIP/REPEAT  : id, in_degree, out_degree                       │
│   BUBBLE      : source, sink, branch_count                      │
│   SEGMENT     : id, name, length, read_count, depth             │
│                                                                 │
│ Operadores WHERE: =  !=  <  >  <=  >=                           │
│ Conectores   : AND, OR  (avaliação esq→dir, sem precedência)    │
│ Funções WHERE: crosses(<path>)   contains_kmer("<seq>")         │
│                                                                 │
│ Exemplos                                                        │
│   FIND TIP WHERE in_degree = 0 RETURN id                        │
│   FIND SEGMENT WHERE length > 1000 AND depth >= 5               │
│   FIND BUBBLE WHERE branch_count >= 2 RETURN source, sink       │
│   FIND SEGMENT WHERE name = "ctg001"                            │
│                                                                 │
│ Comandos internos                                               │
│   .help                — esta mensagem                          │
│   .stats               — estatísticas do grafo/banco carregado  │
│   .load <arquivo.gfa>  — carrega/recarrega um arquivo GFA       │
│   .quit  /  .exit      — sai do REPL                            │
│                                                                 │
│ Dica: ';' encerra uma query multi-linha                         │
└─────────────────────────────────────────────────────────────────┘
)");
}

// ─── Exibição de estatísticas ─────────────────────────────────────────────────

static void print_stats(const Session& sess) {
    if (!sess.has_data()) {
        std::println("  Nenhum dado carregado. Use .load <arquivo.gfa>");
        return;
    }

    std::println("");
    if (!sess.gfa_path.empty())
        std::println("  GFA      : {}", sess.gfa_path);
    if (!sess.db_path.empty())
        std::println("  SQLite   : {}", sess.db_path);

    if (sess.stats) {
        const auto& s = *sess.stats;
        std::println("  Nós      : {}", s.node_count);
        std::println("  Segmentos: {}", s.segment_count);
        std::println("  Links    : {}", s.link_count);
        std::println("  Tempo total ingest: {:.2f} s", s.elapsed_total_s);
        std::println("    passada1   : {:.2f} s", s.elapsed_io1_s);
        std::println("    build MPHF : {:.2f} s", s.elapsed_build_s);
        std::println("    passada2   : {:.2f} s", s.elapsed_ingest_s);
        std::println("    SQLite     : {:.2f} s", s.elapsed_sqlite_s);
        std::println("    topologia  : {:.2f} s", s.elapsed_topology_s);
    }
    std::println("");
}

// ─── Carregamento de GFA ──────────────────────────────────────────────────────

static void load_gfa(Session& sess, const std::string& gfa_path, int threads,
                     const std::string& db_path_override = "") {
    namespace fs = std::filesystem;

    if (!fs::exists(gfa_path)) {
        std::println(stderr, "  Erro: arquivo não encontrado: {}", gfa_path);
        return;
    }

    // Determina caminho do SQLite
    std::string db_path = db_path_override;
    if (db_path.empty()) {
        fs::path gfa_stem = fs::path(gfa_path).stem();
        db_path = (fs::path("data") / gfa_stem).string() + ".sqlite";
        // Garante que o diretório existe
        fs::create_directories(fs::path(db_path).parent_path());
    }

    // Remove banco anterior para evitar conflito de chaves duplicadas
    // O banco é inteiramente derivado do GFA — pode ser recriado a qualquer momento
    if (fs::exists(db_path)) {
        fs::remove(db_path);
        std::println("  (banco anterior removido: {})", db_path);
    }

    std::println("  Carregando {} ...", gfa_path);
    std::println("  SQLite   : {}", db_path);
    std::println("  Threads  : {}", threads);

    sess.ingestor.reset();

    const auto result = sess.ingestor.ingest(gfa_path, threads,
                                             /*gamma=*/2.0,
                                             /*verbose=*/false, db_path,
                                             /*build_topology=*/true);

    if (!result) {
        std::println(stderr, "  Erro no ingest: {}", result.error());
        sess.planner.reset();
        return;
    }

    sess.stats = *result;
    sess.gfa_path = gfa_path;
    sess.db_path = db_path;
    sess.planner = std::make_unique<QueryPlanner>(sess.ingestor.topology(), db_path);

    std::println("  Pronto: {} nós, {} links ({:.2f} s)", result->node_count, result->link_count, result->elapsed_total_s);
}

// ─── Execução de uma query ────────────────────────────────────────────────────

static void execute_query(const Session& sess, const std::string& query_str) {
    if (!sess.has_data()) {
        std::println(stderr, "  Nenhum dado carregado. Use .load <arquivo.gfa> ou --gfa.");
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();

    const auto parsed = parse_ggql(query_str);
    if (!parsed) {
        std::println(stderr, "  Erro de sintaxe: {}", parsed.error());
        return;
    }

    const auto result = sess.planner->execute(*parsed);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!result) {
        std::println(stderr, "  Erro: {}", result.error());
        return;
    }

    std::print("{}", result->format_table());
    std::println("  Tempo: {:.2f} ms\n", ms);
}

// ─── Loop principal ───────────────────────────────────────────────────────────

static void repl_loop(Session& sess, int threads) {
    const bool tty = ::isatty(STDIN_FILENO);
    std::string buffer; // acumulador multi-linha

    auto prompt = [&]() {
        if (!tty)
            return;
        std::print("{}", buffer.empty() ? "ggql> " : "   -> ");
        std::fflush(stdout);
    };

    prompt();
    for (std::string line; std::getline(std::cin, line);) {
        const std::string trimmed = trim(line);

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
                std::println(stderr, "  Buffer pendente descartado.");
                buffer.clear();
            }

            if (trimmed == ".quit" || trimmed == ".exit") {
                std::println("  bye.");
                return;
            } else if (trimmed == ".help") {
                print_help();
            } else if (trimmed == ".stats") {
                print_stats(sess);
            } else if (trimmed.starts_with(".load ")) {
                const std::string path = trim(trimmed.substr(6));
                if (path.empty())
                    std::println(stderr, "  Uso: .load <arquivo.gfa>");
                else
                    load_gfa(sess, path, threads);
            } else {
                std::println(stderr, "  Comando desconhecido: {}  (tente .help)", trimmed);
            }

            prompt();
            continue;
        }

        // ── Acumula linha no buffer ────────────────────────────────────────────
        if (!buffer.empty())
            buffer += '\n';
        buffer += trimmed;

        // ── Verifica se a query está completa (termina em ';') ────────────────
        if (buffer.back() == ';') {
            buffer.pop_back(); // remove o ';'
            const std::string q = trim(buffer);
            buffer.clear();
            if (!q.empty())
                execute_query(sess, q);
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
        const std::string q = trim(buffer);
        if (!q.empty())
            execute_query(sess, q);
    }
}

// ─── Parsing de argumentos ────────────────────────────────────────────────────

struct Args {
    std::string gfa_path;
    std::string db_path;
    int threads = 4;
    bool help = false;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if ((arg == "--gfa" || arg == "-g") && i + 1 < argc)
            a.gfa_path = argv[++i];
        else if ((arg == "--db" || arg == "-d") && i + 1 < argc)
            a.db_path = argv[++i];
        else if ((arg == "--threads" || arg == "-t") && i + 1 < argc)
            a.threads = std::stoi(argv[++i]);
        else if (arg == "--help" || arg == "-h")
            a.help = true;
        else
            std::println(stderr, "  Argumento desconhecido ignorado: {}", arg);
    }
    return a;
}

static void usage(const char* prog) {
    std::print("Uso: {} [opções]\n"
               "  --gfa <arquivo.gfa>    carrega e indexa o arquivo GFA\n"
               "  --db  <arquivo.sqlite> abre banco SQLite existente\n"
               "  --threads <N>          threads para ingest (padrão: 4)\n"
               "  --help                 esta mensagem\n", prog);
}

// ─── Banner ───────────────────────────────────────────────────────────────────

static void print_banner() {
    std::println(R"(
  ██████╗  ██████╗  ██████╗ ██╗
 ██╔════╝ ██╔════╝ ██╔═══██╗██║
 ██║  ███╗██║  ███╗██║   ██║██║
 ██║   ██║██║   ██║██║▄▄ ██║██║
 ╚██████╔╝╚██████╔╝╚██████╔╝███████╗
  ╚═════╝  ╚═════╝  ╚══▀▀═╝ ╚══════╝

 Genomic Graph Query Language — REPL interativo
 Digite .help para ajuda ou .quit para sair.
)");
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    if (args.help) {
        usage(argv[0]);
        return 0;
    }

    print_banner();

    Session sess;

    // ── Carrega GFA se fornecido pela linha de comando ─────────────────────────
    if (!args.gfa_path.empty()) {
        load_gfa(sess, args.gfa_path, args.threads, args.db_path);
    } else if (!args.db_path.empty()) {
        // Apenas SQLite — sem topologia CSR (SEGMENT funciona, TIP/BUBBLE/REPEAT não)
        sess.db_path = args.db_path;
        sess.planner = std::make_unique<QueryPlanner>(nullptr, args.db_path);
        std::println("  Banco SQLite aberto: {}", args.db_path);
        std::println("  (FIND TIP / BUBBLE / REPEAT requerem --gfa)\n");
    } else {
        std::print("  Nenhum dado carregado.\n"
                   "  Use .load <arquivo.gfa>  ou  passe --gfa na linha de comando.\n\n");
    }

    repl_loop(sess, args.threads);
    return 0;
}
