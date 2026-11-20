// A strict reader for the TOML a package's calx.toml and calx.lock are written in (CLX-370).
//
// The compiler is C++, so it can't use std.toml. This reads the part of TOML 1.0 those files
// need: comments, `[table]` and `[[array]]` headers, bare, quoted and dotted keys, basic and
// literal strings on one line, decimal integers, booleans, arrays and inline tables. Anything else,
// a multi-line string, a float or a date, is an error that says it isn't read here, at its place.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace calx::toml {

enum class Kind : std::uint8_t { string, integer, boolean, array, table };

struct Value {
    Kind kind = Kind::table;
    std::string string;
    std::int64_t integer = 0;
    bool boolean = false;
    std::vector<Value> items;                             // an array's
    std::vector<Value> entries;                           // a table's, in the order they're written
    std::string key;                                      // its key, in the table that holds it
    std::uint32_t begin = 0, end = 0;                     // the value's bytes, or a table's header's
    std::uint32_t key_begin = 0, key_end = 0;             // the key that names it, where it has one

    // How the table came to be, for TOML's rules on defining one twice.
    bool header = false;    // `[a]` named it
    bool implicit = false;  // only as part of a longer name, `[a.b]` or `a.b = 1`
    bool fixed = false;     // an inline table, or an array of them, which nothing can add to
    bool dotted = false;    // made by a dotted key, `a.b = 1`, so a header can't open it again

    const Value* get(std::string_view name) const;
};

// A place in the text and what's wrong there.
struct Error {
    std::uint32_t at = 0;
    std::uint32_t end = 0;
    std::string message;
};

// Reads `text` into `root`, a table. Returns false with the first error in `error`.
bool parse(std::string_view text, Value& root, Error& error);

// The name of a kind, as an error says what a value should have been: "a string".
const char* kind_name(Kind k);

// `s` written as a TOML basic string, quotes included.
std::string quote(std::string_view s);

}  // namespace calx::toml
