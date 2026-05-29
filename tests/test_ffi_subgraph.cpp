// test_ffi_subgraph.cpp — Testes do C12 (subgrafo induzido p/ o GraphView).
//
// Cobre ggql_engine_subgraph_from_result:
//   - two-pass (dims-only → aloca → preenche) com halo_hops=1
//   - halo_hops=0 (só o resultado, sem vizinhos)
//   - arestas não-direcionadas, dedupadas, sem self-loop, em range
//   - is_result: 1 nos primeiros result_count, 0 no halo
//   - truncamento (node_cap estourado → truncated, sem materializar)
//   - códigos de erro: -1 (args), -2 (struct_size), -4 (halo_hops), -5 (buffer)
//
// Requer: chm13_subset_sm.gfa.idx em $GGQL_TEST_IDX ou
//         ../data/chm13_subset_sm.gfa.idx

#include "ffi/ffi.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

static const char* get_idx() {
    const char* env = std::getenv("GGQL_TEST_IDX");
    if (env && *env) { return env; }
    return "../data/chm13_subset_sm.gfa.idx";
}

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FALHA: %s\n", msg);
        std::exit(1);
    }
}

// Lê até `want` ids de segmento via FIND SEGMENT RETURN id.
static std::vector<int64_t> sample_ids(GgqlEngineHandle eng, uint32_t want) {
    GgqlResultHandle r = ggql_engine_execute(eng, "FIND SEGMENT RETURN id", want, 0);
    check(r != nullptr, "FIND SEGMENT RETURN id deve funcionar");
    const int64_t rows = ggql_result_row_count(r);
    std::vector<int64_t> ids;
    ids.reserve(static_cast<size_t>(rows));
    for (int64_t i = 0; i < rows; ++i) {
        check(ggql_result_cell_type(r, static_cast<int32_t>(i), 0) == GGQL_TYPE_INT,
              "col 0 (id) deve ser INT");
        ids.push_back(ggql_result_cell_int(r, static_cast<int32_t>(i), 0));
    }
    ggql_result_free(r);
    return ids;
}

// Valida que `edges` (2×edge_count ints) são pares canônicos lo<hi em range,
// sem self-loop e sem duplicata. Retorna o nº de arestas únicas vistas.
static int64_t validate_edges(const std::vector<int32_t>& edges, int64_t node_count) {
    std::set<std::pair<int32_t, int32_t>> seen;
    for (size_t i = 0; i + 1 < edges.size(); i += 2) {
        const int32_t a = edges[i], b = edges[i + 1];
        check(a >= 0 && a < node_count, "edge endpoint a em range");
        check(b >= 0 && b < node_count, "edge endpoint b em range");
        check(a < b, "edge canônica lo<hi (não-direcionada, sem self-loop)");
        check(seen.insert({a, b}).second, "edge sem duplicata (dedupada)");
    }
    return static_cast<int64_t>(seen.size());
}

int main() {
    const char* idx = get_idx();
    GgqlEngineHandle eng = ggql_engine_load(idx);
    check(eng != nullptr, "engine_load deve funcionar");

    // Confirma topologia disponível (subgraph exige CSR).
    GgqlEngineCapabilities caps{};
    caps.struct_size = sizeof(GgqlEngineCapabilities);
    check(ggql_engine_capabilities(eng, &caps) == 0, "capabilities deve funcionar");
    check(caps.has_topology, "idx de teste precisa ter topologia (CSR)");

    const std::vector<int64_t> ids = sample_ids(eng, 40);
    check(!ids.empty(), "amostra de ids não deve ser vazia");

    // ── 1. halo_hops=1, two-pass completo ────────────────────────────────────
    {
        GgqlSubgraphDims dims{};
        dims.struct_size = sizeof(GgqlSubgraphDims);

        // 1ª passada: dims-only (buffers NULL, caps 0).
        int64_t rc = ggql_engine_subgraph_from_result(
            eng, ids.data(), static_cast<int64_t>(ids.size()),
            /*halo_hops=*/1, /*node_cap=*/0, &dims,
            nullptr, 0, nullptr, 0, nullptr, 0);
        check(rc == 0, "subgraph dims-only deve retornar 0");
        check(!dims.truncated, "sem cap → não truncado");
        check(dims.result_count == static_cast<int64_t>(ids.size()),
              "result_count == nº de ids distintos da amostra");
        check(dims.node_count >= dims.result_count,
              "node_count >= result_count (halo só adiciona)");
        check(dims.edge_count >= 0, "edge_count não-negativo");

        // 2ª passada: aloca pelos dims e preenche.
        std::vector<int32_t> edges(static_cast<size_t>(dims.edge_count) * 2);
        std::vector<int64_t> seg_ids(static_cast<size_t>(dims.node_count));
        std::vector<uint8_t> is_result(static_cast<size_t>(dims.node_count));

        GgqlSubgraphDims dims2{};
        dims2.struct_size = sizeof(GgqlSubgraphDims);
        rc = ggql_engine_subgraph_from_result(
            eng, ids.data(), static_cast<int64_t>(ids.size()),
            1, 0, &dims2,
            edges.data(), static_cast<int64_t>(edges.size()),
            seg_ids.data(), static_cast<int64_t>(seg_ids.size()),
            is_result.data(), static_cast<int64_t>(is_result.size()));
        check(rc == 0, "subgraph fill deve retornar 0");
        check(dims2.node_count == dims.node_count, "dims estáveis entre passadas (node)");
        check(dims2.edge_count == dims.edge_count, "dims estáveis entre passadas (edge)");

        // is_result: primeiros result_count são 1, resto 0.
        for (int64_t i = 0; i < dims2.node_count; ++i) {
            const uint8_t want = (i < dims2.result_count) ? 1 : 0;
            check(is_result[static_cast<size_t>(i)] == want,
                  "is_result: 1 no resultado, 0 no halo");
        }
        // Os primeiros result_count seg_ids são os ids da amostra (em ordem).
        for (size_t i = 0; i < ids.size(); ++i) {
            check(seg_ids[i] == ids[i], "seg_ids[resultado] == ids de entrada (ordem)");
        }
        const int64_t uniq = validate_edges(edges, dims2.node_count);
        check(uniq == dims2.edge_count, "edge_count bate com pares únicos");
        std::fprintf(stderr,
                     "INFO: halo=1 → %lld nós (%lld result + %lld halo), %lld arestas\n",
                     (long long)dims2.node_count, (long long)dims2.result_count,
                     (long long)(dims2.node_count - dims2.result_count),
                     (long long)dims2.edge_count);
    }

    // ── 2. halo_hops=0: só o resultado, sem vizinhos ─────────────────────────
    {
        GgqlSubgraphDims dims{};
        dims.struct_size = sizeof(GgqlSubgraphDims);
        const int64_t rc = ggql_engine_subgraph_from_result(
            eng, ids.data(), static_cast<int64_t>(ids.size()),
            /*halo_hops=*/0, 0, &dims, nullptr, 0, nullptr, 0, nullptr, 0);
        check(rc == 0, "halo=0 dims-only deve retornar 0");
        check(dims.node_count == dims.result_count,
              "halo=0 → node_count == result_count (sem halo)");
        check(dims.result_count == static_cast<int64_t>(ids.size()),
              "halo=0 → result_count == nº de ids");
    }

    // ── 3. Truncamento: node_cap estoura já no resultado ─────────────────────
    {
        GgqlSubgraphDims dims{};
        dims.struct_size = sizeof(GgqlSubgraphDims);
        const int64_t small_cap = 5;
        check(static_cast<int64_t>(ids.size()) > small_cap,
              "amostra precisa ser maior que o cap p/ testar truncamento");
        const int64_t rc = ggql_engine_subgraph_from_result(
            eng, ids.data(), static_cast<int64_t>(ids.size()),
            1, small_cap, &dims, nullptr, 0, nullptr, 0, nullptr, 0);
        check(rc == 0, "truncamento não é erro (retorna 0)");
        check(dims.truncated, "node_cap estourado → truncated=true");
        check(dims.edge_count == 0, "truncado → sem arestas materializadas");
    }

    // ── 4. Seed único + halo: arestas devem conectar a vizinhança ────────────
    // Procura um id cujo halo_hops=1 traga >1 nó (tem vizinhos).
    {
        bool found = false;
        for (int64_t seed : ids) {
            GgqlSubgraphDims dims{};
            dims.struct_size = sizeof(GgqlSubgraphDims);
            const int64_t rc = ggql_engine_subgraph_from_result(
                eng, &seed, 1, 1, 0, &dims, nullptr, 0, nullptr, 0, nullptr, 0);
            check(rc == 0, "seed único deve retornar 0");
            check(dims.result_count == 1, "seed único → result_count=1");
            if (dims.node_count > 1) {
                check(dims.edge_count >= 1,
                      "seed com vizinhos → pelo menos 1 aresta induzida");
                found = true;
                break;
            }
        }
        std::fprintf(stderr, "INFO: seed com vizinhos %s na amostra\n",
                     found ? "encontrado" : "NÃO encontrado (grafo esparso?)");
    }

    // ── 5. Códigos de erro ───────────────────────────────────────────────────
    {
        GgqlSubgraphDims dims{};
        dims.struct_size = sizeof(GgqlSubgraphDims);

        // -1: dims NULL
        check(ggql_engine_subgraph_from_result(
                  eng, ids.data(), 1, 1, 0, nullptr, nullptr, 0, nullptr, 0, nullptr, 0) == -1,
              "dims NULL → -1");
        // -1: seg_ids_in NULL com seg_count>0
        check(ggql_engine_subgraph_from_result(
                  eng, nullptr, 5, 1, 0, &dims, nullptr, 0, nullptr, 0, nullptr, 0) == -1,
              "seg_ids_in NULL com count>0 → -1");
        // -1: engine NULL
        check(ggql_engine_subgraph_from_result(
                  nullptr, ids.data(), 1, 1, 0, &dims, nullptr, 0, nullptr, 0, nullptr, 0) == -1,
              "engine NULL → -1");
        // -2: struct_size insuficiente
        {
            GgqlSubgraphDims stale{};
            stale.struct_size = 4;  // < sizeof(GgqlSubgraphDims)
            check(ggql_engine_subgraph_from_result(
                      eng, ids.data(), 1, 1, 0, &stale, nullptr, 0, nullptr, 0, nullptr, 0) == -2,
                  "struct_size pequeno → -2");
        }
        // -4: halo_hops não suportado
        check(ggql_engine_subgraph_from_result(
                  eng, ids.data(), 1, 2, 0, &dims, nullptr, 0, nullptr, 0, nullptr, 0) == -4,
              "halo_hops=2 → -4");
        // -5: buffer fornecido pequeno demais
        {
            GgqlSubgraphDims d{};
            d.struct_size = sizeof(GgqlSubgraphDims);
            int64_t tiny_ids[1];
            const int64_t rc = ggql_engine_subgraph_from_result(
                eng, ids.data(), static_cast<int64_t>(ids.size()), 1, 0, &d,
                nullptr, 0, tiny_ids, 1, nullptr, 0);
            check(rc == -5, "seg_ids buffer cap=1 < node_count → -5");
        }
    }

    // ── 6. seg_count=0 é válido (subgrafo vazio) ─────────────────────────────
    {
        GgqlSubgraphDims dims{};
        dims.struct_size = sizeof(GgqlSubgraphDims);
        const int64_t rc = ggql_engine_subgraph_from_result(
            eng, nullptr, 0, 1, 0, &dims, nullptr, 0, nullptr, 0, nullptr, 0);
        check(rc == 0, "seg_count=0 → 0 (subgrafo vazio)");
        check(dims.node_count == 0 && dims.edge_count == 0 && dims.result_count == 0,
              "seg_count=0 → tudo zero");
    }

    ggql_engine_destroy(eng);
    std::puts("test_ffi_subgraph: todos os testes passaram.");
    return 0;
}
