#pragma once

// gfa_topology_types.hpp — tipos POD compartilhados entre as camadas
// de topologia GFA (CSR+EF e gBWT).
//
// DEPENDÊNCIAS INTENCIONALMENTE MÍNIMAS: apenas <cstdint>, <string>, <vector>.
// Este header pode ser incluído por qualquer TU sem puxar SDSL ou libgbwt.
//
// Tipos definidos aqui:
//   LinkRecord  — link bruto do ingest (16 bytes)
//   EdgeRef     — aresta decodificada (node_id + orient)
//   Bubble      — superbubble detectada por find_bubbles
//   PathRecord  — entrada para build_path_index (camada gBWT)

#include <cstdint>
#include <string>
#include <vector>

namespace ggql {

// ── LinkRecord: link bruto coletado no ingest — 16 bytes (sem padding) ────
//
// Orientações empacotadas no bit 0 de cada campo (mesmo encoding que E_fwd_):
//   from_packed = (from_id << 1) | (from_orient == '-' ? 1 : 0)
//   to_packed   = (to_id   << 1) | (to_orient   == '-' ? 1 : 0)
//
// 16 bytes vs 32 bytes do layout with chars = 50% menos tráfego de memória
// no radix sort (crítico: 4 passes × 2 CSRs = 8 varreduras completas do vetor).
struct LinkRecord {
    uint64_t from_packed; // (from_id << 1) | orient_bit
    uint64_t to_packed;   // (to_id   << 1) | orient_bit

    static LinkRecord make(uint64_t from_id, char from_orient, uint64_t to_id,
                           char to_orient) noexcept {
        return {(from_id << 1) | (from_orient == '-' ? 1u : 0u),
                (to_id << 1) | (to_orient == '-' ? 1u : 0u)};
    }

    uint64_t from_id() const noexcept { return from_packed >> 1; }
    char from_orient() const noexcept { return (from_packed & 1u) ? '-' : '+'; }
    uint64_t to_id() const noexcept { return to_packed >> 1; }
    char to_orient() const noexcept { return (to_packed & 1u) ? '-' : '+'; }

    // Inverte from↔to (para construção do CSR reverso)
    LinkRecord reversed() const noexcept { return {to_packed, from_packed}; }
};

// ── EdgeRef: arestas decodificadas para uso nos algoritmos ─────────────────
struct EdgeRef {
    uint64_t node_id;
    char orient; // '+' ou '-'
};

// ── Bubble: resultado de find_bubbles ──────────────────────────────────────
struct Bubble {
    uint64_t source;
    uint64_t sink;
    std::vector<uint64_t> branches; // nós intermediários em qualquer ramo
};

// ── PathRecord: entrada para build_path_index (camada gBWT) ────────────────
//
// nodes[i] = (node_id << 1) | orient_bit  — mesmo encoding de E_fwd_.
// name     = identificador textual do path (ex: "HG002#0#chr1").
struct PathRecord {
    std::string name;
    std::vector<uint64_t> nodes; // encoded-values na ordem do path
};

} // namespace ggql
