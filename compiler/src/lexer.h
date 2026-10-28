// The lexer: source text to tokens.
#pragma once

#include <string_view>
#include <vector>

#include "common.h"
#include "source.h"

namespace calx {

#define CALX_KEYWORDS(X)                                                                            \
    X(kw_and, "and") X(kw_break, "break") X(kw_checkout, "checkout")                                \
    X(kw_const, "const") X(kw_continue, "continue") X(kw_dyn, "dyn") X(kw_else, "else")             \
    X(kw_enum, "enum") X(kw_extern, "extern") X(kw_fail, "fail") X(kw_false, "false")               \
    X(kw_fn, "fn") X(kw_for, "for") X(kw_if, "if") X(kw_impl, "impl") X(kw_import, "import")        \
    X(kw_in, "in") X(kw_interface, "interface") X(kw_let, "let") X(kw_loop, "loop")                 \
    X(kw_match, "match") X(kw_module, "module") X(kw_mut, "mut") X(kw_none, "none")                 \
    X(kw_not, "not") X(kw_or, "or") X(kw_own, "own") X(kw_parallel, "parallel") X(kw_pub, "pub")    \
    X(kw_return, "return") X(kw_self, "self") X(kw_struct, "struct")                                \
    X(kw_true, "true") X(kw_try, "try") X(kw_type, "type") X(kw_unsafe, "unsafe") X(kw_var, "var")  \
    X(kw_while, "while")

#define CALX_PUNCT(X)                                                                               \
    X(lparen, "(") X(rparen, ")") X(lbracket, "[") X(rbracket, "]") X(lbrace, "{") X(rbrace, "}")   \
    X(comma, ",") X(colon, ":") X(semicolon, ";") X(dot, ".") X(dotdot, "..") X(dotdoteq, "..=")    \
    X(arrow, "->") X(fat_arrow, "=>") X(at, "@") X(question, "?") X(question_question, "??")        \
    X(question_dot, "?.") X(bang, "!") X(pipe, "|") X(plus, "+") X(minus, "-") X(star, "*")         \
    X(slash, "/") X(percent, "%") X(shl, "<<") X(shr, ">>") X(ushr, ">>>") X(amp, "&")              \
    X(caret, "^") X(tilde, "~") X(eq, "=") X(eqeq, "==") X(ne, "!=") X(lt, "<") X(le, "<=")         \
    X(gt, ">") X(ge, ">=") X(plus_eq, "+=") X(minus_eq, "-=") X(star_eq, "*=") X(slash_eq, "/=")    \
    X(percent_eq, "%=") X(shl_eq, "<<=") X(shr_eq, ">>=") X(ushr_eq, ">>>=") X(amp_eq, "&=")        \
    X(pipe_eq, "|=") X(caret_eq, "^=")

enum class Tok : std::uint8_t {
    eof,
    newline,
    doc_comment,  // `/// text`, kept so the parser can attach it to the next item
    ident,
    int_lit,
    float_lit,
    char_lit,
    string_lit,  // also `prefix"..."`, with the prefix in `prefix`
#define X(name, text) name,
    CALX_KEYWORDS(X) CALX_PUNCT(X)
#undef X
};

const char* tok_text(Tok t);  // "identifier", "`fn`", "`+`" and so on, for messages

// One piece of a string literal: either decoded text, or the span of an interpolated expression.
struct StringPart {
    bool is_expr = false;
    std::string_view text;  // decoded, for text parts
    Span span;              // the expression's source, for expression parts
};

struct Token {
    Tok kind = Tok::eof;
    Span span;
    // For identifiers, the name. For a prefix string, the prefix.
    Name name;
    // For int, float and char literals: the digits without underscores or suffix, and the suffix.
    std::string_view digits;
    std::string_view suffix;
    std::uint64_t int_value = 0;  // ints (radix applied) and chars (the code point)
    bool overflow = false;        // the digits don't fit in 64 bits
    // For string literals, the parts in order.
    Slice<StringPart> parts;
    // True if whitespace or a comment came between the previous token and this one.
    bool space_before = false;
};

// Lexes a whole file, or a span of it (an interpolated expression). Errors are reported through
// `diags`, and the lexer keeps going, so one run reports every lexical error.
std::vector<Token> lex(const SourceFile& file, std::uint32_t file_id, std::uint32_t begin, std::uint32_t end,
                       Names& names, Arena& arena, Diagnostics& diags);

}  // namespace calx
