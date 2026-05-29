// bench_tokenize — baseline isolado do H2 (custo do scan de token de sequência).
//
// Replica FIELMENTE o loop de S-lines do Pass1 (ingestor.cpp:92-107):
//   consome 'S','\t' → tokenize 2 campos (nome curto + sequência longa) →
//   soma tokens[1].size() → skip_line.
//
// Compara duas implementações do delimitador de campo:
//   SCALAR : next_token byte-a-byte (código atual do ingestor.hpp:328-343).
//   MEMCHR : nome via scalar (curto) + sequência via memchr (SIMD glibc) —
//            o fix proposto no H2. Acha o fim da linha (memchr '\n') uma vez e
//            delimita a sequência com memchr '\t' dentro dela.
//
// Mede sobre o arquivo INTEIRO, cache quente, N iters. Saída = MB/s e
// MB/s só-sequência + speedup. Isola next_token de MPHF/topologia/I/O.
//
// Compilar (mesmas flags do build Release):
//   g++-14 -O3 -march=native -std=c++23 -o bench_tokenize bench_tokenize.cpp
// Uso: bench_tokenize <gfa> [iters=7]

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ── helpers idênticos ao ingestor.hpp ───────────────────────────────────────
static inline const char* skip_line(const char* p, const char* end) noexcept {
    const void* f = ::memchr(p, '\n', static_cast<size_t>(end - p));
    return f ? static_cast<const char*>(f) + 1 : end;
}

// next_token SCALAR — cópia exata do ingestor.hpp:328-343.
static inline std::string_view next_token_scalar(const char*& p, const char* end) noexcept {
    const char* start = p;
    while (p < end && *p != '\t' && *p != '\n' && *p != '\r') {
        ++p;
    }
    std::string_view sv(start, static_cast<size_t>(p - start));
    if (p < end && *p == '\t') {
        ++p;
    }
    return sv;
}

// ── Variante A: tokenize escalar (atual) ─────────────────────────────────────
// Retorna soma de bytes de sequência (tokens[1]) + checksum do nome.
static std::pair<uint64_t, uint64_t> run_scalar(const char* p, const char* end) noexcept {
    uint64_t seq_bytes = 0, name_chk = 0;
    while (p < end) {
        char first = *p;
        if (first == 'S') {
            ++p;
            if (p >= end || *p != '\t') { p = skip_line(p, end); continue; }
            ++p;
            std::string_view name = next_token_scalar(p, end);  // curto
            std::string_view seq  = next_token_scalar(p, end);  // LONGO (hot)
            if (!name.empty()) { name_chk += name.size() + static_cast<unsigned char>(name[0]); }
            seq_bytes += seq.size();
            p = skip_line(p, end);
        } else {
            p = skip_line(p, end);
        }
    }
    return {seq_bytes, name_chk};
}

// ── Variante B: H2 — nome escalar, sequência via memchr ──────────────────────
static std::pair<uint64_t, uint64_t> run_memchr(const char* p, const char* end) noexcept {
    uint64_t seq_bytes = 0, name_chk = 0;
    while (p < end) {
        char first = *p;
        if (first == 'S') {
            ++p;
            if (p >= end || *p != '\t') { p = skip_line(p, end); continue; }
            ++p;
            std::string_view name = next_token_scalar(p, end);  // curto: scalar
            // sequência: acha fim da linha (1 memchr) e delimita com memchr '\t'
            const char* le = static_cast<const char*>(
                ::memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!le) { le = end; }
            const char* tab = static_cast<const char*>(
                ::memchr(p, '\t', static_cast<size_t>(le - p)));
            const char* seq_end = tab ? tab : le;
            std::string_view seq(p, static_cast<size_t>(seq_end - p));
            if (!name.empty()) { name_chk += name.size() + static_cast<unsigned char>(name[0]); }
            seq_bytes += seq.size();
            p = (le < end) ? le + 1 : end;
        } else {
            p = skip_line(p, end);
        }
    }
    return {seq_bytes, name_chk};
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: %s <gfa> [iters=7]\n", argv[0]); return 1; }
    const char* path = argv[1];
    const int iters = (argc >= 3) ? std::atoi(argv[2]) : 7;

    int fd = ::open(path, O_RDONLY);
    if (fd < 0) { std::perror("open"); return 1; }
    struct stat st{};
    ::fstat(fd, &st);
    size_t sz = static_cast<size_t>(st.st_size);
    void* m = ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) { std::perror("mmap"); return 1; }
    ::madvise(m, sz, MADV_WILLNEED);
    const char* data = static_cast<const char*>(m);
    const char* end = data + sz;

    using clk = std::chrono::high_resolution_clock;
    using sec = std::chrono::duration<double>;
    const double mib = static_cast<double>(sz) / (1024.0 * 1024.0);

    std::printf("bench_tokenize · %s · %.0f MiB · iters=%d\n", path, mib, iters);
    std::printf("%-60s\n", "------------------------------------------------------------");

    auto bench = [&](const char* tag, auto fn) {
        uint64_t sb = 0, ck = 0;
        double best = 1e30, total = 0;
        for (int i = 0; i < iters; ++i) {
            auto t0 = clk::now();
            auto [s, c] = fn(data, end);
            double dt = sec(clk::now() - t0).count();
            sb = s; ck = c;
            if (i > 0) { total += dt; if (dt < best) best = dt; }  // descarta iter 0
        }
        double mean = total / (iters - 1);
        std::printf("%-8s  mean=%.4fs  best=%.4fs  %.0f MB/s  (seq=%.0f MB/s)  seqbytes=%lu chk=%lu\n",
                    tag, mean, best, mib / mean, (static_cast<double>(sb) / (1024.0 * 1024.0)) / mean,
                    static_cast<unsigned long>(sb), static_cast<unsigned long>(ck));
        return std::pair<double, uint64_t>{mean, sb};
    };

    auto [m_scalar, sb_scalar] = bench("SCALAR", run_scalar);
    auto [m_memchr, sb_memchr] = bench("MEMCHR", run_memchr);

    std::printf("%-60s\n", "------------------------------------------------------------");
    if (sb_scalar != sb_memchr) {
        std::printf("AVISO: seqbytes divergem (%lu vs %lu) — variantes não equivalentes!\n",
                    static_cast<unsigned long>(sb_scalar), static_cast<unsigned long>(sb_memchr));
    }
    std::printf("speedup (scalar/memchr): %.2f×\n", m_scalar / m_memchr);
    ::munmap(m, sz);
    return 0;
}
