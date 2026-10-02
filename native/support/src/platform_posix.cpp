#if !defined(_WIN32)

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "platform.h"

extern char** environ;

namespace qstate::support::platform {

namespace {

FileStat fromStat(const struct stat& st) {
    FileStat s;
    s.exists = true;
    s.isDir = S_ISDIR(st.st_mode);
    s.isRegular = S_ISREG(st.st_mode);
    s.size = static_cast<uint64_t>(st.st_size);
#if defined(__APPLE__)
    s.mtimeNs = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    s.mtimeNs = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    s.inode = static_cast<uint64_t>(st.st_ino);
    s.device = static_cast<uint64_t>(st.st_dev);
    return s;
}

}  // namespace

std::string errorMessage(int error) { return std::generic_category().message(error); }

FileStat statPath(const std::string& path, bool followSymlinks) {
    struct stat st;
    const int rc = followSymlinks ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
    if (rc != 0) {
        FileStat s;
        s.error = (errno == ENOENT || errno == ENOTDIR) ? 0 : errno;
        return s;
    }
    return fromStat(st);
}

NativeFile::~NativeFile() {
    if (handle_ >= 0) ::close(static_cast<int>(handle_));
}

std::unique_ptr<NativeFile> NativeFile::open(const std::string& path, int* error) {
    int fd;
    do {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        if (error) *error = errno;
        return nullptr;
    }
    auto f = std::make_unique<NativeFile>();
    f->handle_ = fd;
    return f;
}

NativeFile::ReadResult NativeFile::readAt(uint64_t offset, void* buffer, size_t length) const {
    ReadResult r;
    auto* out = static_cast<char*>(buffer);
    while (r.bytes < length) {
        const size_t want = std::min<size_t>(length - r.bytes, size_t{1} << 30);
        const ssize_t n = ::pread(static_cast<int>(handle_), out + r.bytes, want, static_cast<off_t>(offset + r.bytes));
        if (n < 0) {
            if (errno == EINTR) continue;
            r.error = errno;
            break;
        }
        if (n == 0) break;  // end of file
        r.bytes += static_cast<size_t>(n);
    }
    return r;
}

FileStat NativeFile::stat() const {
    struct stat st;
    if (::fstat(static_cast<int>(handle_), &st) != 0) {
        FileStat s;
        s.error = errno;
        return s;
    }
    return fromStat(st);
}

std::string homeDirectory() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return {};
}

std::string configBaseDirectory() {
#if defined(__APPLE__)
    const std::string home = homeDirectory();
    return home.empty() ? std::string() : home + "/Library/Application Support";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x && x[0] == '/') return x;
    const std::string home = homeDirectory();
    return home.empty() ? std::string() : home + "/.config";
#endif
}

std::string currentDirectory() {
    std::vector<char> buf(4096);
    while (true) {
        if (::getcwd(buf.data(), buf.size())) return buf.data();
        if (errno != ERANGE) return {};
        buf.resize(buf.size() * 2);
    }
}

std::string platformName() {
#if defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

unsigned long processId() { return static_cast<unsigned long>(::getpid()); }

bool writeFileAtomic(const std::string& path, const std::string& data, std::string* error) {
    static std::atomic<unsigned> counter{0};
    const std::string tmp = path + ".tmp" + std::to_string(::getpid()) + "." + std::to_string(counter++);
    auto fail = [&](const char* what, int e) {
        if (error) *error = std::string(what) + " '" + tmp + "': " + errorMessage(e);
        ::unlink(tmp.c_str());
        return false;
    };
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return fail("cannot create", errno);
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            return fail("cannot write", e);
        }
        done += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int e = errno;
        ::close(fd);
        return fail("cannot sync", e);
    }
    if (::close(fd) != 0) return fail("cannot close", errno);
    if (::rename(tmp.c_str(), path.c_str()) != 0) return fail("cannot rename", errno);
    return true;
}

ProcessResult runProcess(const ProcessSpec& spec) {
    ProcessResult result;
    if (spec.argv.empty()) {
        result.error = "empty command line";
        return result;
    }
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::pipe2(outPipe, O_CLOEXEC) != 0 || ::pipe2(errPipe, O_CLOEXEC) != 0) {
        result.error = "pipe failed: " + errorMessage(errno);
        for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) ::close(fd);
        }
        return result;
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);  // own process group: the whole tree can be stopped
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2);

    std::vector<std::string> envStrings;
    for (char** e = environ; e && *e; ++e) {
        bool overridden = false;
        for (const auto& kv : spec.extraEnv) {
            if (std::strncmp(*e, kv.first.c_str(), kv.first.size()) == 0 && (*e)[kv.first.size()] == '=') {
                overridden = true;
                break;
            }
        }
        if (!overridden) envStrings.emplace_back(*e);
    }
    for (const auto& kv : spec.extraEnv) envStrings.push_back(kv.first + "=" + kv.second);
    std::vector<char*> envp;
    for (auto& s : envStrings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<std::string> args = spec.argv;
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    if (rc != 0) {
        result.error = "cannot start '" + spec.argv[0] + "': " + errorMessage(rc);
        ::close(outPipe[0]);
        ::close(errPipe[0]);
        return result;
    }
    result.started = true;

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(spec.timeoutMs > 0 ? spec.timeoutMs : 0);
    bool killed = false;
    int fds[2] = {outPipe[0], errPipe[0]};
    bool open[2] = {true, true};
    std::vector<char> buf(256 * 1024);

    while (open[0] || open[1]) {
        if (spec.cancel && spec.cancel()) {
            result.cancelled = true;
            break;
        }
        int timeout = spec.cancel ? 100 : -1;
        if (spec.timeoutMs > 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) {
                result.timedOut = true;
                break;
            }
            timeout = static_cast<int>(std::min<long long>(left, spec.cancel ? 100 : 1000000));
        }
        pollfd pfds[2];
        int n = 0;
        int which[2];
        for (int i = 0; i < 2; i++) {
            if (!open[i]) continue;
            pfds[n] = {fds[i], POLLIN, 0};
            which[n++] = i;
        }
        const int pr = ::poll(pfds, static_cast<nfds_t>(n), timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            result.error = "poll failed: " + errorMessage(errno);
            break;
        }
        if (pr == 0) continue;  // re-check the deadline
        bool stop = false;
        for (int k = 0; k < n && !stop; k++) {
            if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const int i = which[k];
            const ssize_t got = ::read(fds[i], buf.data(), buf.size());
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                open[i] = false;
                continue;
            }
            if (got == 0) {
                open[i] = false;
                continue;
            }
            if (i == 0) {
                if (spec.onStdout && !spec.onStdout(buf.data(), static_cast<size_t>(got))) {
                    result.aborted = true;
                    stop = true;
                }
            } else {
                if (spec.onStderr) spec.onStderr(buf.data(), static_cast<size_t>(got));
                result.stderrText.append(buf.data(), static_cast<size_t>(got));
                if (result.stderrText.size() > spec.maxStderrBytes) {
                    result.stderrText.erase(0, result.stderrText.size() - spec.maxStderrBytes);
                }
            }
        }
        if (stop) break;
    }
    ::close(outPipe[0]);
    ::close(errPipe[0]);
    int status = 0;
    if (result.timedOut || result.aborted || result.cancelled || !result.error.empty()) {
        // Polite first (git removes its lock files and temporary packs on SIGTERM), then the hammer.
        killed = true;
        ::kill(-pid, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 200 && !reaped; i++) {
            const pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid || (w < 0 && errno != EINTR)) {
                reaped = true;
            } else {
                ::usleep(10 * 1000);
            }
        }
        if (!reaped) {
            ::kill(-pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
        ::kill(-pid, SIGKILL);  // anything the leader left behind
        return result;
    }
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!killed) {
        if (WIFEXITED(status)) {
            result.exitCode = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exitCode = 128 + WTERMSIG(status);
        }
    }
    return result;
}

}  // namespace qstate::support::platform

#endif
