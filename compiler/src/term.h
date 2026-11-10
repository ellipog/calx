// How calx looks in a terminal (CLX-250): Calx's amber, its mark and its dither, symbols, bars and
// a status line that redraws itself. Output to a pipe or a file, in CI, or with NO_COLOR set is
// plain, the same bytes it always was, so scripts, tests and editors read what they always read;
// only a terminal someone's watching gets colour, symbols and animation.
//
// The dither is the shade characters ░ ▒ ▓ █, a quarter, a half, three quarters and all of a cell
// in ink. Drawn in one colour on the terminal's background they fade a colour in or out, as a
// heading's chip does into the background; drawn in one colour on another, they blend the two, as
// the test tally does where its passes meet its failures.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace calx::term {

// What a stream can show.
struct Caps {
    bool color = false;      // ANSI colour and the symbols
    bool truecolor = false;  // 24-bit colour, for the amber
    bool live = false;       // lines that redraw themselves: a terminal, not CI
    int columns = 80;
};

// What standard output and standard error can show, worked out the first time each is asked for.
// NO_COLOR turns colour off; CLICOLOR_FORCE=1 or FORCE_COLOR turns it on, with animation, for a
// recording or a screenshot. On Windows, asking turns on the console's ANSI codes and UTF-8.
const Caps& out();
const Caps& err();

// Text in a style, if `c` shows colour, and as it is if not.
std::string accent(const Caps& c, std::string_view s);  // Calx's amber: names, commands, the mark
std::string good(const Caps& c, std::string_view s);    // green
std::string warning(const Caps& c, std::string_view s); // yellow
std::string bad(const Caps& c, std::string_view s);     // red
std::string dim(const Caps& c, std::string_view s);     // grey, for what's secondary
std::string bold(const Caps& c, std::string_view s);

// `text` with each `...` in it in the accent: with its backticks if `ticks`, as an error message
// keeps them around a name, or without, for advice that names a command to run. As it is if `c`
// shows no colour.
std::string code(const Caps& c, std::string_view text, bool ticks);

// The marks a line starts with, coloured: ✓, ! and ✗.
std::string tick(const Caps& c);
std::string bang(const Caps& c);
std::string cross(const Caps& c);

// How many columns `s` takes: its characters, not counting the escape codes that colour them.
int width(std::string_view s);
// `s` with spaces after it to make it `columns` wide, if it's narrower.
std::string pad(std::string_view s, int columns);
// `text`'s words in lines of at most `columns`, breaking only at spaces.
std::vector<std::string> wrap(std::string_view text, int columns);

// The colours a chip, a band and the ratio bar come in.
enum class Tone { accent, good, warning, bad };

// A heading: `text` on a chip of amber that dithers out into the background, ` text ▓▒░`.
std::string heading(const Caps& c, std::string_view text);
// A result: `text` on a chip that dithers in and out, `░▒▓ text ▓▒░`.
std::string band(const Caps& c, std::string_view text, Tone tone);

// The Calx mark, the C whose ends cross to make the x, in seven rows of 22 cells shaded by how
// much of each the mark covers, in amber, with `lines` beside it, one row each, `indent` columns
// in. Each row ends in a line end. Without colour, just `lines`, as they are.
std::string beside_mark(const Caps& c, const std::vector<std::string>& lines, int indent = 1);

// A bar `width` cells wide, `fraction` of it filled: full cells, then an edge that fades through
// ▓ ▒ ░ over a few cells, on a grey ░ track. Given a `frame`, the edge breathes, for a bar that's
// being drawn again and again while something runs.
std::string bar(const Caps& c, double fraction, int width, int frame = -1);
// A bar `width` cells wide split between `counts`, each in its tone, blended in dither where one
// meets the next, such as the passes and failures of a test run.
std::string ratio(const Caps& c, const std::vector<std::pair<std::size_t, Tone>>& counts, int width);
// A comet of dither that sweeps to and fro across a ░ track `width` cells wide, at `frame`.
std::string scanner(const Caps& c, int frame, int width);

// `seconds` as `420 ms`, `4.20 s`, `12.3 s` or `2 min 5 s`.
std::string duration(double seconds);
// `seconds` so far, for a status line: `0.4 s`, `12.3 s`, `2 min 5 s`.
std::string elapsed(double seconds);

// A status line on standard error, such as `░▒▓█   Building hello.calx  1.2 s`, which a thread of
// its own draws, with a scanner that sweeps to and fro and the time so far, once `after` has
// passed, and redraws as `set` changes it, so a step that's quick never shows one. `progress`
// puts a bar in the scanner's place. It's `indent` columns in, where the mark of the line that
// takes its place goes: 0 for a build's `✓ Built`, 2 for a test's `  ✓ name`. `stop` clears it,
// and then prints `done` in its place if that's given. On a stream that isn't live it does
// nothing at all.
class Status {
public:
    explicit Status(std::string text, int indent = 0, std::chrono::milliseconds after = std::chrono::milliseconds(250));
    // A status line that `draw` draws, given the frame and the seconds since it began.
    Status(std::function<std::string(int frame, double seconds)> draw, std::chrono::milliseconds after);
    ~Status();
    void set(std::string text);
    void progress(double fraction);
    void stop(const std::string& done = "");

private:
    void start();
    void run();
    std::mutex mu_;
    std::condition_variable cv_;
    std::string text_;
    std::function<std::string(int, double)> draw_;
    double fraction_ = -1;
    bool stopping_ = false;
    bool shown_ = false;
    int indent_ = 0;
    std::chrono::milliseconds after_;
    std::chrono::steady_clock::time_point began_;
    std::thread thread_;
};

// Takes the status line being drawn, if there is one, off the screen, for what's about to be
// written to standard error in its place, such as an error.
void quiet();

// Changes what the status line being drawn says, if there is one, as a build moves from step to
// step.
void step(std::string text);

}  // namespace calx::term
