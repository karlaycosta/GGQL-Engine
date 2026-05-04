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
//   - vector<pair<uint64_t,uint32_t>> sorted por hash, construído na 1ª chamada a lookup_name().
//   - Nunca gravado em disco. Lookup: lower_bound por hash + verificação string_view real
//     (resolve colisões raríssimas de hash64).
//   - Memória ~12 B/chave (vs ~50 B do antigo unordered_map: -78% no chm13_100w).
//   - Custo adiado: 0 ms no load(); ~330 ms (chm13_100w 4,3M) na primeira query WHERE name=.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ggql {

// ── Registro por segmento (usado transitoriamente entre Passada 2 e flush) ──
struct SegmentRecord {
    uint64_t internal_id = 0;   // mphf_->lookup(original_id) — índice RAM
    std::string original_id;    // nome do nó no GFA (ex: "ctg123")
    uint32_t length = 0;        // tamanho da sequência em pares de base
    int32_t read_count = -1;    // tag RC:i (-1 = ausente)
    float depth = -1.f;         // tag DP:f (-1 = ausente)
    std::string_view sequence;  // sequência completa (apontando para o mmap do GFA)
};

// ── MmapBuffer ───────────────────────────────────────────────────────────
// RAII para mmap read-only de arquivo inteiro. Usado para carregar chars
// de metadata_strings.bin/metadata_seqs.bin sem cópia para vector<char>.
//
// Move-only. munmap acontece no destrutor.
class MmapBuffer {
  public:
    MmapBuffer() = default;

    explicit MmapBuffer(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("mmap: falha ao abrir " + path);
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw std::runtime_error("mmap: fstat falhou em " + path);
        }
        len_ = static_cast<size_t>(st.st_size);
        if (len_ == 0) {
            ::close(fd);
            return;
        }
        void* addr = ::mmap(nullptr, len_, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (addr == MAP_FAILED) {
            len_ = 0;
            throw std::runtime_error("mmap: mapeamento falhou em " + path);
        }
        addr_ = addr;
    }

    ~MmapBuffer() noexcept {
        if (addr_ != nullptr) {
            ::munmap(addr_, len_);
        }
    }

    MmapBuffer(MmapBuffer&& o) noexcept
        : addr_(std::exchange(o.addr_, nullptr)),
          len_(std::exchange(o.len_, 0)) {}

    MmapBuffer& operator=(MmapBuffer&& o) noexcept {
        if (this != &o) {
            if (addr_ != nullptr) {
                ::munmap(addr_, len_);
            }
            addr_ = std::exchange(o.addr_, nullptr);
            len_ = std::exchange(o.len_, 0);
        }
        return *this;
    }

    MmapBuffer(const MmapBuffer&) = delete;
    MmapBuffer& operator=(const MmapBuffer&) = delete;

    [[nodiscard]] const char* data() const noexcept {
        return static_cast<const char*>(addr_);
    }
    [[nodiscard]] size_t size() const noexcept { return len_; }
    [[nodiscard]] bool empty() const noexcept { return len_ == 0; }

  private:
    void* addr_ = nullptr;
    size_t len_ = 0;
};

// ── GfaMetadataStore ─────────────────────────────────────────────────────
class GfaMetadataStore {
  public:
    GfaMetadataStore() = default;
    ~GfaMetadataStore() = default;

    // Não copiável nem movível: std::once_flag (name_index_once_) não é movível,
    // tornando os defaults implicitamente deletados. Tornar explícito evita
    // a armadilha do `= default` que vira `= delete` em silêncio. Callers
    // devem usar std::unique_ptr<GfaMetadataStore> (ver ingestor.cpp).
    GfaMetadataStore(const GfaMetadataStore&) = delete;
    GfaMetadataStore& operator=(const GfaMetadataStore&) = delete;
    GfaMetadataStore(GfaMetadataStore&&) = delete;
    GfaMetadataStore& operator=(GfaMetadataStore&&) = delete;

    void init(size_t num_nodes) {
        num_nodes_ = num_nodes;
        lengths_.assign(num_nodes, 0);
        read_counts_.assign(num_nodes, -1);
        depths_.assign(num_nodes, -1.f);
        name_offsets_.assign(num_nodes + 1, 0);
        seq_offsets_.assign(num_nodes + 1, 0);
        // B8: reseta estruturas auxiliares para que init() seja idempotente.
        // Sem isto, uma segunda chamada a init() deixaria os índices secundários
        // e o buffer de chars apontando para estado anterior.
        length_sorted_vals_.clear();
        length_sorted_ids_.clear();
        depth_sorted_vals_.clear();
        depth_sorted_ids_.clear();
        name_chars_.clear();
        seq_chars_.clear();
    }

    // Preenche os arrays de Struct-of-Arrays usando os records extraídos na Passada 2.
    //
    // P2+P6: pipeline single-pass paralelo:
    //   1. Loop paralelo em records → preenche lengths_/read_counts_/depths_ e
    //      grava em offsets_[id+1] o tamanho daquele segmento. Writes em
    //      índices únicos (1 por internal_id) — sem race.
    //   2. Prefix-sum sequencial: tamanhos → offsets absolutos.
    //   3. Aloca buffers raw (unique_ptr<char[]> via new char[N]) — não zera
    //      os ~334 MB do chm13_100w.
    //   4. Loop paralelo em records → memcpy chars na posição final.
    //      Writes em ranges não-sobrepostos por id.
    //
    // Pré-condição (B5): chamar no máximo uma vez por instância. std::once_flag
    // do name_index_ não pode ser resetado, então um 2º flush deixaria o
    // índice de nomes (se já construído) apontando para chars realocados.
    // O ingest atual respeita isso (ver ingestor.cpp).
    void flush_segments(std::span<const SegmentRecord> records) {
        if (records.empty()) {
            return;
        }

        // Garante offsets zerados (init() já faz, mas torna a função robusta a
        // chamadas isoladas). Os writes da pass 1 só preenchem [id+1].
        std::fill(name_offsets_.begin(), name_offsets_.end(), 0);
        std::fill(seq_offsets_.begin(), seq_offsets_.end(), 0);

        // 1. Pass paralela: metadados simples + tamanhos.
        const ptrdiff_t M = static_cast<ptrdiff_t>(records.size());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (ptrdiff_t i = 0; i < M; ++i) {
            const auto& rec = records[i];
            const uint64_t id = rec.internal_id;
            lengths_[id] = rec.length;
            read_counts_[id] = rec.read_count;
            depths_[id] = rec.depth;
            name_offsets_[id + 1] = rec.original_id.size();
            seq_offsets_[id + 1] = rec.sequence.size() + 1; // +1 para '$'
        }

        // 2. Prefix-sum: tamanhos → offsets absolutos.
        for (size_t i = 1; i <= num_nodes_; ++i) {
            name_offsets_[i] += name_offsets_[i - 1];
            seq_offsets_[i] += seq_offsets_[i - 1];
        }

        // 3. Aloca buffers raw sem zeragem (default-init de char[] em new é
        //    no-op para POD).
        const size_t total_name = name_offsets_[num_nodes_];
        const size_t total_seq = seq_offsets_[num_nodes_];
        char* name_buf = name_chars_.alloc_raw_uninit(total_name);
        char* seq_buf = seq_chars_.alloc_raw_uninit(total_seq);

        // 4. Pass paralela: memcpy chars. Cada (id) escreve em range único.
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (ptrdiff_t i = 0; i < M; ++i) {
            const auto& rec = records[i];
            const uint64_t id = rec.internal_id;
            const size_t name_off = name_offsets_[id];
            const size_t seq_off = seq_offsets_[id];
            std::memcpy(name_buf + name_off, rec.original_id.data(),
                        rec.original_id.size());
            std::memcpy(seq_buf + seq_off, rec.sequence.data(),
                        rec.sequence.size());
            // Sentinel '$' para evitar k-mers cruzarem segmentos no FM-Index.
            seq_buf[seq_off + rec.sequence.size()] = '$';
        }

        build_length_index();
        build_depth_index();
    }

    // ── Índice secundário de comprimento ─────────────────────────────────────

    // Constrói dois arrays paralelos ordenados por length.
    // Chamado automaticamente por flush_segments(); pode ser chamado
    // manualmente após load() se length_index.bin não existir no cache.
    //
    // P3: sort de pares (length, id) em vez de sort indireto via lambda.
    // O sort original fazia 2 loads em lengths_ por compare em índices arbitrários
    // (cache hostile). Agora cada compare é local nos pares — 2–4× speedup
    // esperado em N grande à custa de ~8N bytes temporários.
    void build_length_index() {
        const size_t n = num_nodes_;
        if (n == 0) {
            length_sorted_vals_.clear();
            length_sorted_ids_.clear();
            return;
        }
        std::vector<std::pair<uint32_t, uint32_t>> pairs(n);
        for (size_t i = 0; i < n; ++i) {
            pairs[i] = {lengths_[i], static_cast<uint32_t>(i)};
        }
        std::sort(pairs.begin(), pairs.end()); // por length, depois por id
        length_sorted_vals_.resize(n);
        length_sorted_ids_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            length_sorted_vals_[i] = pairs[i].first;
            length_sorted_ids_[i] = pairs[i].second;
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
    // O filtro >= 0.f também elimina NaN, mantendo strict weak ordering no sort.
    //
    // P3: mesma técnica de sort de pares de build_length_index().
    void build_depth_index() {
        std::vector<std::pair<float, uint32_t>> pairs;
        pairs.reserve(num_nodes_);
        for (size_t i = 0; i < num_nodes_; ++i) {
            if (depths_[i] >= 0.f) {
                pairs.emplace_back(depths_[i], static_cast<uint32_t>(i));
            }
        }
        std::sort(pairs.begin(), pairs.end());
        const size_t k = pairs.size();
        depth_sorted_vals_.resize(k);
        depth_sorted_ids_.resize(k);
        for (size_t i = 0; i < k; ++i) {
            depth_sorted_vals_[i] = pairs[i].first;
            depth_sorted_ids_[i] = pairs[i].second;
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
    // Estrutura: vector<pair<uint64_t hash, uint32_t id>> ordenado por hash.
    // Lookup: lower_bound em hash + linear probing para colisões (raríssimas
    // em hash64 com 4,3M chaves: prob ≈ 4,3M² / 2^64 ≈ 10⁻⁶) + verificação
    // string_view real para correção em caso de colisão.
    // Memória: 12 B/chave (vs 50+ B do unordered_map). Custo adiado para a 1ª
    // query WHERE name=.

    [[nodiscard]] bool has_name_index() const noexcept { return num_nodes_ > 0; }

    // Retorna o internal_id para o nome dado, ou nullopt se não encontrado.
    // Constrói o índice na primeira chamada (thread-safe via call_once).
    [[nodiscard]] std::optional<uint32_t>
    lookup_name(std::string_view name) const noexcept {
        std::call_once(name_index_once_, [this]() {
            name_index_.resize(num_nodes_);
            const std::hash<std::string_view> hasher;
            for (size_t i = 0; i < num_nodes_; ++i) {
                name_index_[i] = {hasher(original_id(i)),
                                  static_cast<uint32_t>(i)};
            }
            std::sort(name_index_.begin(), name_index_.end(),
                      [](const auto& a, const auto& b) noexcept {
                          return a.first < b.first;
                      });
        });
        const std::hash<std::string_view> hasher;
        const uint64_t h = hasher(name);
        auto it = std::lower_bound(
            name_index_.begin(), name_index_.end(), h,
            [](const std::pair<uint64_t, uint32_t>& p,
               uint64_t v) noexcept { return p.first < v; });
        for (; it != name_index_.end() && it->first == h; ++it) {
            if (original_id(it->second) == name) {
                return it->second;
            }
        }
        return std::nullopt;
    }

    void save(const std::string& dir) const {
        namespace fs = std::filesystem;
        fs::create_directories(dir);

        // Falhas de open ou write devem propagar — caches meio-gravados são
        // pior que ausência de cache (load fica em estado inconsistente).
        auto open_out = [](const std::string& p) {
            std::ofstream o(p, std::ios::binary);
            if (!o) {
                throw std::runtime_error("Falha ao abrir para escrita: " + p);
            }
            return o;
        };
        auto check_ok = [](std::ofstream& o, const std::string& p) {
            if (!o) {
                throw std::runtime_error("Falha ao escrever: " + p);
            }
        };

        {
            const auto path = dir + "/metadata.bin";
            auto out = open_out(path);
            uint64_t nn = num_nodes_;
            out.write(reinterpret_cast<const char*>(&nn), sizeof(nn));
            out.write(reinterpret_cast<const char*>(lengths_.data()),
                      static_cast<std::streamsize>(num_nodes_ * sizeof(uint32_t)));
            out.write(reinterpret_cast<const char*>(read_counts_.data()),
                      static_cast<std::streamsize>(num_nodes_ * sizeof(int32_t)));
            out.write(reinterpret_cast<const char*>(depths_.data()),
                      static_cast<std::streamsize>(num_nodes_ * sizeof(float)));
            check_ok(out, path);
        }
        {
            const auto path = dir + "/metadata_strings.bin";
            auto out = open_out(path);
            out.write(reinterpret_cast<const char*>(name_offsets_.data()),
                      static_cast<std::streamsize>((num_nodes_ + 1) * sizeof(uint64_t)));
            const uint64_t char_size = name_chars_.size();
            out.write(reinterpret_cast<const char*>(&char_size), sizeof(char_size));
            if (char_size > 0) {
                out.write(name_chars_.data(),
                          static_cast<std::streamsize>(char_size));
            }
            check_ok(out, path);
        }
        {
            const auto path = dir + "/metadata_seqs.bin";
            auto out = open_out(path);
            out.write(reinterpret_cast<const char*>(seq_offsets_.data()),
                      static_cast<std::streamsize>((num_nodes_ + 1) * sizeof(uint64_t)));
            const uint64_t char_size = seq_chars_.size();
            out.write(reinterpret_cast<const char*>(&char_size), sizeof(char_size));
            if (char_size > 0) {
                out.write(seq_chars_.data(),
                          static_cast<std::streamsize>(char_size));
            }
            check_ok(out, path);
        }
        // Índices secundários — ausentes em caches antigos; load() faz fallback gracioso
        if (has_length_index()) {
            const auto path = dir + "/length_index.bin";
            auto out = open_out(path);
            const uint64_t n = length_sorted_vals_.size();
            out.write(reinterpret_cast<const char*>(&n), sizeof(n));
            out.write(reinterpret_cast<const char*>(length_sorted_vals_.data()),
                      static_cast<std::streamsize>(n * sizeof(uint32_t)));
            out.write(reinterpret_cast<const char*>(length_sorted_ids_.data()),
                      static_cast<std::streamsize>(n * sizeof(uint32_t)));
            check_ok(out, path);
        }
        if (has_depth_index()) {
            const auto path = dir + "/depth_index.bin";
            auto out = open_out(path);
            const uint64_t n = depth_sorted_vals_.size();
            out.write(reinterpret_cast<const char*>(&n), sizeof(n));
            out.write(reinterpret_cast<const char*>(depth_sorted_vals_.data()),
                      static_cast<std::streamsize>(n * sizeof(float)));
            out.write(reinterpret_cast<const char*>(depth_sorted_ids_.data()),
                      static_cast<std::streamsize>(n * sizeof(uint32_t)));
            check_ok(out, path);
        }
        // name_index_ não é persistido — recriado lazy na 1ª query WHERE name=.
    }

    void load(const std::string& dir) {
        namespace fs = std::filesystem;

        // Cache truncado deve falhar alto em vez de silenciar com lixo. Para
        // arquivos opcionais (legacy compat), distinguimos "ausente" via
        // fs::exists() — ausência ainda faz fallback; presença + corrupção
        // lança.
        auto open_in = [](const std::string& p) {
            std::ifstream i(p, std::ios::binary);
            if (!i) {
                throw std::runtime_error("Falha ao abrir para leitura: " + p);
            }
            return i;
        };
        auto read_exact = [](std::ifstream& in, void* dst, size_t bytes,
                             const std::string& p) {
            in.read(reinterpret_cast<char*>(dst),
                    static_cast<std::streamsize>(bytes));
            if (!in || in.gcount() != static_cast<std::streamsize>(bytes)) {
                throw std::runtime_error("Cache truncado: " + p);
            }
        };

        {
            const auto path = dir + "/metadata.bin";
            auto in = open_in(path);
            uint64_t nn = 0;
            read_exact(in, &nn, sizeof(nn), path);
            num_nodes_ = nn;
            lengths_.resize(nn);
            read_counts_.resize(nn);
            depths_.resize(nn);
            read_exact(in, lengths_.data(), nn * sizeof(uint32_t), path);
            read_exact(in, read_counts_.data(), nn * sizeof(int32_t), path);
            read_exact(in, depths_.data(), nn * sizeof(float), path);
        }
        // metadata_strings.bin / metadata_seqs.bin: mmap o arquivo inteiro;
        // copia os offsets para vetor (~33 MB no chm13_100w) mas evita copiar
        // o payload de chars (até 270 MB no chm13_100w → economiza centenas
        // de ms no boot e o RSS fica baixo até a primeira query tocar páginas).
        // Layout do arquivo: offsets[N+1] | char_size:u64 | chars[char_size].
        auto load_chars_section =
            [&](const std::string& path, std::vector<uint64_t>& offsets,
                CharSection& section, bool optional) {
                if (optional && !fs::exists(path)) {
                    return; // legacy compat
                }
                MmapBuffer m{path}; // throws on failure
                const size_t header_offsets = (num_nodes_ + 1) * sizeof(uint64_t);
                const size_t header_size_field = sizeof(uint64_t);
                const size_t header_total = header_offsets + header_size_field;
                if (m.size() < header_total) {
                    throw std::runtime_error("Cache truncado: " + path);
                }
                offsets.resize(num_nodes_ + 1);
                std::memcpy(offsets.data(), m.data(), header_offsets);
                uint64_t char_size = 0;
                std::memcpy(&char_size, m.data() + header_offsets,
                            header_size_field);
                if (m.size() < header_total + char_size) {
                    throw std::runtime_error("Cache truncado: " + path);
                }
                // section aponta para a região de chars dentro do mmap.
                // O mmap é movido para dentro do CharSection (RAII).
                section.set_mapped(std::move(m), header_total,
                                   static_cast<size_t>(char_size));
            };

        load_chars_section(dir + "/metadata_strings.bin", name_offsets_,
                           name_chars_, /*optional=*/false);
        load_chars_section(dir + "/metadata_seqs.bin", seq_offsets_,
                           seq_chars_, /*optional=*/true);
        {
            const auto path = dir + "/length_index.bin";
            if (fs::exists(path)) {
                auto in = open_in(path);
                uint64_t n = 0;
                read_exact(in, &n, sizeof(n), path);
                length_sorted_vals_.resize(n);
                length_sorted_ids_.resize(n);
                read_exact(in, length_sorted_vals_.data(),
                           n * sizeof(uint32_t), path);
                read_exact(in, length_sorted_ids_.data(),
                           n * sizeof(uint32_t), path);
            } else {
                build_length_index();
            }
        }
        {
            const auto path = dir + "/depth_index.bin";
            if (fs::exists(path)) {
                auto in = open_in(path);
                uint64_t n = 0;
                read_exact(in, &n, sizeof(n), path);
                depth_sorted_vals_.resize(n);
                depth_sorted_ids_.resize(n);
                read_exact(in, depth_sorted_vals_.data(),
                           n * sizeof(float), path);
                read_exact(in, depth_sorted_ids_.data(),
                           n * sizeof(uint32_t), path);
            } else {
                build_depth_index();
            }
        }
    }

    // ── Accessors O(1) ────────────────────────────────────────────────────────
    // Todos noexcept — operator[] de std::vector e construtor de string_view
    // não lançam. Bounds checking é responsabilidade do caller (ids vêm do MPHF).
    [[nodiscard]] size_t size() const noexcept { return num_nodes_; }
    [[nodiscard]] uint32_t length(uint64_t id) const noexcept { return lengths_[id]; }
    [[nodiscard]] std::span<const uint32_t> lengths_span() const noexcept { return lengths_; }
    [[nodiscard]] int32_t read_count(uint64_t id) const noexcept { return read_counts_[id]; }
    [[nodiscard]] float depth(uint64_t id) const noexcept { return depths_[id]; }

    [[nodiscard]] std::string_view original_id(uint64_t id) const noexcept {
        const uint64_t start = name_offsets_[id];
        const uint64_t end = name_offsets_[id + 1];
        return std::string_view(name_chars_.data() + start, end - start);
    }

    [[nodiscard]] std::string_view sequence(uint64_t id) const noexcept {
        // seq_chars_ pode estar vazio após take_sequences() ou em caches
        // antigos sem metadata_seqs.bin — sequence(id) precisa retornar {}
        // em vez de fazer ponteiro+offset em buffer vazio (UB).
        if (seq_chars_.empty()) {
            return {};
        }
        const uint64_t start = seq_offsets_[id];
        const uint64_t end = seq_offsets_[id + 1];
        if (end > start) {
            return std::string_view(seq_chars_.data() + start,
                                    end - start - 1); // exclude '$'
        }
        return {};
    }

    // Move o buffer de sequências raw para o caller (usado pelo ingest para
    // alimentar o FM-Index builder via std::span). Após chamada, sequence()
    // retorna {} (B3) e seq_chars_ fica vazio.
    // Lança se o buffer está mmapped (path de load) — esse caminho não existe
    // na prática.
    [[nodiscard]] std::pair<std::unique_ptr<char[]>, size_t> take_sequences() {
        if (seq_chars_.is_mapped()) {
            throw std::runtime_error(
                "take_sequences(): buffer está mmapped (load path); "
                "implementação só permite mover do path de ingest");
        }
        return seq_chars_.release_owned();
    }

    [[nodiscard]] const std::vector<uint64_t>& all_seq_offsets() const noexcept {
        return seq_offsets_;
    }

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
        if (it == seq_offsets_.begin() || it == seq_offsets_.end()) {
            return std::nullopt;
        }
        return static_cast<uint64_t>(
            std::distance(seq_offsets_.begin(), it) - 1);
    }

  private:
    // CharSection — encapsula um buffer de chars que pode ser:
    //   (a) owned: unique_ptr<char[]> alocado SEM zerar em flush_segments()
    //       (path de ingest; new char[N] é default-init = uninitialized para POD,
    //       evitando ~334 MB de memset no chm13_100w);
    //   (b) mapped: MmapBuffer apontando para metadata_strings.bin/
    //       metadata_seqs.bin (path de load — zero cópia, paginação on-demand).
    // payload/payload_size sempre apontam para o conteúdo ativo.
    struct CharSection {
        std::unique_ptr<char[]> owned;
        size_t owned_size = 0;
        MmapBuffer mapped;
        const char* payload = nullptr;
        size_t payload_size = 0;

        [[nodiscard]] const char* data() const noexcept { return payload; }
        [[nodiscard]] size_t size() const noexcept { return payload_size; }
        [[nodiscard]] bool empty() const noexcept { return payload_size == 0; }
        [[nodiscard]] bool is_mapped() const noexcept { return !mapped.empty(); }

        // Aloca buffer raw (sem zeragem) de tamanho n. Retorna ponteiro
        // gravável para o caller preencher via memcpy.
        char* alloc_raw_uninit(size_t n) {
            mapped = MmapBuffer{};
            owned.reset(new char[n]); // default-init: NÃO zera para POD char
            owned_size = n;
            payload = owned.get();
            payload_size = n;
            return owned.get();
        }
        // Substitui pelo mmap. payload aponta dentro do mmap em offset bytes,
        // com tamanho bytes. m sai vazio (move).
        void set_mapped(MmapBuffer&& m, size_t offset, size_t bytes) {
            owned.reset();
            owned_size = 0;
            mapped = std::move(m);
            payload = mapped.empty() ? nullptr : mapped.data() + offset;
            payload_size = bytes;
        }
        // Move o ownership do buffer raw para o caller.
        // Após chamada, payload/size = 0.
        std::pair<std::unique_ptr<char[]>, size_t> release_owned() noexcept {
            std::pair<std::unique_ptr<char[]>, size_t> out{std::move(owned),
                                                            owned_size};
            owned_size = 0;
            payload = nullptr;
            payload_size = 0;
            return out;
        }
        void clear() noexcept {
            owned.reset();
            owned_size = 0;
            mapped = MmapBuffer{};
            payload = nullptr;
            payload_size = 0;
        }
    };

    size_t num_nodes_ = 0;
    std::vector<uint32_t> lengths_;
    std::vector<int32_t>  read_counts_;
    std::vector<float>    depths_;
    std::vector<uint64_t> name_offsets_; // tamanho: num_nodes_ + 1
    CharSection           name_chars_;
    std::vector<uint64_t> seq_offsets_;  // tamanho: num_nodes_ + 1
    CharSection           seq_chars_;

    // Índice secundário de comprimento — arrays paralelos ordenados por length
    std::vector<uint32_t> length_sorted_vals_;
    std::vector<uint32_t> length_sorted_ids_;

    // Índice secundário de depth — apenas nós com depth >= 0
    std::vector<float>    depth_sorted_vals_;
    std::vector<uint32_t> depth_sorted_ids_;

    // Índice invertido name → id (lazy, não persistido).
    // Sorted vector<(hash64, internal_id)> + linear probe para colisões.
    mutable std::once_flag name_index_once_;
    mutable std::vector<std::pair<uint64_t, uint32_t>> name_index_;
};

} // namespace ggql
