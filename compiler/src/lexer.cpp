#include "lexer.h"

#include <string>

namespace calx {

const char* tok_text(Tok t) {
    switch (t) {
        case Tok::eof: return "end of file";
        case Tok::newline: return "end of line";
        case Tok::doc_comment: return "doc comment";
        case Tok::ident: return "identifier";
        case Tok::int_lit: return "integer literal";
        case Tok::float_lit: return "float literal";
        case Tok::char_lit: return "character literal";
        case Tok::string_lit: return "string literal";
#define X(name, text) \
    case Tok::name: return "`" text "`";
            CALX_KEYWORDS(X) CALX_PUNCT(X)
#undef X
    }
    return "token";
}

namespace {

struct Keyword {
    std::string_view text;
    Tok kind;
};

constexpr Keyword kKeywords[] = {
#define X(name, text) {text, Tok::name},
    CALX_KEYWORDS(X)
#undef X
};

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Lexer {
public:
    Lexer(const SourceFile& file, std::uint32_t file_id, std::uint32_t begin, std::uint32_t end, Names& names,
          Arena& arena, Diagnostics& diags)
        : src_(file.text), file_(file_id), pos_(begin), end_(end), names_(names), arena_(arena), diags_(diags) {}

    std::vector<Token> run() {
        bool space = false;
        while (true) {
            space |= skip_space_and_comments();
            if (pos_ >= end_) break;
            char c = src_[pos_];
            if (c == '\n') {
                pos_++;
                // A newline ends a statement only at top level or directly inside braces.
                bool in_braces = brackets_.empty() || brackets_.back() == '{';
                if (in_braces && !out_.empty() && out_.back().kind != Tok::newline) {
                    push(Tok::newline, pos_ - 1, pos_);
                }
                space = true;
                continue;
            }
            std::size_t before = out_.size();
            lex_token();
            if (out_.size() > before) out_.back().space_before = space;
            space = false;
        }
        if (!brackets_.empty()) {
            diags_.error(Span{file_, open_at_.back(), open_at_.back() + 1},
                         std::string("unclosed `") + brackets_.back() + "`");
        }
        Token eof;
        eof.kind = Tok::eof;
        eof.span = Span{file_, end_, end_};
        out_.push_back(eof);
        return std::move(out_);
    }

private:
    Span span(std::uint32_t a, std::uint32_t b) const { return Span{file_, a, b}; }

    Token& push(Tok kind, std::uint32_t a, std::uint32_t b) {
        Token t;
        t.kind = kind;
        t.span = span(a, b);
        out_.push_back(t);
        return out_.back();
    }

    char peek(std::uint32_t ahead = 0) const { return pos_ + ahead < end_ ? src_[pos_ + ahead] : '\0'; }

    // Skips spaces, tabs, carriage returns and `//` comments, but not newlines. A `///` comment
    // becomes a doc comment token. Returns whether anything was skipped.
    bool skip_space_and_comments() {
        bool skipped = false;
        while (pos_ < end_) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r') {
                pos_++;
                skipped = true;
            } else if (c == '/' && peek(1) == '/') {
                std::uint32_t start = pos_;
                bool doc = peek(2) == '/' && peek(3) != '/';
                while (pos_ < end_ && src_[pos_] != '\n') pos_++;
                if (doc) {
                    Token& t = push(Tok::doc_comment, start, pos_);
                    std::uint32_t text = start + 3;
                    if (text < pos_ && src_[text] == ' ') text++;
                    t.digits = std::string_view(src_).substr(text, pos_ - text);
                    if (!t.digits.empty() && t.digits.back() == '\r') t.digits.remove_suffix(1);
                }
                skipped = true;
            } else {
                break;
            }
        }
        return skipped;
    }

    void lex_token() {
        std::uint32_t start = pos_;
        char c = src_[pos_];
        if (is_ident_start(c)) {
            while (pos_ < end_ && is_ident_char(src_[pos_])) pos_++;
            std::string_view text = std::string_view(src_).substr(start, pos_ - start);
            // `name"..."` with nothing between them is a prefixed string.
            if (pos_ < end_ && src_[pos_] == '"') {
                lex_string(start, names_.intern(text));
                return;
            }
            for (const Keyword& k : kKeywords) {
                if (k.text == text) {
                    push(k.kind, start, pos_);
                    return;
                }
            }
            push(Tok::ident, start, pos_).name = names_.intern(text);
            return;
        }
        if (is_digit(c)) {
            lex_number();
            return;
        }
        if (c == '"') {
            lex_string(start, Name{});
            return;
        }
        if (c == '#') {
            std::uint32_t p = pos_;
            while (p < end_ && src_[p] == '#') p++;
            if (p < end_ && src_[p] == '"') {
                lex_raw_string(start);
                return;
            }
        }
        if (c == '\'') {
            lex_char();
            return;
        }
        lex_punct();
    }

    void lex_punct() {
        std::uint32_t start = pos_;
        char c = src_[pos_];
        char n1 = peek(1), n2 = peek(2), n3 = peek(3);
        auto emit = [&](Tok kind, std::uint32_t len) {
            pos_ += len;
            push(kind, start, pos_);
        };
        switch (c) {
            case '(': case '[': case '{':
                brackets_.push_back(c);
                open_at_.push_back(start);
                emit(c == '(' ? Tok::lparen : c == '[' ? Tok::lbracket : Tok::lbrace, 1);
                return;
            case ')': case ']': case '}': {
                char open = c == ')' ? '(' : c == ']' ? '[' : '{';
                if (brackets_.empty()) {
                    diags_.error(span(start, start + 1), std::string("`") + c + "` without a matching `" + open + "`");
                } else {
                    if (brackets_.back() != open) {
                        diags_.error(span(start, start + 1),
                                     std::string("`") + c + "` closes `" + brackets_.back() + "`");
                    }
                    brackets_.pop_back();
                    open_at_.pop_back();
                }
                // A newline right before a closing brace ends the last statement in it, and is no
                // longer needed.
                emit(c == ')' ? Tok::rparen : c == ']' ? Tok::rbracket : Tok::rbrace, 1);
                return;
            }
            case ',': emit(Tok::comma, 1); return;
            case ';': emit(Tok::semicolon, 1); return;
            case '@': emit(Tok::at, 1); return;
            case '~': emit(Tok::tilde, 1); return;
            case ':': emit(Tok::colon, 1); return;
            case '.':
                if (n1 == '.' && n2 == '=') emit(Tok::dotdoteq, 3);
                else if (n1 == '.') emit(Tok::dotdot, 2);
                else emit(Tok::dot, 1);
                return;
            case '-':
                if (n1 == '>') emit(Tok::arrow, 2);
                else if (n1 == '=') emit(Tok::minus_eq, 2);
                else emit(Tok::minus, 1);
                return;
            case '=':
                if (n1 == '>') emit(Tok::fat_arrow, 2);
                else if (n1 == '=') emit(Tok::eqeq, 2);
                else emit(Tok::eq, 1);
                return;
            case '?':
                if (n1 == '?') emit(Tok::question_question, 2);
                else if (n1 == '.') emit(Tok::question_dot, 2);
                else emit(Tok::question, 1);
                return;
            case '!':
                if (n1 == '=') emit(Tok::ne, 2);
                else emit(Tok::bang, 1);
                return;
            case '|': if (n1 == '=') emit(Tok::pipe_eq, 2); else emit(Tok::pipe, 1); return;
            case '+': if (n1 == '=') emit(Tok::plus_eq, 2); else emit(Tok::plus, 1); return;
            case '*': if (n1 == '=') emit(Tok::star_eq, 2); else emit(Tok::star, 1); return;
            case '/': if (n1 == '=') emit(Tok::slash_eq, 2); else emit(Tok::slash, 1); return;
            case '%': if (n1 == '=') emit(Tok::percent_eq, 2); else emit(Tok::percent, 1); return;
            case '&': if (n1 == '=') emit(Tok::amp_eq, 2); else emit(Tok::amp, 1); return;
            case '^': if (n1 == '=') emit(Tok::caret_eq, 2); else emit(Tok::caret, 1); return;
            case '<':
                if (n1 == '<' && n2 == '=') emit(Tok::shl_eq, 3);
                else if (n1 == '<') emit(Tok::shl, 2);
                else if (n1 == '=') emit(Tok::le, 2);
                else emit(Tok::lt, 1);
                return;
            case '>':
                // `>>` and `>>>` are lexed as one token each; the parser splits them where they
                // close generic arguments, as in `List<List<i32>>`.
                if (n1 == '>' && n2 == '>' && n3 == '=') emit(Tok::ushr_eq, 4);
                else if (n1 == '>' && n2 == '>') emit(Tok::ushr, 3);
                else if (n1 == '>' && n2 == '=') emit(Tok::shr_eq, 3);
                else if (n1 == '>') emit(Tok::shr, 2);
                else if (n1 == '=') emit(Tok::ge, 2);
                else emit(Tok::gt, 1);
                return;
            default:
                break;
        }
        // Skip one whole UTF-8 character, so the message names it.
        std::uint32_t len = 1;
        auto b = static_cast<unsigned char>(c);
        if (b >= 0xF0) len = 4;
        else if (b >= 0xE0) len = 3;
        else if (b >= 0xC0) len = 2;
        pos_ = std::min(pos_ + len, end_);
        diags_.error(span(start, pos_), "unexpected character `" + std::string(src_.substr(start, pos_ - start)) + "`");
    }

    void lex_number() {
        std::uint32_t start = pos_;
        int radix = 10;
        if (src_[pos_] == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B' ||
                                  peek(1) == 'o' || peek(1) == 'O')) {
            char r = peek(1);
            radix = (r == 'x' || r == 'X') ? 16 : (r == 'b' || r == 'B') ? 2 : 8;
            pos_ += 2;
        }
        std::string digits;
        auto take_digits = [&](int base) {
            while (pos_ < end_) {
                char c = src_[pos_];
                if (c == '_') {
                    pos_++;
                } else if (digit_value(c) < base) {
                    digits += c;
                    pos_++;
                } else {
                    break;
                }
            }
        };
        take_digits(radix);
        bool is_float = false;
        // A fraction needs a digit after the dot, so `0..3` is a range, and right after a `.` a
        // number is always an integer, so `t.0.1` is two field accesses.
        bool after_dot = !out_.empty() && out_.back().kind == Tok::dot && !out_.back().space_before;
        if (radix == 10 && !after_dot && peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            digits += '.';
            pos_++;
            take_digits(10);
        }
        if (radix == 10 && !after_dot && (peek() == 'e' || peek() == 'E') &&
            (is_digit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && is_digit(peek(2))))) {
            is_float = true;
            digits += 'e';
            pos_++;
            if (peek() == '+' || peek() == '-') digits += src_[pos_++];
            take_digits(10);
        }
        std::uint32_t suffix_start = pos_;
        while (pos_ < end_ && is_ident_char(src_[pos_])) pos_++;
        std::string_view suffix = std::string_view(src_).substr(suffix_start, pos_ - suffix_start);
        if (suffix == "f32" || suffix == "f64") is_float = radix == 10 ? true : is_float;

        Token& t = push(is_float ? Tok::float_lit : Tok::int_lit, start, pos_);
        t.digits = arena_.copy(digits);
        t.suffix = suffix;
        if (digits.empty()) {
            diags_.error(t.span, "a number needs at least one digit");
            return;
        }
        if (!is_float) {
            std::uint64_t v = 0;
            for (char c : digits) {
                auto d = static_cast<std::uint64_t>(digit_value(c));
                if (v > (UINT64_MAX - d) / static_cast<std::uint64_t>(radix)) t.overflow = true;
                v = v * static_cast<std::uint64_t>(radix) + d;
            }
            t.int_value = v;
        }
    }

    // Decodes one escape after a backslash at pos_. Returns the code point, or -1 on error.
    std::int64_t escape() {
        std::uint32_t start = pos_ - 1;
        char c = peek();
        pos_++;
        switch (c) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case '0': return 0;
            case '\\': return '\\';
            case '"': return '"';
            case '\'': return '\'';
            case '{': return '{';
            case '}': return '}';
            case 'u': {
                if (peek() != '{') break;
                pos_++;
                std::uint32_t cp = 0;
                int n = 0;
                while (pos_ < end_ && digit_value(src_[pos_]) < 16 && n < 7) {
                    cp = cp * 16 + static_cast<std::uint32_t>(digit_value(src_[pos_++]));
                    n++;
                }
                if (peek() != '}' || n == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    diags_.error(span(start, pos_), "`\\u{...}` needs 1 to 6 hex digits naming a Unicode scalar value");
                    if (peek() == '}') pos_++;
                    return -1;
                }
                pos_++;
                return cp;
            }
            default: break;
        }
        diags_.error(span(start, pos_), "unknown escape `\\" + std::string(1, c) + "`");
        return -1;
    }

    // One UTF-8 code point at pos_, advancing past it.
    std::uint32_t utf8_char() {
        auto b0 = static_cast<unsigned char>(src_[pos_]);
        int len = b0 < 0x80 ? 1 : b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : b0 >= 0xC0 ? 2 : 1;
        std::uint32_t cp = len == 1 ? b0 : len == 2 ? (b0 & 0x1F) : len == 3 ? (b0 & 0x0F) : (b0 & 0x07);
        pos_++;
        for (int i = 1; i < len && pos_ < end_; i++) cp = (cp << 6) | (static_cast<unsigned char>(src_[pos_++]) & 0x3F);
        return cp;
    }

    void lex_char() {
        std::uint32_t start = pos_;
        pos_++;  // the quote
        std::int64_t cp = -1;
        if (peek() == '\\') {
            pos_++;
            cp = escape();
        } else if (peek() != '\'' && peek() != '\n' && pos_ < end_) {
            cp = utf8_char();
        }
        if (peek() != '\'') {
            while (pos_ < end_ && src_[pos_] != '\'' && src_[pos_] != '\n') pos_++;
            if (peek() == '\'') pos_++;
            diags_.error(span(start, pos_), "a character literal holds exactly one character");
            push(Tok::char_lit, start, pos_);
            return;
        }
        pos_++;
        Token& t = push(Tok::char_lit, start, pos_);
        t.int_value = cp < 0 ? 0 : static_cast<std::uint64_t>(cp);
    }

    // A string literal from the opening quote at pos_, with `{expr}` interpolations. The
    // expressions are kept as spans and parsed later.
    void lex_string(std::uint32_t start, Name prefix) {
        pos_++;  // the quote
        std::vector<StringPart> parts;
        std::string text;
        auto flush_text = [&] {
            if (!text.empty()) {
                StringPart p;
                p.text = arena_.copy(text);
                parts.push_back(p);
                text.clear();
            }
        };
        bool closed = false;
        while (pos_ < end_) {
            char c = src_[pos_];
            if (c == '"') {
                pos_++;
                closed = true;
                break;
            }
            if (c == '\n') break;
            if (c == '\\') {
                pos_++;
                std::int64_t cp = escape();
                if (cp >= 0) append_utf8(text, static_cast<std::uint32_t>(cp));
                continue;
            }
            if (c == '{') {
                flush_text();
                std::uint32_t expr_start = ++pos_;
                if (!skip_interpolation()) {
                    diags_.error(span(expr_start - 1, pos_), "unclosed `{` in a string; write `\\{` for a brace");
                    break;
                }
                StringPart p;
                p.is_expr = true;
                p.span = span(expr_start, pos_ - 1);
                if (p.span.begin == p.span.end) diags_.error(span(expr_start - 1, pos_), "empty `{}` in a string");
                parts.push_back(p);
                continue;
            }
            if (c == '}') {
                diags_.error(span(pos_, pos_ + 1), "`}` in a string; write `\\}` for a brace");
                pos_++;
                continue;
            }
            text += c;
            pos_++;
        }
        flush_text();
        if (!closed) diags_.error(span(start, pos_), "unclosed string literal");
        Token& t = push(Tok::string_lit, start, pos_);
        t.name = prefix;
        t.parts = arena_.copy(parts);
    }

    // `#"..."#`, a raw string (D125), for text full of braces, quotes and backslashes, such as
    // code an attribute generates. Nothing in it is an escape, `{` and `}` are themselves, and
    // `#{expr}` interpolates, even straight after a quote. With more `#`s at both ends, such as
    // `##"..."##`, the text can hold `"#`, and interpolation takes as many: `##{expr}`. One whose
    // `#"` ends its line is a block:
    // its text is the lines up to the closing line, less the closing line's indentation, so it can
    // be indented with the code around it.
    void lex_raw_string(std::uint32_t start) {
        std::uint32_t hashes = 0;
        while (src_[pos_] == '#') {
            hashes++;
            pos_++;
        }
        pos_++;  // the quote
        auto closes_at = [&](std::uint32_t p) {
            if (src_[p] != '"') return false;
            for (std::uint32_t i = 1; i <= hashes; i++) {
                if (p + i >= end_ || src_[p + i] != '#') return false;
            }
            // `"#{name}` is a quote and an interpolation, as generated code often wants, since
            // nothing follows a string with `{`.
            return !(p + hashes + 1 < end_ && src_[p + hashes + 1] == '{');
        };
        auto interpolates_at = [&](std::uint32_t p) {
            for (std::uint32_t i = 0; i < hashes; i++) {
                if (p + i >= end_ || src_[p + i] != '#') return false;
            }
            return p + hashes < end_ && src_[p + hashes] == '{';
        };
        std::uint32_t after = pos_;
        if (after < end_ && src_[after] == '\r') after++;
        bool block = after < end_ && src_[after] == '\n';
        std::uint32_t first = block ? after + 1 : pos_;
        // Find the end first: a block's closing line says how much indentation its lines lose.
        std::uint32_t close = 0;
        bool found = false;
        for (std::uint32_t p = first; p < end_;) {
            if (closes_at(p)) {
                close = p;
                found = true;
                break;
            }
            if (!block && src_[p] == '\n') break;
            if (interpolates_at(p)) {
                pos_ = p + hashes + 1;
                if (!skip_interpolation()) break;
                p = pos_;
                continue;
            }
            p++;
        }
        if (!found) {
            pos_ = start + hashes + 1;
            while (pos_ < end_ && src_[pos_] != '\n') pos_++;
            diags_.error(span(start, pos_), block ? "unclosed raw string: it ends at `\"" + std::string(hashes, '#') + "`"
                                                  : "unclosed raw string; one that spans lines starts its text on the line after `#\"`");
            push(Tok::string_lit, start, pos_);
            return;
        }
        std::uint32_t end = close;  // the text ends here
        std::string_view indent;
        if (block) {
            std::uint32_t line = close;
            while (line > first && src_[line - 1] != '\n') line--;
            indent = std::string_view(src_).substr(line, close - line);
            for (char ch : indent) {
                if (ch != ' ' && ch != '\t') {
                    diags_.error(span(close, close + 1 + hashes), "a block's closing `\"" + std::string(hashes, '#') +
                                                                      "` goes on a line of its own");
                    indent = {};
                    break;
                }
            }
            // The line break before the closing line isn't part of the text.
            end = line > first ? line - 1 : first;
            if (end > first && src_[end - 1] == '\r') end--;
            if (line == first) end = first;
        }
        std::vector<StringPart> parts;
        std::string text;
        auto flush_text = [&] {
            if (!text.empty()) {
                StringPart part;
                part.text = arena_.copy(text);
                parts.push_back(part);
                text.clear();
            }
        };
        bool line_start = block;
        for (std::uint32_t p = first; p < end;) {
            if (line_start) {
                line_start = false;
                std::uint32_t q = p;
                while (q < end && (src_[q] == ' ' || src_[q] == '\t')) q++;
                bool blank = q >= end || src_[q] == '\n' || src_[q] == '\r';
                if (std::string_view(src_).substr(p, indent.size()) == indent) {
                    p += static_cast<std::uint32_t>(indent.size());
                } else if (blank) {
                    p = q;
                } else {
                    diags_.error(span(p, q), "this line is indented less than the raw string's closing `\"" +
                                                 std::string(hashes, '#') + "`, whose indentation every line loses");
                    p = q;
                }
                continue;
            }
            char ch = src_[p];
            if (ch == '\r' && p + 1 < end && src_[p + 1] == '\n') {
                p++;
                continue;
            }
            if (ch == '\n') {
                text += '\n';
                p++;
                line_start = true;
                continue;
            }
            if (interpolates_at(p)) {
                flush_text();
                std::uint32_t expr_start = p + hashes + 1;
                pos_ = expr_start;
                skip_interpolation();
                StringPart part;
                part.is_expr = true;
                part.span = span(expr_start, pos_ - 1);
                if (part.span.begin == part.span.end) {
                    diags_.error(span(p, pos_), "empty `" + std::string(hashes, '#') + "{}` in a raw string");
                }
                parts.push_back(part);
                p = pos_;
                continue;
            }
            text += ch;
            p++;
        }
        flush_text();
        pos_ = close + 1 + hashes;
        Token& t = push(Tok::string_lit, start, pos_);
        t.parts = arena_.copy(parts);
    }

    // Skips an interpolated expression up to and past its closing brace, through nested braces
    // and strings. Returns false if the line ends first.
    bool skip_interpolation() {
        int depth = 0;
        while (pos_ < end_) {
            char c = src_[pos_];
            if (c == '\n') return false;
            if (c == '#') {
                // A raw string, on this line: to its closing quote and `#`s, through its own
                // interpolations.
                std::uint32_t p = pos_, hashes = 0;
                while (p < end_ && src_[p] == '#') {
                    p++;
                    hashes++;
                }
                if (p < end_ && src_[p] == '"') {
                    pos_ = p + 1;
                    for (;;) {
                        if (pos_ >= end_ || src_[pos_] == '\n') return false;
                        std::uint32_t h = 0;
                        while (h < hashes && pos_ + 1 + h < end_ && src_[pos_ + 1 + h] == '#') h++;
                        bool after = pos_ + 1 + hashes < end_ && src_[pos_ + 1 + hashes] == '{';
                        if (src_[pos_] == '"' && h == hashes && !after) {
                            pos_ += 1 + hashes;
                            break;
                        }
                        std::uint32_t k = 0;
                        while (k < hashes && pos_ + k < end_ && src_[pos_ + k] == '#') k++;
                        if (k == hashes && pos_ + k < end_ && src_[pos_ + k] == '{') {
                            pos_ += k + 1;
                            if (!skip_interpolation()) return false;
                            continue;
                        }
                        pos_++;
                    }
                    continue;
                }
                pos_++;
                continue;
            }
            if (c == '"') {
                pos_++;
                while (pos_ < end_ && src_[pos_] != '"' && src_[pos_] != '\n') {
                    if (src_[pos_] == '\\') pos_++;
                    else if (src_[pos_] == '{') {
                        pos_++;
                        if (!skip_interpolation()) return false;
                        continue;
                    }
                    pos_++;
                }
                if (peek() != '"') return false;
                pos_++;
                continue;
            }
            if (c == '\'') {
                pos_++;
                if (peek() == '\\') pos_ += 2;
                while (pos_ < end_ && src_[pos_] != '\'' && src_[pos_] != '\n') pos_++;
                if (peek() == '\'') pos_++;
                continue;
            }
            if (c == '{') depth++;
            if (c == '}') {
                if (depth == 0) {
                    pos_++;
                    return true;
                }
                depth--;
            }
            pos_++;
        }
        return false;
    }

    std::string_view src_;
    std::uint32_t file_;
    std::uint32_t pos_;
    std::uint32_t end_;
    Names& names_;
    Arena& arena_;
    Diagnostics& diags_;
    std::vector<Token> out_;
    std::vector<char> brackets_;
    std::vector<std::uint32_t> open_at_;
};

}  // namespace

std::vector<Token> lex(const SourceFile& file, std::uint32_t file_id, std::uint32_t begin, std::uint32_t end,
                       Names& names, Arena& arena, Diagnostics& diags) {
    return Lexer(file, file_id, begin, end, names, arena, diags).run();
}

}  // namespace calx
