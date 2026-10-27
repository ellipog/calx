#include "parser.h"

#include <string>

namespace calx {

const char* op_text(Op op) {
    switch (op) {
        case Op::neg: return "-";
        case Op::bit_not: return "~";
        case Op::log_not: return "not";
        case Op::mul: return "*";
        case Op::div: return "/";
        case Op::rem: return "%";
        case Op::add: return "+";
        case Op::sub: return "-";
        case Op::shl: return "<<";
        case Op::shr: return ">>";
        case Op::ushr: return ">>>";
        case Op::bit_and: return "&";
        case Op::bit_xor: return "^";
        case Op::bit_or: return "|";
        case Op::coalesce: return "??";
        case Op::eq: return "==";
        case Op::ne: return "!=";
        case Op::lt: return "<";
        case Op::le: return "<=";
        case Op::gt: return ">";
        case Op::ge: return ">=";
        case Op::log_and: return "and";
        case Op::log_or: return "or";
    }
    return "?";
}

namespace {

// Binding power of the infix operators, from loosest to tightest. Ranges sit below all of them
// and are parsed separately. `not` is a prefix operator between `and` and the comparisons.
enum Prec : int {
    kOr = 2,
    kAnd = 3,
    kNot = 4,
    kCompare = 5,
    kCoalesce = 6,
    kBitOr = 7,
    kBitXor = 8,
    kBitAnd = 9,
    kShift = 10,
    kAdd = 11,
    kMul = 12,
};

struct BinOp {
    Op op;
    int prec;
    bool right_assoc;
};

bool binary_op(Tok t, BinOp& out) {
    switch (t) {
        case Tok::kw_or: out = {Op::log_or, kOr, false}; return true;
        case Tok::kw_and: out = {Op::log_and, kAnd, false}; return true;
        case Tok::eqeq: out = {Op::eq, kCompare, false}; return true;
        case Tok::ne: out = {Op::ne, kCompare, false}; return true;
        case Tok::lt: out = {Op::lt, kCompare, false}; return true;
        case Tok::le: out = {Op::le, kCompare, false}; return true;
        case Tok::gt: out = {Op::gt, kCompare, false}; return true;
        case Tok::ge: out = {Op::ge, kCompare, false}; return true;
        case Tok::question_question: out = {Op::coalesce, kCoalesce, true}; return true;
        case Tok::pipe: out = {Op::bit_or, kBitOr, false}; return true;
        case Tok::caret: out = {Op::bit_xor, kBitXor, false}; return true;
        case Tok::amp: out = {Op::bit_and, kBitAnd, false}; return true;
        case Tok::shl: out = {Op::shl, kShift, false}; return true;
        case Tok::shr: out = {Op::shr, kShift, false}; return true;
        case Tok::ushr: out = {Op::ushr, kShift, false}; return true;
        case Tok::plus: out = {Op::add, kAdd, false}; return true;
        case Tok::minus: out = {Op::sub, kAdd, false}; return true;
        case Tok::star: out = {Op::mul, kMul, false}; return true;
        case Tok::slash: out = {Op::div, kMul, false}; return true;
        case Tok::percent: out = {Op::rem, kMul, false}; return true;
        default: return false;
    }
}

bool compound_op(Tok t, Op& out) {
    switch (t) {
        case Tok::plus_eq: out = Op::add; return true;
        case Tok::minus_eq: out = Op::sub; return true;
        case Tok::star_eq: out = Op::mul; return true;
        case Tok::slash_eq: out = Op::div; return true;
        case Tok::percent_eq: out = Op::rem; return true;
        case Tok::shl_eq: out = Op::shl; return true;
        case Tok::shr_eq: out = Op::shr; return true;
        case Tok::ushr_eq: out = Op::ushr; return true;
        case Tok::amp_eq: out = Op::bit_and; return true;
        case Tok::pipe_eq: out = Op::bit_or; return true;
        case Tok::caret_eq: out = Op::bit_xor; return true;
        default: return false;
    }
}

// Thrown to unwind out of a construct after a syntax error; caught at statement and item level.
struct SyntaxError {};

class Parser {
public:
    Parser(ParseContext ctx, std::uint32_t file_id, std::vector<Token> tokens)
        : ctx_(ctx), file_(file_id), toks_(std::move(tokens)) {}

    Module* module() {
        auto* m = ctx_.arena.make<Module>();
        m->file = file_;
        skip_newlines();
        std::string_view doc = docs();
        if (at(Tok::kw_module)) {
            m->doc = doc;
            doc = {};
            next();
            m->name = path();
            end_line();
        }
        std::vector<Item*> items;
        while (!at(Tok::eof)) {
            try {
                if (Item* it = item(doc)) items.push_back(it);
            } catch (SyntaxError&) {
                recover_item();
            }
            doc = {};
            skip_newlines();
            if (at(Tok::doc_comment)) doc = docs();
        }
        m->items = ctx_.arena.copy(items);
        return m;
    }

    // An interpolated `{expr}` in a string: the whole token list must be one expression.
    Expr* lone_expr() {
        Expr* e = nullptr;
        try {
            skip_newlines();
            e = expr();
            skip_newlines();
            if (!at(Tok::eof)) error(peek().span, std::string("expected `}` to end the interpolation, found ") + tok_text(peek().kind));
        } catch (SyntaxError&) {
        }
        return e;
    }

private:
    // --- tokens --------------------------------------------------------------------------

    const Token& peek(std::size_t ahead = 0) const {
        std::size_t i = pos_ + ahead;
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    bool at(Tok t) const { return peek().kind == t; }
    const Token& next() {
        const Token& t = toks_[pos_];
        if (pos_ + 1 < toks_.size()) pos_++;
        return t;
    }
    bool accept(Tok t) {
        if (!at(t)) return false;
        next();
        return true;
    }
    Span prev_span() const { return toks_[pos_ > 0 ? pos_ - 1 : 0].span; }

    [[noreturn]] void fail(Span span, std::string message) {
        error(span, std::move(message));
        throw SyntaxError{};
    }
    void error(Span span, std::string message) {
        // One error per position: recovery can otherwise report the same spot twice.
        if (span.begin == last_error_ && span.file == file_) return;
        last_error_ = span.begin;
        ctx_.diags.error(span, std::move(message));
    }

    const Token& expect(Tok t, const char* context) {
        if (!at(t)) {
            fail(peek().span, std::string("expected ") + tok_text(t) + " " + context + ", found " + tok_text(peek().kind));
        }
        return next();
    }

    Name ident(const char* context) { return expect(Tok::ident, context).name; }

    void skip_newlines() {
        while (at(Tok::newline) || at(Tok::semicolon)) next();
    }
    // Newlines, but also stray doc comments, where nothing can be documented.
    void skip_lines() {
        while (at(Tok::newline) || at(Tok::semicolon) || at(Tok::doc_comment)) next();
    }

    // A statement or item ends at a newline, a `;`, or the brace that closes its block.
    void end_line() {
        if (at(Tok::newline) || at(Tok::semicolon)) {
            skip_newlines();
            return;
        }
        if (at(Tok::rbrace) || at(Tok::eof)) return;
        fail(peek().span, std::string("expected the end of the line, found ") + tok_text(peek().kind));
    }

    // After an error in an item: skip to the next line that starts at the item's nesting level.
    void recover_item() {
        int depth = 0;
        while (!at(Tok::eof)) {
            Tok k = peek().kind;
            if (k == Tok::lbrace) depth++;
            if (k == Tok::rbrace) {
                if (depth == 0) {
                    next();
                    return;
                }
                depth--;
                if (depth == 0) {
                    next();
                    return;
                }
            }
            if (k == Tok::newline && depth == 0) {
                next();
                return;
            }
            next();
        }
    }

    // After an error in a statement: skip to the end of its line, or to its block's closing brace.
    void recover_stmt() {
        int depth = 0;
        while (!at(Tok::eof)) {
            Tok k = peek().kind;
            if (k == Tok::lbrace) depth++;
            if (k == Tok::rbrace) {
                if (depth == 0) return;
                depth--;
            }
            if (k == Tok::newline && depth == 0) {
                next();
                return;
            }
            next();
        }
    }

    // `>` that closes generic arguments: splits `>>`, `>>>`, `>=` and the like.
    void expect_close_angle() {
        Token& t = toks_[pos_];
        Tok rest;
        switch (t.kind) {
            case Tok::gt: next(); return;
            case Tok::shr: rest = Tok::gt; break;
            case Tok::ushr: rest = Tok::shr; break;
            case Tok::ge: rest = Tok::eq; break;
            case Tok::shr_eq: rest = Tok::ge; break;
            case Tok::ushr_eq: rest = Tok::shr_eq; break;
            default: fail(t.span, std::string("expected `>` to close the generic arguments, found ") + tok_text(t.kind));
        }
        t.kind = rest;
        t.span.begin += 1;
        t.space_before = false;
    }

    std::string_view docs() {
        if (!at(Tok::doc_comment)) return {};
        std::string text;
        while (at(Tok::doc_comment)) {
            if (!text.empty()) text += '\n';
            text += next().digits;
            skip_newlines();
        }
        return ctx_.arena.copy(text);
    }

    template <typename T>
    Slice<T> slice(const std::vector<T>& v) { return ctx_.arena.copy(v); }

    Path path() {
        std::vector<Name> parts;
        Span start = peek().span;
        parts.push_back(ident("in a name"));
        while (at(Tok::dot) && peek(1).kind == Tok::ident) {
            next();
            parts.push_back(next().name);
        }
        Path p;
        p.parts = slice(parts);
        p.span = Span::join(start, prev_span());
        return p;
    }

    // --- items ---------------------------------------------------------------------------

    std::vector<Attribute> attributes() {
        std::vector<Attribute> out;
        while (at(Tok::at)) {
            Span start = next().span;
            Attribute a;
            a.name = path();
            if (at(Tok::lparen)) a.args = call_args();
            a.span = Span::join(start, prev_span());
            out.push_back(a);
            skip_newlines();
        }
        return out;
    }

    Item* new_item(ItemKind kind, Span start) {
        auto* it = ctx_.arena.make<Item>();
        it->kind = kind;
        it->span = start;
        return it;
    }

    Item* item(std::string_view doc) {
        Span start = peek().span;
        std::vector<Attribute> attrs = attributes();
        if (doc.empty() && at(Tok::doc_comment)) doc = docs();
        bool is_pub = accept(Tok::kw_pub);
        Item* it = nullptr;
        switch (peek().kind) {
            case Tok::kw_fn: it = fn_item(start); break;
            case Tok::kw_struct: it = struct_item(start, false); break;
            case Tok::kw_actor:
                next();
                if (!at(Tok::kw_struct)) fail(peek().span, "expected `struct` after `actor`");
                it = struct_item(start, true);
                break;
            case Tok::kw_enum: it = enum_item(start); break;
            case Tok::kw_interface: it = interface_item(start); break;
            case Tok::kw_impl: it = impl_item(start); break;
            case Tok::kw_const: it = const_item(start); break;
            case Tok::kw_type: it = type_alias(start); break;
            case Tok::kw_import: it = import_item(start); break;
            case Tok::kw_extern: it = extern_item(start); break;
            case Tok::ident:
                if (peek(1).kind == Tok::lbrace) {
                    it = attribute_block(start);
                    break;
                }
                [[fallthrough]];
            default:
                fail(peek().span, std::string("expected an item such as `fn`, `struct` or `import`, found ") +
                                      tok_text(peek().kind));
        }
        it->is_pub = is_pub;
        it->doc = doc;
        it->attributes = slice(attrs);
        it->span = Span::join(start, prev_span());
        return it;
    }

    std::vector<GenericParam> generics() {
        std::vector<GenericParam> out;
        if (!accept(Tok::lt)) return out;
        while (!at(Tok::gt)) {
            GenericParam g;
            g.span = peek().span;
            g.name = ident("as a generic parameter");
            if (accept(Tok::colon)) {
                std::vector<TypeExpr*> bounds;
                bounds.push_back(type());
                while (accept(Tok::plus)) bounds.push_back(type());
                g.bounds = slice(bounds);
            }
            out.push_back(g);
            if (!accept(Tok::comma)) break;
        }
        expect_close_angle();
        return out;
    }

    Param param() {
        Param p;
        p.span = peek().span;
        if (at(Tok::kw_self)) {
            next();
            p.is_self = true;
            p.name = ctx_.names.intern("self");
            if (accept(Tok::colon)) param_type(p);
        } else {
            p.name = ident("as a parameter name");
            expect(Tok::colon, "after the parameter name");
            param_type(p);
        }
        if (accept(Tok::eq)) p.default_value = expr();
        p.span = Span::join(p.span, prev_span());
        return p;
    }

    void param_type(Param& p) {
        if (accept(Tok::kw_mut)) p.mode = ParamMode::mut;
        else if (accept(Tok::kw_own)) p.mode = ParamMode::own;
        p.type = type();
    }

    Item* fn_item(Span start) {
        expect(Tok::kw_fn, "");
        Item* it = new_item(ItemKind::fn, start);
        it->name_span = peek().span;
        it->name = ident("as the function's name");
        it->generics = slice(generics());
        fn_signature(it);
        if (at(Tok::lbrace)) it->body = block();
        else end_line();
        return it;
    }

    void fn_signature(Item* it) {
        expect(Tok::lparen, "to start the parameter list");
        std::vector<Param> params;
        while (!at(Tok::rparen)) {
            params.push_back(param());
            if (!accept(Tok::comma)) break;
        }
        expect(Tok::rparen, "to end the parameter list");
        it->params = slice(params);
        if (accept(Tok::arrow)) it->result = type();
        if (accept(Tok::bang)) it->error = type();
    }

    void field_list(std::vector<FieldDecl>& fields) {
        expect(Tok::lbrace, "to start the fields");
        skip_newlines();
        while (!at(Tok::rbrace) && !at(Tok::eof)) {
            try {
                FieldDecl f;
                f.doc = docs();
                std::vector<Attribute> attrs = attributes();
                f.attributes = slice(attrs);
                f.span = peek().span;
                f.is_pub = accept(Tok::kw_pub);
                f.name = ident("as a field name");
                expect(Tok::colon, "after the field name");
                f.type = type();
                if (accept(Tok::eq)) f.default_value = expr();
                f.span = Span::join(f.span, prev_span());
                fields.push_back(f);
                if (!accept(Tok::comma)) end_line();
            } catch (SyntaxError&) {
                recover_stmt();
            }
            skip_newlines();
        }
        expect(Tok::rbrace, "to end the fields");
    }

    Item* struct_item(Span start, bool actor) {
        expect(Tok::kw_struct, "");
        Item* it = new_item(ItemKind::struct_, start);
        it->is_actor = actor;
        it->name_span = peek().span;
        it->name = ident("as the struct's name");
        it->generics = slice(generics());
        std::vector<FieldDecl> fields;
        if (at(Tok::lparen)) {
            // `struct Name(String)`: a wrapper whose fields are `.0`, `.1` and so on.
            next();
            it->is_tuple_struct = true;
            std::uint32_t i = 0;
            while (!at(Tok::rparen)) {
                FieldDecl f;
                f.span = peek().span;
                f.name = ctx_.names.intern(std::to_string(i++));
                f.type = type();
                fields.push_back(f);
                if (!accept(Tok::comma)) break;
            }
            expect(Tok::rparen, "to end the wrapped types");
            end_line();
        } else if (at(Tok::lbrace)) {
            field_list(fields);
        } else {
            end_line();
        }
        it->fields = slice(fields);
        return it;
    }

    Item* enum_item(Span start) {
        expect(Tok::kw_enum, "");
        Item* it = new_item(ItemKind::enum_, start);
        it->name_span = peek().span;
        it->name = ident("as the enum's name");
        it->generics = slice(generics());
        if (accept(Tok::colon)) it->repr = type();
        expect(Tok::lbrace, "to start the variants");
        skip_newlines();
        std::vector<VariantDecl> variants;
        while (!at(Tok::rbrace) && !at(Tok::eof)) {
            try {
                VariantDecl v;
                v.doc = docs();
                v.span = peek().span;
                v.name = ident("as a variant name");
                if (accept(Tok::lparen)) {
                    std::vector<TypeExpr*> payload;
                    while (!at(Tok::rparen)) {
                        payload.push_back(type());
                        if (!accept(Tok::comma)) break;
                    }
                    expect(Tok::rparen, "to end the variant's data");
                    v.payload = slice(payload);
                } else if (at(Tok::lbrace)) {
                    std::vector<FieldDecl> fields;
                    field_list(fields);
                    v.fields = slice(fields);
                    v.has_fields = true;
                }
                if (accept(Tok::eq)) v.value = expr();
                v.span = Span::join(v.span, prev_span());
                variants.push_back(v);
                if (!accept(Tok::comma)) end_line();
            } catch (SyntaxError&) {
                recover_stmt();
            }
            skip_newlines();
        }
        expect(Tok::rbrace, "to end the variants");
        it->variants = slice(variants);
        return it;
    }

    // The items inside `interface`, `impl`, `extern` and attribute blocks.
    std::vector<Item*> member_items() {
        expect(Tok::lbrace, "to start the block");
        std::vector<Item*> members;
        skip_newlines();
        while (!at(Tok::rbrace) && !at(Tok::eof)) {
            try {
                std::string_view doc = docs();
                members.push_back(item(doc));
            } catch (SyntaxError&) {
                recover_item();
            }
            skip_newlines();
        }
        expect(Tok::rbrace, "to end the block");
        return members;
    }

    Item* interface_item(Span start) {
        expect(Tok::kw_interface, "");
        Item* it = new_item(ItemKind::interface, start);
        it->name_span = peek().span;
        it->name = ident("as the interface's name");
        it->generics = slice(generics());
        it->members = slice(member_items());
        return it;
    }

    Item* impl_item(Span start) {
        expect(Tok::kw_impl, "");
        Item* it = new_item(ItemKind::impl, start);
        it->generics = slice(generics());
        TypeExpr* first = type();
        if (accept(Tok::kw_for)) {
            it->interface = first;
            it->self_type = type();
        } else {
            it->self_type = first;
        }
        it->members = slice(member_items());
        return it;
    }

    Item* const_item(Span start) {
        expect(Tok::kw_const, "");
        Item* it = new_item(ItemKind::const_, start);
        it->name_span = peek().span;
        it->name = ident("as the constant's name");
        if (accept(Tok::colon)) it->type = type();
        if (accept(Tok::eq)) it->value = expr();
        end_line();
        return it;
    }

    Item* type_alias(Span start) {
        expect(Tok::kw_type, "");
        Item* it = new_item(ItemKind::type_alias, start);
        it->name_span = peek().span;
        it->name = ident("as the type's name");
        it->generics = slice(generics());
        if (accept(Tok::eq)) it->self_type = type();
        end_line();
        return it;
    }

    Item* import_item(Span start) {
        expect(Tok::kw_import, "");
        Item* it = new_item(ItemKind::import, start);
        std::vector<Name> parts;
        Span p0 = peek().span;
        parts.push_back(ident("after `import`"));
        std::vector<ImportName> names;
        while (accept(Tok::dot)) {
            if (at(Tok::lbrace)) {
                next();
                skip_newlines();
                while (!at(Tok::rbrace)) {
                    ImportName n;
                    n.span = peek().span;
                    n.name = ident("in the import list");
                    names.push_back(n);
                    skip_newlines();
                    if (!accept(Tok::comma)) break;
                    skip_newlines();
                }
                expect(Tok::rbrace, "to end the import list");
                break;
            }
            parts.push_back(ident("in the import path"));
        }
        it->import_path.parts = slice(parts);
        it->import_path.span = Span::join(p0, prev_span());
        it->import_names = slice(names);
        end_line();
        return it;
    }

    Item* extern_item(Span start) {
        expect(Tok::kw_extern, "");
        std::string_view abi = "C";
        if (at(Tok::string_lit)) {
            const Token& t = next();
            abi = t.parts.empty() ? std::string_view{} : t.parts[0].text;
        }
        if (at(Tok::lbrace)) {
            Item* it = new_item(ItemKind::extern_block, start);
            it->abi = abi;
            it->members = slice(member_items());
            for (Item* m : it->members) {
                m->is_extern = true;
                m->abi = abi;
            }
            return it;
        }
        Item* it = fn_item(start);
        it->is_extern = true;
        it->abi = abi;
        return it;
    }

    Item* attribute_block(Span start) {
        Item* it = new_item(ItemKind::attribute_block, start);
        it->name_span = peek().span;
        it->name = next().name;
        it->members = slice(member_items());
        return it;
    }

    // --- types ---------------------------------------------------------------------------

    TypeExpr* new_type(TypeExprKind kind, Span span) {
        auto* t = ctx_.arena.make<TypeExpr>();
        t->kind = kind;
        t->span = span;
        return t;
    }

    TypeExpr* type() {
        TypeExpr* t = type_base();
        while (at(Tok::question) && !peek().space_before) {
            Span s = next().span;
            TypeExpr* o = new_type(TypeExprKind::optional, Span::join(t->span, s));
            o->inner = t;
            t = o;
        }
        return t;
    }

    TypeExpr* type_base() {
        Span start = peek().span;
        switch (peek().kind) {
            case Tok::ident: {
                TypeExpr* t = new_type(TypeExprKind::named, start);
                if (ctx_.names.text(peek().name) == "Self") {
                    next();
                    t->kind = TypeExprKind::self;
                    return t;
                }
                t->path = path();
                if (at(Tok::lt) && !peek().space_before) {
                    next();
                    std::vector<TypeExpr*> args;
                    while (!at(Tok::gt) && !at(Tok::shr) && !at(Tok::ushr)) {
                        args.push_back(type());
                        if (!accept(Tok::comma)) break;
                    }
                    expect_close_angle();
                    t->args = slice(args);
                }
                t->span = Span::join(start, prev_span());
                return t;
            }
            case Tok::lbracket: {
                next();
                TypeExpr* t = new_type(TypeExprKind::array, start);
                t->inner = type();
                expect(Tok::semicolon, "between an array's element type and its length");
                t->length = expr();
                expect(Tok::rbracket, "to end the array type");
                t->span = Span::join(start, prev_span());
                return t;
            }
            case Tok::lparen: {
                next();
                TypeExpr* t = new_type(TypeExprKind::tuple, start);
                std::vector<TypeExpr*> elems;
                while (!at(Tok::rparen)) {
                    elems.push_back(type());
                    if (!accept(Tok::comma)) break;
                }
                expect(Tok::rparen, "to end the tuple type");
                if (elems.size() == 1 && toks_[pos_ - 2].kind != Tok::comma) return elems[0];  // `(T)` is just `T`
                t->args = slice(elems);
                t->span = Span::join(start, prev_span());
                return t;
            }
            case Tok::kw_fn: {
                next();
                TypeExpr* t = new_type(TypeExprKind::fn, start);
                expect(Tok::lparen, "to start the function type's parameters");
                std::vector<TypeExpr*> params;
                while (!at(Tok::rparen)) {
                    params.push_back(type());
                    if (!accept(Tok::comma)) break;
                }
                expect(Tok::rparen, "to end the function type's parameters");
                t->args = slice(params);
                if (accept(Tok::arrow)) t->inner = type();
                if (accept(Tok::bang)) t->error = type();
                t->span = Span::join(start, prev_span());
                return t;
            }
            case Tok::star: {
                next();
                TypeExpr* t = new_type(TypeExprKind::pointer, start);
                if (at(Tok::kw_const)) {
                    next();
                    t->is_const = true;
                }
                t->inner = type_base();
                t->span = Span::join(start, prev_span());
                return t;
            }
            case Tok::kw_dyn: {
                next();
                TypeExpr* t = new_type(TypeExprKind::dyn, start);
                t->inner = type_base();
                t->span = Span::join(start, prev_span());
                return t;
            }
            default:
                fail(start, std::string("expected a type, found ") + tok_text(peek().kind));
        }
    }

    // --- statements ----------------------------------------------------------------------

    Stmt* new_stmt(StmtKind kind, Span span) {
        auto* s = ctx_.arena.make<Stmt>();
        s->kind = kind;
        s->span = span;
        return s;
    }

    Block* block() {
        Span start = expect(Tok::lbrace, "to start a block").span;
        auto* b = ctx_.arena.make<Block>();
        std::vector<Stmt*> stmts;
        skip_lines();
        while (!at(Tok::rbrace) && !at(Tok::eof)) {
            try {
                stmts.push_back(stmt());
            } catch (SyntaxError&) {
                recover_stmt();
            }
            skip_lines();
        }
        Span end = expect(Tok::rbrace, "to end the block").span;
        // A block's value is its last statement, if that's an expression.
        if (!stmts.empty() && stmts.back()->kind == StmtKind::expr) {
            b->tail = stmts.back()->value;
            stmts.pop_back();
        }
        b->stmts = slice(stmts);
        b->span = Span::join(start, end);
        return b;
    }

    // The expression or block after `else` in `let x = opt else <diverge>`.
    void diverge(Stmt* s) {
        if (at(Tok::lbrace)) {
            s->else_block = block();
            return;
        }
        Span start = peek().span;
        Stmt* inner = nullptr;
        switch (peek().kind) {
            case Tok::kw_return: case Tok::kw_break: case Tok::kw_continue: case Tok::kw_fail:
                inner = simple_jump();
                break;
            default:
                s->else_ = expr();
                return;
        }
        auto* b = ctx_.arena.make<Block>();
        std::vector<Stmt*> one{inner};
        b->stmts = slice(one);
        b->span = Span::join(start, prev_span());
        s->else_block = b;
    }

    Stmt* simple_jump() {
        Span start = peek().span;
        Tok k = next().kind;
        StmtKind kind = k == Tok::kw_return ? StmtKind::return_
                        : k == Tok::kw_break ? StmtKind::break_
                        : k == Tok::kw_continue ? StmtKind::continue_
                                                : StmtKind::fail;
        Stmt* s = new_stmt(kind, start);
        bool has_value = !at(Tok::newline) && !at(Tok::semicolon) && !at(Tok::rbrace) && !at(Tok::eof);
        if (kind == StmtKind::fail && !has_value) fail(peek().span, "`fail` needs an error value");
        if (kind != StmtKind::continue_ && has_value) s->value = expr();
        s->span = Span::join(start, prev_span());
        return s;
    }

    Stmt* stmt() {
        Span start = peek().span;
        Stmt* s = nullptr;
        switch (peek().kind) {
            case Tok::kw_let:
            case Tok::kw_var: {
                bool is_var = next().kind == Tok::kw_var;
                s = new_stmt(StmtKind::let, start);
                s->is_var = is_var;
                s->pattern = pattern();
                if (accept(Tok::colon)) s->type = type();
                if (accept(Tok::eq)) {
                    skip_newlines();
                    s->value = expr();
                    if (accept(Tok::kw_else)) diverge(s);
                }
                break;
            }
            case Tok::kw_return: case Tok::kw_break: case Tok::kw_continue: case Tok::kw_fail:
                s = simple_jump();
                break;
            case Tok::kw_while: {
                next();
                s = new_stmt(StmtKind::while_, start);
                s->cond = expr_no_struct();
                s->body = block();
                break;
            }
            case Tok::kw_parallel:
            case Tok::kw_for: {
                s = new_stmt(StmtKind::for_, start);
                if (accept(Tok::kw_parallel)) {
                    s->is_parallel = true;
                    if (accept(Tok::lparen)) {
                        s->pool = expr();
                        expect(Tok::rparen, "after the pool");
                    }
                }
                expect(Tok::kw_for, "after `parallel`");
                s->pattern = pattern();
                expect(Tok::kw_in, "after the loop variable");
                if (accept(Tok::kw_mut)) s->iter_mut = true;
                s->value = expr_no_struct();
                s->body = block();
                break;
            }
            case Tok::kw_checkout: {
                next();
                s = new_stmt(StmtKind::checkout, start);
                s->pattern = pattern();
                expect(Tok::eq, "after the name in `checkout`");
                s->value = expr();
                if (accept(Tok::kw_else)) diverge(s);
                break;
            }
            case Tok::kw_send: {
                next();
                s = new_stmt(StmtKind::send, start);
                s->target = postfix(primary(true));
                s->value = expr();
                break;
            }
            case Tok::kw_fn:
            case Tok::kw_const:
            case Tok::at: {
                s = new_stmt(StmtKind::item, start);
                s->item = item({});
                s->span = s->item->span;
                return s;  // items end their own lines
            }
            default: {
                Expr* e = expr();
                Op op{};
                if (at(Tok::eq) || compound_op(peek().kind, op)) {
                    bool compound = !at(Tok::eq);
                    next();
                    skip_newlines();
                    s = new_stmt(StmtKind::assign, start);
                    s->target = e;
                    s->op = op;
                    s->compound = compound;
                    s->value = expr();
                } else {
                    s = new_stmt(StmtKind::expr, start);
                    s->value = e;
                }
                break;
            }
        }
        s->span = Span::join(start, prev_span());
        end_line();
        return s;
    }

    // --- patterns ------------------------------------------------------------------------

    Pattern* new_pattern(PatternKind kind, Span span) {
        auto* p = ctx_.arena.make<Pattern>();
        p->kind = kind;
        p->span = span;
        return p;
    }

    Pattern* pattern() {
        Span start = peek().span;
        switch (peek().kind) {
            case Tok::kw_none: next(); return new_pattern(PatternKind::none, start);
            case Tok::kw_mut: {
                next();
                Pattern* p = new_pattern(PatternKind::binding, start);
                p->name = ident("after `mut`");
                p->is_mut = true;
                p->span = Span::join(start, prev_span());
                return p;
            }
            case Tok::lparen: {
                next();
                Pattern* p = new_pattern(PatternKind::tuple, start);
                std::vector<Pattern*> elems;
                while (!at(Tok::rparen)) {
                    elems.push_back(pattern());
                    if (!accept(Tok::comma)) break;
                }
                expect(Tok::rparen, "to end the tuple pattern");
                p->elems = slice(elems);
                p->span = Span::join(start, prev_span());
                return p;
            }
            case Tok::ident: {
                if (ctx_.names.text(peek().name) == "_") {
                    next();
                    return new_pattern(PatternKind::wildcard, start);
                }
                Path path = this->path();
                if (at(Tok::lparen)) {
                    next();
                    Pattern* p = new_pattern(PatternKind::variant, start);
                    p->path = path;
                    std::vector<Pattern*> elems;
                    while (!at(Tok::rparen)) {
                        elems.push_back(pattern());
                        if (!accept(Tok::comma)) break;
                    }
                    expect(Tok::rparen, "to end the variant's pattern");
                    p->elems = slice(elems);
                    p->span = Span::join(start, prev_span());
                    return p;
                }
                if (at(Tok::lbrace)) {
                    next();
                    Pattern* p = new_pattern(PatternKind::record, start);
                    p->path = path;
                    std::vector<FieldPattern> fields;
                    skip_newlines();
                    while (!at(Tok::rbrace)) {
                        if (accept(Tok::dotdot)) {
                            p->has_rest = true;
                            skip_newlines();
                            break;
                        }
                        FieldPattern f;
                        f.span = peek().span;
                        f.name = ident("as a field name");
                        if (accept(Tok::colon)) f.pattern = pattern();
                        fields.push_back(f);
                        skip_newlines();
                        if (!accept(Tok::comma)) break;
                        skip_newlines();
                    }
                    expect(Tok::rbrace, "to end the record pattern");
                    p->fields = slice(fields);
                    p->span = Span::join(start, prev_span());
                    return p;
                }
                // A lone name binds, unless the checker finds a variant by that name. A dotted
                // name is always a variant.
                Pattern* p = new_pattern(path.parts.size() == 1 ? PatternKind::binding : PatternKind::variant, start);
                p->path = path;
                p->name = path.parts[0];
                p->span = path.span;
                return p;
            }
            default: {
                Pattern* p = new_pattern(PatternKind::literal, start);
                p->value = pattern_literal();
                if (at(Tok::dotdot) || at(Tok::dotdoteq)) {
                    p->kind = PatternKind::range;
                    p->inclusive = next().kind == Tok::dotdoteq;
                    p->end = pattern_literal();
                }
                p->span = Span::join(start, prev_span());
                return p;
            }
        }
    }

    Expr* pattern_literal() {
        Span start = peek().span;
        switch (peek().kind) {
            case Tok::int_lit: case Tok::float_lit: case Tok::char_lit: case Tok::string_lit:
            case Tok::kw_true: case Tok::kw_false:
                return primary(false);
            case Tok::minus: {
                next();
                Expr* e = new_expr(ExprKind::unary, start);
                e->op = Op::neg;
                if (!at(Tok::int_lit) && !at(Tok::float_lit)) fail(peek().span, "expected a number after `-` in a pattern");
                e->lhs = primary(false);
                e->span = Span::join(start, prev_span());
                return e;
            }
            default:
                fail(start, std::string("expected a pattern, found ") + tok_text(peek().kind));
        }
    }

    // --- expressions ---------------------------------------------------------------------

    Expr* new_expr(ExprKind kind, Span span) {
        auto* e = ctx_.arena.make<Expr>();
        e->kind = kind;
        e->span = span;
        return e;
    }

    // In `if`, `while`, `for` and `match` heads, a `{` starts the body, not a struct literal.
    Expr* expr_no_struct() {
        bool saved = no_struct_;
        no_struct_ = true;
        Expr* e = expr();
        no_struct_ = saved;
        return e;
    }

    Expr* expr() {
        Span start = peek().span;
        Expr* lhs = nullptr;
        if (!at(Tok::dotdot) && !at(Tok::dotdoteq)) lhs = binary(kOr);
        if (at(Tok::dotdot) || at(Tok::dotdoteq)) {
            Expr* r = new_expr(ExprKind::range, start);
            r->inclusive = next().kind == Tok::dotdoteq;
            r->lhs = lhs;
            if (starts_expr()) r->rhs = binary(kOr);
            r->span = Span::join(start, prev_span());
            return r;
        }
        return lhs;
    }

    bool starts_expr() const {
        switch (peek().kind) {
            case Tok::ident: case Tok::int_lit: case Tok::float_lit: case Tok::char_lit: case Tok::string_lit:
            case Tok::kw_true: case Tok::kw_false: case Tok::kw_none: case Tok::kw_self: case Tok::kw_not:
            case Tok::kw_if: case Tok::kw_match: case Tok::kw_loop: case Tok::kw_try: case Tok::kw_unsafe:
            case Tok::lparen: case Tok::lbracket: case Tok::minus: case Tok::tilde: case Tok::pipe:
                return true;
            case Tok::lbrace:
                return !no_struct_;
            default:
                return false;
        }
    }

    Expr* binary(int min_prec) {
        Span start = peek().span;
        Expr* lhs;
        if (at(Tok::kw_not)) {
            next();
            lhs = new_expr(ExprKind::unary, start);
            lhs->op = Op::log_not;
            lhs->lhs = binary(kCompare);
            lhs->span = Span::join(start, prev_span());
        } else {
            lhs = unary();
        }
        while (true) {
            BinOp b;
            if (!binary_op(peek().kind, b) || b.prec < min_prec) break;
            // `a | b` after a call could also start a trailing closure; postfix() takes those first.
            Span op_span = next().span;
            skip_newlines();
            Expr* rhs = binary(b.right_assoc ? b.prec : b.prec + 1);
            Expr* e = new_expr(ExprKind::binary, Span::join(start, prev_span()));
            e->op = b.op;
            e->lhs = lhs;
            e->rhs = rhs;
            lhs = e;
            BinOp after;
            if (b.prec == kCompare && binary_op(peek().kind, after) && after.prec == kCompare) {
                fail(peek().span, "comparisons don't chain; write `a < b and b < c`");
            }
            (void)op_span;
        }
        return lhs;
    }

    Expr* unary() {
        Span start = peek().span;
        if (at(Tok::minus) || at(Tok::tilde)) {
            Op op = next().kind == Tok::minus ? Op::neg : Op::bit_not;
            Expr* e = new_expr(ExprKind::unary, start);
            e->op = op;
            e->lhs = unary();
            e->span = Span::join(start, prev_span());
            return e;
        }
        if (at(Tok::kw_try)) {
            next();
            Expr* e = new_expr(ExprKind::try_, start);
            e->lhs = unary();
            e->span = Span::join(start, prev_span());
            return e;
        }
        return postfix(primary(false));
    }

    std::vector<Arg> arg_list() {
        std::vector<Arg> args;
        while (!at(Tok::rparen) && !at(Tok::eof)) {
            Arg a;
            a.span = peek().span;
            if (at(Tok::ident) && peek(1).kind == Tok::colon) {
                a.name = next().name;
                next();
            }
            if (at(Tok::kw_mut)) {
                next();
                a.is_mut = true;
            }
            bool saved = no_struct_;
            no_struct_ = false;
            a.value = expr();
            no_struct_ = saved;
            a.span = Span::join(a.span, prev_span());
            args.push_back(a);
            if (!accept(Tok::comma)) break;
        }
        expect(Tok::rparen, "to end the arguments");
        return args;
    }

    Slice<Arg> call_args() {
        expect(Tok::lparen, "to start the arguments");
        return slice(arg_list());
    }

    // `|a, b| { ... }` right after a call is a trailing closure. Anything else starting with `|`
    // is a bitwise or.
    bool trailing_closure_ahead() const {
        if (!at(Tok::pipe)) return false;
        std::size_t i = 1;
        while (true) {
            Tok k = peek(i).kind;
            if (k == Tok::pipe) return peek(i + 1).kind == Tok::lbrace;
            if (k != Tok::ident && k != Tok::comma && k != Tok::colon && k != Tok::kw_mut) return false;
            i++;
        }
    }

    Expr* postfix(Expr* e) {
        while (true) {
            Span start = e->span;
            switch (peek().kind) {
                case Tok::dot: {
                    next();
                    Expr* f = new_expr(ExprKind::field, start);
                    f->lhs = e;
                    if (at(Tok::int_lit)) {
                        f->is_tuple_index = true;
                        f->tuple_index = static_cast<std::uint32_t>(next().int_value);
                    } else {
                        f->member = ident("after `.`");
                    }
                    f->span = Span::join(start, prev_span());
                    e = f;
                    break;
                }
                case Tok::question_dot: {
                    next();
                    Expr* f = new_expr(ExprKind::optional_field, start);
                    f->lhs = e;
                    f->member = ident("after `?.`");
                    f->span = Span::join(start, prev_span());
                    e = f;
                    break;
                }
                case Tok::lparen: {
                    if (peek().space_before && e->kind != ExprKind::name && e->kind != ExprKind::field) return e;
                    next();
                    Expr* c = new_expr(ExprKind::call, start);
                    c->callee = e;
                    c->args = slice(arg_list());
                    if (trailing_closure_ahead()) c->trailing = closure();
                    c->span = Span::join(start, prev_span());
                    e = c;
                    break;
                }
                case Tok::lbracket: {
                    next();
                    Expr* i = new_expr(ExprKind::index, start);
                    i->lhs = e;
                    bool saved = no_struct_;
                    no_struct_ = false;
                    i->rhs = expr();
                    no_struct_ = saved;
                    expect(Tok::rbracket, "to end the index");
                    i->span = Span::join(start, prev_span());
                    e = i;
                    break;
                }
                case Tok::bang: {
                    next();
                    Expr* u = new_expr(ExprKind::unwrap, Span::join(start, prev_span()));
                    u->lhs = e;
                    e = u;
                    break;
                }
                case Tok::lbrace: {
                    // `Path { field: value }`: a struct literal, where one is allowed.
                    if (no_struct_ || !is_path_expr(e)) return e;
                    e = struct_literal(e);
                    break;
                }
                default:
                    return e;
            }
        }
    }

    static bool is_path_expr(const Expr* e) {
        if (e->kind == ExprKind::name) return true;
        return e->kind == ExprKind::field && !e->is_tuple_index && is_path_expr(e->lhs);
    }

    Path to_path(const Expr* e) {
        std::vector<Name> parts;
        const Expr* cur = e;
        while (cur->kind == ExprKind::field) {
            parts.insert(parts.begin(), cur->member);
            cur = cur->lhs;
        }
        parts.insert(parts.begin(), cur->path.parts.begin(), cur->path.parts.end());
        Path p;
        p.parts = slice(parts);
        p.span = e->span;
        return p;
    }

    Expr* struct_literal(Expr* type_expr) {
        Expr* s = new_expr(ExprKind::struct_lit, type_expr->span);
        s->type_path = to_path(type_expr);
        next();  // `{`
        bool saved = no_struct_;
        no_struct_ = false;
        std::vector<FieldInit> fields;
        skip_newlines();
        while (!at(Tok::rbrace) && !at(Tok::eof)) {
            FieldInit f;
            f.span = peek().span;
            f.name = ident("as a field name");
            if (accept(Tok::colon)) {
                skip_newlines();
                f.value = expr();
            }
            f.span = Span::join(f.span, prev_span());
            fields.push_back(f);
            skip_newlines();
            if (!accept(Tok::comma)) break;
            skip_newlines();
        }
        no_struct_ = saved;
        expect(Tok::rbrace, "to end the struct literal");
        s->fields = slice(fields);
        s->span = Span::join(s->span, prev_span());
        return s;
    }

    Expr* closure() {
        Span start = expect(Tok::pipe, "to start the closure's parameters").span;
        Expr* c = new_expr(ExprKind::closure, start);
        std::vector<Param> params;
        while (!at(Tok::pipe)) {
            Param p;
            p.span = peek().span;
            if (accept(Tok::kw_mut)) p.mode = ParamMode::mut;
            p.name = ident("as a closure parameter");
            if (accept(Tok::colon)) p.type = type();
            params.push_back(p);
            if (!accept(Tok::comma)) break;
        }
        expect(Tok::pipe, "to end the closure's parameters");
        c->params = slice(params);
        bool saved = no_struct_;
        no_struct_ = false;
        c->body = at(Tok::lbrace) ? block_expr() : expr();
        no_struct_ = saved;
        c->span = Span::join(start, prev_span());
        return c;
    }

    Expr* block_expr() {
        Span start = peek().span;
        Expr* e = new_expr(ExprKind::block, start);
        bool saved = no_struct_;
        no_struct_ = false;
        e->block = block();
        no_struct_ = saved;
        e->span = e->block->span;
        return e;
    }

    Expr* if_expr() {
        Span start = expect(Tok::kw_if, "").span;
        Expr* e = new_expr(ExprKind::if_, start);
        if (accept(Tok::kw_let)) {
            e->let_pattern = pattern();
            expect(Tok::eq, "after the pattern in `if let`");
        }
        e->cond = expr_no_struct();
        e->block = block_body();
        // `else` may start the next line.
        std::size_t save = pos_;
        skip_newlines();
        if (accept(Tok::kw_else)) {
            e->else_ = at(Tok::kw_if) ? if_expr() : block_expr();
        } else {
            pos_ = save;
        }
        e->span = Span::join(start, prev_span());
        return e;
    }

    Block* block_body() {
        bool saved = no_struct_;
        no_struct_ = false;
        Block* b = block();
        no_struct_ = saved;
        return b;
    }

    Expr* match_expr() {
        Span start = expect(Tok::kw_match, "").span;
        Expr* e = new_expr(ExprKind::match, start);
        e->scrutinee = expr_no_struct();
        expect(Tok::lbrace, "to start the match arms");
        bool saved = no_struct_;
        no_struct_ = false;
        std::vector<MatchArm> arms;
        skip_lines();
        while (!at(Tok::rbrace) && !at(Tok::eof)) {
            try {
                MatchArm arm;
                arm.span = peek().span;
                arm.pattern = pattern();
                if (accept(Tok::kw_if)) arm.guard = expr();
                expect(Tok::fat_arrow, "after the match pattern");
                skip_newlines();
                arm.body = expr();
                arm.span = Span::join(arm.span, prev_span());
                arms.push_back(arm);
                if (!accept(Tok::comma) && !at(Tok::rbrace)) end_line();
            } catch (SyntaxError&) {
                recover_stmt();
            }
            skip_lines();
        }
        no_struct_ = saved;
        expect(Tok::rbrace, "to end the match");
        e->arms = slice(arms);
        e->span = Span::join(start, prev_span());
        return e;
    }

    Expr* string_literal(const Token& t) {
        Expr* e = new_expr(ExprKind::string_lit, t.span);
        e->literal = t;
        std::vector<Expr*> parts;
        for (const StringPart& part : t.parts) {
            if (!part.is_expr) continue;
            const SourceFile& f = ctx_.sources.file(file_);
            std::vector<Token> toks = lex(f, file_, part.span.begin, part.span.end, ctx_.names, ctx_.arena, ctx_.diags);
            Parser sub(ctx_, file_, std::move(toks));
            Expr* inner = sub.lone_expr();
            if (!inner) inner = new_expr(ExprKind::none_lit, part.span);
            parts.push_back(inner);
        }
        e->interpolations = slice(parts);
        return e;
    }

    Expr* primary(bool simple) {
        Span start = peek().span;
        switch (peek().kind) {
            case Tok::int_lit: {
                Expr* e = new_expr(ExprKind::int_lit, start);
                e->literal = next();
                return e;
            }
            case Tok::float_lit: {
                Expr* e = new_expr(ExprKind::float_lit, start);
                e->literal = next();
                return e;
            }
            case Tok::char_lit: {
                Expr* e = new_expr(ExprKind::char_lit, start);
                e->literal = next();
                return e;
            }
            case Tok::string_lit: return string_literal(next());
            case Tok::kw_true: case Tok::kw_false: {
                Expr* e = new_expr(ExprKind::bool_lit, start);
                e->bool_value = next().kind == Tok::kw_true;
                return e;
            }
            case Tok::kw_none: next(); return new_expr(ExprKind::none_lit, start);
            case Tok::kw_self: next(); return new_expr(ExprKind::self, start);
            case Tok::ident: {
                Expr* e = new_expr(ExprKind::name, start);
                std::vector<Name> one{next().name};
                e->path.parts = slice(one);
                e->path.span = start;
                return e;
            }
            default: break;
        }
        if (simple) fail(start, std::string("expected a destination, found ") + tok_text(peek().kind));
        switch (peek().kind) {
            case Tok::lparen: {
                next();
                bool saved = no_struct_;
                no_struct_ = false;
                std::vector<Expr*> elems;
                bool trailing_comma = false;
                while (!at(Tok::rparen)) {
                    elems.push_back(expr());
                    trailing_comma = false;
                    if (!accept(Tok::comma)) break;
                    trailing_comma = true;
                }
                no_struct_ = saved;
                expect(Tok::rparen, "to close the parenthesis");
                if (elems.size() == 1 && !trailing_comma) {
                    elems[0]->parens = true;
                    return elems[0];
                }
                Expr* e = new_expr(ExprKind::tuple, Span::join(start, prev_span()));
                e->elems = slice(elems);
                return e;
            }
            case Tok::lbracket: {
                next();
                bool saved = no_struct_;
                no_struct_ = false;
                Expr* e = new_expr(ExprKind::array_lit, start);
                std::vector<Expr*> elems;
                if (!at(Tok::rbracket)) {
                    elems.push_back(expr());
                    if (accept(Tok::semicolon)) {
                        e->repeat = expr();
                    } else {
                        while (accept(Tok::comma)) {
                            if (at(Tok::rbracket)) break;
                            elems.push_back(expr());
                        }
                    }
                }
                no_struct_ = saved;
                expect(Tok::rbracket, "to end the array");
                e->elems = slice(elems);
                e->span = Span::join(start, prev_span());
                return e;
            }
            case Tok::lbrace: return block_expr();
            case Tok::kw_if: return if_expr();
            case Tok::kw_match: return match_expr();
            case Tok::kw_loop: {
                next();
                Expr* e = new_expr(ExprKind::loop, start);
                e->block = block_body();
                e->span = Span::join(start, prev_span());
                return e;
            }
            case Tok::kw_unsafe: {
                next();
                Expr* e = new_expr(ExprKind::unsafe_block, start);
                e->block = block_body();
                e->span = Span::join(start, prev_span());
                return e;
            }
            case Tok::pipe: return closure();
            default:
                fail(start, std::string("expected an expression, found ") + tok_text(peek().kind));
        }
    }

    ParseContext ctx_;
    std::uint32_t file_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    bool no_struct_ = false;
    std::uint32_t last_error_ = UINT32_MAX;
};

}  // namespace

Module* parse_file(ParseContext ctx, std::uint32_t file_id) {
    const SourceFile& f = ctx.sources.file(file_id);
    std::vector<Token> toks = lex(f, file_id, 0, static_cast<std::uint32_t>(f.text.size()), ctx.names, ctx.arena, ctx.diags);
    Parser p(ctx, file_id, std::move(toks));
    return p.module();
}

}  // namespace calx
