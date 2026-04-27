#pragma once

// union_find.hpp — Union-Find com path-compression + union-by-rank.
//
// Interface:
//   UnionFind uf(n);
//   uf.unite(a, b)        → une os grupos de a e b; retorna true se estavam separados
//   uf.find(a)            → representante canônico do grupo de a (compressão de caminho)
//   uf.same(a, b)         → true se a e b estão no mesmo grupo
//   uf.size(n)            → número de elementos alocados
//
// Complexidade: O(α(n)) amortizado por operação (quase constante).
// Thread-safety: nenhuma — cada thread deve ter sua própria instância.

#include <cstdint>
#include <numeric>
#include <vector>

namespace ggql::detail {

class UnionFind {
  public:
    explicit UnionFind(uint64_t n) : parent_(n), rank_(n, 0) {
        std::iota(parent_.begin(), parent_.end(), uint64_t{0});
    }

    // Encontra o representante do grupo de x (path compression).
    uint64_t find(uint64_t x) noexcept {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]]; // path halving
            x = parent_[x];
        }
        return x;
    }

    // Une os grupos de a e b (union by rank). Retorna true se havia separação.
    bool unite(uint64_t a, uint64_t b) noexcept {
        a = find(a);
        b = find(b);
        if (a == b) {
            return false;
        }
        if (rank_[a] < rank_[b]) {
            auto t = a;
            a = b;
            b = t;
        }
        parent_[b] = a;
        if (rank_[a] == rank_[b]) {
            ++rank_[a];
        }
        return true;
    }

    bool same(uint64_t a, uint64_t b) noexcept { return find(a) == find(b); }

    uint64_t size() const noexcept { return static_cast<uint64_t>(parent_.size()); }

  private:
    std::vector<uint64_t> parent_;
    std::vector<uint8_t> rank_; // grau <= 64 para 2^64 nós
};

} // namespace ggql::detail
