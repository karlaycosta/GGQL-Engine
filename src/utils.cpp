// utils.cpp — Utilitários de sistema: detecção de instruções SIMD do processador.

#include "ggql/utils.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fmt/core.h>
#include <fmt/color.h>
#include <ranges>
#include <string_view>
#include <vector>

#ifdef __x86_64__
    #include <cpuid.h>
#endif

namespace ggql {

// ─── Formatadores ─────────────────────────────────────────────────────────────

// Substitui ponto decimal por vírgula (pt-BR) numa string já formatada.
static std::string ptbr(std::string s) noexcept {
    for (auto& c : s) {
        if (c == '.') { c = ','; }
    }
    return s;
}

[[nodiscard]] std::string format_size(std::size_t bytes) noexcept {
    if (bytes >= 1024ull * 1024 * 1024) {
        return ptbr(fmt::format("{:.2f} GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)));
    } else if (bytes >= 1024ull * 1024) {
        return ptbr(fmt::format("{:.2f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0)));
    } else if (bytes >= 1024ull) {
        return ptbr(fmt::format("{:.2f} KB", static_cast<double>(bytes) / 1024.0));
    } else {
        return fmt::format("{} B", bytes);
    }
}

[[nodiscard]] std::string format_seconds(double s) noexcept {
    if (s < 1e-6) {
        return ptbr(fmt::format("{:.2f} ns", s * 1e9));
    }
    if (s < 1e-3) {
        return ptbr(fmt::format("{:.2f} μs", s * 1e6));
    }
    if (s < 1.0) {
        return ptbr(fmt::format("{:.1f} ms", s * 1e3));
    }
    if (s < 60.0) {
        return ptbr(fmt::format("{:.2f} s", s));
    }
    return ptbr(fmt::format("{:.2f} min", s / 60.0));
}

// ─── Detecção de instruções SIMD ──────────────────────────────────────────────

struct CpuFeature {
    std::string_view name;
    bool supported;
};

#ifdef __x86_64__

// Descritor constexpr: identifica qual bit de qual leaf/registrador testar.
struct FeatureDesc {
    std::string_view name;
    std::uint32_t leaf;
    std::uint32_t subleaf;
    int reg; // 0=EAX  1=EBX  2=ECX  3=EDX
    int bit;
};

static constexpr auto kFeatureTable = std::to_array<FeatureDesc>({
    // — Leaf 1 EDX —
    {"SSE", 1, 0, 3, 25},
    {"SSE2", 1, 0, 3, 26},
    // — Leaf 1 ECX —
    {"SSE3", 1, 0, 2, 0},
    {"SSSE3", 1, 0, 2, 9},
    {"SSE4.1", 1, 0, 2, 19},
    {"SSE4.2", 1, 0, 2, 20},
    {"POPCNT", 1, 0, 2, 23},
    {"AVX", 1, 0, 2, 28},
    {"FMA", 1, 0, 2, 12},
    {"F16C", 1, 0, 2, 29},
    // — Leaf 7 EBX —
    {"AVX2", 7, 0, 1, 5},
    {"BMI1", 7, 0, 1, 3},
    {"BMI2", 7, 0, 1, 8},
    {"AVX-512F", 7, 0, 1, 16},
    {"AVX-512DQ", 7, 0, 1, 17},
    {"AVX-512BW", 7, 0, 1, 30},
    {"AVX-512VL", 7, 0, 1, 31},
    // — Leaf 0x80000001 ECX (AMD) —
    {"SSE4a", 0x80000001, 0, 2, 6},
});

static std::array<std::uint32_t, 4> cpuid(std::uint32_t leaf, std::uint32_t subleaf = 0) {
    std::array<std::uint32_t, 4> regs{};
    __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
    return regs;
}

std::vector<CpuFeature> detect_cpu_features() {
    auto leaf1 = cpuid(1);
    auto leaf7 = cpuid(7, 0);
    auto max_ext = cpuid(0x80000000)[0];
    auto leaf81 = max_ext >= 0x80000001 ? cpuid(0x80000001) : std::array<std::uint32_t, 4>{};

    auto regs_for = [&](std::uint32_t leaf) -> const auto& {
        if (leaf == 7) {
            return leaf7;
        }
        if (leaf == 0x80000001) {
            return leaf81;
        }
        return leaf1;
    };

    auto view = kFeatureTable | std::views::transform([&](const FeatureDesc& d) -> CpuFeature {
                    return {d.name, static_cast<bool>((regs_for(d.leaf)[d.reg] >> d.bit) & 1)};
                });

    return std::ranges::to<std::vector<CpuFeature>>(view);
}

#else

std::vector<CpuFeature> detect_cpu_features() {
    return {{"(não-x86: detecção indisponível)", false}};
}

#endif

bool has_cpu_feature(std::string_view name) {
    auto features = detect_cpu_features();
    auto it = std::ranges::find(features, name, &CpuFeature::name);
    return it != features.end() && it->supported;
}

void print_cpu_features() {
    fmt::println("Instruções SIMD do processador:");
    for (auto& [name, ok] : detect_cpu_features()) {
        fmt::println("  {}: {}", name, ok ? "sim" : "não");
    }
}

} // namespace ggql
