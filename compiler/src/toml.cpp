#include "toml.h"

#include <cstdio>

namespace calx::toml {

const Value* Value::get(std::string_view name) const {
    for (const Value& v : entries) {
        if (v.key == name) return &v;
    }
    return nullptr;
}

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::string: return "a string";
        case Kind::integer: return "a number";
        case Kind::boolean: return "true or false";
        case Kind::array: return "an array";
        case Kind::table: return "a table";
    }
    return "a value";
}

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20 || u == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", u);
            out += buf;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

namespace {

std::string q(std::string_view s) { return "`" + std::string(s) + "`"; }

bool bare_key_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// A key's parts as written, each with where it is.
struct Key {
    std::vector<std::string> parts;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> at;
    std::uint32_t begin = 0, end = 0;
};

class Parser {
public:
    Parser(std::string_view text, Error& error) : s_(text), error_(error) {}

    bool run(Value& root) {
        root = Value{};
        root.kind = Kind::table;
        // A byte order mark, as some editors write one.
        if (s_.substr(0, 3) == "\xEF\xBB\xBF") i_ = 3;
        Value* table = &root;
        for (;;) {
            skip_blank_lines();
            if (done()) return true;
            char c = s_[i_];
            if (c == '[') {
                bool array = i_ + 1 < s_.size() && s_[i_ + 1] == '[';
                std::uint32_t begin = pos();
                i_ += array ? 2 : 1;
                spaces();
                Key key;
                if (!read_key(key)) return false;
                spaces();
                if (array ? s_.substr(i_, 2) != "]]" : (done() || s_[i_] != ']')) {
                    return fail(pos(), array ? "expected `]]` to end the table's name" : "expected `]` to end the table's name");
                }
                i_ += array ? 2 : 1;
                std::uint32_t end = pos();
                if (!end_of_line()) return false;
                table = array ? open_array_table(root, key, begin, end) : open_table(root, key, begin, end);
                if (!table) return false;
            } else {
                if (!key_value(*table, false)) return false;
                if (!end_of_line()) return false;
            }
        }
    }

private:
    bool done() const { return i_ >= s_.size(); }
    std::uint32_t pos() const { return static_cast<std::uint32_t>(i_); }

    bool fail(std::uint32_t at, std::string message, std::uint32_t end = 0) {
        error_.at = at;
        error_.end = end > at ? end : at + 1;
        error_.message = std::move(message);
        return false;
    }

    void spaces() {
        while (!done() && (s_[i_] == ' ' || s_[i_] == '\t')) i_++;
    }

    // Past a comment, if one starts here, to the end of its line. False if it holds a control
    // character, which TOML doesn't allow.
    bool comment() {
        if (done() || s_[i_] != '#') return true;
        while (!done() && s_[i_] != '\n') {
            unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '\r' && i_ + 1 < s_.size() && s_[i_ + 1] == '\n') break;
            if ((c < 0x20 && c != '\t') || c == 0x7f) return fail(pos(), "a comment can't hold a control character");
            i_++;
        }
        return true;
    }

    // Whether a line ends here, `\n` or `\r\n`, and moves past it.
    bool newline() {
        if (!done() && s_[i_] == '\n') {
            i_++;
            return true;
        }
        if (s_.substr(i_, 2) == "\r\n") {
            i_ += 2;
            return true;
        }
        return false;
    }

    void skip_blank_lines() {
        for (;;) {
            spaces();
            if (!comment()) return;
            if (!newline()) return;
        }
    }

    // After a header or a value: spaces, a comment, then the line's end.
    bool end_of_line() {
        spaces();
        if (!comment()) return false;
        if (done() || newline()) return true;
        return fail(pos(), "expected the end of the line here; each key and value is on a line of its own");
    }

    bool read_key(Key& key) {
        key.begin = pos();
        for (;;) {
            std::uint32_t begin = pos();
            std::string part;
            if (done()) return fail(pos(), "expected a key");
            char c = s_[i_];
            if (c == '"' || c == '\'') {
                Value v;
                if (!read_string(v)) return false;
                part = v.string;
            } else if (bare_key_char(c)) {
                while (!done() && bare_key_char(s_[i_])) part += s_[i_++];
            } else if (c == '=' || c == ']' || c == '.') {
                return fail(pos(), "expected a key before this");
            } else {
                return fail(pos(), "a bare key is letters, digits, `_` and `-`; put any other key in quotes");
            }
            key.parts.push_back(std::move(part));
            key.at.emplace_back(begin, pos());
            key.end = pos();
            spaces();
            if (done() || s_[i_] != '.') return true;
            i_++;
            spaces();
        }
    }

    std::string dotted(const Key& key, std::size_t upto) const {
        std::string out;
        for (std::size_t k = 0; k <= upto && k < key.parts.size(); k++) out += (k ? "." : "") + key.parts[k];
        return out;
    }

    Value* child(Value& table, const std::string& name) {
        for (Value& v : table.entries) {
            if (v.key == name) return &v;
        }
        return nullptr;
    }

    Value* add(Value& table, const std::string& name, Value v) {
        v.key = name;
        table.entries.push_back(std::move(v));
        return &table.entries.back();
    }

    // The table that holds the last part of `key`, under `root`, for a header: each part before
    // it is a table, made if it isn't there, or an array of tables, whose last it means.
    Value* walk(Value& root, const Key& key) {
        Value* t = &root;
        for (std::size_t k = 0; k + 1 < key.parts.size(); k++) {
            Value* next = child(*t, key.parts[k]);
            if (!next) {
                Value v;
                v.kind = Kind::table;
                v.implicit = true;
                v.key_begin = key.at[k].first;
                v.key_end = key.at[k].second;
                next = add(*t, key.parts[k], std::move(v));
            } else if (next->kind == Kind::array && !next->fixed) {
                next = &next->items.back();
            } else if (next->kind != Kind::table) {
                fail(key.at[k].first, q(dotted(key, k)) + " is already a value, so it can't hold a table", key.at[k].second);
                return nullptr;
            } else if (next->fixed) {
                fail(key.at[k].first, q(dotted(key, k)) + " is an inline table, which can't be added to", key.at[k].second);
                return nullptr;
            }
            t = next;
        }
        return t;
    }

    Value* open_table(Value& root, const Key& key, std::uint32_t begin, std::uint32_t end) {
        Value* parent = walk(root, key);
        if (!parent) return nullptr;
        const std::string& last = key.parts.back();
        Value* t = child(*parent, last);
        std::string name = dotted(key, key.parts.size());
        if (!t) {
            Value v;
            v.kind = Kind::table;
            v.header = true;
            v.begin = begin;
            v.end = end;
            v.key_begin = key.at.back().first;
            v.key_end = key.at.back().second;
            return add(*parent, last, std::move(v));
        }
        if (t->kind != Kind::table) {
            fail(begin, q(name) + " is already a value, so it can't be a table", end);
            return nullptr;
        }
        if (t->header || t->dotted || t->fixed || !t->implicit) {
            fail(begin, "the table " + q("[" + name + "]") + " is written twice; give each of its keys in one place", end);
            return nullptr;
        }
        t->implicit = false;
        t->header = true;
        t->begin = begin;
        t->end = end;
        return t;
    }

    Value* open_array_table(Value& root, const Key& key, std::uint32_t begin, std::uint32_t end) {
        Value* parent = walk(root, key);
        if (!parent) return nullptr;
        const std::string& last = key.parts.back();
        Value* a = child(*parent, last);
        std::string name = dotted(key, key.parts.size());
        if (!a) {
            Value v;
            v.kind = Kind::array;
            v.begin = begin;
            v.end = end;
            v.key_begin = key.at.back().first;
            v.key_end = key.at.back().second;
            a = add(*parent, last, std::move(v));
        } else if (a->kind != Kind::array || a->fixed) {
            fail(begin, q(name) + " is already a value that isn't an array of tables", end);
            return nullptr;
        }
        Value t;
        t.kind = Kind::table;
        t.header = true;
        t.begin = begin;
        t.end = end;
        t.key_begin = key.at.back().first;
        t.key_end = key.at.back().second;
        a->items.push_back(std::move(t));
        return &a->items.back();
    }

    // `key = value`, in `table`. In an inline table, what it makes is fixed too.
    bool key_value(Value& table, bool inline_table) {
        Key key;
        if (!read_key(key)) return false;
        spaces();
        if (done() || s_[i_] != '=') return fail(pos(), "expected `=` after the key " + q(dotted(key, key.parts.size())));
        i_++;
        spaces();
        Value v;
        if (!read_value(v)) return false;
        v.key_begin = key.begin;
        v.key_end = key.end;
        Value* t = &table;
        for (std::size_t k = 0; k + 1 < key.parts.size(); k++) {
            Value* next = child(*t, key.parts[k]);
            if (!next) {
                Value d;
                d.kind = Kind::table;
                d.dotted = true;
                d.fixed = inline_table;
                d.key_begin = key.at[k].first;
                d.key_end = key.at[k].second;
                d.begin = key.at[k].first;
                d.end = key.at[k].second;
                next = add(*t, key.parts[k], std::move(d));
            } else if (next->kind != Kind::table || !next->dotted || (next->fixed && !inline_table)) {
                return fail(key.at[k].first, q(dotted(key, k)) + " is already defined, so this can't add to it", key.at[k].second);
            }
            t = next;
        }
        if (child(*t, key.parts.back())) {
            return fail(key.begin, "the key " + q(dotted(key, key.parts.size())) + " is given twice", key.end);
        }
        add(*t, key.parts.back(), std::move(v));
        return true;
    }

    bool read_value(Value& v) {
        v.begin = pos();
        if (done() || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '#') return fail(pos(), "expected a value after `=`");
        char c = s_[i_];
        bool ok;
        if (c == '"' || c == '\'') {
            ok = read_string(v);
        } else if (c == '[') {
            ok = read_array(v);
        } else if (c == '{') {
            ok = read_inline_table(v);
        } else if (s_.substr(i_, 4) == "true" && delimited(i_ + 4)) {
            v.kind = Kind::boolean;
            v.boolean = true;
            i_ += 4;
            ok = true;
        } else if (s_.substr(i_, 5) == "false" && delimited(i_ + 5)) {
            v.kind = Kind::boolean;
            i_ += 5;
            ok = true;
        } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' || s_.substr(i_, 3) == "inf" || s_.substr(i_, 3) == "nan") {
            ok = read_number(v);
        } else {
            ok = fail(pos(), "expected a value: a string in quotes, a number, true or false, an array or an inline table");
        }
        v.end = pos();
        return ok;
    }

    bool delimited(std::size_t at) const {
        if (at >= s_.size()) return true;
        char c = s_[at];
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ']' || c == '}' || c == '#';
    }

    bool read_number(Value& v) {
        std::size_t start = i_;
        while (!done() && !delimited(i_)) i_++;
        std::string_view t = s_.substr(start, i_ - start);
        std::uint32_t b = static_cast<std::uint32_t>(start), e = pos();
        std::size_t k = 0;
        bool negative = false;
        if (k < t.size() && (t[k] == '+' || t[k] == '-')) negative = t[k++] == '-';
        if (t.substr(k, 2) == "0x" || t.substr(k, 2) == "0o" || t.substr(k, 2) == "0b") {
            return fail(b, "numbers are read in decimal here", e);
        }
        bool digits = k < t.size();
        bool prev_digit = false;
        for (std::size_t j = k; j < t.size(); j++) {
            char c = t[j];
            if (c >= '0' && c <= '9') {
                prev_digit = true;
            } else if (c == '_' && prev_digit && j + 1 < t.size() && t[j + 1] >= '0' && t[j + 1] <= '9') {
                prev_digit = false;
            } else {
                digits = false;
                break;
            }
        }
        if (!digits) {
            if (t.find_first_of(".eE:") != std::string_view::npos || t.find("inf") != std::string_view::npos ||
                t.find("nan") != std::string_view::npos || t.find('-', k) != std::string_view::npos) {
                return fail(b, q(t) + " isn't read here: calx.toml has whole numbers, and a version is a string, as \"1.2.0\"", e);
            }
            return fail(b, q(t) + " isn't a number", e);
        }
        if (t.size() - k > 1 && t[k] == '0') return fail(b, "a number can't start with 0", e);
        std::uint64_t n = 0;
        for (std::size_t j = k; j < t.size(); j++) {
            if (t[j] == '_') continue;
            std::uint64_t d = static_cast<std::uint64_t>(t[j] - '0');
            if (n > (static_cast<std::uint64_t>(INT64_MAX) + (negative ? 1 : 0) - d) / 10) return fail(b, q(t) + " is too big", e);
            n = n * 10 + d;
        }
        v.kind = Kind::integer;
        v.integer = negative ? static_cast<std::int64_t>(0 - n) : static_cast<std::int64_t>(n);
        return true;
    }

    bool read_array(Value& v) {
        v.kind = Kind::array;
        v.fixed = true;
        i_++;
        for (;;) {
            if (!blank_in_array()) return false;
            if (done()) return fail(v.begin, "this array isn't closed with `]`");
            if (s_[i_] == ']') {
                i_++;
                return true;
            }
            Value item;
            if (!read_value(item)) return false;
            fix(item);
            v.items.push_back(std::move(item));
            if (!blank_in_array()) return false;
            if (done()) return fail(v.begin, "this array isn't closed with `]`");
            if (s_[i_] == ',') {
                i_++;
            } else if (s_[i_] != ']') {
                return fail(pos(), "expected `,` or `]` after the array's value");
            }
        }
    }

    // Spaces, line ends and comments, which an array can have between its values.
    bool blank_in_array() {
        for (;;) {
            spaces();
            if (!comment()) return false;
            if (!newline()) return true;
        }
    }

    static void fix(Value& v) {
        v.fixed = true;
        for (Value& c : v.entries) fix(c);
        for (Value& c : v.items) fix(c);
    }

    bool read_inline_table(Value& v) {
        v.kind = Kind::table;
        v.fixed = true;
        i_++;
        spaces();
        if (!done() && s_[i_] == '}') {
            i_++;
            return true;
        }
        for (;;) {
            spaces();
            if (done() || s_[i_] == '\n' || s_[i_] == '\r') {
                return fail(v.begin, "an inline table is on one line, and this one isn't closed with `}` on it");
            }
            if (!key_value(v, true)) return false;
            spaces();
            if (done() || s_[i_] == '\n' || s_[i_] == '\r') {
                return fail(v.begin, "an inline table is on one line, and this one isn't closed with `}` on it");
            }
            if (s_[i_] == '}') {
                i_++;
                break;
            }
            if (s_[i_] != ',') return fail(pos(), "expected `,` or `}` after the inline table's value");
            i_++;
            spaces();
            if (!done() && s_[i_] == '}') return fail(pos(), "an inline table can't end with `,`");
        }
        for (Value& c : v.entries) fix(c);
        return true;
    }

    bool read_string(Value& v) {
        v.kind = Kind::string;
        std::uint32_t begin = pos();
        char quote = s_[i_];
        if (s_.substr(i_, 3) == std::string(3, quote)) {
            return fail(begin, "multi-line strings aren't read here; write the string on one line", begin + 3);
        }
        i_++;
        std::string out;
        for (;;) {
            if (done() || s_[i_] == '\n' || s_[i_] == '\r') return fail(begin, "this string isn't closed on its line");
            unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == static_cast<unsigned char>(quote)) {
                i_++;
                break;
            }
            if ((c < 0x20 && c != '\t') || c == 0x7f) return fail(pos(), "a string can't hold a control character; write it with an escape");
            if (c == '\\' && quote == '"') {
                if (!escape(out)) return false;
                continue;
            }
            out += static_cast<char>(c);
            i_++;
        }
        v.string = std::move(out);
        return true;
    }

    bool escape(std::string& out) {
        std::uint32_t at = pos();
        i_++;
        if (done()) return fail(at, "this string isn't closed on its line");
        char e = s_[i_++];
        switch (e) {
            case 'b': out += '\b'; return true;
            case 't': out += '\t'; return true;
            case 'n': out += '\n'; return true;
            case 'f': out += '\f'; return true;
            case 'r': out += '\r'; return true;
            case '"': out += '"'; return true;
            case '\\': out += '\\'; return true;
            case 'u':
            case 'U': {
                std::size_t n = e == 'u' ? 4 : 8;
                std::uint32_t cp = 0;
                for (std::size_t k = 0; k < n; k++) {
                    char h = i_ < s_.size() ? s_[i_] : '\0';
                    int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (d < 0) return fail(at, std::string("`\\") + e + "` takes " + std::to_string(n) + " hex digits", pos() + 1);
                    cp = cp * 16 + static_cast<std::uint32_t>(d);
                    i_++;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail(at, "that isn't a Unicode character", pos());
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
                return true;
            }
            default:
                return fail(at, "`\\" + std::string(1, e) + "` isn't an escape; the escapes are \\b \\t \\n \\f \\r \\\" \\\\ \\uXXXX and \\UXXXXXXXX", pos());
        }
    }

    std::string_view s_;
    std::size_t i_ = 0;
    Error& error_;
};

}  // namespace

bool parse(std::string_view text, Value& root, Error& error) { return Parser(text, error).run(root); }

}  // namespace calx::toml
