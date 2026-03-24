# Gramática Formal GGQL 1.0

Este documento descreve a gramática formal da linguagem **GGQL** (_Genomic
Graph Query Language_), implementada em `include/ggql/query/ggql_grammar.hpp`
utilizando PEGTL (_Parsing Expression Grammar Template Library_).

---

## Estrutura Geral

```
query     ← find_stmt | search_stmt
```

### `find_stmt`
Busca entidades do grafo com filtros e projeção de campos.

```
find_stmt     ← FIND entity_type [ where_clause ] [ return_clause ]
entity_type   ← SEGMENT | EDGE | PATH | BUBBLE | TIP | REPEAT
```

### `search_stmt`
Busca de k-mer por sequência (Phase 2 — FM-Index).

```
search_stmt   ← SEARCH KMER str_value [ return_clause ]
```

---

## Cláusula WHERE

```
where_clause  ← WHERE condition { logical_op condition }*
condition     ← condition_expr | fn_crosses | fn_contains
condition_expr← field op value
fn_crosses    ← crosses ( identifier )
fn_contains   ← contains_kmer ( str_value )
logical_op    ← AND | OR
```

**Avaliação**: esquerda para direita, sem precedência entre AND e OR.
Use parênteses de agrupamento se precisar de precedência explícita (Phase 2).

---

## Operadores

| Operador | Significado |
|---|---|
| `=`  | igual |
| `!=` | diferente |
| `<`  | menor que |
| `>`  | maior que |
| `<=` | menor ou igual |
| `>=` | maior ou igual |

---

## Valores

```
value    ← int_value | str_value
int_value← ['-'] digit+
str_value← '"' [^"\n]* '"'
```

---

## Cláusula RETURN

```
return_clause ← RETURN field { ',' field }*
field         ← identifier        (não pode ser keyword reservada)
```

Se `RETURN` for omitido, todos os campos do schema da entidade são retornados.

---

## Campos por Entidade

| Entidade | Campos disponíveis |
|---|---|
| `TIP`     | `id`, `in_degree`, `out_degree` |
| `REPEAT`  | `id`, `in_degree`, `out_degree` |
| `BUBBLE`  | `source`, `sink`, `branch_count` |
| `SEGMENT` | `id`, `name`, `length`, `read_count`, `depth` |

---

## Keywords Reservadas

`FIND` `SEARCH` `KMER` `WHERE` `RETURN` `AND` `OR`
`SEGMENT` `EDGE` `PATH` `BUBBLE` `TIP` `REPEAT`
`crosses` `contains_kmer`

Uma keyword não pode ser usada como nome de campo.

---

## Exemplos

```sql
FIND TIP RETURN id
FIND TIP WHERE in_degree = 0 RETURN id, out_degree
FIND REPEAT WHERE out_degree >= 3 RETURN id, out_degree
FIND BUBBLE WHERE branch_count >= 2 RETURN source, sink, branch_count
FIND SEGMENT WHERE length > 50000 RETURN id, name, length
FIND SEGMENT WHERE length > 10000 AND depth >= 5 RETURN name, length
FIND SEGMENT WHERE depth < 2 OR length > 100000 RETURN id, name
FIND SEGMENT WHERE name = "ctg_001a" RETURN id, length, depth
```

---

## Implementação (PEGTL)

A gramática usa o template `kw<S>` que garante borda de palavra para keywords —
impede que `FIND` case em `FINDER`, por exemplo.

O estado incremental (`ParseState`) acumula campos em variáveis `tmp_*` que são
promovidos para os tipos AST (`FindQuery`, `WhereClause`, etc.) nas ações finais
de cada sub-regra.

API pública:
```cpp
#include "ggql/query/ggql_grammar.hpp"

std::expected<ggql::GgqlQuery, std::string> result = ggql::grammar::parse_ggql(input);
```

