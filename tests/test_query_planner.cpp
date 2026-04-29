// test_query_planner.cpp — Testes unitários de QueryPlanner::execute().
// Verifica: FIND TIP, FIND REPEAT, filtros WHERE, projeção RETURN, erros.

#include "ggql/query/query_planner.hpp"
#include "ggql/query/ast.hpp"
#include "ggql/storage/topology.hpp"
#include "ggql/storage/topology_types.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>

// Grafo em diamante:
//   0 → 1 (+/+),  0 → 2 (+/+)
//   1 → 3 (+/+),  2 → 3 (+/+)
//
// Graus:  nó 0: in=0 out=2   nó 1: in=1 out=1
//         nó 2: in=1 out=1   nó 3: in=2 out=0
//
// Tips:   {0 (in=0), 3 (out=0)}
// Repeats (out>=2): {0}

static ggql::GfaCsrTopology build_diamond() {
    using LR = ggql::LinkRecord;
    std::vector<LR> links = {
        LR::make(0, '+', 1, '+'),
        LR::make(0, '+', 2, '+'),
        LR::make(1, '+', 3, '+'),
        LR::make(2, '+', 3, '+'),
    };
    ggql::GfaCsrTopology topo;
    topo.build(4, std::move(links), 1);
    return topo;
}

static ggql::WhereClause make_where(std::string field, ggql::Operator op, int64_t val) {
    ggql::WhereClause wc;
    wc.conditions.push_back(ggql::CompareCondition{std::move(field), op, val});
    return wc;
}

// ── FIND TIP ─────────────────────────────────────────────────────────────────

static void test_find_tip_all() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::TIP;

    auto res = planner.execute(q);
    assert(res.has_value());
    assert(res->total_hits == 2);
    assert(res->rows.size() == 2);

    // Colunas padrão: id, in_degree, out_degree
    assert(res->columns.size() == 3);
}

static void test_find_tip_where_out_zero() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::TIP;
    q.where  = make_where("out_degree", ggql::Operator::EQ, 0);

    auto res = planner.execute(q);
    assert(res.has_value());
    assert(res->rows.size() == 1);
    assert(std::get<int64_t>(res->rows[0].at("id")) == 3);
    assert(std::get<int64_t>(res->rows[0].at("in_degree")) == 2);
}

static void test_find_tip_where_in_zero() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::TIP;
    q.where  = make_where("in_degree", ggql::Operator::EQ, 0);

    auto res = planner.execute(q);
    assert(res.has_value());
    assert(res->rows.size() == 1);
    assert(std::get<int64_t>(res->rows[0].at("id")) == 0);
    assert(std::get<int64_t>(res->rows[0].at("out_degree")) == 2);
}

static void test_find_tip_where_no_match() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::TIP;
    // Nenhum tip tem out_degree >= 10
    q.where  = make_where("out_degree", ggql::Operator::GEQ, 10);

    auto res = planner.execute(q);
    assert(res.has_value());
    assert(res->rows.empty());
    assert(res->total_hits == 0);
}

// ── FIND REPEAT ──────────────────────────────────────────────────────────────

static void test_find_repeat_all() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::REPEAT;

    auto res = planner.execute(q);
    assert(res.has_value());
    assert(res->rows.size() == 1);
    assert(std::get<int64_t>(res->rows[0].at("id")) == 0);
    assert(std::get<int64_t>(res->rows[0].at("out_degree")) == 2);
}

static void test_find_repeat_where_no_match() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::REPEAT;
    // Nó 0 (único repeat) tem in_degree=0, não >=1
    q.where  = make_where("in_degree", ggql::Operator::GEQ, 1);

    auto res = planner.execute(q);
    assert(res.has_value());
    assert(res->rows.empty());
}

// ── Projeção RETURN ───────────────────────────────────────────────────────────

static void test_return_fields_single_column() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity        = ggql::EntityType::REPEAT;
    q.return_fields = {"id"};

    auto res = planner.execute(q);
    assert(res.has_value());
    assert(res->columns.size() == 1);
    assert(res->columns[0] == "id");
    assert(res->rows.size() == 1);
    assert(res->rows[0].count("id") == 1);
}

static void test_unknown_return_field_error() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity        = ggql::EntityType::TIP;
    q.return_fields = {"campo_inexistente"};

    auto res = planner.execute(q);
    assert(!res.has_value());
    assert(!res.error().empty());
}

static void test_unknown_where_field_error() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::TIP;
    q.where  = make_where("campo_inexistente", ggql::Operator::EQ, 0);

    auto res = planner.execute(q);
    assert(!res.has_value());
}

// ── Sem backend ───────────────────────────────────────────────────────────────

static void test_null_topology_error() {
    ggql::QueryPlanner planner(nullptr);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::TIP;

    auto res = planner.execute(q);
    assert(!res.has_value());
    assert(!res.error().empty());
}

static void test_null_metadata_segment_error() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo, nullptr);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::SEGMENT;

    auto res = planner.execute(q);
    assert(!res.has_value());
}

// ── Phase-2 not-implemented ───────────────────────────────────────────────────

static void test_find_edge_not_implemented() {
    auto topo = build_diamond();
    ggql::QueryPlanner planner(&topo);

    ggql::FindQuery q;
    q.entity = ggql::EntityType::EDGE;

    auto res = planner.execute(q);
    assert(!res.has_value());
}

int main() {
    test_find_tip_all();
    test_find_tip_where_out_zero();
    test_find_tip_where_in_zero();
    test_find_tip_where_no_match();
    test_find_repeat_all();
    test_find_repeat_where_no_match();
    test_return_fields_single_column();
    test_unknown_return_field_error();
    test_unknown_where_field_error();
    test_null_topology_error();
    test_null_metadata_segment_error();
    test_find_edge_not_implemented();
    return EXIT_SUCCESS;
}
