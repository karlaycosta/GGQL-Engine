# TODO — ggql_engine

## Próximos passos

### 1. GGQL Grammar (PEGTL)
- **Arquivo**: `include/ggql/query/ggql_grammar.hpp`
- **PEGTL path**: `third_party/pegtl/include` (já no repo)
- **Sintaxe alvo**:
  ```
  FIND TIP|BUBBLE|REPEAT|SEGMENT
  [WHERE field op value [AND field op value ...]]
  [RETURN field, ...]
  ```
- **Regras PEGTL necessárias**: `rule_find`, `rule_where`, `rule_condition`, `rule_return`, `rule_field`, `rule_op`, `rule_value`
- **Tipos AST**: `FindClause`, `WhereClause`, `Condition`, `ReturnClause`, `GgqlQuery`

### 2. QueryPlanner C++
- **Arquivo**: `include/ggql/query/query_planner.hpp`
- **Input**: AST `GgqlQuery` parseado
- **Passos**:
  1. Verificar tipo de query (TIP / BUBBLE / REPEAT / SEGMENT)
  2. Chamar método correspondente em `GfaCsrTopology` (`find_tips()`, `find_bubbles()`, `find_repeats()`)
  3. Para SEGMENT: usar lookup MPHF + SQLite `WHERE internal_id IN (...)`
  4. Aplicar filtros WHERE sobre os resultados
  5. Retornar campos do RETURN

---

## Limpezas menores (baixa prioridade)

- [ ] Remover includes não usados de `gfa_csr_topology.hpp`: `<execution>`, `<ranges>`, `<cassert>`, `<stdexcept>`, `<string>`
- [ ] Paralelizar `find_tips()` e `find_repeats()` com `std::execution::par_unseq` (fazer junto com QueryPlanner)
- [ ] Substituir `unordered_set` em `find_bubbles()` por bitmap `vector<bool>` (fazer ao expor via GGQL)
- [ ] Adicionar guarda contra ciclos que não convergem dentro de `max_depth` em `find_bubbles()`
