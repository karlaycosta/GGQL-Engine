#pragma once

#include <string>
#include <cstddef>

namespace ggql {

[[nodiscard]] std::string format_size(std::size_t bytes) noexcept;

// Formata duração em segundos usando separador decimal pt-BR (vírgula).
// Exemplos: 0,000042 s → "42 ns" | 0,003 s → "3 ms" | 2,20 s → "2,20 s" | 125 s → "2,08 min"
[[nodiscard]] std::string format_seconds(double s) noexcept;

} // namespace ggql
