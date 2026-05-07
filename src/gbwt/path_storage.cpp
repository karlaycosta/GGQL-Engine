#include "ggql/gbwt/path_storage.hpp"

#include "ggql/storage/checksum_builder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

#ifdef _OPENMP
    #include <omp.h>
#endif

namespace ggql {

namespace {

// Format on-disk (LE-only, x86_64). Header é fixo de 32 bytes; payload é
// nodes (total_nodes × sizeof(node_type)) + offsets ((num_paths+1) × 8B);
// footer é xxh64 de header+payload (8B). Bump kVersion ao mudar layout.
//
// kVersion = 2 (Fase 6 onda 4): node_type passou de uint64_t → uint32_t.
// Caches v1 não são lidos — exigem reingest.
constexpr std::array<char, 8> kMagic    = {'G', 'G', 'Q', 'L', 'P', 'S', '\0', '\0'};
constexpr std::uint32_t       kVersion  = 2;
constexpr std::uint32_t       kFlags    = 0;

template <typename T>
void write_pod(std::ostream& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
T read_pod(std::istream& in) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return value;
}

}  // namespace

PathStorage PathStorage::from_paths(std::vector<std::vector<node_type>>&& paths) {
    PathStorage ps;
    const std::size_t n = paths.size();

    // Pre-compute prefix-sum dos offsets serialmente (O(n), trivial).
    std::vector<std::uint64_t> offs(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        offs[i + 1] = offs[i] + paths[i].size();
    }
    const std::size_t total = offs.back();

    ps.nodes_.resize(total);
    ps.offsets_ = std::move(offs);

    // Cópia em paralelo: cada path escreve numa região disjunta de nodes_,
    // sem race. Para POD (uint32_t), memcpy interno é o ideal.
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (std::size_t i = 0; i < n; ++i) {
        if (!paths[i].empty()) {
            std::ranges::copy(paths[i], ps.nodes_.begin() + ps.offsets_[i]);
        }
    }

    // Libera RAM dos paths originais em batch após o copy paralelo. Fora do
    // loop OMP para evitar race em allocators thread-local.
    for (auto& p : paths) {
        std::vector<node_type>().swap(p);
    }
    std::vector<std::vector<node_type>>().swap(paths);

    return ps;
}

void PathStorage::extract_into(std::size_t path_id, std::vector<node_type>& out) const {
    const auto v = nodes_view(path_id);
    out.assign(v.begin(), v.end());   // memcpy direto (mesmo tipo)
}

void PathStorage::extract_into(std::size_t path_id, std::vector<std::uint64_t>& out) const {
    const auto v = nodes_view(path_id);
    // resize + ranges::copy ao invés de assign(begin,end). assign de tipos
    // distintos cai em loop genérico; resize fixa o tamanho upfront e
    // ranges::copy é vetorizável (vpmovzxdq) com -O3.
    out.resize(v.size());
    std::ranges::copy(v, out.begin());
}

std::span<const PathStorage::node_type>
PathStorage::nodes_view(std::size_t path_id) const noexcept {
    [[assume(path_id < num_paths())]];   // pré-condição (C++23)
    const std::size_t begin = offsets_[path_id];
    const std::size_t end   = offsets_[path_id + 1];
    // subspan via std::span ativa bounds check em libstdc++ debug; em release
    // otimiza para o mesmo memcpy de ponteiros que a versão raw.
    return std::span<const node_type>(nodes_).subspan(begin, end - begin);
}

std::size_t PathStorage::path_size(std::size_t path_id) const noexcept {
    [[assume(path_id < num_paths())]];   // pré-condição (C++23)
    return offsets_[path_id + 1] - offsets_[path_id];
}

std::size_t PathStorage::memory_bytes() const noexcept {
    return nodes_.capacity() * sizeof(node_type)
         + offsets_.capacity() * sizeof(std::uint64_t);
}

void PathStorage::save(std::ostream& out) const {
    ChecksumBuilder cs;

    const std::uint64_t np = num_paths();
    const std::uint64_t tn = total_nodes();

    out.write(kMagic.data(), kMagic.size());
    cs.add(kMagic.data(), kMagic.size());

    write_pod(out, kVersion); cs.add(kVersion);
    write_pod(out, kFlags);   cs.add(kFlags);
    write_pod(out, np);       cs.add(np);
    write_pod(out, tn);       cs.add(tn);

    if (tn > 0) {
        out.write(reinterpret_cast<const char*>(nodes_.data()),
                  static_cast<std::streamsize>(nodes_.size() * sizeof(node_type)));
        cs.add(nodes_);
    } else {
        cs.add(nodes_);  // hash de vazio para preservar alinhamento estrutural
    }

    out.write(reinterpret_cast<const char*>(offsets_.data()),
              static_cast<std::streamsize>(offsets_.size() * sizeof(std::uint64_t)));
    cs.add(offsets_);

    const std::uint64_t checksum = cs.finalize();
    write_pod(out, checksum);

    if (!out.good()) {
        throw std::runtime_error("PathStorage::save: stream write failed");
    }
}

PathStorage PathStorage::load(std::istream& in) {
    std::array<char, 8> magic{};
    in.read(magic.data(), magic.size());
    if (!in.good()) {
        throw std::runtime_error("PathStorage::load: failed to read magic");
    }
    if (magic != kMagic) {
        throw std::runtime_error("PathStorage::load: bad magic");
    }

    const auto version = read_pod<std::uint32_t>(in);
    if (version != kVersion) {
        throw std::runtime_error(
            "PathStorage::load: versão " + std::to_string(version) +
            " não suportada (esperado " + std::to_string(kVersion) +
            "). Cache antigo — regenere via `ggql ingest`.");
    }

    const auto flags = read_pod<std::uint32_t>(in);
    const auto np    = read_pod<std::uint64_t>(in);
    const auto tn    = read_pod<std::uint64_t>(in);

    PathStorage ps;
    // assign(N, 0) zera todos os bytes — defesa explícita contra leituras
    // parciais que deixariam offsets[0] ≠ 0 mascarando inconsistência. resize
    // não garantiria isso (vector<node_type> default-init = 0, mas é frágil
    // depender desse contrato implícito).
    ps.nodes_.assign(static_cast<std::size_t>(tn), 0);
    ps.offsets_.assign(static_cast<std::size_t>(np) + 1, 0);

    if (tn > 0) {
        in.read(reinterpret_cast<char*>(ps.nodes_.data()),
                static_cast<std::streamsize>(tn * sizeof(node_type)));
    }
    in.read(reinterpret_cast<char*>(ps.offsets_.data()),
            static_cast<std::streamsize>((np + 1) * sizeof(std::uint64_t)));

    const auto stored_checksum = read_pod<std::uint64_t>(in);

    if (!in.good()) {
        throw std::runtime_error("PathStorage::load: read failure");
    }

    ChecksumBuilder cs;
    cs.add(magic.data(), magic.size());
    cs.add(version);
    cs.add(flags);
    cs.add(np);
    cs.add(tn);
    cs.add(ps.nodes_);
    cs.add(ps.offsets_);

    if (cs.finalize() != stored_checksum) {
        throw std::runtime_error("PathStorage::load: checksum mismatch");
    }

    if (ps.offsets_.front() != 0) {
        throw std::runtime_error("PathStorage::load: offsets[0] != 0");
    }
    if (ps.offsets_.back() != tn) {
        throw std::runtime_error("PathStorage::load: offsets.back() != total_nodes");
    }
    for (std::size_t i = 1; i < ps.offsets_.size(); ++i) {
        if (ps.offsets_[i] < ps.offsets_[i - 1]) {
            throw std::runtime_error("PathStorage::load: offsets non-monotonic");
        }
    }

    return ps;
}

// ── PathStorageBuilder ─────────────────────────────────────────────────────

void PathStorageBuilder::reserve(std::size_t num_paths_hint,
                                 std::size_t total_nodes_hint) {
    nodes_.reserve(total_nodes_hint);
    offsets_.reserve(num_paths_hint + 1);
}

void PathStorageBuilder::insert(std::span<const PathStorage::node_type> seq) {
    if (finished_) {
        throw std::logic_error("PathStorageBuilder::insert: already finished");
    }
    nodes_.insert(nodes_.end(), seq.begin(), seq.end());
    offsets_.push_back(nodes_.size());
}

PathStorage PathStorageBuilder::finish() {
    if (finished_) {
        throw std::logic_error("PathStorageBuilder::finish: already finished");
    }
    finished_ = true;
    PathStorage ps;
    ps.nodes_   = std::move(nodes_);
    ps.offsets_ = std::move(offsets_);
    return ps;
}

}  // namespace ggql
