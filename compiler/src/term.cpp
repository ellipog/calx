#include "term.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace calx::term {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Whether the terminal takes the 256-colour palette, where it can't show 24-bit colour.
bool palette_256 = false;

Caps detect(int fd) {
    Caps c;
    std::string force_color = env("FORCE_COLOR");
    bool force = env("CLICOLOR_FORCE") == "1" || (!force_color.empty() && force_color != "0");
    bool tty = false;
#ifdef _WIN32
    HANDLE h = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (h != NULL && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        // A console before Windows 10 takes no ANSI codes, and counts as no terminal.
        tty = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) || SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        if (tty) {
            // The symbols are UTF-8, as everything calx writes is; the runtime does the same for
            // the programs it runs.
            SetConsoleOutputCP(CP_UTF8);
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (GetConsoleScreenBufferInfo(h, &info)) c.columns = info.srWindow.Right - info.srWindow.Left + 1;
        }
    }
    // Every console that takes ANSI codes takes 24-bit colour too.
    c.truecolor = true;
#else
    tty = isatty(fd) != 0;
    if (tty) {
        struct winsize w;
        if (ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) c.columns = w.ws_col;
    }
    std::string ct = env("COLORTERM");
    c.truecolor = ct == "truecolor" || ct == "24bit" || !env("WT_SESSION").empty() || force;
    palette_256 = env("TERM").find("256") != std::string::npos;
#endif
    bool off = !env("NO_COLOR").empty() || env("TERM") == "dumb";
    c.color = force || (tty && !off);
    c.live = c.color && (force || env("CI").empty());
    // Forced on where there's no terminal to measure, COLUMNS says how wide to draw, as it does
    // for other tools.
    if (!tty) c.columns = std::atoi(env("COLUMNS").c_str());
    if (c.columns < 20) c.columns = 80;
    return c;
}

const char* kReset = "\x1b[0m";

std::string sgr(const std::string& params) { return "\x1b[" + params + "m"; }

// A tone's colour as a foreground and as a background. The amber is the brand's (`--calx` on
// calxlang.com), which reads on a dark terminal and a light one alike, or the nearest the
// terminal's palette has.
struct Ink {
    std::string fg, bg;
};

Ink ink(const Caps& c, Tone tone) {
    switch (tone) {
    case Tone::accent:
        if (c.truecolor) return {"38;2;188;133;54", "48;2;188;133;54"};
        if (palette_256) return {"38;5;136", "48;5;136"};
        return {"33", "43"};
    case Tone::good: return {"32", "42"};
    case Tone::warning: return {"33", "43"};
    case Tone::bad: return {"31", "41"};
    }
    return {"39", "49"};
}

// The ink of the text on a chip: near black, which reads on each tone's colour.
std::string chip_text(const Caps& c) { return c.truecolor ? "38;2;24;24;27;1" : "30"; }

// `s` in the style `code`, which comes back after each style inside it ends, so a dim line with a
// command in the accent is dim on both sides of it.
std::string paint(const Caps& c, const std::string& code, std::string_view s) {
    if (!c.color) return std::string(s);
    std::string out = code;
    std::size_t at = 0;
    for (std::size_t end; (end = s.find(kReset, at)) != std::string_view::npos; at = end + 4) {
        out.append(s.substr(at, end + 4 - at));
        out += code;
    }
    out.append(s.substr(at));
    return out + kReset;
}

// The runes of `s` up to `n` of them.
std::string runes_up_to(const std::string& s, int n) {
    std::size_t i = 0;
    int count = 0;
    while (i < s.size() && count < n) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        i += b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        count++;
    }
    return s.substr(0, std::min(i, s.size()));
}

// The shades by how much of a cell they ink, in quarters: none, ░, ▒, ▓, █.
const char* const kShade[] = {" ", "\xE2\x96\x91", "\xE2\x96\x92", "\xE2\x96\x93", "\xE2\x96\x88"};

// Cells of shade, each in a style, written with one escape code for each run of the same style.
struct Cells {
    const Caps& c;
    std::string out, style;
    void add(const std::string& s, const char* shade) {
        if (s != style) {
            if (!style.empty()) out += kReset;
            if (!s.empty()) out += sgr(s);
            style = s;
        }
        out += shade;
    }
    std::string done() {
        if (!style.empty()) out += kReset;
        return out;
    }
};

// The grey of a track, under a bar or a scanner.
const char* kTrack = "90";

// The mark, from assets/logo/calx-mark.svg: how much of each cell it covers, in quarters, at seven
// rows of twenty-two cells, a cell being twice as tall as it's wide. Fewer rows lose the C's hole.
constexpr int kMarkRows = 7;
constexpr int kMarkColumns = 22;
const char* const kMark[kMarkRows] = {
    "0002344443210000013442",
    "0244443334443101344431",
    "2444200001343234443100",
    "3443000000013444320000",
    "2444200001344432243100",
    "0244443334443101344431",
    "0002344443210000013442",
};

// The Status being drawn, which `quiet` takes off the screen before anything else is written.
std::atomic<Status*> active{nullptr};

}  // namespace

const Caps& out() {
    static const Caps c = detect(1);
    return c;
}

const Caps& err() {
    static const Caps c = detect(2);
    return c;
}

std::string accent(const Caps& c, std::string_view s) { return paint(c, sgr(ink(c, Tone::accent).fg), s); }
std::string good(const Caps& c, std::string_view s) { return paint(c, "\x1b[32m", s); }
std::string warning(const Caps& c, std::string_view s) { return paint(c, "\x1b[33m", s); }
std::string bad(const Caps& c, std::string_view s) { return paint(c, "\x1b[31m", s); }
std::string dim(const Caps& c, std::string_view s) { return paint(c, "\x1b[90m", s); }
std::string bold(const Caps& c, std::string_view s) { return paint(c, "\x1b[1m", s); }

std::string code(const Caps& c, std::string_view text, bool ticks) {
    if (!c.color) return std::string(text);
    std::string out;
    std::size_t at = 0;
    for (;;) {
        std::size_t open = text.find('`', at);
        std::size_t close = open == std::string_view::npos ? open : text.find('`', open + 1);
        if (close == std::string_view::npos) break;
        out += text.substr(at, open - at);
        std::string_view inner = text.substr(ticks ? open : open + 1, close - open + (ticks ? 1 : -1));
        out += accent(c, inner);
        at = close + 1;
    }
    out += text.substr(at);
    return out;
}

std::string tick(const Caps& c) { return good(c, "\xE2\x9C\x93"); }    // ✓
std::string bang(const Caps& c) { return warning(c, "!"); }
std::string cross(const Caps& c) { return bad(c, "\xE2\x9C\x97"); }    // ✗

int width(std::string_view s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b == 0x1b && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7e)) i++;
            i++;
            continue;
        }
        i += b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        n++;
    }
    return n;
}

std::string pad(std::string_view s, int columns) {
    int w = width(s);
    return std::string(s) + std::string(w < columns ? columns - w : 0, ' ');
}

std::vector<std::string> wrap(std::string_view text, int columns) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0;
    while (at < text.size()) {
        // A word runs to the next space outside backticks, so `calx eval "1 << 20"` stays whole.
        std::size_t end = at;
        bool quoted = false;
        while (end < text.size() && (quoted || text[end] != ' ')) {
            if (text[end] == '`') quoted = !quoted;
            end++;
        }
        std::string_view word = text.substr(at, end - at);
        at = end + 1;
        if (word.empty()) continue;
        if (!line.empty() && width(line) + 1 + width(word) > columns) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += ' ';
        line += word;
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

std::string heading(const Caps& c, std::string_view text) {
    if (!c.color) return std::string(text);
    Ink a = ink(c, Tone::accent);
    return sgr(a.bg + ";" + chip_text(c)) + " " + std::string(text) + " " + kReset + sgr(a.fg) + kShade[3] + kShade[2] +
           kShade[1] + kReset;
}

std::string band(const Caps& c, std::string_view text, Tone tone) {
    if (!c.color) return std::string(text);
    Ink t = ink(c, tone);
    return sgr(t.fg) + kShade[1] + kShade[2] + kShade[3] + kReset + sgr(t.bg + ";" + chip_text(c)) + " " +
           std::string(text) + " " + kReset + sgr(t.fg) + kShade[3] + kShade[2] + kShade[1] + kReset;
}

std::string beside_mark(const Caps& c, const std::vector<std::string>& lines, int indent) {
    std::string out;
    if (!c.color) {
        for (const std::string& line : lines) out += line + "\n";
        return out;
    }
    std::size_t rows = std::max<std::size_t>(kMarkRows, lines.size());
    for (std::size_t r = 0; r < rows; r++) {
        std::string row(indent, ' ');
        if (r < kMarkRows) {
            Cells cells{c, "", ""};
            for (const char* q = kMark[r]; *q; q++) cells.add(*q == '0' ? "" : ink(c, Tone::accent).fg, kShade[*q - '0']);
            row += cells.done();
        } else {
            row += std::string(kMarkColumns, ' ');
        }
        if (r < lines.size() && !lines[r].empty()) row += "   " + lines[r];
        out += row + "\n";
    }
    return out;
}

std::string bar(const Caps& c, double fraction, int width, int frame) {
    // The edge fades over kEdge cells: a cell's ink is how far the fill has gone past it, so the
    // fill runs from wholly before the first cell to wholly after the last.
    constexpr double kEdge = 4;
    fraction = std::clamp(fraction, 0.0, 1.0);
    double head = (0.5 - kEdge / 2) + fraction * (width + kEdge - 1);
    if (frame >= 0 && fraction > 0 && fraction < 1) head += 0.6 * std::sin(frame * 0.7);
    std::string fg = ink(c, Tone::accent).fg;
    Cells cells{c, "", ""};
    for (int i = 0; i < width; i++) {
        double v = std::clamp((head - (i + 0.5)) / kEdge + 0.5, 0.0, 1.0);
        int q = static_cast<int>(v * 4 + 0.5);
        if (q == 0) cells.add(kTrack, kShade[1]);
        else cells.add(fg, kShade[q]);
    }
    return cells.done();
}

std::string ratio(const Caps& c, const std::vector<std::pair<std::size_t, Tone>>& counts, int width) {
    std::size_t total = 0;
    for (auto& [n, tone] : counts) total += n;
    Cells cells{c, "", ""};
    if (total == 0) {
        for (int i = 0; i < width; i++) cells.add(kTrack, kShade[1]);
        return cells.done();
    }
    // Each part's cells, at least one for a part that isn't empty, by the largest remainder.
    std::vector<int> share(counts.size(), 0);
    int given = 0;
    for (std::size_t k = 0; k < counts.size(); k++) {
        if (counts[k].first == 0) continue;
        share[k] = std::max(1, static_cast<int>(static_cast<double>(counts[k].first) * width / total));
        given += share[k];
    }
    while (given > width) {
        auto most = std::max_element(share.begin(), share.end());
        (*most)--;
        given--;
    }
    while (given < width) {
        std::size_t best = 0;
        double gap = -1;
        for (std::size_t k = 0; k < counts.size(); k++) {
            double g = static_cast<double>(counts[k].first) * width / total - share[k];
            if (counts[k].first && g > gap) gap = g, best = k;
        }
        share[best]++;
        given++;
    }
    std::vector<std::size_t> owner;
    for (std::size_t k = 0; k < counts.size(); k++) owner.insert(owner.end(), share[k], k);
    for (int i = 0; i < width; i++) {
        std::size_t k = owner[i];
        Ink here = ink(c, counts[k].second);
        // Where this part meets the next, both wide enough to share cells, blend them over three:
        // ▓ ▒ in this one's colour on the next one's, then ░.
        int end = 0;
        for (std::size_t j = 0; j <= k; j++) end += share[j];
        int start = end - share[k];
        std::size_t next = k + 1;
        while (next < counts.size() && share[next] == 0) next++;
        if (next < counts.size() && share[k] >= 3 && share[next] >= 3 && i >= end - 2) {
            cells.add(here.fg + ";" + ink(c, counts[next].second).bg, kShade[i == end - 2 ? 3 : 2]);
            continue;
        }
        std::size_t prev = k;
        while (prev > 0 && share[prev - 1] == 0) prev--;
        if (prev > 0 && share[k] >= 3 && share[prev - 1] >= 3 && i == start) {
            cells.add(ink(c, counts[prev - 1].second).fg + ";" + here.bg, kShade[1]);
            continue;
        }
        cells.add(here.fg, kShade[4]);
    }
    return cells.done();
}

std::string scanner(const Caps& c, int frame, int width) {
    int span = std::max(1, width - 1);
    int at = frame % (2 * span);
    bool rightward = at < span;
    int head = rightward ? at : 2 * span - at;
    std::string fg = ink(c, Tone::accent).fg;
    Cells cells{c, "", ""};
    for (int i = 0; i < width; i++) {
        int behind = rightward ? head - i : i - head;
        int q = behind >= 0 && behind < 4 ? 4 - behind : 0;
        if (q == 0) cells.add(kTrack, kShade[1]);
        else cells.add(fg, kShade[q]);
    }
    return cells.done();
}

std::string duration(double seconds) {
    char buf[48];
    if (seconds < 1) {
        std::snprintf(buf, sizeof buf, "%.0f ms", seconds * 1000);
    } else if (seconds < 10) {
        std::snprintf(buf, sizeof buf, "%.2f s", seconds);
    } else if (seconds < 60) {
        std::snprintf(buf, sizeof buf, "%.1f s", seconds);
    } else {
        int s = static_cast<int>(seconds + 0.5);
        std::snprintf(buf, sizeof buf, "%d min %d s", s / 60, s % 60);
    }
    return buf;
}

std::string elapsed(double seconds) {
    char buf[48];
    if (seconds < 60) {
        std::snprintf(buf, sizeof buf, "%.1f s", seconds);
    } else {
        int s = static_cast<int>(seconds);
        std::snprintf(buf, sizeof buf, "%d min %d s", s / 60, s % 60);
    }
    return buf;
}

Status::Status(std::string text, int indent, std::chrono::milliseconds after)
    : text_(std::move(text)), indent_(indent), after_(after) {
    start();
}

Status::Status(std::function<std::string(int, double)> draw, std::chrono::milliseconds after)
    : draw_(std::move(draw)), after_(after) {
    start();
}

void Status::start() {
    if (!err().live) return;
    Status* none = nullptr;
    if (!active.compare_exchange_strong(none, this)) return;  // one at a time
    began_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this] { run(); });
}

Status::~Status() { stop(); }

void Status::set(std::string text) {
    std::lock_guard<std::mutex> lock(mu_);
    text_ = std::move(text);
}

void Status::progress(double fraction) {
    std::lock_guard<std::mutex> lock(mu_);
    fraction_ = fraction;
}

void Status::run() {
    std::unique_lock<std::mutex> lock(mu_);
    if (cv_.wait_for(lock, after_, [this] { return stopping_; })) return;
    // What's waiting to go to standard output goes first, so it comes above the status line.
    std::fflush(stdout);
    const Caps& c = err();
    for (int frame = 0;; frame++) {
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began_).count();
        std::string body;
        if (draw_) {
            body = draw_(frame, seconds);
        } else {
            bool filling = fraction_ >= 0;
            int lead = filling ? 16 : 8;
            std::string time = seconds >= 1 ? elapsed(seconds) : "";
            int room = c.columns - indent_ - lead - 2 - (time.empty() ? 0 : width(time) + 2) - 1;
            body = std::string(indent_, ' ') + (filling ? bar(c, fraction_, lead, frame) : scanner(c, frame, lead)) +
                   "  " + runes_up_to(text_, std::max(0, room)) + (time.empty() ? "" : "  " + dim(c, time));
        }
        std::string line = "\r\x1b[2K" + body;
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
        shown_ = true;
        if (cv_.wait_for(lock, std::chrono::milliseconds(80), [this] { return stopping_; })) return;
    }
}

void Status::stop(const std::string& done) {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    thread_.join();
    Status* self = this;
    active.compare_exchange_strong(self, nullptr);
    if (shown_) std::fputs("\r\x1b[2K", stderr);
    if (!done.empty()) std::fprintf(stderr, "%s\n", done.c_str());
    std::fflush(stderr);
}

void quiet() {
    if (Status* s = active.load()) s->stop();
}

void step(std::string text) {
    if (Status* s = active.load()) s->set(std::move(text));
}

}  // namespace calx::term
