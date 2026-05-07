#pragma once

// snarl_decomposition.hpp — Detecção de snarls (port nativo do algoritmo Billi).
//
// Implementação única: port literal de at-cg/billi (Bhat/Mahajan/Jain 2025),
// adaptado da pipeline run_decompose() para operar diretamente sobre nossa
// GfaCsrTopology (sem reler GFA). Resultado idêntico ao binário Billi
// (paridade 21/21 nos testes oficiais).
//
// API:
//   std::vector<SnarlRecord> find_all_snarls(
//       const GfaCsrTopology& topo,
//       int num_threads = 1);
//
// Pipeline interna: biedged graph → compaction → tip identification →
// SESE (Single-Entry-Single-Exit) → hairpin detection → mark_nodes
// validation → SnarlRecord output.

#include "ggql/storage/topology_types.hpp"

#include <vector>

namespace ggql {

class GfaCsrTopology; // fwd decl

namespace snarl_decomposition {

// PORT LITERAL do algoritmo Billi (at-cg/billi, Bhat/Mahajan/Jain 2025).
// Resultado idêntico ao binário Billi (paridade 21/21 testes oficiais).
//
// Os SnarlRecord retornados têm:
//   source              — ID do nó-boundary menor (panbubble ou hairpin)
//   sink                — ID do nó-boundary maior
//   branch_count        — sempre 2 (boundary pair)
//   internal_node_count — sempre 0 (não populado neste port)
//   has_cycles          — 1 para hairpins, 0 para panbubbles canônicas
//
// Otimizações aplicadas sobre o port original:
//   - DFS iterativo em todas as 7 funções (dfs_comp, dfs_depth,
//     sese_minbracket, sese, hairp, dfs_nesting, mark_nodes) —
//     sem stack overflow em grafos com cadeias > 100K nós
//   - Pool allocators (std::deque) para Edge e BracketList —
//     ponteiros estáveis, zero leaks, destruição O(1) em batch
//   - vector<uint8_t> em vez de vector<bool> para mark/is_bridge
//   - unordered_map com hash rápido para canonical_sese e bracket tracking
//   - const auto& em todos os range-for sobre adjacências
//   - Proteção contra loops infinitos em mark_nodes (visit_order guard)
//
// Single-threaded por ora (num_threads reservado para uso futuro).
std::vector<SnarlRecord> find_all_snarls(const GfaCsrTopology& topo, int num_threads = 1);

} // namespace snarl_decomposition
} // namespace ggql
