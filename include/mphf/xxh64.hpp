// xxh64.hpp — Implementação inline minimal do algoritmo XXH64.
//
// Extraído e adaptado de xxHash (https://github.com/Cyan4973/xxHash)
// Autor original: Yann Collet
// Licença: BSD 2-Clause
//
// Apenas XXH64() é fornecido — suficiente para uso com BooPHF/StringViewHasher.
// Para streaming, canonical form ou XXH3/XXH128, use o xxhash.h completo.

#pragma once

#include <cstddef> // size_t
#include <cstdint> // uint8_t, uint32_t, uint64_t
#include <cstring> // memcpy

// ── Primes XXH64 ─────────────────────────────────────────────────────────────
static constexpr uint64_t XXH_PRIME64_1 = 0x9E3779B185EBCA87ULL;
static constexpr uint64_t XXH_PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
static constexpr uint64_t XXH_PRIME64_3 = 0x165667B19E3779F9ULL;
static constexpr uint64_t XXH_PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
static constexpr uint64_t XXH_PRIME64_5 = 0x27D4EB2F165667C5ULL;

// ── Helpers
// ───────────────────────────────────────────────────────────────────

// Leitura não-alinhada portable (evita UB de strict-aliasing)
inline uint64_t xxh_read64(const void *ptr) noexcept {
  uint64_t v;
  __builtin_memcpy(&v, ptr, sizeof(v));
  return v;
}
inline uint32_t xxh_read32(const void *ptr) noexcept {
  uint32_t v;
  __builtin_memcpy(&v, ptr, sizeof(v));
  return v;
}

inline uint64_t xxh_rotl64(uint64_t x, int r) noexcept {
  return (x << r) | (x >> (64 - r));
}

// ── Round e merge
// ─────────────────────────────────────────────────────────────
inline uint64_t xxh64_round(uint64_t acc, uint64_t input) noexcept {
  acc += input * XXH_PRIME64_2;
  acc = xxh_rotl64(acc, 31);
  acc *= XXH_PRIME64_1;
  return acc;
}

inline uint64_t xxh64_merge_round(uint64_t acc, uint64_t val) noexcept {
  val = xxh64_round(0, val);
  acc ^= val;
  acc = acc * XXH_PRIME64_1 + XXH_PRIME64_4;
  return acc;
}

// ── Finalize (avalanche)
// ──────────────────────────────────────────────────────
inline uint64_t xxh64_avalanche(uint64_t h64) noexcept {
  h64 ^= h64 >> 33;
  h64 *= XXH_PRIME64_2;
  h64 ^= h64 >> 29;
  h64 *= XXH_PRIME64_3;
  h64 ^= h64 >> 32;
  return h64;
}

// ── Finalize (tail de até 31 bytes restantes)
// ─────────────────────────────────
inline uint64_t xxh64_finalize(uint64_t h64, const uint8_t *ptr,
                               size_t len) noexcept {
  len &= 31;
  while (len >= 8) {
    uint64_t k1 = xxh64_round(0, xxh_read64(ptr));
    ptr += 8;
    len -= 8;
    h64 ^= k1;
    h64 = xxh_rotl64(h64, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
  }
  if (len >= 4) {
    h64 ^= static_cast<uint64_t>(xxh_read32(ptr)) * XXH_PRIME64_1;
    ptr += 4;
    len -= 4;
    h64 = xxh_rotl64(h64, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
  }
  while (len-- > 0) {
    h64 ^= static_cast<uint64_t>(*ptr++) * XXH_PRIME64_5;
    h64 = xxh_rotl64(h64, 11) * XXH_PRIME64_1;
  }
  return xxh64_avalanche(h64);
}

// ── Ponto de entrada público
// ────────────────────────────────────────────────── Assinatura idêntica à do
// xxhash.h: XXH64(data, len, seed) → uint64_t
inline uint64_t XXH64(const void *input, size_t length,
                      uint64_t seed) noexcept {
  const auto *p = static_cast<const uint8_t *>(input);
  const uint8_t *const end = p + length;
  uint64_t h64;

  if (length >= 32) {
    // Fase de acumulação em 4 lanes (loop principal)
    uint64_t v1 = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
    uint64_t v2 = seed + XXH_PRIME64_2;
    uint64_t v3 = seed;
    uint64_t v4 = seed - XXH_PRIME64_1;

    const uint8_t *const limit = end - 32;
    do {
      v1 = xxh64_round(v1, xxh_read64(p));
      p += 8;
      v2 = xxh64_round(v2, xxh_read64(p));
      p += 8;
      v3 = xxh64_round(v3, xxh_read64(p));
      p += 8;
      v4 = xxh64_round(v4, xxh_read64(p));
      p += 8;
    } while (p <= limit);

    h64 = xxh_rotl64(v1, 1) + xxh_rotl64(v2, 7) + xxh_rotl64(v3, 12) +
          xxh_rotl64(v4, 18);
    h64 = xxh64_merge_round(h64, v1);
    h64 = xxh64_merge_round(h64, v2);
    h64 = xxh64_merge_round(h64, v3);
    h64 = xxh64_merge_round(h64, v4);
  } else {
    h64 = seed + XXH_PRIME64_5;
  }

  h64 += static_cast<uint64_t>(length);
  return xxh64_finalize(h64, p, end - p);
}
