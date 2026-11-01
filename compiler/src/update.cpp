#include "update.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "toolchain.h"

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace calx {

namespace {

#ifdef _WIN32
constexpr const char* kUpdater = "calx-update.exe";
constexpr const char* kInstall = "irm https://calxlang.com/install.ps1 | iex";
#else
constexpr const char* kUpdater = "calx-update";
constexpr const char* kInstall = "curl -fsSL https://calxlang.com/install.sh | sh";
#endif

constexpr auto kDay = std::chrono::hours(24);

// A release or a nightly, which CI stamps with its commit. A build from source isn't one.
bool is_release() { return CALX_COMMIT[0] != 0; }

bool has_env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

fs::path updater() { return self_exe().parent_path() / kUpdater; }

// The installation: the folder above bin/.
std::string install_home() { return self_exe().parent_path().parent_path().string(); }

// The lines of `text`, without their line ends.
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        out.push_back(std::move(line));
        at = end + 1;
    }
    return out;
}

long long now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool older_than_a_day(const fs::path& file) {
    std::error_code ec;
    auto t = fs::last_write_time(file, ec);
    return ec || fs::file_time_type::clock::now() - t > kDay;
}

}  // namespace

int run_update(const std::vector<std::string>& args) {
    std::error_code ec;
    fs::path u = updater();
    if (!is_release() || !fs::is_regular_file(u, ec)) {
        std::fprintf(stderr,
                     "calx update: this calx (%s) was built from source, so `git pull` and a rebuild update it.\n"
                     "To install a release instead, which `calx update` can keep up to date:\n  %s\n",
                     self_exe().string().c_str(), kInstall);
        return 1;
    }
    std::vector<std::string> argv = {u.string(), "--home", install_home(), "--current", CALX_VERSION};
    argv.insert(argv.end(), args.begin(), args.end());
    std::fflush(stdout);
    std::fflush(stderr);
    int status = run_process(argv);
    if (status < 0) {
        std::fprintf(stderr, "calx update: can't run %s\n", u.string().c_str());
        return 1;
    }
    // What the daily check found is about the version that was running. Whatever happened, the
    // next command checks again.
    fs::remove(fs::path(cache_dir()) / "update" / "latest", ec);
    return status;
}

void update_notice() {
    if (!is_release() || has_env("CI") || has_env("CALX_NO_UPDATE_CHECK") || !isatty(fileno(stderr))) return;
    fs::path dir = fs::path(cache_dir()) / "update";
    fs::path latest = dir / "latest";
    // The daily check writes the version it checked for, the newer one or an empty line, and when
    // it checked, in seconds since 1970, so this one read says everything.
    std::string text;
    read_file(latest.string(), text);
    std::vector<std::string> l = lines(text);
    bool current = l.size() >= 3 && l[0] == CALX_VERSION;
    std::string newer = current ? l[1] : std::string();
    long long checked_at = current ? std::atoll(l[2].c_str()) : 0;
    long long now = now_seconds();
    if (!current || now - checked_at > 24 * 60 * 60 || checked_at > now) {
        std::error_code ec;
        fs::path u = updater();
        if (fs::is_regular_file(u, ec)) {
            // The time moves on first, so other calx commands meanwhile don't start checks of
            // their own.
            fs::create_directories(dir, ec);
            write_file_atomic(latest.string(), std::string(CALX_VERSION) + "\n" + newer + "\n" + std::to_string(now) + "\n");
            start_detached({u.string(), "--home", install_home(), "--current", CALX_VERSION, "--background",
                            latest.string()});
        }
    }
    if (newer.empty()) return;
    fs::path shown = dir / "shown";
    std::string said;
    read_file(shown.string(), said);
    if (said == newer && !older_than_a_day(shown)) return;
    std::fprintf(stderr, "calx %s is out; you have %s. `calx update` installs it.\n", newer.c_str(), CALX_VERSION);
    write_file_atomic(shown.string(), newer);
}

}  // namespace calx
