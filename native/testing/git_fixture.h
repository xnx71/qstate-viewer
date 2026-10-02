// A throw-away git repository for tests: commits with deterministic, increasing dates, tags and branches, made with
// the real `git` executable. Used as the "remote" of mirror / service tests (a plain path is a valid repository URL).
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace qstate::testing {

class GitFixtureRepo {
public:
    // Creates `dir` (and parents) and initialises an empty repository there, default branch "main".
    explicit GitFixtureRepo(const std::filesystem::path& dir) : dir_(dir) {
        std::filesystem::create_directories(dir_);
        git({"init", "--quiet", "-b", "main"});
    }

    const std::filesystem::path& dir() const { return dir_; }
    std::string url() const { return dir_.string(); }

    void write(const std::string& relPath, const std::string& content) {
        const auto path = dir_ / relPath;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    void remove(const std::string& relPath) { std::filesystem::remove_all(dir_ / relPath); }

    // Stages everything and commits (an empty commit is allowed). Returns the new commit's sha.
    std::string commit(const std::string& message) {
        git({"add", "-A"});
        git({"commit", "--quiet", "--allow-empty", "-m", message}, /*dated=*/true);
        return head();
    }

    std::string head() { return git({"rev-parse", "HEAD"}); }

    // Lightweight tag at HEAD, or an annotated one (own tagger date).
    void tag(const std::string& name, bool annotated = false) {
        if (annotated) {
            git({"tag", "-a", "-m", "tag " + name, name}, /*dated=*/true);
        } else {
            git({"tag", name});
        }
    }

    void branch(const std::string& name) { git({"branch", name}); }
    void checkout(const std::string& name) { git({"checkout", "--quiet", name}); }
    void checkoutNew(const std::string& name) { git({"checkout", "--quiet", "-b", name}); }

    // Runs git in the repository; returns its trimmed stdout. Throws std::runtime_error on a non-zero exit.
    std::string git(const std::vector<std::string>& args, bool dated = false) {
        std::string cmd;
        if (dated) {
            char date[64];
            std::snprintf(date, sizeof date, "2024-01-%02dT12:00:00 +0000", 1 + (++clock_ % 28));
            cmd += std::string("GIT_AUTHOR_DATE='") + date + "' GIT_COMMITTER_DATE='" + date + "' ";
        }
        cmd += "git -C " + quote(dir_.string()) +
               " -c user.name=test -c user.email=test@example.com -c commit.gpgsign=false -c tag.gpgsign=false";
        for (const auto& a : args) cmd += " " + quote(a);
        cmd += " 2>&1";
        std::string out;
        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (pipe == nullptr) throw std::runtime_error("cannot run git");
        char buf[4096];
        while (size_t n = std::fread(buf, 1, sizeof buf, pipe)) out.append(buf, n);
        const int status = ::pclose(pipe);
        if (status != 0) throw std::runtime_error("git failed: " + cmd + "\n" + out);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }

private:
    static std::string quote(const std::string& s) {
        std::string q = "'";
        for (char c : s) {
            if (c == '\'') {
                q += "'\\''";
            } else {
                q += c;
            }
        }
        return q + "'";
    }

    std::filesystem::path dir_;
    int clock_ = 0;
};

} // namespace qstate::testing
