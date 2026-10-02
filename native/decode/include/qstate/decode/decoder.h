// StateDecoder: (schema type tree) + (ByteSource) -> lazily expandable tree, container logical views, native
// table views (sort / filter / paging), node location by byte offset and byte search.
//
// Thread-safety: a StateDecoder is immutable after construction; every query method is const and may be called
// concurrently from several threads. Internal caches (DecodeCache) are synchronised. The ByteSource must support
// concurrent reads.
//
// Generations: every query carries `Query::generation`. When it differs from the previous one seen by this decoder
// all cached data of this decoder are dropped (the caller bumps the generation when the file changed on disk).
// If the underlying file changes while a query runs, results may be inconsistent but never unsafe.
//
// Errors: NotFoundError (bad NodeId / offset / view), InvalidArgumentError (bad filter / sort / pattern),
// CancelledError (Query::cancel became true). All derive from std::runtime_error.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "qstate/decode/byte_source.h"
#include "qstate/decode/cache.h"
#include "qstate/decode/identity.h"
#include "qstate/decode/schema_index.h"
#include "qstate/decode/values.h"

namespace qstate::decode {

struct NotFoundError : std::runtime_error { using std::runtime_error::runtime_error; };
struct InvalidArgumentError : std::runtime_error { using std::runtime_error::runtime_error; };
struct CancelledError : std::runtime_error { CancelledError() : std::runtime_error("cancelled") {} };

using ContractNameFn = std::function<std::string(std::uint32_t)>;

struct DecoderConfig {
    std::shared_ptr<const IdentityCodec> identity; // null: BasicIdentityCodec without checksum ("????" suffix)
    ContractNameFn contractName;                   // contract index -> asset name ("" = unknown); may be null
    std::shared_ptr<DecodeCache> cache;            // null: private cache of `cacheBytes`
    std::size_t cacheBytes = DecodeCache::kDefaultCapacity;
    std::uint32_t scope = 0;                       // keys this decoder's entries when the cache is shared (e.g. contract index)
    std::string rootLabel;                         // label of the root node (default: type name)
};

struct Query {
    std::uint64_t generation = 0;
    const std::atomic<bool>* cancel = nullptr; // checked inside long operations
};

enum class ChildView : std::uint8_t { Logical, Raw };

struct ChildrenRequest {
    ChildView view = ChildView::Logical;
    std::uint64_t offset = 0;
    std::uint64_t limit = 200; // clamped to [1, 1000]
    bool hideEmpty = false;    // array-like parents: skip all-zero elements
};

struct SearchRequest {
    std::string query;
    std::string mode = "auto"; // auto | id | hex | int | text
    std::uint64_t limit = 200; // clamped to [1, 5000]
};

class StateDecoder {
public:
    // `rootType` is the state record type (ContractSchema::stateType).
    StateDecoder(std::shared_ptr<const SchemaIndex> index, TypeId rootType, std::shared_ptr<const ByteSource> source,
                 DecoderConfig config = {});
    StateDecoder(std::shared_ptr<const schema::Schema> schema, TypeId rootType, std::shared_ptr<const ByteSource> source,
                 DecoderConfig config = {});
    ~StateDecoder();
    StateDecoder(const StateDecoder&) = delete;
    StateDecoder& operator=(const StateDecoder&) = delete;

    NodeInfo node(const NodeId& id, const Query& q = {}) const;
    ChildrenPage children(const NodeId& id, const ChildrenRequest& req = {}, const Query& q = {}) const;
    NodeLocation locate(std::uint64_t offset, const Query& q = {}) const;
    // Exact position of every node on the path to `id` inside its parent's child list (see NodeReveal). Collection
    // elements are listed below their PoV (path ... container, PoV, element), exactly like state.children shows them.
    NodeReveal reveal(const NodeId& id, bool hideEmpty = false, const Query& q = {}) const;
    SearchResult search(const SearchRequest& req, const Query& q = {}) const;

    TableInfo describeTable(const NodeId& id, const std::string& view = {}, const Query& q = {}) const;
    TablePage tableRows(const TableRequest& req, const Query& q = {}) const;

    std::uint64_t stateSize() const; // sizeof(state type) from the schema
    std::uint64_t fileSize() const;  // ByteSource::size()
    DecodeCache& cache() const;

    class Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace qstate::decode
