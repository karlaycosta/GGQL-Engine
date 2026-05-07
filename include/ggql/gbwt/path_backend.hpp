#pragma once

// IPathBackend — contrato comum entre as implementacoes de armazenamento
// de paths/haplotipos por shard usadas em GfaCsrTopology::path_stores_.
//
// Atualmente duas impls coexistem:
//   - ggql::PathStorage  (uint32 cru; ratio 4 B/no; extract via memcpy)
//   - ggql::GbwtBackend  (gbwt::GBWT comprimido; ratio ~1-2 bits/no; extract via LF)
//
// A escolha do backend acontece no ingest via flag (PathBackendKind em
// IngestProfile). load() detecta o backend correto via magic bytes do shard.
//
// Operacoes especificas de cada backend (zero-copy nodes_view de PathStorage,
// locate/find de GbwtBackend) ficam na classe concreta — testes e usos
// avancados acessam diretamente, sem passar pela interface.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

namespace ggql {

class IPathBackend {
  public:
    using node_type = std::uint32_t;

    virtual ~IPathBackend() = default;

    // Hot path: copia a sequencia de nos do path para o buffer.
    // Substitui o conteudo de `out` por completo.
    virtual void extract_into(std::size_t path_id,
                              std::vector<std::uint64_t>& out) const = 0;

    // ── Metadados ────────────────────────────────────────────────────────────

    [[nodiscard]] virtual std::size_t num_paths()    const noexcept = 0;
    [[nodiscard]] virtual std::size_t total_nodes()  const noexcept = 0;
    [[nodiscard]] virtual std::size_t memory_bytes() const noexcept = 0;

    // ── Persistencia ─────────────────────────────────────────────────────────
    //
    // save() escreve o shard inteiro inline (header + payload + checksum). O
    // primeiro bloco escrito DEVE ser um magic identificando o backend, para
    // que o reader possa fazer dispatch sem metadado externo.
    //
    // load() nao e virtual — cada backend tem sua propria factory estatica.
    // O reader de shard (read_path_stores_payload) le o magic, faz dispatch
    // para a load() correta e retorna unique_ptr<IPathBackend>.

    virtual void save(std::ostream& out) const = 0;

    // Identificador legivel do backend para logs/telemetria.
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
};

}  // namespace ggql
