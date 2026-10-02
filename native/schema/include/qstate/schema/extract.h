// Schema extraction: Qubic core sources -> qstate::schema::Schema.
//
// Pipeline: preprocess the translation unit rooted at src/contract_core/contract_def.h, parse it with the generic
// C++ front-end (qstate::cpp::Program), read the table `contractDescriptions[]` and lay out the state type of every
// contract. Qubic specific knowledge is limited to: the root file name, the name of the table, and the recognition
// of the QPI types (by template / type name) that get a semantic Role.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qstate/cpp/source.h"
#include "qstate/schema/model.h"

namespace qstate::schema {

struct ExtractOptions {
    // Translation unit root and include search path, relative to the core root.
    std::string rootFile = "src/contract_core/contract_def.h";
    std::vector<std::string> includeDirs = {"src", ""};
    // Predefined macros: mirrors the production (MSVC) build of the node. Extra defines (e.g. the test-example
    // contracts via INCLUDE_CONTRACT_TEST_EXAMPLES) are appended by the caller.
    std::vector<std::pair<std::string, std::string>> defines = {{"_MSC_VER", "1944"}, {"_WIN64", "1"}, {"_M_X64", "1"}};
    // Name of the namespace-scope table with one row per contract: {assetName, constructionEpoch, destructionEpoch,
    // stateSize}.
    std::string tableName = "contractDescriptions";
};

struct ExtractStats {
    std::size_t fileCount = 0;          // source files read
    std::size_t tokenCount = 0;         // preprocessed tokens
    double preprocessMs = 0;
    double parseMs = 0;
    double layoutMs = 0;
    double totalMs = 0;
};

struct ExtractResult {
    std::shared_ptr<Schema> schema;     // never null; contracts may be empty when the root file is missing
    std::vector<std::string> files;     // files read (relative paths), for watching them
    ExtractStats stats;
};

// Never throws for bad sources: problems become Schema::diags (Error severity when the whole extraction failed).
ExtractResult extractSchema(const qstate::cpp::SourceProvider& source, const ExtractOptions& options = {});

} // namespace qstate::schema
