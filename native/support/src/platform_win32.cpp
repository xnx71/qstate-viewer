// Win32 variant of the platform layer. Written against the documented API and kept compiling in principle;
// only the Linux variant is verified by the test suite.
#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <system_error>

#include "platform.h"

namespace qstate::support::platform {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

int64_t fileTimeToUnixNs(FILETIME ft) {
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100 ns since 1601
    constexpr uint64_t kEpochDiff = 116444736000000000ULL;
    return t >= kEpochDiff ? static_cast<int64_t>((t - kEpochDiff) * 100) : 0;
}

FileStat fromHandleInfo(const BY_HANDLE_FILE_INFORMATION& info) {
    FileStat s;
    s.exists = true;
    s.isDir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    s.isRegular = !s.isDir;
    s.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    s.mtimeNs = fileTimeToUnixNs(info.ftLastWriteTime);
    s.inode = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    s.device = info.dwVolumeSerialNumber;
    return s;
}

}  // namespace

std::string errorMessage(int error) { return std::system_category().message(error); }

FileStat statPath(const std::string& path, bool) {
    HANDLE h = ::CreateFileW(widen(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        FileStat s;
        const DWORD e = ::GetLastError();
        s.error = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? 0 : static_cast<int>(e);
        return s;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    FileStat s;
    if (::GetFileInformationByHandle(h, &info)) s = fromHandleInfo(info);
    else s.error = static_cast<int>(::GetLastError());
    ::CloseHandle(h);
    return s;
}

NativeFile::~NativeFile() {
    if (handle_ != -1) ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
}

std::unique_ptr<NativeFile> NativeFile::open(const std::string& path, int* error) {
    // Share everything (including delete) so that the node can rename / rewrite files while we hold the handle.
    HANDLE h = ::CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = static_cast<int>(::GetLastError());
        return nullptr;
    }
    auto f = std::make_unique<NativeFile>();
    f->handle_ = reinterpret_cast<intptr_t>(h);
    return f;
}

NativeFile::ReadResult NativeFile::readAt(uint64_t offset, void* buffer, size_t length) const {
    ReadResult r;
    auto* out = static_cast<char*>(buffer);
    while (r.bytes < length) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(length - r.bytes, size_t{1} << 30));
        OVERLAPPED ov{};
        const uint64_t pos = offset + r.bytes;
        ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
        DWORD got = 0;
        if (!::ReadFile(reinterpret_cast<HANDLE>(handle_), out + r.bytes, want, &got, &ov)) {
            const DWORD e = ::GetLastError();
            if (e != ERROR_HANDLE_EOF) r.error = static_cast<int>(e);
            break;
        }
        if (got == 0) break;
        r.bytes += got;
    }
    return r;
}

FileStat NativeFile::stat() const {
    BY_HANDLE_FILE_INFORMATION info{};
    FileStat s;
    if (::GetFileInformationByHandle(reinterpret_cast<HANDLE>(handle_), &info)) s = fromHandleInfo(info);
    else s.error = static_cast<int>(::GetLastError());
    return s;
}

std::string homeDirectory() {
    if (const char* h = std::getenv("USERPROFILE"); h && *h) return h;
    return {};
}

std::string configBaseDirectory() {
    if (const char* a = std::getenv("APPDATA"); a && *a) return a;
    return {};
}

std::string currentDirectory() {
    std::wstring buf(MAX_PATH, L'\0');
    DWORD n = ::GetCurrentDirectoryW(static_cast<DWORD>(buf.size()), buf.data());
    if (n > buf.size()) {
        buf.resize(n);
        n = ::GetCurrentDirectoryW(static_cast<DWORD>(buf.size()), buf.data());
    }
    buf.resize(n);
    return narrow(buf);
}

std::string platformName() { return "windows"; }

unsigned long processId() { return static_cast<unsigned long>(::GetCurrentProcessId()); }

bool writeFileAtomic(const std::string& path, const std::string& data, std::string* error) {
    static std::atomic<unsigned> counter{0};
    const std::string tmp = path + ".tmp" + std::to_string(::GetCurrentProcessId()) + "." + std::to_string(counter++);
    HANDLE h = ::CreateFileW(widen(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto fail = [&](const char* what) {
        if (error) *error = std::string(what) + " '" + tmp + "': " + errorMessage(static_cast<int>(::GetLastError()));
        ::DeleteFileW(widen(tmp).c_str());
        return false;
    };
    if (h == INVALID_HANDLE_VALUE) return fail("cannot create");
    size_t done = 0;
    while (done < data.size()) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - done, size_t{1} << 30));
        if (!::WriteFile(h, data.data() + done, want, &wrote, nullptr)) {
            const bool r = fail("cannot write");
            ::CloseHandle(h);
            return r;
        }
        done += wrote;
    }
    if (!::FlushFileBuffers(h)) {
        const bool r = fail("cannot sync");
        ::CloseHandle(h);
        return r;
    }
    ::CloseHandle(h);
    if (!::MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail("cannot rename");
    }
    return true;
}

namespace {

std::wstring quoteArg(const std::string& arg) {
    std::wstring w = widen(arg);
    if (!w.empty() && w.find_first_of(L" \t\n\v\"") == std::wstring::npos) return w;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : w) {
        if (c == L'\\') {
            backslashes++;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out += c;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

}  // namespace

ProcessResult runProcess(const ProcessSpec& spec) {
    ProcessResult result;
    if (spec.argv.empty()) {
        result.error = "empty command line";
        return result;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!::CreatePipe(&outR, &outW, &sa, 0) || !::CreatePipe(&errR, &errW, &sa, 0)) {
        result.error = "pipe failed: " + errorMessage(static_cast<int>(::GetLastError()));
        return result;
    }
    ::SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    for (const auto& kv : spec.extraEnv) ::SetEnvironmentVariableW(widen(kv.first).c_str(), widen(kv.second).c_str());

    std::wstring cmd;
    for (size_t i = 0; i < spec.argv.size(); i++) {
        if (i) cmd += L' ';
        cmd += quoteArg(spec.argv[i]);
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(outW);
    ::CloseHandle(errW);
    if (!ok) {
        result.error = "cannot start '" + spec.argv[0] + "': " + errorMessage(static_cast<int>(::GetLastError()));
        ::CloseHandle(outR);
        ::CloseHandle(errR);
        return result;
    }
    result.started = true;

    // Drain stderr on a helper thread-free basis: poll both pipes with PeekNamedPipe.
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(spec.timeoutMs > 0 ? spec.timeoutMs : 0);
    std::vector<char> buf(256 * 1024);
    bool outOpen = true, errOpen = true, killed = false;
    auto pump = [&](HANDLE h, bool& open, bool isOut) {
        DWORD avail = 0;
        if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
            open = false;
            return false;
        }
        if (avail == 0) return false;
        DWORD got = 0;
        if (!::ReadFile(h, buf.data(), static_cast<DWORD>(std::min<size_t>(buf.size(), avail)), &got, nullptr) || got == 0) {
            open = false;
            return false;
        }
        if (isOut) {
            if (spec.onStdout && !spec.onStdout(buf.data(), got)) result.aborted = true;
        } else if (result.stderrText.size() < spec.maxStderrBytes) {
            result.stderrText.append(buf.data(), std::min<size_t>(got, spec.maxStderrBytes - result.stderrText.size()));
        }
        return true;
    };
    while ((outOpen || errOpen) && !result.aborted) {
        bool progress = false;
        if (outOpen) progress |= pump(outR, outOpen, true);
        if (errOpen && !result.aborted) progress |= pump(errR, errOpen, false);
        if (spec.timeoutMs > 0 && Clock::now() >= deadline) {
            result.timedOut = true;
            break;
        }
        if (!progress) {
            if (::WaitForSingleObject(pi.hProcess, 5) == WAIT_OBJECT_0) {
                // Process ended: drain what is left, then stop.
                while (pump(outR, outOpen, true) && !result.aborted) {}
                while (pump(errR, errOpen, false)) {}
                break;
            }
        }
    }
    if (result.timedOut || result.aborted) {
        ::TerminateProcess(pi.hProcess, 1);
        killed = true;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    if (!killed) result.exitCode = static_cast<int>(code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(outR);
    ::CloseHandle(errR);
    return result;
}

}  // namespace qstate::support::platform

#endif
