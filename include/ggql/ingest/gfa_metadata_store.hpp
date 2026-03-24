#pragma once

// GfaMetadataStore — armazena metadados de segmentos GFA no SQLite.
//
// Design:
//   - internal_id é o valor EXATO retornado por mphf_->lookup(), garantindo
//     o mapeamento 1:1 entre RAM (MPHF/SDSL) e disco (SQLite) em O(1).
//   - A sequência de DNA NÃO é armazenada aqui — fica na estrutura SDSL
//     (Árvore Wavelet) na RAM, evitando duplicação de dezenas de GB.
//   - Escrita em batch com transação única + PRAGMAs de performance.
//   - Thread-safety: collect_segment() é chamado nas threads OpenMP,
//     mas flush_all() é sempre serial (SQLite não suporta escritores
//     concorrentes mesmo em WAL).

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

namespace ggql {

// ── Registro por segmento ─────────────────────────────────────────────────
struct SegmentRecord {
  uint64_t    internal_id  = 0;   // mphf_->lookup(original_id) — índice RAM
  std::string original_id;        // nome do nó no GFA (ex: "ctg123")
  uint32_t    length       = 0;   // tamanho da sequência em pares de base
  int32_t     read_count   = -1;  // tag RC:i (-1 = ausente)
  float       depth        = -1.f;// tag DP:f (-1 = ausente)
};

// ── GfaMetadataStore ─────────────────────────────────────────────────────
class GfaMetadataStore {
public:
  GfaMetadataStore() = default;
  ~GfaMetadataStore() { close(); }

  // Não copiável
  GfaMetadataStore(const GfaMetadataStore &) = delete;
  GfaMetadataStore &operator=(const GfaMetadataStore &) = delete;

  // Abre (ou cria) o banco e aplica PRAGMAs + schema
  std::expected<void, std::string> open(const std::string &db_path);

  // Grava um lote de SegmentRecord em transação única.
  // Deve ser chamado após juntar todos os registros das threads (serial).
  std::expected<void, std::string>
  flush_segments(std::span<const SegmentRecord> records);

  void close() noexcept;

private:
  sqlite3      *db_   = nullptr;
  sqlite3_stmt *stmt_ = nullptr;

  std::expected<void, std::string> apply_pragmas();
  std::expected<void, std::string> create_schema();
  std::expected<void, std::string> prepare_insert();

  // Executa sql simples sem bind (para PRAGMAs e DDL)
  std::expected<void, std::string> exec(const char *sql) noexcept;
};

// ── Implementações ────────────────────────────────────────────────────────

inline std::expected<void, std::string>
GfaMetadataStore::exec(const char *sql) noexcept {
  char *errmsg = nullptr;
  int rc = ::sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    std::string msg = errmsg ? errmsg : "sqlite3_exec falhou";
    ::sqlite3_free(errmsg);
    return std::unexpected(msg);
  }
  return {};
}

inline std::expected<void, std::string>
GfaMetadataStore::open(const std::string &db_path) {
  int rc = ::sqlite3_open(db_path.c_str(), &db_);
  if (rc != SQLITE_OK) {
    std::string msg = ::sqlite3_errmsg(db_);
    ::sqlite3_close(db_);
    db_ = nullptr;
    return std::unexpected("sqlite3_open falhou: " + msg);
  }

  if (auto r = apply_pragmas(); !r) return r;
  if (auto r = create_schema(); !r) return r;
  if (auto r = prepare_insert(); !r) return r;
  return {};
}

inline std::expected<void, std::string> GfaMetadataStore::apply_pragmas() {
  // page_size DEVE ser definido antes da primeira tabela ser criada.
  // 32 KB de página reduz o número de páginas B-Tree à metade vs 4KB padrão,
  // diminuindo o custo de maintenão do índice durante o bulk insert.
  if (auto r = exec("PRAGMA page_size = 32768;"); !r)      return r;
  // OFF: sem journal nem WAL — zero overhead de log de transação.
  // Aceitável porque o banco pode ser recriado integralmente a partir do GFA.
  if (auto r = exec("PRAGMA journal_mode = OFF;"); !r)     return r;
  // OFF: elimina qualquer fsync restante.
  if (auto r = exec("PRAGMA synchronous = OFF;"); !r)      return r;
  // EXCLUSIVE: evita lock files e overhead de concorrência (uso único).
  if (auto r = exec("PRAGMA locking_mode = EXCLUSIVE;"); !r) return r;
  // 2 GB de cache de página em memória — reduz I/O na B-Tree de índice.
  if (auto r = exec("PRAGMA cache_size = -2097152;"); !r)  return r;
  // Arquivos temporários (sort, index build) ficam na RAM.
  if (auto r = exec("PRAGMA temp_store = MEMORY;"); !r)    return r;
  return {};
}

inline std::expected<void, std::string> GfaMetadataStore::create_schema() {
  // internal_id = INTEGER PRIMARY KEY → alias direto do ROWID (B-Tree O(1))
  // Atribuímos EXPLICITAMENTE o valor do MPHF — NÃO usamos autoincrement,
  // pois a ordem de inserção difere do índice do MPHF.
  // Índice secundário criado JUNTO com a tabela: mantê-lo inline é mais rápido
  // do que criá-lo após o bulk insert (evita releitura de 85M linhas + sort).
  return exec(R"sql(
    CREATE TABLE IF NOT EXISTS segments (
        internal_id  INTEGER PRIMARY KEY,
        original_id  TEXT    NOT NULL,
        length       INTEGER NOT NULL,
        read_count   INTEGER,          -- tag RC:i do GFA (NULL = ausente)
        depth        REAL              -- tag DP:f do GFA (NULL = ausente)
    );
    CREATE UNIQUE INDEX IF NOT EXISTS idx_segments_orig
        ON segments (original_id);
  )sql");
}

inline std::expected<void, std::string> GfaMetadataStore::prepare_insert() {
  // INSERT simples (sem OR IGNORE): o banco começa vazio a cada ingestão,
  // então não há conflitos. OR IGNORE forçaria um lookup na B-Tree por linha.
  const char *sql =
      "INSERT INTO segments "
      "(internal_id, original_id, length, read_count, depth) "
      "VALUES (?, ?, ?, ?, ?);";
  int rc = ::sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
  if (rc != SQLITE_OK)
    return std::unexpected("sqlite3_prepare_v2 falhou: " +
                           std::string(::sqlite3_errmsg(db_)));
  return {};
}

inline std::expected<void, std::string>
GfaMetadataStore::flush_segments(std::span<const SegmentRecord> records) {
  if (!db_ || !stmt_)
    return std::unexpected("GfaMetadataStore não está aberto");
  if (records.empty())
    return {};

  // Uma única transação para todos os registros — diferença de 100-1000×
  // em relação a commits por linha
  if (auto r = exec("BEGIN TRANSACTION;"); !r) return r;

  for (const auto &rec : records) {
    // ① internal_id — índice exato do MPHF
    ::sqlite3_bind_int64(stmt_, 1, static_cast<sqlite3_int64>(rec.internal_id));
    // ② original_id — string_view não nula: usa SQLITE_STATIC (zero-copy)
    //    mas como SegmentRecord já possui std::string, o ponteiro é estável
    ::sqlite3_bind_text(stmt_, 2, rec.original_id.c_str(),
                        static_cast<int>(rec.original_id.size()),
                        SQLITE_STATIC);
    // ③ length
    ::sqlite3_bind_int(stmt_, 3, static_cast<int>(rec.length));
    // ④ read_count (NULL se ausente)
    if (rec.read_count >= 0)
      ::sqlite3_bind_int(stmt_, 4, rec.read_count);
    else
      ::sqlite3_bind_null(stmt_, 4);
    // ⑤ depth (NULL se ausente)
    if (rec.depth >= 0.f)
      ::sqlite3_bind_double(stmt_, 5, static_cast<double>(rec.depth));
    else
      ::sqlite3_bind_null(stmt_, 5);

    int rc = ::sqlite3_step(stmt_);
    ::sqlite3_reset(stmt_);
    if (rc != SQLITE_DONE) {
      // Captura a mensagem ANTES do ROLLBACK (que reinicia o estado de erro)
      const std::string err = ::sqlite3_errmsg(db_);
      exec("ROLLBACK;");
      return std::unexpected("Erro durante flush_segments: " + err);
    }
  }

  if (auto r = exec("COMMIT;"); !r) return r;

  // Cria o índice secundário UMA VEZ após todos os dados estarem gravados.
  // Fazê-lo antes do bulk insert tornaria cada INSERT ~10× mais lento
  // (atualização incremental da B-Tree para 85M linhas).
  if (auto r = exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_segments_orig "
        "ON segments (original_id);"); !r)
    return r;

  return {};
}

inline void GfaMetadataStore::close() noexcept {
  if (stmt_) {
    ::sqlite3_finalize(stmt_);
    stmt_ = nullptr;
  }
  if (db_) {
    ::sqlite3_close(db_);
    db_ = nullptr;
  }
}

} // namespace ggql
