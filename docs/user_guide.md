# Guia do Usuário: GGQL 1.0

Bem-vindo à documentação do **GGQL Engine** — um motor de consulta de grafos de
pangenoma em C++23. Este guia ensina a usar o REPL e a linguagem GGQL.

---

## 1. Iniciando o REPL

```bash
# Ingere um arquivo GFA e abre o REPL
./build/ggql_repl --gfa caminho/para/grafo.gfa --db /tmp/meta.sqlite --threads 8
```

Você verá um banner e o prompt `ggql> `. A qualquer momento digite `.help` para
ver os comandos disponíveis.

**Outros modos:**

```bash
# Apenas SQLite existente (sem topologia GFA)
./build/ggql_repl --db /tmp/meta.sqlite

# Modo batch (entrada via pipe)
printf 'FIND TIP RETURN id\n.quit\n' | ./build/ggql_repl --gfa grafo.gfa --db /tmp/m.sqlite
```

---

## 2. Entidades do Grafo

| Entidade | O que representa |
|---|---|
| `SEGMENT` | Segmento de sequência (contig, unitig) |
| `TIP`     | Nó de ponta — grau de entrada ou saída = 0 |
| `REPEAT`  | Nó com múltiplas saídas (possível repetição) |
| `BUBBLE`  | Par (source, sink) com caminhos alternativos entre eles |
| `EDGE`    | Aresta de sobreposição (Phase 2) |
| `PATH`    | Caminho nomeado (Phase 2) |

---

## 3. Consultas FIND

### 3.1 Consulta simples

```sql
-- Todos os tips do grafo
FIND TIP

-- Apenas tips sem entrada (fontes do grafo)
FIND TIP WHERE in_degree = 0 RETURN id

-- Tips sem saída (sumidouros)
FIND TIP WHERE out_degree = 0 RETURN id
```

### 3.2 Filtro em SEGMENT

```sql
-- Segmentos longos bem cobertos
FIND SEGMENT WHERE length > 50000 AND depth >= 10 RETURN id, name, length, depth

-- Segmentos de baixa cobertura (suspeitos de erro)
FIND SEGMENT WHERE depth < 2 RETURN id, name, length

-- Segmentos por nome exato
FIND SEGMENT WHERE name = "ctg_001a" RETURN id, length, depth

-- Condições OR
FIND SEGMENT WHERE length < 1000 OR length > 500000 RETURN id, name, length
```

### 3.3 Bubbles e Repeats

```sql
-- Bolhas complexas (3+ caminhos alternativos)
FIND BUBBLE WHERE branch_count >= 3 RETURN source, sink, branch_count

-- Todos os repeats com alta bifurcação
FIND REPEAT WHERE out_degree >= 5 RETURN id, out_degree
```

---

## 4. Queries multi-linha

O REPL aceita queries em múltiplas linhas. Termine com `;` ou pressione Enter
em uma linha vazia após a query:

```
ggql> FIND SEGMENT
...>   WHERE length > 30000
...>     AND depth >= 5
...>   RETURN id, name, length;
```

---

## 5. Meta-comandos

| Comando | Descrição |
|---|---|
| `.help` | Exibe referência rápida da linguagem |
| `.stats` | Nós, arestas, segmentos e tempo de ingest |
| `.load <arquivo.gfa>` | Recarrega um novo GFA em runtime |
| `.quit` / `.exit` | Encerra o REPL |

---

## 6. Interpretando os resultados

Os resultados são exibidos em formato tabular ASCII:

```
┌────────┬─────────────┬────────┐
│ id     │ name        │ length │
├────────┼─────────────┼────────┤
│  42    │ ctg_00002a  │  87431 │
│ 137    │ ctg_00008f  │  93201 │
└────────┴─────────────┴────────┘
2 linhas em 0.38ms
```

---

## 7. Gerando GFA sintético

```bash
python3 data/gen_gfa.py <n_segmentos> <saida.gfa>

# Exemplos
python3 data/gen_gfa.py 10000   /tmp/test_10k.gfa
python3 data/gen_gfa.py 1000000 /tmp/bench_1M.gfa
```

O gerador produz GFAs com `SEQ=*` e tag `LN:i:<comprimento>` (formato realista
para hifiasm/verkko/HPRC).

---

## 8. Próximos recursos (Phase 2)

- `FIND EDGE WHERE` — consultas em arestas
- `FIND PATH WHERE crosses(...)` — caminhos que cruzam regiões
- `SEARCH KMER "<seq>"` — localização de k-mers via FM-Index

