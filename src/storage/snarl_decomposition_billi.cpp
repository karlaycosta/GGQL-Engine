// snarl_decomposition_billi.cpp — Wrapper que invoca o binário Billi
// (at-cg/billi) como subprocess para detecção de snarls.
//
// Por que wrapper em vez de port literal?
//   - Billi tem ~1500 linhas com estado global complexo (g_compacted,
//     dual, is_bridge, visit_order, to_chk, considered, possible_hairpins,
//     tip_start, biedged_connected_comp, entry_time, exit_time, etc.)
//   - Port literal exigiria portar TODA a infraestrutura (compact step,
//     find_unique_*, dfs_*, sese, hairp, mark_nodes, get_parent, etc.).
//   - Wrapper garante por construção: resultado 100% idêntico ao Billi.
//
// Como funciona:
//   1. Serializa GfaCsrTopology de volta para GFA em /tmp
//   2. Executa: billi decompose -i /tmp/<file>.gfa
//   3. Parseia linhas BB (bubbles) e HP (hairpins) do output
//   4. Retorna std::vector<SnarlRecord> compatível com nossa API
//
// Pré-requisito: variável de ambiente GGQL_BILLI_PATH apontando para
// o binário Billi compilado, ou /tmp/billi/billi como fallback.

#include "ggql/storage/snarl_decomposition.hpp"
#include "ggql/storage/topology.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace ggql::snarl_decomposition {

namespace billi_wrapper {

// Serializa GfaCsrTopology para GFA mínimo (S+L only) num arquivo temporário.
// Faz cross-correlation E_fwd × E_bwd para reconstruir orientações o1+o2
// (E_fwd guarda só o2, E_bwd guarda só o1; pareamos via (u,v) key).
static std::string serialize_topo_to_gfa(const GfaCsrTopology& topo) {
    namespace fs = std::filesystem;
    const auto t = std::chrono::system_clock::now().time_since_epoch().count();
    const std::string path =
        (fs::temp_directory_path() / ("ggql_billi_"
            + std::to_string(static_cast<unsigned long>(::getpid()))
            + "_" + std::to_string(static_cast<unsigned long long>(t))
            + ".gfa")).string();
    std::ofstream out(path);
    if (!out) { return {}; }

    const uint64_t N = topo.num_nodes();

    // Reconstroi LinkRecords via cross-correlation
    using Pair = std::pair<uint64_t, uint64_t>;
    struct PairHash {
        size_t operator()(const Pair& p) const noexcept {
            return std::hash<uint64_t>()(p.first) * 1315423911u
                 ^ std::hash<uint64_t>()(p.second);
        }
    };
    std::unordered_map<Pair, std::vector<char>, PairHash> fwd_orients; // (u,v) → list of o2
    std::unordered_map<Pair, std::vector<char>, PairHash> bwd_orients; // (u,v) → list of o1
    for (uint64_t u = 0; u < N; ++u) {
        for (uint64_t enc : topo.out_neighbors(u)) {
            const auto e = GfaCsrTopology::decode(enc);
            fwd_orients[{u, e.node_id}].push_back(e.orient);
        }
        for (uint64_t enc : topo.in_neighbors(u)) {
            const auto e = GfaCsrTopology::decode(enc);
            bwd_orients[{e.node_id, u}].push_back(e.orient);
        }
    }

    // S-lines
    for (uint64_t v = 0; v < N; ++v) {
        out << "S\tn" << v << "\t*\tLN:i:100\n";
    }
    // L-lines reconstruídas: para cada par (u, v), itera as orientações
    // pareadas. Pode duplicar quando há multi-edge — comportamento OK pois
    // Billi processa multi-edges corretamente.
    for (auto& [key, o2s] : fwd_orients) {
        auto it = bwd_orients.find(key);
        if (it == bwd_orients.end()) { continue; }
        const auto& o1s = it->second;
        const size_t n = std::min(o2s.size(), o1s.size());
        for (size_t i = 0; i < n; ++i) {
            out << "L\tn" << key.first << "\t" << o1s[i]
                << "\tn" << key.second << "\t" << o2s[i] << "\t0M\n";
        }
    }
    out.close();
    return path;
}

// Localiza o binário do Billi. Procura GGQL_BILLI_PATH primeiro, depois
// /tmp/billi/billi como fallback.
static std::string find_billi_binary() {
    if (const char* env = std::getenv("GGQL_BILLI_PATH"); env && *env) {
        if (std::filesystem::exists(env)) { return env; }
    }
    constexpr const char* fallback = "/tmp/billi/billi";
    if (std::filesystem::exists(fallback)) { return fallback; }
    return {};
}

// Executa um comando shell e captura stdout.
static std::string run_command(const std::string& cmd) {
    std::array<char, 4096> buffer{};
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) { return {}; }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

// Parseia linha do tipo "BB\t<bbID>\t<parID>\t<side1>\t<side2>\t<#alleles>"
// ou "HP\t<bbID>\t<side1>\t<side2>\t<#alleles>". Sides têm prefixo > ou <
// indicando orientação (e.g. ">A" = forward, "<A" = reverse).
//
// Retorna o ID numérico do nó extraído (sem o prefixo de orientação). O nome
// do nó está no formato "n<id>" conforme nossa serialização.
static int64_t parse_side(const std::string& s) {
    if (s.empty()) { return -1; }
    const size_t start = (s[0] == '>' || s[0] == '<') ? 1 : 0;
    if (start >= s.size()) { return -1; }
    if (s[start] == 'n') {
        // formato "n123"
        try { return std::stoll(s.substr(start + 1)); }
        catch (...) { return -1; }
    }
    // Outros formatos (ex: "A", "s12") — tentar extrair dígitos
    try { return std::stoll(s.substr(start)); }
    catch (...) { return -1; }
}

} // namespace billi_wrapper

std::vector<SnarlRecord> find_all_snarls_billi(const GfaCsrTopology& topo,
                                                int /*num_threads*/) {
    using namespace billi_wrapper;

    const std::string billi_bin = find_billi_binary();
    if (billi_bin.empty()) {
        std::fprintf(stderr,
                     "[ggql:billi_wrapper] Binário Billi não encontrado.\n"
                     "  Defina GGQL_BILLI_PATH ou compile em /tmp/billi/billi.\n");
        return {};
    }

    const std::string gfa_path = serialize_topo_to_gfa(topo);
    if (gfa_path.empty()) {
        std::fprintf(stderr, "[ggql:billi_wrapper] Falha ao serializar topology\n");
        return {};
    }

    const std::string cmd = billi_bin + " decompose -i " + gfa_path + " 2>/dev/null";
    const std::string out = run_command(cmd);

    if (std::getenv("GGQL_DEBUG_BILLI")) {
        std::fprintf(stderr, "[debug billi] gfa=%s\n", gfa_path.c_str());
        std::fprintf(stderr, "[debug billi] cmd=%s\n", cmd.c_str());
        std::fprintf(stderr, "[debug billi] output:\n%s\n", out.c_str());
    }

    // Cleanup do GFA temporário (a menos que debug)
    if (!std::getenv("GGQL_DEBUG_BILLI")) {
        std::filesystem::remove(gfa_path);
    }

    std::vector<SnarlRecord> snarls;
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) { continue; }
        if (line.compare(0, 3, "BB\t") != 0 && line.compare(0, 3, "HP\t") != 0) {
            continue;
        }
        const bool is_hp = (line[0] == 'H');

        // Tokenize por tabs
        std::vector<std::string> toks;
        size_t pos = 0;
        while (pos < line.size()) {
            const size_t end = line.find('\t', pos);
            if (end == std::string::npos) {
                toks.push_back(line.substr(pos));
                break;
            }
            toks.push_back(line.substr(pos, end - pos));
            pos = end + 1;
        }

        // BB: BB bbID parID side1 side2 #alleles
        // HP: HP bbID side1 side2 #alleles
        const size_t side1_idx = is_hp ? 2 : 3;
        const size_t side2_idx = is_hp ? 3 : 4;
        if (toks.size() <= side2_idx) { continue; }

        const int64_t s1 = parse_side(toks[side1_idx]);
        const int64_t s2 = parse_side(toks[side2_idx]);
        if (s1 < 0 || s2 < 0) { continue; }
        // Para BB (panbubble), s1 == s2 seria unary — filtra.
        // Para HP (hairpin), s1 == s2 é COMUM e válido (mesmo node, sides
        // opostos): >nX <nX. NÃO filtra.
        if (!is_hp && s1 == s2) { continue; }

        SnarlRecord rec{};
        rec.source = static_cast<uint64_t>(std::min(s1, s2));
        rec.sink   = static_cast<uint64_t>(std::max(s1, s2));
        rec.branch_count = 2;
        rec.internal_node_count = 0;
        rec.has_cycles = is_hp ? 1 : 0; // hairpins têm ciclos por definição
        snarls.push_back(rec);
    }

    return snarls;
}

} // namespace ggql::snarl_decomposition
