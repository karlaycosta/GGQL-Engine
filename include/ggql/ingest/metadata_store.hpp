#pragma once

// GfaMetadataStore — armazena metadados de segmentos GFA em arrays carregáveis na RAM.
//
// Design:
//   - internal_id é o valor EXATO retornado por mphf_->lookup(), garantindo
//     o mapeamento 1:1 entre RAM (MPHF/SDSL) e Metadados em O(1).
//   - Sem o custo e lentidão de uma engine de B-Tree (SQLite).
//
// Índices secundários persistidos (gerados em flush_segments(), salvos em save()):
//   - length_index.bin: dois arrays uint32_t sorted por length. O(log N) para WHERE length OP X.
//   - depth_index.bin:  dois arrays (float + uint32_t) sorted por depth. O(log N) para WHERE depth OP X.
//                       Ausente quando nenhum nó tem dp:f: (ex.: CHM13). Caches antigos: fallback.
//
// Índice invertido name→id (lazy, não persistido):
//   - unordered_map<string_view, uint32_t> construído na primeira chamada a lookup_name().
//   - Nunca gravado em disco: string_views apontam para name_chars_ em RAM.
//   - Custo adiado: 0 ms no load(); ~2 ms (SM) ou ~42 ms (10W) na primeira query WHERE name=.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ggql {

// ── Registro por segmento (Usado transitoriamente) ───────────────────────
struct SegmentRecord {
    uint64_t internal_id = 0; // mphf_->lookup(original_id) — índice RAM
    std::string original_id;  // nome do nó no GFA (ex: "ctg123")
    uint32_t length = 0;      // tamanho da sequência em pares de base
    int32_t read_count = -1;  // tag RC:i (-1 = ausente)
    float depth = -1.f;       // tag DP:f (-1 = ausente)
    std::string_view sequence;// <-- NEW (toda a seq original)
};

// ── GfaMetadataStore ─────────────────────────────────────────────────────
class GfaMetadataStore {
  public:
    GfaMetadataStore() = default;
    ~GfaMetadataStore() = default;

    // Não copiável
    GfaMetadataStore(const GfaMetadataStore&) = delete;
    GfaMetadataStore& operator=(const GfaMetadataStore&) = delete;

    // Movível
    GfaMetadataStore(GfaMetadataStore&&) = default;
    GfaMetadataStore& operator=(GfaMetadataStore&&) = default;

    void init(size_t num_nodes) {
        num_nodes_ = num_nodes;
        lengths_.assign(num_nodes, 0);
        read_counts_.assign(num_nodes, -1);
        depths_.assign(num_nodes, -1.f);
        name_offsets_.assign(num_nodes + 1, 0);
        seq_offsets_.assign(num_nodes + 1, 0);
    }

    // Preenche os arrays de Struct-of-Arrays usando os records extraídos na Passada 2
    void flush_segments(std::span<const SegmentRecord> records) {
        if (records.empty()) {
            return;
        }

        size_t total_string_size = 0;
        size_t total_seq_size = 0;
        for (const auto& rec : records) {
            total_string_size += rec.original_id.size();
            total_seq_size += rec.sequence.size() + 1; // +1 para o sentinel '$'
        }

        name_chars_.reserve(total_string_size);
        seq_chars_.reserve(total_seq_size);

        // Organiza as strings na sua ordem de internal_id final
        std::vector<std::string_view> temp_names(num_nodes_);
        std::vector<std::string_view> temp_seqs(num_nodes_);
        for (const auto& rec : records) {
            temp_names[rec.internal_id] = rec.original_id;
            temp_seqs[rec.internal_id] = rec.sequence;
            lengths_[rec.internal_id] = rec.length;
            read_counts_[rec.internal_id] = rec.read_count;
            depths_[rec.internal_id] = rec.depth;
        }

        uint64_t current_name_offset = 0;
        uint64_t current_seq_offset = 0;
        for (size_t i = 0; i < num_nodes_; ++i) {
            name_offsets_[i] = current_name_offset;
            current_name_offset += temp_names[i].size();
            name_chars_.insert(name_chars_.end(), temp_names[i].begin(), temp_names[i].end());

            seq_offsets_[i] = current_seq_offset;
            current_seq_offset += temp_seqs[i].size() + 1;
            seq_chars_.insert(seq_chars_.end(), temp_seqs[i].begin(), temp_seqs[i].end());
            seq_chars_.push_back('$'); // delimitador para previnir falsos cruzamentos de k-mers
        }
        name_offsets_[num_nodes_] = current_name_offset;
        seq_offsets_[num_nodes_] = current_seq_offset;

        build_length_index();
        build_depth_index();
    }

    // ── Índice secundário de comprimento ─────────────────────────────────────

    // Constrói dois arrays paralelos ordenados por length.
    // Chamado automaticamente por flush_segments(); pode ser chamado
    // manualmente após load() se length_index.bin não existir no cache.
    void build_length_index() {
        const size_t n = num_nodes_;
        length_sorted_ids_.resize(n);
        std::iota(length_sorted_ids_.begin(), length_sorted_ids_.end(), 0u);
        std::sort(length_sorted_ids_.begin(), length_sorted_ids_.end(),
                  [this](uint32_t a, uint32_t b) noexcept {
                      return lengths_[a] < lengths_[b];
                  });
        length_sorted_vals_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            length_sorted_vals_[i] = lengths_[length_sorted_ids_[i]];
        }
    }

    [[nodiscard]] bool has_length_index() const noexcept {
        return !length_sorted_ids_.empty();
    }

    // Retorna span de IDs cujo length está em [lo, hi] (ambos inclusive).
    // IDs retornados estão em ordem crescente de comprimento.
    // Requer has_length_index() == true.
    [[nodiscard]] std::span<const uint32_t>
    ids_in_length_range(uint32_t lo, uint32_t hi) const noexcept {
        const auto beg = std::lower_bound(length_sorted_vals_.begin(),
                                          length_sorted_vals_.end(), lo);
        const auto end = std::upper_bound(beg, length_sorted_vals_.end(), hi);
        const size_t off = static_cast<size_t>(beg - length_sorted_vals_.begin());
        const size_t cnt = static_cast<size_t>(end - beg);
        return std::span<const uint32_t>(length_sorted_ids_.data() + off, cnt);
    }

    // ── Índice secundário de depth ────────────────────────────────────────────

    // Constrói dois arrays paralelos ordenados por depth.
    // Exclui nós sem depth (depth < 0) — NULL não satisfaz nenhum predicado numérico.
    void build_depth_index() {
        depth_sorted_ids_.clear();
        depth_sorted_ids_.reserve(num_nodes_);
        for (size_t i = 0; i < num_nodes_; ++i) {
            if (depths_[i] >= 0.f) { depth_sorted_ids_.push_back(static_cast<uint32_t>(i)); }
        }
        std::sort(depth_sorted_ids_.begin(), depth_sorted_ids_.end(),
                  [this](uint32_t a, uint32_t b) noexcept { return depths_[a] < depths_[b]; });
        depth_sorted_vals_.resize(depth_sorted_ids_.size());
        for (size_t i = 0; i < depth_sorted_ids_.size(); ++i) {
            depth_sorted_vals_[i] = depths_[depth_sorted_ids_[i]];
        }
    }

    // Retorna false quando nenhum nó possui depth (ex.: CHM13 sem dp:f:).
    [[nodiscard]] bool has_depth_index() const noexcept {
        return !depth_sorted_vals_.empty();
    }

    // Retorna span de IDs cujo depth está em [lo, hi] (ambos inclusive).
    // Requer has_depth_index() == true.
    [[nodiscard]] std::span<const uint32_t>
    ids_in_depth_range(float lo, float hi) const noexcept {
        const auto beg = std::lower_bound(depth_sorted_vals_.begin(),
                                          depth_sorted_vals_.end(), lo);
        const auto end = std::upper_bound(beg, depth_sorted_vals_.end(), hi);
        const size_t off = static_cast<size_t>(beg - depth_sorted_vals_.begin());
        const size_t cnt = static_cast<size_t>(end - beg);
        return std::span<const uint32_t>(depth_sorted_ids_.data() + off, cnt);
    }

    // ── Índice invertido name → id (lazy) ────────────────────────────────────
    //
    // Construído na primeira chamada a lookup_name() via std::call_once.
    // Custo: O(N) — ~2 ms (SM 131k) ou ~42 ms (10W 3,2M) na primeira query.
    // Zero overhead no load() do cache.

    [[nodiscard]] bool has_name_index() const noexcept { return num_nodes_ > 0; }

    // Retorna o internal_id para o nome dado, ou nullopt se não encontrado.
    // Constrói o índice na primeira chamada (thread-safe via call_once).
    [[nodiscard]] std::optional<uint32_t>
    lookup_name(std::string_view name) const noexcept {
        std::call_once(name_index_once_, [this]() {
            name_to_id_.reserve(num_nodes_);
            for (size_t i = 0; i < num_nodes_; ++i) {
                name_to_id_.emplace(original_id(i), static_cast<uint32_t>(i));
            }
        });
        auto it = name_to_id_.find(name);
        if (it == name_to_id_.end()) { return std::nullopt; }
        return it->second;
    }

    void save(const std::string& dir) const {
        namespace fs = std::filesystem;
        fs::create_directories(dir);

        {
            std::ofstream out(dir + "/metadata.bin", std::ios::binary);
            if (out) {
                uint64_t nn = num_nodes_;
                out.write(reinterpret_cast<const char*>(&nn), sizeof(nn));
                out.write(reinterpret_cast<const char*>(lengths_.data()),
                          static_cast<std::streamsize>(num_nodes_ * sizeof(uint32_t)));
                out.write(reinterpret_cast<const char*>(read_counts_.data()),
                          static_cast<std::streamsize>(num_nodes_ * sizeof(int32_t)));
                out.write(reinterpret_cast<const char*>(depths_.data()),
                          static_cast<std::streamsize>(num_nodes_ * sizeof(float)));
            }
        }
        {
            std::ofstream out(dir + "/metadata_strings.bin", std::ios::binary);
            if (out) {
                out.write(reinterpret_cast<const char*>(name_offsets_.data()),
                          static_cast<std::streamsize>((num_nodes_ + 1) * sizeof(uint64_t)));
                uint64_t char_size = name_chars_.size();
                out.write(reinterpret_cast<const char*>(&char_size), sizeof(char_size));
                out.write(name_chars_.data(), static_cast<std::streamsize>(char_size));
            }
        }
        {
            std::ofstream out(dir + "/metadata_seqs.bin", std::ios::binary);
            if (out) {
                out.write(reinterpret_cast<const char*>(seq_offsets_.data()),
                          static_cast<std::streamsize>((num_nodes_ + 1) * sizeof(uint64_t)));
                uint64_t char_size = seq_chars_.size();
                out.write(reinterpret_cast<const char*>(&char_size), sizeof(char_size));
                out.write(seq_chars_.data(), static_cast<std::streamsize>(char_size));
            }
        }
        // Índices secundários — ausentes em caches antigos; load() faz fallback gracioso
        if (has_length_index()) {
            std::ofstream out(dir + "/length_index.bin", std::ios::binary);
            if (out) {
                const uint64_t n = length_sorted_vals_.size();
                out.write(reinterpret_cast<const char*>(&n), sizeof(n));
                out.write(reinterpret_cast<const char*>(length_sorted_vals_.data()),
                          static_cast<std::streamsize>(n * sizeof(uint32_t)));
                out.write(reinterpret_cast<const char*>(length_sorted_ids_.data()),
                          static_cast<std::streamsize>(n * sizeof(uint32_t)));
            }
        }
        if (has_depth_index()) {
            std::ofstream out(dir + "/depth_index.bin", std::ios::binary);
            if (out) {
                const uint64_t n = depth_sorted_vals_.size();
                out.write(reinterpret_cast<const char*>(&n), sizeof(n));
                out.write(reinterpret_cast<const char*>(depth_sorted_vals_.data()),
                          static_cast<std::streamsize>(n * sizeof(float)));
                out.write(reinterpret_cast<const char*>(depth_sorted_ids_.data()),
                          static_cast<std::streamsize>(n * sizeof(uint32_t)));
            }
        }
        // name_to_id_ não é persistido — string_views apontam para name_chars_ em RAM.
    }

    void load(const std::string& dir) {
        {
            std::ifstream in(dir + "/metadata.bin", std::ios::binary);
            if (in) {
                uint64_t nn = 0;
                in.read(reinterpret_cast<char*>(&nn), sizeof(nn));
                num_nodes_ = nn;
                lengths_.resize(nn);
                read_counts_.resize(nn);
                depths_.resize(nn);

                in.read(reinterpret_cast<char*>(lengths_.data()),
                        static_cast<std::streamsize>(nn * sizeof(uint32_t)));
                in.read(reinterpret_cast<char*>(read_counts_.data()),
                        static_cast<std::streamsize>(nn * sizeof(int32_t)));
                in.read(reinterpret_cast<char*>(depths_.data()),
                        static_cast<std::streamsize>(nn * sizeof(float)));
            } else {
                throw std::runtime_error("Não foi possível carregar " + dir + "/metadata.bin");
            }
        }
        {
            std::ifstream in(dir + "/metadata_strings.bin", std::ios::binary);
            if (in) {
                name_offsets_.resize(num_nodes_ + 1);
                in.read(reinterpret_cast<char*>(name_offsets_.data()),
                        static_cast<std::streamsize>((num_nodes_ + 1) * sizeof(uint64_t)));

                uint64_t char_size = 0;
                in.read(reinterpret_cast<char*>(&char_size), sizeof(char_size));
                name_chars_.resize(char_size);
                in.read(name_chars_.data(), static_cast<std::streamsize>(char_size));
            } else {
                throw std::runtime_error("Não foi possível carregar " + dir +
                                         "/metadata_strings.bin");
            }
        }
        {
            std::ifstream in(dir + "/metadata_seqs.bin", std::ios::binary);
            if (in) {
                seq_offsets_.resize(num_nodes_ + 1);
                in.read(reinterpret_cast<char*>(seq_offsets_.data()),
                        static_cast<std::streamsize>((num_nodes_ + 1) * sizeof(uint64_t)));

                uint64_t char_size = 0;
                in.read(reinterpret_cast<char*>(&char_size), sizeof(char_size));
                seq_chars_.resize(char_size);
                in.read(seq_chars_.data(), static_cast<std::streamsize>(char_size));
            }
            // Ignore missing seqs — compatible with older cache versions
        }
        {
            std::ifstream in(dir + "/length_index.bin", std::ios::binary);
            if (in) {
                uint64_t n = 0;
                in.read(reinterpret_cast<char*>(&n), sizeof(n));
                length_sorted_vals_.resize(n);
                length_sorted_ids_.resize(n);
                in.read(reinterpret_cast<char*>(length_sorted_vals_.data()),
                        static_cast<std::streamsize>(n * sizeof(uint32_t)));
                in.read(reinterpret_cast<char*>(length_sorted_ids_.data()),
                        static_cast<std::streamsize>(n * sizeof(uint32_t)));
            } else {
                build_length_index();
            }
        }
        {
            std::ifstream in(dir + "/depth_index.bin", std::ios::binary);
            if (in) {
                uint64_t n = 0;
                in.read(reinterpret_cast<char*>(&n), sizeof(n));
                depth_sorted_vals_.resize(n);
                depth_sorted_ids_.resize(n);
                in.read(reinterpret_cast<char*>(depth_sorted_vals_.data()),
                        static_cast<std::streamsize>(n * sizeof(float)));
                in.read(reinterpret_cast<char*>(depth_sorted_ids_.data()),
                        static_cast<std::streamsize>(n * sizeof(uint32_t)));
            } else {
                build_depth_index();
            }
        }
    }

    // ── Accessors O(1) ────────────────────────────────────────────────────────
    size_t size() const noexcept { return num_nodes_; }
    uint32_t length(uint64_t id) const { return lengths_[id]; }
    std::span<const uint32_t> lengths_span() const noexcept { return lengths_; }
    int32_t read_count(uint64_t id) const { return read_counts_[id]; }
    float depth(uint64_t id) const { return depths_[id]; }

    std::string_view original_id(uint64_t id) const {
        uint64_t start = name_offsets_[id];
        uint64_t end = name_offsets_[id + 1];
        return std::string_view(&name_chars_[start], end - start);
    }

    std::string_view sequence(uint64_t id) const {
        uint64_t start = seq_offsets_[id];
        uint64_t end = seq_offsets_[id + 1];
        if (end > start) {
            return std::string_view(&seq_chars_[start], end - start - 1); // exclude '$'
        }
        return {};
    }

    const std::vector<char>& all_sequences() const { return seq_chars_; }
    std::vector<char> take_sequences() { return std::move(seq_chars_); }
    const std::vector<uint64_t>& all_seq_offsets() const { return seq_offsets_; }

    // Mapeia um offset (posição em seq_chars_) para o internal_id do segmento
    // que o contém. Retorna nullopt se o offset estiver antes do primeiro
    // segmento ou se o store estiver vazio.
    // O(log N) via busca binária em seq_offsets_ (array contíguo ordenado).
    [[nodiscard]] std::optional<uint64_t>
    segment_id_from_offset(uint64_t offset) const noexcept {
        if (seq_offsets_.empty()) {
            return std::nullopt;
        }
        auto it = std::upper_bound(seq_offsets_.begin(), seq_offsets_.end(),
                                   offset);
        if (it == seq_offsets_.begin()) {
            return std::nullopt;
        }
        return static_cast<uint64_t>(
            std::distance(seq_offsets_.begin(), it) - 1);
    }

  private:
    size_t num_nodes_ = 0;
    std::vector<uint32_t> lengths_;
    std::vector<int32_t>  read_counts_;
    std::vector<float>    depths_;
    std::vector<uint64_t> name_offsets_; // tamanho: num_nodes_ + 1
    std::vector<char>     name_chars_;
    std::vector<uint64_t> seq_offsets_;  // tamanho: num_nodes_ + 1
    std::vector<char>     seq_chars_;

    // Índice secundário de comprimento — arrays paralelos ordenados por length
    std::vector<uint32_t> length_sorted_vals_;
    std::vector<uint32_t> length_sorted_ids_;

    // Índice secundário de depth — apenas nós com depth >= 0
    std::vector<float>    depth_sorted_vals_;
    std::vector<uint32_t> depth_sorted_ids_;

    // Índice invertido name → id (lazy, não persistido; string_views apontam para name_chars_)
    mutable std::once_flag name_index_once_;
    mutable std::unordered_map<std::string_view, uint32_t> name_to_id_;
};

} // namespace ggql
