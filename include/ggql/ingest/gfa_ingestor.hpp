#pragma once

// GfaIngestorMmapParallel — versão paralela com OpenMP para benchmark de
// performance.
//
// Otimizações implementadas:
//   1. Paralelização com OpenMP (Passada 1 e 2)
//   2. posix_fadvise(POSIX_FADV_SEQUENTIAL) antes do mmap
//   3. MADV_HUGEPAGE para reduzir TLB misses (requer huge pages habilitadas)
//   4. MADV_SEQUENTIAL mantido para readahead
//   5. Chunking do arquivo por thread com alinhamento em '\n'

#include <array>
#include <charconv> // std::from_chars (parse tags RC:i, DP:f)
#include <chrono>
#include <climits>
#include <cstring> // memrchr, memchr
#include <expected>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility> // unreachable
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ggql/ingest/gfa_metadata_store.hpp"
#include "ggql/storage/gfa_csr_topology.hpp"
#include "mphf/BooPHF.h"
#include "mphf/xxh64.hpp"

#ifdef _OPENMP
#include <omp.h>
#endif

namespace ggql {

// ── RAII wrapper para mmap com otimizações
// ─────────────────────────────────────
struct MmapFileParallel {
  const char *data = nullptr;
  size_t size = 0;

  MmapFileParallel() = default;

  // Fábrica retorna expected em vez de throw no construtor (C++23)
  static std::expected<MmapFileParallel, std::string>
  open(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      return std::unexpected("mmap: nao foi possivel abrir: " + path);
    }

    struct stat st{};
    if (::fstat(fd, &st) < 0) {
      ::close(fd);
      return std::unexpected("mmap: fstat falhou: " + path);
    }
    size_t new_size = static_cast<size_t>(st.st_size);
    if (new_size == 0) {
      ::close(fd);
      return MmapFileParallel{};
    }

    // posix_fadvise ANTES do mmap — dica ao kernel de acesso sequencial
    ::posix_fadvise(fd, 0, st.st_size, POSIX_FADV_SEQUENTIAL);

    void *ptr = ::mmap(nullptr, new_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (ptr == MAP_FAILED) {
      return std::unexpected("mmap: mmap() falhou: " + path);
    }

    MmapFileParallel m;
    m.data = static_cast<const char *>(ptr);
    m.size = new_size;

    // MADV_SEQUENTIAL para readahead otimizado
    ::madvise(ptr, new_size, MADV_SEQUENTIAL);

    // MADV_HUGEPAGE para reduzir TLB misses (requer huge pages habilitadas)
#ifdef MADV_HUGEPAGE
    ::madvise(ptr, new_size, MADV_HUGEPAGE);
#endif
    return m;
  }

  ~MmapFileParallel() {
    if (data && size) {
      ::munmap(const_cast<char *>(data), size);
    }
  }

  // Não copiável — recurso único
  MmapFileParallel(const MmapFileParallel &) = delete;
  MmapFileParallel &operator=(const MmapFileParallel &) = delete;
  MmapFileParallel(MmapFileParallel &&o) noexcept : data(o.data), size(o.size) {
    o.data = nullptr;
    o.size = 0;
  }
};

// ── Hasher
// ─────────────────────────────────────────────────────────────────────
struct StringViewHasherParallel {
  uint64_t operator()(std::string_view key, uint64_t seed = 0) const {
    return XXH64(key.data(), key.size(), seed);
  }
};

using boophf_parallel_t =
    boomphf::mphf<std::string_view, StringViewHasherParallel>;

// ── Resultado
// ─────────────────────────────────────────────────────────────────
struct IngestStatsParallel {
  uint64_t segment_count = 0;
  uint64_t link_count = 0;
  uint64_t node_count = 0;
  double elapsed_io1_s = 0.0;
  double elapsed_build_s = 0.0;
  double elapsed_ingest_s = 0.0;
  double elapsed_sqlite_s = 0.0;    // 0 se db_path não fornecido
  double elapsed_topology_s = 0.0;  // 0 se build_topology=false
  double elapsed_total_s = 0.0;
};

// ── GfaIngestorMmapParallel
// ───────────────────────────────────────────────────
class GfaIngestorMmapParallel {
public:
  GfaIngestorMmapParallel() = default;
  ~GfaIngestorMmapParallel() = default;

  // db_path: caminho para o SQLite de metadados (vazio = não grava)
  // build_topology: true → constrói GfaCsrTopology acessível via topology()
  std::expected<IngestStatsParallel, std::string>
  ingest(const std::string &filepath, int num_threads = 1, double gamma = 2.0,
         bool verbose = true, const std::string &db_path = "",
         bool build_topology = false);

  // Retorna ponteiro para a topologia CSR construída (nullptr se build_topology=false)
  GfaCsrTopology* topology() noexcept { return topology_.get(); }

  // Libera na ordem correta: MPHF antes do mmap (que é o backing das
  // string_views).
  void reset() noexcept {
    topology_.reset();  // 0º: topologia antes do mphf (usa node_ids do mphf)
    mphf_.reset();      // 1º: para de usar as string_views do mmap
    mmap_data_.reset(); // 2º: libera o backing
  }

private:
  // INVARIANTE DE LIFETIME: mmap_data_ DEVE ser declarado APÓS mphf_.
  // mphf_ guarda string_views que apontam para mmap_data_->data.
  // Membros C++ são destruídos na ordem INVERSA de declaração —
  // logo mphf_ é destruído 1º e mmap_data_ por último. NÃO reordene.
  std::unique_ptr<boophf_parallel_t> mphf_; // ← destruído primeiro
  std::unique_ptr<MmapFileParallel>
      mmap_data_; // ← destruído depois (backing de mphf_)
  std::unique_ptr<GfaCsrTopology> topology_;

  // ── Utilitários de parsing ────────────────────────────────────────────────

  static const char *skip_line(const char *p, const char *end) noexcept {
    const void *found = ::memchr(p, '\n', static_cast<size_t>(end - p));
    return found ? static_cast<const char *>(found) + 1 : end;
  }

  static std::string_view next_token(const char *&p, const char *end) noexcept {
    const char *start = p;
    // Com -O3 -march=native o GCC vetoriza este loop com AVX2 (32 B/ciclo).
    // Substituir por dois memchr é mais lento: aumenta o custo de chamadas e
    // escaneia os dados duas vezes. A vetorização automática já é ótima aqui.
    while (p < end && *p != '\t' && *p != '\n' && *p != '\r')
      ++p;
    std::string_view sv(start, static_cast<size_t>(p - start));
    if (p < end && *p == '\t')
      ++p;
    return sv;
  }

  static size_t tokenize(const char *&p, const char *end,
                         std::array<std::string_view, 8> &out,
                         size_t max_fields) noexcept {
    size_t n = 0;
#ifdef __GNUC__
#pragma GCC unroll 8
#endif
    while (n < max_fields && p < end && *p != '\n') {
      out[n++] = next_token(p, end);
    }
    p = skip_line(p, end);
    return n;
  }

  // Encontra início de linha mais próximo (evita dividir linha no meio).
  // Usa memrchr (POSIX) — busca reversa com SIMD via libc, ordens de magnitude
  // mais rápido que varredura byte a byte para contigs longos (50k+ bases).
  static const char *align_to_line_start(const char *p,
                                         const char *data_start) noexcept {
    if (p <= data_start) {
      return data_start;
    }

    ptrdiff_t len = p - data_start;
    const void *found = ::memrchr(data_start, '\n', static_cast<size_t>(len));
    if (!found) {
      return data_start;
    }

    return static_cast<const char *>(found) + 1;
  }

  // Passada 1 PARALELA: cada thread processa um chunk
  std::vector<std::string_view> collect_keys_parallel(int num_threads);

  // Passada 2 PARALELA: cada thread processa seu chunk.
  // collect_records=true → preenche o vetor de SegmentRecord para o SQLite.
  // collect_links=true   → preenche o vetor de LinkRecord para topologia CSR.
  struct PassadaDoisResult {
    IngestStatsParallel         stats;
    std::vector<SegmentRecord>  records;
    std::vector<LinkRecord>     links;
  };
  PassadaDoisResult
  process_data_parallel(int num_threads, bool collect_records, bool collect_links);

  // Parseia tags opcionais GFA (formato TAG:TYPE:VALUE) a partir de start_idx.
  // Preenche out_rc (RC:i), out_dp (DP:f) e out_ln (LN:i) se presentes.
  static void parse_optional_tags(const std::string_view *toks, size_t ntok,
                                  size_t start_idx, int32_t &out_rc,
                                  float &out_dp,
                                  uint32_t &out_ln) noexcept {
    for (size_t i = start_idx; i < ntok; ++i) {
      std::string_view sv = toks[i];
      // Formato mínimo: XX:Y:V (5 chars)
      if (sv.size() < 5 || sv[2] != ':' || sv[4] != ':') {
        continue;
      }

      const char type = sv[3];
      std::string_view val = sv.substr(5);
      if (sv[0] == 'R' && sv[1] == 'C' && type == 'i') {
        int32_t v{};
        if (std::from_chars(val.data(), val.data() + val.size(), v).ec ==
            std::errc{}) {
          out_rc = v;
        }
      } else if (sv[0] == 'D' && sv[1] == 'P' && type == 'f') {
        float v{};
        if (std::from_chars(val.data(), val.data() + val.size(), v).ec ==
            std::errc{}) {
          out_dp = v;
        }
      } else if (sv[0] == 'L' && sv[1] == 'N' && type == 'i') {
        // LN:i — comprimento explícito (presente quando SEQ é '*')
        uint32_t v{};
        if (std::from_chars(val.data(), val.data() + val.size(), v).ec ==
            std::errc{}) {
          out_ln = v;
        }
      }
    }
  }
};

// ── Implementações
// ────────────────────────────────────────────────────────────

inline std::vector<std::string_view>
GfaIngestorMmapParallel::collect_keys_parallel(int num_threads) {
  const size_t total_size = mmap_data_->size;
  const char *data_start = mmap_data_->data;
  const char *data_end = data_start + total_size;

  // Pre-aloca vetor global; limitado ao tamanho do arquivo para evitar OOM
  std::vector<std::string_view> keys;
  keys.reserve(std::min(total_size / 100, size_t(1 << 27)));

  // Cada thread coleta localmente, depois merge serial
  std::vector<std::vector<std::string_view>> thread_keys(num_threads);

  // chunk_size pré-calculado fora da região paralela — evita divisão redundante
  // por thread
  const size_t chunk_size = total_size / static_cast<size_t>(num_threads);

#pragma omp parallel num_threads(num_threads)
  {
    int tid = 0;
#ifdef _OPENMP
    tid = omp_get_thread_num();
#endif
    const char *chunk_start = data_start + tid * chunk_size;
    const char *chunk_end =
        (tid == num_threads - 1) ? data_end : (chunk_start + chunk_size);

    // Alinha chunk_start para início de linha (exceto thread 0)
    if (tid > 0) {
      chunk_start = align_to_line_start(chunk_start, data_start);
    }

    // Thread N processa até encontrar o início do chunk N+1
    // (garante que nenhuma linha seja perdida)
    if (tid < num_threads - 1) {
      const char *next_chunk = data_start + (tid + 1) * chunk_size;
      chunk_end = align_to_line_start(next_chunk, data_start);
    }

    // Reserva local (estimativa)
    // Heurístico: ~100 bytes/linha para arquivos sintéticos curtos,
    // mas grafos reais (HPRC) têm sequências longas (~500 bytes/segmento).
    // Limita em 15M/thread para evitar ~7.5 GB de desperdício em arquivos
    // grandes.
    thread_keys[tid].reserve(std::min(chunk_size / 100, size_t(15'000'000)));

    const char *p = chunk_start;
    std::array<std::string_view, 8> tokens;

    while (p < chunk_end) {
      if (*p != 'S') {
        p = skip_line(p, chunk_end);
        continue;
      }
      ++p;
      if (p >= chunk_end || *p != '\t') {
        p = skip_line(p, chunk_end);
        continue;
      }
      ++p;

      // tokenize com max_fields=2: lê ID + sequência de DNA.
      // A leitura da sequência é um efeito colateral útil: aquece o page cache
      // para a Passada 2, que lê o mesmo arquivo logo em seguida.
      // Trocar por extração direta do ID piora a Passada 2 em ~6 s (medido).
      size_t ntok = tokenize(p, chunk_end, tokens, 2);
      if (ntok >= 1 && !tokens[0].empty()) {
        thread_keys[tid].push_back(tokens[0]);
      }
    }
  }

  // Merge serial (mais rápido que contenção em push_back global)
  for (auto &local : thread_keys) {
    keys.insert(keys.end(), local.begin(), local.end());
  }

  return keys;
}

inline GfaIngestorMmapParallel::PassadaDoisResult
GfaIngestorMmapParallel::process_data_parallel(int num_threads,
                                               bool collect_records,
                                               bool collect_links) {
  const size_t total_size = mmap_data_->size;
  const char *data_start = mmap_data_->data;
  const char *data_end = data_start + total_size;

  // Contadores locais por thread — alignas(64) elimina false sharing.
  struct alignas(64) Counter {
    uint64_t value = 0;
  };
  std::vector<Counter> local_segments(num_threads);
  std::vector<Counter> local_links(num_threads);

  // Registros de segmento por thread — alocados apenas se collect_records=true.
  std::vector<std::vector<SegmentRecord>> thread_recs(
      collect_records ? static_cast<size_t>(num_threads) : 0);
  if (collect_records) {
    // Estimativa: ~500 bytes/segmento no HPRC; limita a 15M/thread
    const size_t est_per_thread =
        std::min(total_size / 500 / static_cast<size_t>(num_threads),
                 size_t(15'000'000));
    for (auto &v : thread_recs)
      v.reserve(est_per_thread);
  }

  // Links por thread — alocados apenas se collect_links=true.
  std::vector<std::vector<LinkRecord>> thread_links(
      collect_links ? static_cast<size_t>(num_threads) : 0);
  if (collect_links) {
    const size_t est_links =
        std::min(total_size / 200 / static_cast<size_t>(num_threads),
                 size_t(20'000'000));
    for (auto &v : thread_links)
      v.reserve(est_links);
  }

  const size_t chunk_size_p2 = total_size / static_cast<size_t>(num_threads);

#pragma omp parallel num_threads(num_threads)
  {
    int tid = 0;
#ifdef _OPENMP
    tid = omp_get_thread_num();
#endif
    const char *chunk_start = data_start + tid * chunk_size_p2;
    const char *chunk_end =
        (tid == num_threads - 1) ? data_end : (chunk_start + chunk_size_p2);

    if (tid > 0)
      chunk_start = align_to_line_start(chunk_start, data_start);
    if (tid < num_threads - 1) {
      const char *next_chunk = data_start + (tid + 1) * chunk_size_p2;
      chunk_end = align_to_line_start(next_chunk, data_start);
    }

    const char *p = chunk_start;
    std::array<std::string_view, 8> tokens;

    auto lookup = [&](std::string_view sv) -> uint64_t {
      return mphf_->lookup(sv);
    };

    while (p < chunk_end) {
      char first = *p;
      ++p;

      if (first == 'S') {
        if (p >= chunk_end || *p != '\t') {
          p = skip_line(p, chunk_end);
          continue;
        }
        ++p;
        // 8 campos: [0]=ID [1]=SEQ [2..7]=tags opcionais (RC:i, DP:f, ...)
        size_t ntok = tokenize(p, chunk_end, tokens, 8);
        if (ntok < 2 || tokens[0].empty()) {
          continue;
        }

        uint64_t id = lookup(tokens[0]);
        if (id == ULLONG_MAX) {
          continue;
        }

        ++local_segments[tid].value;

        if (collect_records) {
          SegmentRecord rec;
          rec.internal_id = id;
          rec.original_id = std::string(tokens[0]);
          // Se a sequência é '*', tenta ler o comprimento da tag LN:i
          uint32_t ln_tag = 0;
          parse_optional_tags(tokens.data(), ntok, 2, rec.read_count,
                              rec.depth, ln_tag);
          rec.length = (tokens[1] == "*")
              ? ln_tag
              : static_cast<uint32_t>(tokens[1].size());
          thread_recs[static_cast<size_t>(tid)].push_back(std::move(rec));
        }

      } else if (first == 'L') {
        if (p >= chunk_end || *p != '\t') {
          p = skip_line(p, chunk_end);
          continue;
        }
        ++p;
        size_t ntok = tokenize(p, chunk_end, tokens, 4);
        if (ntok < 4 || tokens[0].empty() || tokens[2].empty()) {
          continue;
        }

        uint64_t from_id = lookup(tokens[0]);
        uint64_t to_id = lookup(tokens[2]);
        if (from_id == ULLONG_MAX || to_id == ULLONG_MAX) {
          continue;
        }

        ++local_links[tid].value;

        if (collect_links) {
          char from_orient = (!tokens[1].empty()) ? tokens[1][0] : '+';
          char to_orient   = (!tokens[3].empty()) ? tokens[3][0] : '+';
          thread_links[static_cast<size_t>(tid)].push_back(
              LinkRecord::make(from_id, from_orient, to_id, to_orient));
        }

      } else {
        p = skip_line(p, chunk_end);
      }
    }
  }

  // Reduce: soma contadores
  IngestStatsParallel stats;
  for (int i = 0; i < num_threads; ++i) {
    stats.segment_count += local_segments[i].value;
    stats.link_count += local_links[i].value;
  }

  // Merge serial dos registros por thread (SQLite não aceita escritores
  // paralelos)
  std::vector<SegmentRecord> all_records;
  if (collect_records) {
    all_records.reserve(stats.segment_count);
    for (auto &v : thread_recs) {
      all_records.insert(all_records.end(), std::make_move_iterator(v.begin()),
                         std::make_move_iterator(v.end()));
    }
  }

  // Merge serial dos links
  std::vector<LinkRecord> all_links;
  if (collect_links) {
    all_links.reserve(stats.link_count);
    for (auto &v : thread_links) {
      all_links.insert(all_links.end(), std::make_move_iterator(v.begin()),
                       std::make_move_iterator(v.end()));
    }
  }

  return PassadaDoisResult{stats, std::move(all_records), std::move(all_links)};
}

inline std::expected<IngestStatsParallel, std::string>
GfaIngestorMmapParallel::ingest(const std::string &filepath, int num_threads,
                                double gamma, bool verbose,
                                const std::string &db_path,
                                bool build_topology) {
  using clock = std::chrono::high_resolution_clock;
  using sec = std::chrono::duration<double>;

  if (num_threads <= 0) {
    return std::unexpected("num_threads deve ser positivo");
  }

#ifdef _OPENMP
  if (verbose) {
    std::print("[GfaIngestorMmapParallel] OpenMP habilitado: {} threads\n",
               num_threads);
  }

#else
  if (verbose) {
    std::print(
        "[GfaIngestorMmapParallel] OpenMP NAO habilitado — rodando serial\n");
  }

  num_threads = 1;
#endif

  // ── mmap + otimizações ────────────────────────────────────────────────────
  auto mmap_res = MmapFileParallel::open(filepath);
  if (!mmap_res) {
    return std::unexpected(mmap_res.error());
  }
  mmap_data_ = std::make_unique<MmapFileParallel>(std::move(*mmap_res));
  if (verbose) {
    std::print("  Arquivo mapeado: {:.1f} MB\n",
               mmap_data_->size / (1024.0 * 1024.0));
  }

  // ── Passada 1 PARALELA ─────────────────────────────────────────────────
  if (verbose) {
    std::print(
        "[GfaIngestorMmapParallel] Passada 1 PARALELA: coletando IDs...\n");
  }
  auto t0 = clock::now();
  auto keys = collect_keys_parallel(num_threads);
  auto t_io1 = clock::now();
  if (keys.empty()) {
    return std::unexpected("Nenhum segmento encontrado em: " + filepath);
  }

  // ── Build MPHF ────────────────────────────────────────────────────────────
  if (verbose) {
    std::print("  {} nos encontrados. Construindo MPHF...\n", keys.size());
  }
  mphf_ = std::make_unique<boophf_parallel_t>(
      keys.size(), keys, num_threads, gamma,
      /*writeEach=*/false, /*progress=*/false);
  auto t_built = clock::now();
  if (verbose) {
    std::print("  MPHF pronto: {:.5f} bits/chave, {:.4f} MB total.\n",
               static_cast<double>(mphf_->totalBitSize()) /
                   static_cast<double>(keys.size()),
               static_cast<double>(mphf_->totalBitSize()) / 8.0 /
                   (1024.0 * 1024.0));
    }

  {
    std::vector<std::string_view> tmp;
    keys.swap(tmp);
  }

  // ── Passada 2 PARALELA ─────────────────────────────────────────────────
  const bool write_db = !db_path.empty();
  if (verbose) {
    std::string suffix;
    if (write_db) suffix += " + metadados";
    if (build_topology) suffix += " + links para topologia";
    std::print("[GfaIngestorMmapParallel] Passada 2 PARALELA: processando"
               "{}...\n", suffix);
  }
  auto [stats, records, links] =
      process_data_parallel(num_threads, write_db, build_topology);
  auto t1 = clock::now();

  stats.elapsed_io1_s = sec(t_io1 - t0).count();
  stats.elapsed_build_s = sec(t_built - t_io1).count();
  stats.elapsed_ingest_s = sec(t1 - t_built).count();
  stats.node_count = mphf_->nbKeys();

  // ── Flush SQLite (serial) ─────────────────────────────────────────────
  if (write_db) {
    if (verbose)
      std::print(
          "[GfaIngestorMmapParallel] Gravando {} segmentos no SQLite: {}\n",
          records.size(), db_path);
    auto t_db0 = clock::now();
    GfaMetadataStore store;
    if (auto r = store.open(db_path); !r)
      return std::unexpected(r.error());
    if (auto r = store.flush_segments(records); !r)
      return std::unexpected(r.error());
    auto t_db1 = clock::now();
    stats.elapsed_sqlite_s = sec(t_db1 - t_db0).count();
    if (verbose)
      std::print("  SQLite flush : {:.4f} s\n", stats.elapsed_sqlite_s);
  }

  // ── Build GfaCsrTopology ─────────────────────────────────────────────
  if (build_topology) {
    // Libera páginas do mmap do page cache antes de alocar os arrays da
    // topologia. Após a passada 2, o arquivo não é mais lido — o mmap
    // continua montado (o mphf_ guarda string_views), mas as páginas físicas
    // podem ser evictadas. Isso reduz o RSS pico em ~44 GB durante o radix
    // sort + alocação dos arrays E_fwd/E_bwd.
    if (mmap_data_)
      ::madvise(const_cast<char*>(mmap_data_->data), mmap_data_->size, MADV_DONTNEED);

    if (verbose)
      std::print("[GfaIngestorMmapParallel] Construindo CSR topology ({} links)...\n",
                 links.size());
    auto t_topo0 = clock::now();
    topology_ = std::make_unique<GfaCsrTopology>();
    topology_->build(mphf_->nbKeys(), std::move(links), num_threads);
    auto t_topo1 = clock::now();
    stats.elapsed_topology_s = sec(t_topo1 - t_topo0).count();
    if (verbose)
      std::print("  CSR topology : {:.4f} s  ({:.1f} MB)\n",
                 stats.elapsed_topology_s,
                 topology_->memory_bytes() / (1024.0 * 1024.0));
  }

  auto t_final = clock::now();
  stats.elapsed_total_s = sec(t_final - t0).count();

  if (verbose) {
    std::print("[GfaIngestorMmapParallel] Concluido em {:.4f} s\n",
               stats.elapsed_total_s);
    std::print("  I/O passada 1: {:.5f} s\n", stats.elapsed_io1_s);
    std::print("  Build MPHF   : {:.5f} s\n", stats.elapsed_build_s);
    std::print("  I/O passada 2: {:.5f} s\n", stats.elapsed_ingest_s);
    if (write_db)
      std::print("  SQLite flush : {:.5f} s\n", stats.elapsed_sqlite_s);
    if (build_topology)
      std::print("  CSR topology : {:.5f} s\n", stats.elapsed_topology_s);
    std::print("  Segmentos    : {}\n", stats.segment_count);
    std::print("  Links        : {}\n", stats.link_count);
    std::print("  Nos unicos   : {}\n", stats.node_count);
  }

  return stats;
}

} // namespace ggql
