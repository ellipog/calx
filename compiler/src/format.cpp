// `calx fmt` (CLX-89). The formatter works on tokens, not on the syntax tree, and keeps the
// program's line breaks, which end statements in Calx, so it can't change what a program means:
//
// - each line is indented four spaces for each line that opened a bracket it's inside, so brackets
//   opened on one line count once; a line that starts by closing a bracket lines up with the line
//   that opened it, and a line that goes on with the statement of the line before, which ended in
//   an operator such as `+`, `and` or `=`, is indented once more;
// - spaces between tokens follow fixed rules, except where the lexer or the parser reads them: a
//   `<` that starts generic arguments, a `?` that makes a type optional, a `(` that calls, and a `.`
//   keep the space or its absence that they had;
// - runs of blank lines become one, and none starts or ends a block or the file;
// - comments are kept as they're written, but for spaces at their ends, and one after code keeps
//   its column, so comments lined up stay lined up;
// - lines end in `\n`, with no spaces before it, and the file ends in one.
//
// Strings that span lines are kept as they are, since their lines are their text. Each result is
// checked: it must lex to the same tokens and comments, and formatting it again must not change it.
#include "format.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "lexer.h"
#include "parser.h"
#include "source.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace calx {

namespace {

constexpr int kIndent = 4;

// A line of the formatted file: its tokens, then a comment, either of which may be missing.
struct Line {
    std::vector<std::size_t> toks;  // indices into the file's tokens
    std::string comment;            // `// ...`, without the spaces after it
    std::uint32_t comment_col = 0;  // its column in the source, in runes
    std::uint32_t start_col = 0;    // the column of the line's first token or comment
    std::uint32_t code_end = 0;     // the source offset just past the line's last token
    int blank_before = 0;           // blank lines before it in the source
    bool shebang = false;           // a `#!` first line, kept as it is
};

// The tokens and comments of a file, as lines.
struct Scanned {
    std::vector<Token> toks;  // without newlines and the end of the file
    std::vector<bool> binary;  // for each token, whether the parser took it as a binary operator
    std::vector<Line> lines;
    std::vector<std::string> comments;  // every comment, for the check
    bool ok = true;
    std::string error;
};

std::string_view text_of(const std::string& text, const Token& t) {
    return std::string_view(text).substr(t.span.begin, t.span.end - t.span.begin);
}

std::string trim_end(std::string_view s) {
    std::size_t n = s.size();
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return std::string(s.substr(0, n));
}

// A comment as it was written, without spaces at its end or the `\r` of a Windows line end.
std::string comment_text(std::string_view s) { return trim_end(s); }

// The column of `offset` in runes, counted from the start of its line.
std::uint32_t column(const std::string& text, std::uint32_t offset) {
    std::uint32_t col = 0;
    for (std::uint32_t i = offset; i > 0 && text[i - 1] != '\n'; i--) {
        if ((static_cast<unsigned char>(text[i - 1]) & 0xC0) != 0x80) col++;
    }
    return col;
}

std::uint32_t runes(std::string_view s) {
    std::uint32_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) n++;
    }
    return n;
}

Scanned scan(const std::string& path, const std::string& text) {
    Scanned s;
    Sources sources;
    Diagnostics diags{sources};
    Names names;
    Arena arena;
    std::uint32_t id = sources.add(path, text);
    const SourceFile& f = sources.file(id);
    std::vector<Token> all = lex(f, id, 0, static_cast<std::uint32_t>(text.size()), names, arena, diags);
    std::vector<std::uint32_t> ops;
    if (!diags.has_errors()) parse_file(ParseContext{sources, names, arena, diags, &ops}, id);
    if (diags.has_errors()) {
        s.ok = false;
        for (const Diagnostic& d : diags.all()) s.error += diags.format(d, false);
        return s;
    }
    std::sort(ops.begin(), ops.end());
    for (const Token& t : all) {
        if (t.kind == Tok::newline || t.kind == Tok::eof) continue;
        s.toks.push_back(t);
        s.binary.push_back(std::binary_search(ops.begin(), ops.end(), t.span.begin));
    }

    Line cur;
    bool has = false;  // whether `cur` has anything on it
    int breaks = 1;    // line ends since the last line with something on it ended
    std::uint32_t pos = 0;
    if (text.size() >= 2 && text[0] == '#' && text[1] == '!') {
        while (pos < text.size() && text[pos] != '\n') pos++;
        cur.shebang = true;
        cur.comment = trim_end(std::string_view(text).substr(0, pos));
        s.lines.push_back(cur);
        cur = Line();
        breaks = 0;
    }
    auto start = [&](std::uint32_t at) {
        if (has) return;
        has = true;
        cur.blank_before = breaks > 0 ? breaks - 1 : 0;
        cur.start_col = column(text, at);
    };
    auto gap = [&](std::uint32_t from, std::uint32_t to) {
        std::uint32_t p = from;
        while (p < to) {
            char c = text[p];
            if (c == '\n') {
                if (has) {
                    s.lines.push_back(std::move(cur));
                    cur = Line();
                    has = false;
                    breaks = 1;
                } else {
                    breaks++;
                }
                p++;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                p++;
            } else if (c == '/' && p + 1 < to && text[p + 1] == '/') {
                std::uint32_t q = p;
                while (q < to && text[q] != '\n') q++;
                start(p);
                cur.comment = comment_text(std::string_view(text).substr(p, q - p));
                cur.comment_col = column(text, p);
                s.comments.push_back(cur.comment);
                p = q;
            } else {
                s.ok = false;
                s.error = path + ": calx fmt doesn't know what to do with the text at line " +
                          std::to_string(f.position(p).first);
                return;
            }
        }
    };
    for (std::size_t i = 0; i < s.toks.size() && s.ok; i++) {
        const Token& t = s.toks[i];
        gap(i == 0 ? pos : s.toks[i - 1].span.end, t.span.begin);
        start(t.span.begin);
        cur.toks.push_back(i);
        cur.code_end = t.span.end;
    }
    if (s.ok) gap(s.toks.empty() ? pos : s.toks.back().span.end, static_cast<std::uint32_t>(text.size()));
    if (has) s.lines.push_back(std::move(cur));
    return s;
}

bool opener(Tok k) { return k == Tok::lparen || k == Tok::lbracket || k == Tok::lbrace; }
bool closer(Tok k) { return k == Tok::rparen || k == Tok::rbracket || k == Tok::rbrace; }

// A token a value ends with.
bool ends_value(Tok k) {
    switch (k) {
        case Tok::ident: case Tok::int_lit: case Tok::float_lit: case Tok::char_lit: case Tok::string_lit:
        case Tok::rparen: case Tok::rbracket: case Tok::kw_self: case Tok::kw_true: case Tok::kw_false:
        case Tok::kw_none:
            return true;
        default:
            return false;
    }
}

// A token after which a `*` or `&` can only start something: a type, such as `*const u8`.
bool before_value(Tok k) {
    switch (k) {
        case Tok::lparen: case Tok::lbracket: case Tok::lbrace: case Tok::comma: case Tok::colon:
        case Tok::semicolon: case Tok::eq: case Tok::eqeq: case Tok::ne: case Tok::le: case Tok::ge:
        case Tok::plus_eq: case Tok::minus_eq: case Tok::star_eq: case Tok::slash_eq:
        case Tok::percent_eq: case Tok::shl_eq: case Tok::shr_eq: case Tok::ushr_eq: case Tok::amp_eq:
        case Tok::pipe_eq: case Tok::caret_eq: case Tok::arrow: case Tok::fat_arrow: case Tok::question_question:
        case Tok::plus: case Tok::minus: case Tok::star: case Tok::slash: case Tok::percent: case Tok::shl:
        case Tok::amp: case Tok::caret: case Tok::tilde: case Tok::kw_return: case Tok::kw_in: case Tok::kw_and:
        case Tok::kw_or: case Tok::kw_not: case Tok::kw_if: case Tok::kw_while: case Tok::kw_match: case Tok::kw_fail:
        case Tok::kw_try:
            return true;
        default:
            return false;
    }
}

// Operators that always take a value on each side, with a space on each side.
bool binary_only(Tok k) {
    switch (k) {
        case Tok::eq: case Tok::eqeq: case Tok::ne: case Tok::le: case Tok::ge: case Tok::plus_eq: case Tok::minus_eq:
        case Tok::star_eq: case Tok::slash_eq: case Tok::percent_eq: case Tok::shl_eq: case Tok::shr_eq:
        case Tok::ushr_eq: case Tok::amp_eq: case Tok::pipe_eq: case Tok::caret_eq: case Tok::arrow:
        case Tok::fat_arrow: case Tok::question_question: case Tok::slash: case Tok::percent: case Tok::caret:
        case Tok::shl:
            return true;
        default:
            return false;
    }
}

// A token a line can end with when the statement goes on, on the next line.
bool continues(Tok k) {
    return binary_only(k) || k == Tok::plus || k == Tok::minus || k == Tok::star || k == Tok::amp ||
           k == Tok::kw_and || k == Tok::kw_or;
}

// A token after which a `(` with no space before it calls: `f(x)`, `f<T>(x)`, `g()(x)`.
bool calls_after(Tok k) {
    return ends_value(k) || k == Tok::gt || k == Tok::shr || k == Tok::ushr || k == Tok::rbrace || k == Tok::question ||
           k == Tok::bang;
}

// Whether the lexer or the parser reads the space between `x` and `y`, so it's kept as it was: a
// `<` with none before it may start generic arguments, a `?` with none makes a type optional, a
// `(` with none calls, and a `.` with none reads a field.
bool space_matters(Tok x, Tok y) {
    return y == Tok::lt || y == Tok::question || y == Tok::dot || (y == Tok::lparen && calls_after(x));
}

bool is_word(Tok k) { return k == Tok::ident || (k >= Tok::kw_and && k <= Tok::kw_while); }

enum class Role { other, binary, unary };

// Whether a space goes between `a` and `b`, two tokens on one line: 1 or 0, or -1 to keep what
// the source had.
int space(const Token& a, Role ra, bool tight_a, const Token& b, Role rb, bool tight_b) {
    Tok x = a.kind, y = b.kind;
    // A `<` that's a comparison takes a space, since only one with none before it can start
    // generic arguments; one that starts them takes none after it, nor does the `>` that ends them.
    if (y == Tok::lt && rb == Role::binary) return 1;
    if (x == Tok::lt) return ra == Role::binary ? 1 : 0;
    if ((y == Tok::gt || y == Tok::shr || y == Tok::ushr) && rb != Role::binary) return 0;
    if (space_matters(x, y)) return -1;
    if (y == Tok::comma || y == Tok::semicolon || y == Tok::rparen || y == Tok::rbracket) return 0;
    if (x == Tok::lparen || x == Tok::lbracket) return 0;
    if (x == Tok::comma || x == Tok::semicolon) return 1;
    if (x == Tok::dot || x == Tok::question_dot || x == Tok::at) return 0;
    if (y == Tok::question_dot) return ends_value(x) ? 0 : -1;
    if (y == Tok::colon) return 0;
    if (x == Tok::colon) return 1;
    if (binary_only(x) || binary_only(y)) return 1;
    if (rb == Role::binary || ra == Role::binary) return 1;
    if (ra == Role::unary) return 0;
    if (x == Tok::dotdot || x == Tok::dotdoteq || y == Tok::dotdot || y == Tok::dotdoteq) return -1;
    // `{ x }`, `{}`, `} else {`, and `import std.fs.{read, write}`.
    if (x == Tok::lbrace) return y == Tok::rbrace || tight_a ? 0 : 1;
    if (y == Tok::rbrace) return tight_b ? 0 : 1;
    if (y == Tok::lbrace) return 1;
    if (x == Tok::rbrace) return is_word(y) ? 1 : -1;
    return -1;
}

std::string render(const std::string& text, const Scanned& s) {
    const std::vector<Token>& toks = s.toks;
    // What the parser took as binary operators are; a `-` or `~` it didn't is a sign, and a `*`
    // or `&` it didn't starts a type, such as `*const u8`. A `+` that isn't an operator joins
    // bounds, as in `T: Add<T> + Copy`.
    std::vector<Role> roles(toks.size(), Role::other);
    for (std::size_t i = 0; i < toks.size(); i++) {
        Tok k = toks[i].kind;
        if (s.binary[i] || k == Tok::plus) {
            roles[i] = Role::binary;
        } else if (k == Tok::minus || k == Tok::tilde) {
            roles[i] = Role::unary;
        } else if ((k == Tok::star || k == Tok::amp) && (i == 0 || before_value(toks[i - 1].kind))) {
            roles[i] = Role::unary;
        }
    }
    // Braces after a `.`, as in `import std.fs.{read, write}`, hold a list, with no spaces inside.
    std::vector<bool> tight(toks.size(), false);
    {
        std::vector<std::size_t> opens;
        for (std::size_t i = 0; i < toks.size(); i++) {
            if (toks[i].kind == Tok::lbrace) {
                tight[i] = i > 0 && toks[i - 1].kind == Tok::dot;
                opens.push_back(i);
            } else if (toks[i].kind == Tok::rbrace && !opens.empty()) {
                tight[i] = tight[opens.back()];
                opens.pop_back();
            }
        }
    }

    // An open bracket: the level of the line it opened on, which the line that closes it goes back
    // to, and the level of what's inside it.
    struct Open {
        Tok kind;
        std::size_t line;
        int outer;
        int inner;
    };
    std::vector<Open> stack;
    std::string out;
    const Line* prev = nullptr;
    const Line* prev_code = nullptr;  // the last line with tokens
    for (std::size_t n = 0; n < s.lines.size(); n++) {
        const Line& line = s.lines[n];
        if (line.shebang) {
            out += line.comment + "\n";
            prev = &line;
            continue;
        }
        bool starts_closed = !line.toks.empty() && closer(toks[line.toks[0]].kind);
        // A line end ends a statement only in braces or at the top, so a line there that comes
        // after an operator goes on with the statement before it.
        bool in_braces = stack.empty() || stack.back().kind == Tok::lbrace;
        int more = in_braces && !starts_closed && prev_code && continues(toks[prev_code->toks.back()].kind) ? 1 : 0;
        int indent = starts_closed && !stack.empty() ? stack.back().outer : (stack.empty() ? 0 : stack.back().inner) + more;
        bool after_open = prev && !prev->shebang && !prev->toks.empty() && prev->comment.empty() &&
                          opener(toks[prev->toks.back()].kind);
        if (prev && line.blank_before > 0 && !after_open && !starts_closed) out += "\n";

        std::string code;
        for (std::size_t j = 0; j < line.toks.size(); j++) {
            std::size_t i = line.toks[j];
            const Token& t = toks[i];
            if (j > 0) {
                int sp = space(toks[i - 1], roles[i - 1], tight[i - 1], t, roles[i], tight[i]);
                if (sp == 1 || (sp == -1 && t.space_before)) code += ' ';
            }
            code += t.kind == Tok::doc_comment ? comment_text(text_of(text, t)) : std::string(text_of(text, t));
            if (opener(t.kind)) {
                if (!stack.empty() && stack.back().line == n) {
                    stack.push_back({t.kind, n, stack.back().outer, stack.back().inner});
                } else {
                    // At most the line's own level: `} else {` opens where the `}` closed, and
                    // `u0: i32) -> bool {`, which closed what made it a continuation, opens where
                    // that started.
                    int outer = std::min(indent, (stack.empty() ? 0 : stack.back().inner) + more);
                    stack.push_back({t.kind, n, outer, outer + 1});
                }
            } else if (closer(t.kind) && !stack.empty()) {
                stack.pop_back();
            }
        }
        std::string pad(static_cast<std::size_t>(indent * kIndent), ' ');
        if (code.empty()) {
            out += pad + line.comment;
        } else {
            out += pad + code;
            if (!line.comment.empty()) {
                // The comment keeps its column, measured from the line's indentation, if the
                // code still fits before it; otherwise it keeps the spaces it had.
                std::size_t nl = code.rfind('\n');
                std::uint32_t width = runes(nl == std::string::npos ? code : std::string_view(code).substr(nl + 1));
                std::int64_t at = static_cast<std::int64_t>(line.comment_col) - line.start_col;
                std::int64_t gap = nl == std::string::npos ? at - width : -1;
                if (gap < 1) gap = std::max<std::int64_t>(1, static_cast<std::int64_t>(line.comment_col) - column(text, line.code_end));
                out += std::string(static_cast<std::size_t>(gap), ' ') + line.comment;
            }
        }
        out += "\n";
        prev = &line;
        if (!line.toks.empty()) prev_code = &line;
    }
    return out;
}

// Whether `a` and `b` have the same tokens, with the same spaces where spaces matter, and the
// same comments.
bool same(const std::string& ta, const Scanned& a, const std::string& tb, const Scanned& b) {
    if (a.toks.size() != b.toks.size() || a.comments != b.comments) return false;
    for (std::size_t i = 0; i < a.toks.size(); i++) {
        const Token& x = a.toks[i];
        const Token& y = b.toks[i];
        if (x.kind != y.kind) return false;
        if (x.kind == Tok::doc_comment) {
            if (comment_text(text_of(ta, x)) != comment_text(text_of(tb, y))) return false;
        } else if (text_of(ta, x) != text_of(tb, y)) {
            return false;
        }
        if (a.binary[i] != b.binary[i]) return false;
        bool spaced_comparison = x.kind == Tok::lt && a.binary[i];
        if (i > 0 && space_matters(a.toks[i - 1].kind, x.kind) && !spaced_comparison && x.space_before != y.space_before) {
            return false;
        }
    }
    return true;
}

// The statements end where they ended: the newline tokens, which the lexer makes only where a
// line end can end one, fall between the same tokens.
std::vector<std::size_t> statement_ends(const std::string& path, const std::string& text) {
    Sources sources;
    Diagnostics diags{sources};
    Names names;
    Arena arena;
    std::uint32_t id = sources.add(path, text);
    std::vector<Token> all = lex(sources.file(id), id, 0, static_cast<std::uint32_t>(text.size()), names, arena, diags);
    std::vector<std::size_t> ends;
    std::size_t n = 0;
    for (const Token& t : all) {
        if (t.kind == Tok::newline) {
            ends.push_back(n);
        } else if (t.kind != Tok::eof) {
            n++;
        }
    }
    // One after the last token ends nothing more, and a file that didn't end in a line end gains one.
    while (!ends.empty() && ends.back() == n) ends.pop_back();
    return ends;
}

}  // namespace

bool format_source(const std::string& path, const std::string& text, std::string& out, std::string& error) {
    // A byte order mark stays where it is, and what follows it is formatted as a file (CLX-206).
    if (has_bom(text)) {
        if (!format_source(path, text.substr(3), out, error)) return false;
        out.insert(0, "\xEF\xBB\xBF");
        return true;
    }
    Scanned s = scan(path, text);
    if (!s.ok) {
        error = s.error;
        return false;
    }
    out = render(text, s);
    Scanned again = scan(path, out);
    if (!again.ok || !same(text, s, out, again) || statement_ends(path, text) != statement_ends(path, out) ||
        render(out, again) != out) {
        error = path + ": calx fmt couldn't format this file without changing it, which is a bug in calx fmt; the file is left as it was\n";
        return false;
    }
    return true;
}

namespace {

bool read_file(const fs::path& p, std::string& text) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// The `.calx` files under `dir`, in a fixed order, past folders whose names start with `.`.
void calx_files(const fs::path& dir, std::vector<fs::path>& out) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        std::error_code e;
        if (it->is_directory(e)) {
            if (!name.empty() && name[0] == '.') it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() == ".calx" && it->is_regular_file(e)) found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    out.insert(out.end(), found.begin(), found.end());
}

}  // namespace

int run_fmt(const std::vector<std::string>& args) {
    bool check = false;
    bool from_stdin = false;
    std::vector<std::string> paths;
    for (const std::string& a : args) {
        if (a == "--check") {
            check = true;
        } else if (a == "--stdin") {
            from_stdin = true;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "calx fmt: unknown option %s; it takes --check, --stdin, and files or folders\n", a.c_str());
            return 2;
        } else {
            paths.push_back(a);
        }
    }
    if (from_stdin) {
        if (paths.size() > 1) {
            std::fprintf(stderr, "calx fmt: --stdin takes at most one path, which names the text in errors\n");
            return 2;
        }
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::string text((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        std::string out, error;
        if (!format_source(paths.empty() ? "<stdin>" : paths[0], text, out, error)) {
            std::fputs(error.c_str(), stderr);
            return 1;
        }
        if (check) return out == text ? 0 : 1;
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }
    std::vector<fs::path> files;
    if (paths.empty()) paths.push_back(".");
    for (const std::string& p : paths) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            calx_files(p, files);
        } else if (fs::exists(p, ec)) {
            files.push_back(p);
        } else {
            std::fprintf(stderr, "calx fmt: %s isn't there\n", p.c_str());
            return 1;
        }
    }
    int status = 0;
    for (const fs::path& p : files) {
        std::string text, out, error;
        if (!read_file(p, text)) {
            std::fprintf(stderr, "calx fmt: couldn't read %s\n", p.string().c_str());
            status = 1;
            continue;
        }
        if (!format_source(p.string(), text, out, error)) {
            std::fputs(error.c_str(), stderr);
            status = 1;
            continue;
        }
        if (out == text) continue;
        if (check) {
            std::printf("%s isn't formatted\n", p.string().c_str());
            status = 1;
            continue;
        }
        std::ofstream o(p, std::ios::binary);
        o << out;
        o.close();
        if (!o) {
            std::fprintf(stderr, "calx fmt: couldn't write %s\n", p.string().c_str());
            status = 1;
            continue;
        }
        std::printf("formatted %s\n", p.string().c_str());
    }
    if (files.empty()) std::printf("calx fmt: no .calx files here\n");
    return status;
}

}  // namespace calx
