#include "record.h"

#include "toolchain.h"

#include <chrono>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#endif

namespace fs = std::filesystem;

namespace calx {

namespace {

// How far back from the build's start a time still counts as during it: two seconds, in a
// Stamp's units, which are 100 ns on Windows and 1 ns elsewhere.
#ifdef _WIN32
constexpr std::int64_t kRacy = 20'000'000;
constexpr std::int64_t kMinute = 60 * 10'000'000LL;
#else
constexpr std::int64_t kRacy = 2'000'000'000;
constexpr std::int64_t kMinute = 60 * 1'000'000'000LL;
#endif

// A program reused within this long of being marked used isn't marked again: the cache's trim
// keeps anything used in the last 10 minutes (kCacheGrace), and marking it is another call.
constexpr std::int64_t kMarkAfter = 5 * kMinute;

constexpr std::string_view kHeader = "calx-record 1";

bool plain_line(const std::string& s) { return s.find('\n') == std::string::npos && s.find('\r') == std::string::npos; }

}  // namespace

Stamp stamp_of(const std::string& path) {
    Stamp s;
#ifdef _WIN32
    // One call that doesn't open the file, unless it's a link, whose own time isn't its target's.
    std::wstring w = long_path(path).wstring();
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d)) return s;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        HANDLE h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return s;
        BY_HANDLE_FILE_INFORMATION info;
        bool got = GetFileInformationByHandle(h, &info) != 0;
        CloseHandle(h);
        if (!got) return s;
        d.nFileSizeHigh = info.nFileSizeHigh;
        d.nFileSizeLow = info.nFileSizeLow;
        d.ftLastWriteTime = info.ftLastWriteTime;
    }
    s.size = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    s.time = static_cast<std::int64_t>((static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) |
                                       d.ftLastWriteTime.dwLowDateTime);
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return s;
    s.size = static_cast<std::uint64_t>(st.st_size);
    s.time = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
#endif
    s.ok = true;
    return s;
}

std::int64_t stamp_now() {
#ifdef _WIN32
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime);
#else
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
#endif
}

std::string absolute_path(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(std::u8string(path.begin(), path.end())), ec);
    if (ec) return path;
    std::u8string u = p.lexically_normal().u8string();
    return std::string(u.begin(), u.end());
}

std::string record_path(const std::string& cache, const std::vector<std::string_view>& key) {
    std::vector<std::string_view> parts = {"record"};
    parts.insert(parts.end(), key.begin(), key.end());
    return (fs::path(cache) / "records" / (hash_hex(parts) + ".txt")).string();
}

bool load_record(const std::string& path, BuildRecord& r) {
    std::string text;
    if (!read_file(path, text)) return false;
    std::string_view rest = text;
    auto next = [&](std::string_view& line) {
        std::size_t end = rest.find('\n');
        if (end == std::string_view::npos) return false;
        line = rest.substr(0, end);
        rest.remove_prefix(end + 1);
        return true;
    };
    // `file SIZE TIME PATH`, `dir TIME PATH`: the numbers, and what's after them.
    auto number = [](std::string_view& s, std::int64_t& v) {
        std::size_t space = s.find(' ');
        if (space == std::string_view::npos || space == 0) return false;
        v = 0;
        bool negative = s[0] == '-';
        for (std::size_t i = negative ? 1 : 0; i < space; i++) {
            if (s[i] < '0' || s[i] > '9') return false;
            v = v * 10 + (s[i] - '0');
        }
        if (negative) v = -v;
        s.remove_prefix(space + 1);
        return true;
    };
    std::string_view line;
    if (!next(line) || line != kHeader) return false;
    bool ended = false;
    while (next(line)) {
        std::size_t space = line.find(' ');
        std::string_view kind = line.substr(0, space);
        std::string_view arg = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
        if (kind == "end") {
            ended = true;
            break;
        } else if (kind == "exe") {
            r.exe = std::string(arg);
        } else if (kind == "test") {
            r.tests.emplace_back(arg);
        } else if (kind == "file") {
            std::int64_t size, time;
            if (!number(arg, size) || !number(arg, time)) return false;
            Stamp s = stamp_of(std::string(arg));
            if (!s.ok || s.size != static_cast<std::uint64_t>(size) || s.time != time) return false;
        } else if (kind == "dir") {
            std::int64_t time;
            if (!number(arg, time)) return false;
            Stamp s = stamp_of(std::string(arg));
            if (!s.ok || s.time != time) return false;
        } else if (kind == "none") {
            if (stamp_of(std::string(arg)).ok) return false;
        } else {
            return false;
        }
    }
    if (!ended || r.exe.empty()) return false;
    Stamp exe = stamp_of(r.exe);
    if (!exe.ok) return false;
    if (exe.time < stamp_now() - kMarkAfter) {
        std::error_code ec;
        fs::last_write_time(long_path(r.exe), fs::file_time_type::clock::now(), ec);
    }
    return true;
}

void save_record(const std::string& path, const BuildRecord& r, std::int64_t start) {
    std::int64_t latest = start - kRacy;
    std::string text(kHeader);
    text += '\n';
    if (r.exe.empty() || !plain_line(r.exe)) return;
    text += "exe " + r.exe + "\n";
    for (const std::string& t : r.tests) {
        if (!plain_line(t)) return;
        text += "test " + t + "\n";
    }
    for (const std::string& f : r.files) {
        Stamp s = stamp_of(f);
        if (!s.ok || s.time > latest || !plain_line(f)) return;
        text += "file " + std::to_string(s.size) + " " + std::to_string(s.time) + " " + f + "\n";
    }
    for (const std::string& f : r.folders) {
        Stamp s = stamp_of(f);
        if (!s.ok || s.time > latest || !plain_line(f)) return;
        text += "dir " + std::to_string(s.time) + " " + f + "\n";
    }
    for (const std::string& f : r.missing) {
        if (stamp_of(f).ok || !plain_line(f)) return;
        text += "none " + f + "\n";
    }
    text += "end\n";
    fs::path dir = fs::path(path).parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!write_file_atomic(path, text)) return;
    // Each is a few KB, so the cache keeps those of the last 1024 builds.
    trim_cache(dir.string(), 1024, std::uintmax_t{64} << 20);
}

}  // namespace calx
