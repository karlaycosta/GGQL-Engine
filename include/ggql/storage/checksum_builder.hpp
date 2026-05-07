#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

#include "mphf/xxh64.hpp"

namespace ggql {

// Concept reusável para tipos hashables como dados (não como endereço).
// Bloqueia ponteiros uniformemente em todos os overloads templated:
// hash de pointer não é reproduzível entre execuções e quase sempre
// é erro de uso.
template <typename T>
concept HashablePod = std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>;

// ChecksumBuilder allows for easy and robust calculation of hashes over
// varying types of data payload using XXH64. This is specifically useful
// for verifying data integrity upon save/load operations of project data.
//
// Builder one-shot por padrão: chamar finalize() não consume o estado
// — adds subsequentes acumulam ao hash. Use reset() para reusar instância.
class ChecksumBuilder {
  public:
    ChecksumBuilder() = default;

    // Adds raw bytes to the hash. Aceita (nullptr, 0) como "zero bytes".
    ChecksumBuilder& add(const void* data, size_t size) {
        add_part(XXH64(size == 0 ? nullptr : data, size, 0));
        return *this;
    }

    // Adds a POD (Plain Old Data) value to the hash
    template <HashablePod T>
    ChecksumBuilder& add(const T& value) {
        add_part(XXH64(&value, sizeof(T), 0));
        return *this;
    }

    // Adds a C-style array to the hash
    template <HashablePod T, size_t N>
    ChecksumBuilder& add(const T (&arr)[N]) {
        add_part(XXH64(arr, N * sizeof(T), 0));
        return *this;
    }

    // Adds a std::vector of POD values to the hash
    template <HashablePod T>
    ChecksumBuilder& add(const std::vector<T>& vec) {
        if (vec.empty()) {
            // Push a zero hash to maintain structural alignment when empty
            add_part(XXH64(nullptr, 0, 0));
        } else {
            add_part(XXH64(vec.data(), vec.size() * sizeof(T), 0));
        }
        return *this;
    }

    // Adds a string view to the hash. Aceita std::string, const char*,
    // string literal, std::string_view via conversão implícita.
    ChecksumBuilder& add(std::string_view sv) {
        add_part(XXH64(sv.empty() ? nullptr : sv.data(), sv.size(), 0));
        return *this;
    }

    // Reseta o builder para reuso. Não realoca o stack buffer.
    void reset() noexcept {
        count_ = 0;
        extra_parts_.clear();
    }

    // Combines all collected hash parts into a final robust hash.
    [[nodiscard("checksum deve ser consumido — descartar é provável bug")]]
    uint64_t finalize() const {
        if (count_ == 0 && extra_parts_.empty()) {
            return 0;
        }
        if (extra_parts_.empty()) {
            return XXH64(parts_.data(), count_ * sizeof(uint64_t), 0);
        }
        // Fallback for > kMaxParts additions
        std::vector<uint64_t> all_parts(parts_.begin(), parts_.begin() + count_);
        all_parts.insert(all_parts.end(), extra_parts_.begin(), extra_parts_.end());
        return XXH64(all_parts.data(), all_parts.size() * sizeof(uint64_t), 0);
    }

  private:
    void add_part(uint64_t hash) {
        if (count_ < kMaxParts) {
            parts_[count_++] = hash;
        } else {
            // Allocate heap only if we exceed stack bounds (rare fallback)
            extra_parts_.push_back(hash);
        }
    }

    // We allow up to 32 independent hashed objects without any heap allocation
    static constexpr size_t kMaxParts = 32;
    std::array<uint64_t, kMaxParts> parts_{};
    size_t count_ = 0;
    std::vector<uint64_t> extra_parts_;
};

} // namespace ggql
