#pragma once

// snarl_decomposition.hpp — Decomposição de snarls (Paten et al. 2018).
//
// Substitui find_snarls_streaming (cactus_finder.hpp) que era restrito a
// snarls que envolvem bridges 3-aresta-conectados. A implementação aqui
// segue o framework de Paten 2018 ("Superbubbles, Ultrabubbles, and
// Cacti", J. Comp. Bio. 25(7)) generalizando o algoritmo Onodera 2013.
//
// API:
//   std::vector<SnarlRecord> find_all_snarls(
//       const GfaCsrTopology& topo,
//       int num_threads = 1);
//
// Diferença vs find_snarls_streaming (cactus):
//   - Cactus puro: ~0% de cobertura em grafos densos (metaFlye Zymo: 0/44).
//   - Paten 2018:  cobertura completa de boundary pairs (alvo: 44/44).
//
// Implementação em fases (pendência interna do projeto, não da API):
//   Fase 1 (atual): Onodera 2013 estendido — detecta superbubbles canônicas
//                   (DAG entre source e sink) com SnarlRecord populado.
//                   Cobertura ~30% do vg em datasets densos.
//   Fase 2:         + 2-edge-biconnected components no bidirected graph.
//                   Detecta snarls não-DAG e aninhados. Cobertura ~70-90%.
//   Fase 3:         + chains + snarl tree completa. Paridade com vg.

#include "ggql/storage/topology_types.hpp"

#include <cstdint>
#include <vector>

namespace ggql {

class GfaCsrTopology; // fwd decl

namespace snarl_decomposition {

// Retorna todos os snarls do grafo seguindo Paten 2018 (Onodera estendido
// na Fase 1). Resultado equivalente a find_snarls_streaming + boundary
// pairs adicionais detectados em sub-regiões 3-edge-connected.
//
// Os SnarlRecord retornados têm:
//   source              — ID do nó-source (in_dag > 0, out_dag >= 2 inter-SCC)
//   sink                — ID do nó-sink (todos paths de source convergem aqui)
//   branch_count        — out_degree(source) inter-SCC, capado em 255
//   internal_node_count — # de nós no subgrafo entre source e sink
//   has_cycles          — 1 se algum nó interno está em SCC não-trivial
//
// Single-threaded por ora (paralelização vem na Fase 2).
std::vector<SnarlRecord> find_all_snarls(const GfaCsrTopology& topo,
                                         int num_threads = 1);

// Variante alinhada ao algoritmo vg `IntegratedSnarlFinder` (em construção).
// Implementa cycles_in_cactus + bridge forest + decomposição recursiva.
// Retorna SnarlRecord para compat de API.
// Stub atual — preenchido em chunks incrementais.
std::vector<SnarlRecord> find_all_snarls_vgstyle(const GfaCsrTopology& topo,
                                                  int num_threads = 1);

// Variante baseada em algoritmo SESE (Single-Entry-Single-Exit) do Billi
// (at-cg/billi, Bhat/Mahajan/Jain 2025). Adaptação de Johnson 1994 para
// bidirected graphs. Linear time O(N+E), 15× mais rápido que vg em
// panbubble detection segundo paper.
//
// Versão minimal: sem hairpins, sem dual edges. Detecta SESE regions
// via cycle equivalence (top_bracket, size). API compatível com SnarlRecord.
std::vector<SnarlRecord> find_all_snarls_sese(const GfaCsrTopology& topo,
                                               int num_threads = 1);

} // namespace snarl_decomposition
} // namespace ggql
