# Instruções de Estilo C++ — ggql_engine

## Chaves obrigatórias em `if`, `else`, `for`, `while`

Sempre usar `{}` em blocos de controle, mesmo com uma única instrução:

```cpp
// ✅ Correto
if (collect_links_for_uf) {
    result.thread_pairs.resize(num_threads);
}

for (auto& v : thread_recs) {
    v.reserve(est_per_thread);
}

// ❌ Errado
if (collect_links_for_uf)
    result.thread_pairs.resize(num_threads);

for (auto& v : thread_recs)
    v.reserve(est_per_thread);
```
