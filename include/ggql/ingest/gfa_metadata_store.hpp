#pragma once

// GfaMetadataStore — armazena metadados de segmentos GFA em arrays carregáveis na RAM.
//
// Design:
//   - internal_id é o valor EXATO retornado por mphf_->lookup(), garantindo
//     o mapeamento 1:1 entre RAM (MPHF/SDSL) e Metadados em O(1).
//   - Sem o custo e lentidão de uma engine de B-Tree (SQLite).

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
            } else {
                // Ignore missing seqs to stay compatible with previous cache versions
            }
        }
    }

    // ── Accessors O(1) ────────────────────────────────────────────────────────
    size_t size() const noexcept { return num_nodes_; }
    uint32_t length(uint64_t id) const { return lengths_[id]; }
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
    std::vector<int32_t> read_counts_;
    std::vector<float> depths_;
    std::vector<uint64_t> name_offsets_; // tamanho: num_nodes_ + 1
    std::vector<char> name_chars_;
    std::vector<uint64_t> seq_offsets_; // tamanho: num_nodes_ + 1
    std::vector<char> seq_chars_;
};

} // namespace ggql
