#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "mphf/xxh64.hpp"

namespace ggql {

// ChecksumBuilder allows for easy and robust calculation of hashes over
// varying types of data payload using XXH64. This is specifically useful
// for verifying data integrity upon save/load operations of project data.
class ChecksumBuilder {
  public:
    ChecksumBuilder() : count_(0) {}

    // Adds raw bytes to the hash
    ChecksumBuilder& add(const void* data, size_t size) {
        add_part(XXH64(data, size, 0));
        return *this;
    }

    // Adds a POD (Plain Old Data) value to the hash
    template <typename T>
    std::enable_if_t<std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>, ChecksumBuilder&>
    add(const T& value) {
        add_part(XXH64(&value, sizeof(T), 0));
        return *this;
    }

    // Adds a C-style array to the hash
    template <typename T, size_t N>
    std::enable_if_t<std::is_trivially_copyable_v<T>, ChecksumBuilder&> add(const T (&arr)[N]) {
        add_part(XXH64(arr, N * sizeof(T), 0));
        return *this;
    }

    // Adds a std::vector of POD values to the hash
    template <typename T>
    std::enable_if_t<std::is_trivially_copyable_v<T>, ChecksumBuilder&>
    add(const std::vector<T>& vec) {
        if (vec.empty()) {
            // Push a zero hash to maintain structural alignment when empty
            add_part(XXH64(nullptr, 0, 0));
        } else {
            add_part(XXH64(vec.data(), vec.size() * sizeof(T), 0));
        }
        return *this;
    }

    // Adds a std::string to the hash
    ChecksumBuilder& add(const std::string& str) {
        if (str.empty()) {
            add_part(XXH64(nullptr, 0, 0));
        } else {
            add_part(XXH64(str.data(), str.size(), 0));
        }
        return *this;
    }

    // Combines all collected hash parts into a final robust hash
    uint64_t finalize() const {
        if (count_ == 0 && extra_parts_.empty()) {
            return 0;
        }
        if (extra_parts_.empty()) {
            return XXH64(parts_, count_ * sizeof(uint64_t), 0);
        } else {
            // Fallback for > kMaxParts additions
            std::vector<uint64_t> all_parts(parts_, parts_ + count_);
            all_parts.insert(all_parts.end(), extra_parts_.begin(), extra_parts_.end());
            return XXH64(all_parts.data(), all_parts.size() * sizeof(uint64_t), 0);
        }
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
    uint64_t parts_[kMaxParts] = {0};
    size_t count_;
    std::vector<uint64_t> extra_parts_;
};

} // namespace ggql
