#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ggql {

// ─── Formatadores ─────────────────────────────────────────────────────────────

[[nodiscard]] std::string format_size(std::size_t bytes) noexcept;

// Formata duração em segundos usando separador decimal pt-BR (vírgula).
// Exemplos: 0,000042 s → "42 ns" | 0,003 s → "3 ms" | 2,20 s → "2,20 s" | 125 s → "2,08 min"
[[nodiscard]] std::string format_seconds(double s) noexcept;

// ─── Detecção de instruções SIMD do processador ──────────────────────────────
// Em hosts não-x86 (ARM, etc.), detect_cpu_features() retorna um único item
// placeholder indicando indisponibilidade.

struct CpuFeature {
    std::string_view name;
    bool supported;
};

[[nodiscard]] std::vector<CpuFeature> detect_cpu_features();
[[nodiscard]] bool has_cpu_feature(std::string_view name);
void print_cpu_features();

} // namespace ggql
