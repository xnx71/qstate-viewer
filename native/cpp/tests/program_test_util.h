// Shared helpers of the parser / consteval / layout / program tests.
#pragma once

#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "qstate/cpp/program.h"
#include "test_env.h"

namespace qstate::cpp::testutil {

// Parses C++ source text directly (no preprocessor); diagnostics are collected into `diags`.
inline std::unique_ptr<Program> parseText(const std::string& code, Diags* diagsOut = nullptr,
                                          ProgramOptions options = {}) {
    Diags diags;
    auto prog = Program::parse(lex(code, 0), {"test.h"}, diags, options);
    if (diagsOut) *diagsOut = prog->diags();
    return prog;
}

inline std::string text(const std::vector<Diag>& diags) {
    std::string s;
    for (const Diag& d : diags) s += d.file + ":" + std::to_string(d.line) + " " + d.message + "\n";
    return s;
}

// Reads a g++ -E output file: "# <line> "<file>" flags" markers set the current file / line, other "#" lines
// (pragmas) are dropped, everything else is lexed line by line.
struct IiUnit {
    std::vector<Token> tokens;
    std::vector<std::string> files;
};

inline bool loadIi(const std::string& path, IiUnit& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::map<std::string, std::uint32_t> fileIds;
    std::uint32_t file = 0;
    std::uint32_t line = 1;
    std::string raw;
    out.files.push_back("<unknown>");
    while (std::getline(in, raw)) {
        std::size_t i = 0;
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
        if (i < raw.size() && raw[i] == '#') {
            std::size_t j = i + 1;
            while (j < raw.size() && raw[j] == ' ') ++j;
            if (j < raw.size() && std::isdigit(static_cast<unsigned char>(raw[j]))) {
                std::uint32_t n = 0;
                while (j < raw.size() && std::isdigit(static_cast<unsigned char>(raw[j]))) n = n * 10 + static_cast<std::uint32_t>(raw[j++] - '0');
                std::size_t q1 = raw.find('"', j);
                std::size_t q2 = q1 == std::string::npos ? q1 : raw.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    std::string f = raw.substr(q1 + 1, q2 - q1 - 1);
                    // keep the part after "/src/" or "/lib/" for readability
                    for (const char* marker : {"/src/", "/lib/"}) {
                        std::size_t m = f.find(marker);
                        if (m != std::string::npos) { f = f.substr(m + 1); break; }
                    }
                    auto it = fileIds.find(f);
                    if (it == fileIds.end()) {
                        it = fileIds.emplace(f, static_cast<std::uint32_t>(out.files.size())).first;
                        out.files.push_back(f);
                    }
                    file = it->second;
                }
                line = n;
            }
            continue; // marker or pragma
        }
        std::vector<Token> toks = lexFragment(raw, file, line);
        for (Token& t : toks) out.tokens.push_back(std::move(t));
        ++line;
    }
    Token end;
    end.kind = TokKind::End;
    out.tokens.push_back(end);
    return true;
}

inline std::uint64_t offsetOf(const TypeLayout& l, const std::string& field) {
    for (const FieldLayout& f : l.fields)
        if (f.name == field) return f.offset;
    FAIL("no field '" << field << "' in " << l.name);
    return 0;
}

inline std::string dataFile(const std::string& name) {
    std::string root = qstate::testing::sourceDir();
    if (root.empty()) {
        std::string here = __FILE__; // .../native/cpp/tests/program_test_util.h
        for (int i = 0; i < 4; ++i) {
            std::size_t p = here.find_last_of('/');
            if (p == std::string::npos) break;
            here = here.substr(0, p);
        }
        root = here;
    }
    return root + "/docs/research/data/" + name;
}

} // namespace qstate::cpp::testutil
