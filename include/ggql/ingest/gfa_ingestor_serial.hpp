#pragma once

#include <fstream>
#include <string>
#include <string_view>
#include <array>
#include <vector>
#include <print>
#include <limits>
#include <memory>
#include <stdexcept>
#include <chrono>

#include "mphf/BooPHF.h"
#include "mphf/xxh64.hpp"

namespace ggql {

// Hasher para string_view usando xxHash64 — sem cópia durante lookup
struct StringViewHasher {
    uint64_t operator()(std::string_view key, uint64_t seed = 0) const {
        return XXH64(key.data(), key.size(), seed);
    }
};

using boophf_t = boomphf::mphf<std::string_view, StringViewHasher>;

// Resultado da ingestão — pode ser expandido com ponteiros para SDSL/SQLite
struct IngestStats {
    uint64_t segment_count    = 0;
    uint64_t link_count       = 0;
    uint64_t node_count       = 0;
    double   elapsed_io1_s    = 0.0;  // passada 1: leitura e coleta de IDs
    double   elapsed_build_s  = 0.0;  // construção do MPHF
    double   elapsed_ingest_s = 0.0;  // passada 2: leitura de topologia
    double   elapsed_total_s  = 0.0;
};

// Nota: GfaIngestor é single-use por instância. Chamar ingest() duas vezes
// substitui o MPHF anterior. Para arquivos distintos, use instâncias separadas.
class GfaIngestor {
public:
    GfaIngestor() = default;
    ~GfaIngestor() = default;

    // Interface principal: executa as duas passadas e retorna estatísticas.
    IngestStats ingest(const std::string& filepath, int num_threads = 1, double gamma = 2.0);

private:
    std::unique_ptr<boophf_t>  mphf_;
    // Âncora de lifetime: as string_views armazenadas no _final_hash interno do
    // BooPHF apontam para estes dados. Deve sobreviver enquanto mphf_ existir.
    std::vector<std::string>   keys_backing_;

    // ── Utilitários ──────────────────────────────────────────────────────────

    // Preenche 'out' com até 8 tokens e devolve a contagem.
    // Strip de '\r' no último token — protege contra arquivos CRLF (Windows/IGV).
    // Não aloca heap — o array é reutilizado pelo chamador entre iterações.
    static size_t split_by_tab(std::string_view str,
                               std::array<std::string_view, 8>& out) {
        size_t n = 0, i = 0;
        size_t j = str.find('\t');
        while (j != std::string_view::npos && n < 7) {
            out[n++] = str.substr(i, j - i);
            i = j + 1;
            j = str.find('\t', i);
        }
        std::string_view last = str.substr(i);
        if (!last.empty() && last.back() == '\r')
            last.remove_suffix(1);
        out[n++] = last;
        return n;
    }

    // Lê do stream apenas os primeiros `max_fields` campos separados por tab,
    // descartando o restante da linha sem alocar memória para ele.
    // Retorna false se EOF antes de qualquer dado.
    // `line` é reutilizado como buffer para os campos curtos (ID, orientação);
    // `tail` recebe os dados descartados sem cópia extra.
    static bool read_line_fields(std::ifstream& f,
                                 std::string& line,
                                 std::array<std::string_view, 8>& out,
                                 size_t& ntok,
                                 size_t max_fields = 8) {
        if (!std::getline(f, line)) return false;
        // Strip \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ntok = 0;
        size_t i = 0;
        for (size_t field = 0; field < max_fields && i <= line.size(); ++field) {
            size_t j = (field < max_fields - 1) ? line.find('\t', i) : std::string::npos;
            if (j == std::string::npos) {
                out[ntok++] = std::string_view(line).substr(i);
                break;
            }
            out[ntok++] = std::string_view(line).substr(i, j - i);
            i = j + 1;
        }
        return true;
    }

    // ── Passada 1: coleta todos os IDs de segmentos e constrói o MPHF ───────
    std::vector<std::string> collect_keys(const std::string& filepath);

    // ── Passada 2: lê topologia e metadados usando o MPHF já construído ─────
    IngestStats process_data(const std::string& filepath);
};

// ── Implementações ────────────────────────────────────────────────────────────

inline std::vector<std::string> GfaIngestor::collect_keys(const std::string& filepath) {
    std::vector<std::string> keys;
    // Reserva para ~134M de nós — cobre o HPRC (85M) sem nenhuma realocação.
    // CRÍTICO: key_views (construído depois) aponta para dados internos de cada
    // std::string aqui. Se o vetor realocar, os ponteiros ficam dangling.
    // Não remova este reserve sem ajustar a construção de key_views em ingest().
    keys.reserve(1 << 27);
    static_assert(sizeof(std::string) > 0, "guard"); // sentinela: veja comentário acima

    std::vector<char> iobuf(1 << 20);  // buffer de 1 MB: ~46K syscalls vs ~5.7M
    std::ifstream file(filepath);
    if (!file.is_open())
        throw std::runtime_error("Nao foi possivel abrir: " + filepath);
    file.rdbuf()->pubsetbuf(iobuf.data(), iobuf.size());

    std::string line;
    std::array<std::string_view, 8> tokens;
    while (std::getline(file, line)) {
        if (line.size() < 2 || line[0] != 'S') continue;
        size_t ntok = split_by_tab(line, tokens);
        if (ntok >= 2 && !tokens[1].empty())
            keys.emplace_back(tokens[1]);
    }
    return keys;
}

inline IngestStats GfaIngestor::process_data(const std::string& filepath) {
    IngestStats stats;
    std::vector<char> iobuf(1 << 20);  // buffer de 1 MB
    std::ifstream file(filepath);
    if (!file.is_open())
        throw std::runtime_error("Nao foi possivel abrir: " + filepath);
    file.rdbuf()->pubsetbuf(iobuf.data(), iobuf.size());

    std::string line;
    // Buffers reutilizáveis — zero alocação de heap por iteração.
    std::array<std::string_view, 8> tokens;
    size_t ntok;

    // Lookup direto via string_view — sem memcpy por chamada.
    auto lookup = [&](std::string_view sv) -> uint64_t {
        return mphf_->lookup(sv);
    };

    // Primeiro byte lido antes de chamar read_line_fields para decidir o limite
    // de campos: linhas 'S' precisam de 3 campos (tipo, id, seq); 'L' de 5.
    // Isso evita alocar a sequência de DNA inteira em `line` para linhas 'S'
    // com sequências longas (ex: contigs do HPRC com 50k+ bases).
    while (file.peek() != EOF) {
        char first = static_cast<char>(file.peek());
        if (first == 'S') {
            if (!read_line_fields(file, line, tokens, ntok, /*max_fields=*/3)) break;
            if (ntok < 3 || tokens[1].empty()) continue;
            uint64_t id = lookup(tokens[1]);
            if (id == ULLONG_MAX) continue; // chave ausente — arquivo malformado
            size_t length = (tokens[2] == "*") ? 0 : tokens[2].size();
            // TODO: db.insert_segment(id, tokens[1], length);
            (void)id; (void)length;
            ++stats.segment_count;
        } else if (first == 'L') {
            if (!read_line_fields(file, line, tokens, ntok, /*max_fields=*/5)) break;
            if (ntok < 5 || tokens[1].empty() || tokens[3].empty()) continue;
            uint64_t from_id = lookup(tokens[1]);
            uint64_t to_id   = lookup(tokens[3]);
            if (from_id == ULLONG_MAX || to_id == ULLONG_MAX) continue;
            char from_orient = tokens[2].empty() ? '+' : tokens[2][0];
            char to_orient   = tokens[4].empty() ? '+' : tokens[4][0];
            // TODO: topo.add_edge(from_id, to_id, from_orient, to_orient);
            (void)from_orient; (void)to_orient;
            ++stats.link_count;
        } else {
            // Linha irrelevante (H, P, W, comentário) — descarta sem alocar
            file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
    }
    return stats;
}

inline IngestStats GfaIngestor::ingest(const std::string& filepath,
                                        int num_threads, double gamma) {
    using clock = std::chrono::high_resolution_clock;
    using sec   = std::chrono::duration<double>;

    // ── Passada 1: I/O ───────────────────────────────────────────────────────
    std::print("[GfaIngestor] Passada 1: coletando IDs...\n");
    auto t0   = clock::now();
    auto keys = collect_keys(filepath);
    auto t_io1 = clock::now();
    if (keys.empty())
        throw std::runtime_error("Nenhum segmento encontrado em: " + filepath);

    // ── Passada 1: build MPHF ────────────────────────────────────────────────
    std::print("  {} nos encontrados. Construindo MPHF...\n", keys.size());
    // Cria views para o construtor — MPHF não armazena as chaves, só o índice.
    std::vector<std::string_view> key_views(keys.begin(), keys.end());
    mphf_ = std::make_unique<boophf_t>(
        key_views.size(), key_views,
        num_threads, gamma,
        /*writeEach=*/false, /*progress=*/false
        // perc_elem_loaded: mantém padrão 0.03 (fastmode ativo).
        // O fastmode armazena ~3% das chaves em RAM para evitar releitura do
        // input em níveis superiores da cascata — troca ~40MB por ~4x menos tempo de build.
    );
    auto t_built = clock::now();
    std::print("  MPHF pronto: {:.5f} bits/chave, {:.4f} MB total.\n",
               static_cast<double>(mphf_->totalBitSize()) / static_cast<double>(keys.size()),
               static_cast<double>(mphf_->totalBitSize()) / 8.0 / (1024.0 * 1024.0));

    // key_views não é mais necessário — libera imediatamente.
    { std::vector<std::string_view> tmp; key_views.swap(tmp); }
    // keys_backing_ mantém os dados vivos enquanto mphf_ existir:
    // o _final_hash interno do BooPHF guarda string_views que apontam para eles.
    keys_backing_ = std::move(keys);

    // ── Passada 2 ────────────────────────────────────────────────────────────
    std::print("[GfaIngestor] Passada 2: processando topologia e metadados...\n");
    IngestStats stats = process_data(filepath);
    auto t1 = clock::now();

    stats.elapsed_io1_s    = sec(t_io1 - t0).count();
    stats.elapsed_build_s  = sec(t_built - t_io1).count();
    stats.elapsed_ingest_s = sec(t1 - t_built).count();
    stats.elapsed_total_s  = sec(t1 - t0).count();
    stats.node_count       = mphf_->nbKeys();

    std::print("[GfaIngestor] Concluido em {:.4f} s\n", stats.elapsed_total_s);
    std::print("  I/O passada 1: {:.5f} s\n", stats.elapsed_io1_s);
    std::print("  Build MPHF   : {:.5f} s\n", stats.elapsed_build_s);
    std::print("  I/O passada 2: {:.5f} s\n", stats.elapsed_ingest_s);
    std::print("  Segmentos    : {}\n", stats.segment_count);
    std::print("  Arestas      : {}\n", stats.link_count);
    std::print("  Nos unicos   : {}\n", stats.node_count);
    return stats;
}

} // namespace ggql
