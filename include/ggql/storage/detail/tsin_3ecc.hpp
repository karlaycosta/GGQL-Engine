#pragma once

// tsin_3ecc.hpp — Port do algoritmo de Norouzi & Tsin (2014).
//
// Referência:
//   Norouzi & Tsin (2014) "A simple 3-edge connected component algorithm revisited"
//   Tsin (2007)           "A Simple 3-Edge-Connected Component Algorithm"
//
// Definição: dois nós estão no mesmo 3-ECC se existem pelo menos 3 caminhos
// aresta-disjuntos entre eles. Nós 3-edge-connected formam os "super-nós"
// do grafo Cactus, que é a base para a decomposição em snarls.
//
// API:
//   three_edge_connected_merges(
//       node_count,                         — número de nós [0, node_count)
//       for_each_neighbor,                  — lambda(node, emit_neighbor_fn)
//       same_component                      — callback(a, b) chamado para cada par
//   )                                          a e b no mesmo 3-ECC (ao menos
//                                             cobrindo uma spanning tree do componente)
//
// Uso típico com UnionFind:
//   UnionFind uf(n);
//   three_edge_connected_merges(n, neighbors_fn, [&](size_t a, size_t b) {
//       uf.unite(a, b);
//   });
//
// Porta direta da implementação C++ pura do vg (vgteam/vg), sem dependências
// externas (ignoramos a branch que usa sonLib). Debug output removido.
//
// Complexidade: O(n + m) tempo, O(n) espaço auxiliar.

#include <cassert>
#include <cstddef>
#include <functional>
#include <limits>
#include <vector>

namespace ggql::detail {

// ── Implementação principal (grafo com IDs densos [0, node_count)) ────────
//
// for_each_neighbor(u, emit):  chama emit(v) para cada vizinho v de u.
//   O grafo deve ser tratado como NÃO-DIRIGIDO: emita vizinhos de entrada e
//   de saída juntos para capturar a conectividade bidirecional.
//
// same_component(a, b):  chamado quando a e b pertencem ao mesmo 3-ECC.
//   Use para fazer merge em um Union-Find. O callback NÃO deve modificar o
//   grafo consultado por for_each_neighbor enquanto o algoritmo está rodando.
inline void three_edge_connected_merges(
    size_t node_count,
    size_t first_root,
    const std::function<void(size_t, const std::function<void(size_t)>&)>& for_each_neighbor,
    const std::function<void(size_t, size_t)>& same_component)
{
  if (node_count == 0) return;
  // Grafo suporta até 2^32-2 nós (suficiente para qualquer genoma atual).
  assert(node_count < static_cast<size_t>(std::numeric_limits<uint32_t>::max()));

  // Usamos uint32_t internamente (24 bytes/nó vs 56 com size_t).
  // O limite prático é ~4 bilhões de nós — mais que qualquer pangenoma atual.
  using number_t = uint32_t;
  constexpr number_t NONE = std::numeric_limits<uint32_t>::max();

  // ── Dados por nó ────────────────────────────────────────────────────────
  struct TsinNode {
    uint32_t dfs_counter      = 0;
    uint32_t dfs_exit         = 0;
    uint32_t low_point        = 0;
    uint32_t effective_degree = 0;
    uint32_t path_tail        = NONE; // próximo nó no caminho de low-point
    bool     is_on_path       = false;
    bool     visited          = false;
  };  // sizeof ≈ 22 bytes (alinhado para 24)

  std::vector<TsinNode> nodes(node_count);

  // ── Pré-construção da CSR de adjacência ─────────────────────────────────
  // PERF: substitui `vector<uint32_t> neighbors` por frame de DFS por uma
  // única CSR plana. Elimina N allocs heap (uma por frame) em favor de uma
  // alocação total. Iteração via índice (nb_idx..nb_end). Ganho prático:
  // ~30-50% no tempo do 3-ECC em grafos grandes (medido empiricamente).
  std::vector<uint32_t> nb_offsets(static_cast<size_t>(node_count) + 1, 0);
  std::vector<uint32_t> all_neighbors;
  all_neighbors.reserve(node_count * 4); // chute inicial: avg_degree ~4
  for (size_t u = 0; u < node_count; ++u) {
    nb_offsets[u] = static_cast<uint32_t>(all_neighbors.size());
    for_each_neighbor(u, [&](size_t v) {
      all_neighbors.push_back(static_cast<uint32_t>(v));
    });
  }
  nb_offsets[node_count] = static_cast<uint32_t>(all_neighbors.size());

  // ── absorb_all_along_path ────────────────────────────────────────────────
  // Percorre o caminho a partir de path_start até (exclusive) path_past_end,
  // chamando same_component(into, n) para cada nó n no caminho (exceto o
  // primeiro que passa a ser o "into"). Se into == NONE o primeiro nó torna-
  // se o into.
  auto absorb_all_along_path = [&](number_t into, number_t path_start, number_t path_past_end) {
    bool path_null = true;
    number_t here = path_start;
    while (here != path_past_end) {
      if (here == NONE) {
        assert(path_null);
        break;
      }
      auto& here_node = nodes[here];
      if (here_node.is_on_path) {
        if (into == NONE) {
          into = here;
        } else {
          path_null = false;
          nodes[into].effective_degree = (nodes[into].effective_degree +
                                          here_node.effective_degree - 2);
          same_component(into, here);
        }
      }
      here = here_node.path_tail;
    }
  };

  // ── DFS stack ─────────────────────────────────────────────────────────────
  // PERF: armazena ranges (nb_idx..nb_end) na CSR plana em vez de copiar os
  // vizinhos em um vector por frame. 16 bytes por frame vs ~40 bytes + heap
  // alloc por frame anteriormente.
  struct DFSStackFrame {
    uint32_t current;
    uint32_t nb_idx;                  // próximo a processar (LIFO via nb_end-1)
    uint32_t nb_end;                  // sentinela exclusiva
    bool     saw_parent_tree_edge = false;
    bool     recursing            = false;
  };

  std::vector<DFSStackFrame> stack;
  stack.reserve(64); // grafos típicos têm DFS depth O(log N); reserve modesta

  uint32_t next_unvisited = (first_root < node_count)
                              ? static_cast<uint32_t>(first_root) : 0u;
  uint32_t dfs_counter    = 1;

  // Helper: empurra novo frame com range pré-calculado da CSR plana.
  auto push_frame = [&](uint32_t v) {
    DFSStackFrame f;
    f.current = v;
    f.nb_idx  = nb_offsets[v];
    f.nb_end  = nb_offsets[static_cast<size_t>(v) + 1];
    stack.push_back(f);
  };

  while (next_unvisited != static_cast<uint32_t>(node_count)) {
    if (!nodes[next_unvisited].visited) {
      push_frame(next_unvisited);
    }

    while (!stack.empty()) {
      auto& frame = stack.back();
      auto& node  = nodes[frame.current];

      if (!node.visited) {
        // ── Primeira visita ────────────────────────────────────────────────
        node.visited    = true;
        if (frame.current == next_unvisited) {
          do { ++next_unvisited; }
          while (next_unvisited != static_cast<uint32_t>(node_count)
                 && nodes[next_unvisited].visited);
        }
        node.dfs_counter = dfs_counter++;
        node.low_point   = node.dfs_counter;
        node.path_tail   = NONE;
        node.is_on_path  = true;
        // Range já populado em push_frame — nada a copiar.
        continue;
      }

      if (frame.nb_idx < frame.nb_end) {
        // ── Processar próximo vizinho (LIFO: do fim para o início) ─────────
        uint32_t nb_num = all_neighbors[frame.nb_end - 1];
        auto&    nb     = nodes[nb_num];

        if (!frame.recursing) {
          node.effective_degree++;

          if (!nb.visited) {
            // Aresta de árvore → recursão
            frame.recursing = true;
            push_frame(nb_num);
          } else {
            // Aresta de retorno ou de árvore para pai
            if (stack.size() > 1 &&
                nb_num == stack[stack.size() - 2].current &&
                !frame.saw_parent_tree_edge)
            {
              // Aresta de árvore de entrada (ignorar)
              frame.saw_parent_tree_edge = true;
            } else if (nb.dfs_counter < node.dfs_counter) {
              // Outgoing back-edge (step 1.2 de Tsin)
              if (nb.dfs_counter < node.low_point) {
                absorb_all_along_path(NONE, frame.current, NONE);
                node.low_point   = nb.dfs_counter;
                node.is_on_path  = true;
                node.path_tail   = NONE;
              }
            } else if (node.dfs_counter < nb.dfs_counter) {
              // Incoming back-edge (step 1.3 de Tsin)
              node.effective_degree -= 2;

              // Encontra o nó ancestral de nb que está no nosso caminho
              uint32_t repl = frame.current;
              uint32_t cand = nodes[repl].path_tail;
              while (cand != NONE &&
                     nodes[cand].dfs_counter <= nb.dfs_counter &&
                     nodes[cand].dfs_exit    >= nb.dfs_exit)
              {
                repl = cand;
                cand = nodes[repl].path_tail;
              }
              absorb_all_along_path(NONE, frame.current, nodes[repl].path_tail);
              node.path_tail = nodes[repl].path_tail;
            } else {
              // Self-loop — esconde a aresta
              node.effective_degree--;
            }
            --frame.nb_end;
          }
        } else {
          // ── Retorno de recursão ──────────────────────────────────────────
          frame.recursing = false;

          if (nb.low_point == nb.dfs_counter) {
            // Bridge edge — esconde a aresta em ambos os lados
            nb.effective_degree--;
            node.effective_degree--;
          } else {
            // Passos 1.1.1 e 1.1.2 de Tsin
            assert(nb.effective_degree != 1);

            if (nb.effective_degree == 2) {
              nb.is_on_path = false;
            }

            if (node.low_point <= nb.low_point) {
              absorb_all_along_path(frame.current, nb_num, NONE);
            } else {
              node.low_point = nb.low_point;
              absorb_all_along_path(NONE, frame.current, NONE);
              node.is_on_path = true;
              node.path_tail  = nb_num;
            }
          }
          --frame.nb_end;
        }
      } else {
        // ── Nó concluído ───────────────────────────────────────────────────
        node.dfs_exit = dfs_counter;
        stack.pop_back();
      }
    }
  }
}

// ── Variante sem first_root (usa nó 0 como raiz) ──────────────────────────
inline void three_edge_connected_merges(
    size_t node_count,
    const std::function<void(size_t, const std::function<void(size_t)>&)>& for_each_neighbor,
    const std::function<void(size_t, size_t)>& same_component)
{
  three_edge_connected_merges(node_count, 0, for_each_neighbor, same_component);
}

} // namespace ggql::detail
