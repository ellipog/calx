#include "check.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace calx {

namespace {

std::string q(std::string_view s) { return "`" + std::string(s) + "`"; }

bool starts_with_underscore(std::string_view s) { return !s.empty() && s[0] == '_'; }

// The integers a type's values are, for deciding which conversions are exact. Every such range
// contains 0, so it's the magnitude of its minimum and its maximum.
struct IntRange {
    std::uint64_t neg;  // the minimum is -neg
    std::uint64_t pos;  // the maximum
};

// `usize` is judged at both its widths (D63): as a source it may be 64 bits wide, and as a target
// it may be only 32.
IntRange source_range(const Type* t) {
    if (t->kind == TypeKind::char_) return {0, 0xFFFF};
    if (t->kind == TypeKind::rune) return {0, 0x10FFFF};
    if (t->is_usize) return {0, UINT64_MAX};
    if (t->is_signed) return {std::uint64_t{1} << (t->bits - 1), (std::uint64_t{1} << (t->bits - 1)) - 1};
    return {0, t->bits == 64 ? UINT64_MAX : (std::uint64_t{1} << t->bits) - 1};
}

IntRange target_range(const Type* t) {
    if (t->is_usize) return {0, 0xFFFFFFFFu};
    return source_range(t);
}

// Whether every value of `from` is exactly a value of `to`.
bool exact_conversion(const Type* from, const Type* to) {
    if (from == to) return true;
    if (is_integral(from) && is_integral(to)) {
        // A rune can't be a surrogate, so only bytes convert to one exactly.
        if (to->kind == TypeKind::rune) return from->kind == TypeKind::int_ && from->bits == 8 && !from->is_signed;
        IntRange a = source_range(from), b = target_range(to);
        return a.neg <= b.neg && a.pos <= b.pos;
    }
    if (is_integral(from) && is_float(to)) {
        IntRange a = source_range(from);
        std::uint64_t limit = std::uint64_t{1} << (to->bits == 32 ? 24 : 53);
        return a.neg <= limit && a.pos <= limit;
    }
    if (is_float(from) && is_float(to)) return from->bits < to->bits;
    return false;
}

struct LoopCtx {
    std::uint32_t id;
    bool is_loop;        // `loop`, which can break with a value
    bool stmt;           // a `loop` whose value isn't used
    Type* expected;      // `loop` used as a value: the type wanted
    Type* break_type;    // the type of the breaks so far
    bool has_break;
};

enum class BlockUse { stmt, value, fn_body };

class Checker {
public:
    Checker(CheckContext ctx, Program& prog) : ctx_(ctx), T(ctx.types), prog_(prog) {
        n_self_ = ctx.names.intern("self");
        n_underscore_ = ctx.names.intern("_");
        n_Self_ = ctx.names.intern("Self");
        for (const char* b : {"print", "println", "panic", "assert"}) builtin_names_.push_back(ctx.names.intern(b));
        for (const char* b : {"some", "ok", "err"}) reserved_names_.push_back(ctx.names.intern(b));
    }

    void run(Module* m, bool need_main) {
        module_ = m;
        prog_.root = m;
        set_prefix(m);
        collect(m);
        for (Item* it : structs_) resolve_fields(it);
        for (Item* it : impls_) collect_impl(it);
        for (Item* f : all_fns_) check_signature(f);
        for (Item* c : consts_) check_const(c);
        for (Item* f : all_fns_) check_fn(f);
        order_types();
        check_unused_items();
        if (need_main) find_main(m);
    }

private:
    // --- reporting -----------------------------------------------------------------------

    void error(Span s, std::string msg) { ctx_.diags.error(s, std::move(msg)); }
    void note(Span s, std::string msg) { ctx_.diags.note(s, std::move(msg)); }
    std::string_view text(Name n) const { return ctx_.names.text(n); }
    std::string name_of(const Type* t) const { return q(t->name); }
    // "an `i32`", "a `u8`": the article goes by how the name is read aloud.
    std::string a_type(const Type* t) const {
        char c = t->name.empty() ? 'x' : t->name[0];
        bool vowel = c == 'i' || c == 'f' || c == 'a' || c == 'e' || c == 'o' || c == 'A' || c == 'E' || c == 'I' ||
                     c == 'O' || c == 'U';
        return (vowel ? "an " : "a ") + q(t->name);
    }
    // The literal as written, with its radix prefix.
    std::string literal_source(const Expr* e) const {
        const SourceFile& f = ctx_.sources.file(e->span.file);
        return f.text.substr(e->span.begin, e->span.end - e->span.begin);
    }

    void not_yet(Span s, const char* what) { error(s, std::string(what) + " aren't implemented yet"); }

    // --- the module ------------------------------------------------------------------------

    void set_prefix(Module* m) {
        std::string_view path = ctx_.sources.file(m->file).path;
        std::size_t slash = path.find_last_of("/\\");
        std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
        std::string_view stem = base.substr(0, base.rfind('.'));
        std::string prefix;
        if (!m->name.parts.empty()) {
            std::string_view last = text(m->name.parts[m->name.parts.size() - 1]);
            if (last != stem) {
                error(m->name.span, "module " + q(last) + " must be in a file named " + q(std::string(last) + ".calx"));
            }
            for (std::size_t i = 0; i < m->name.parts.size(); i++) {
                if (i) prefix += '_';
                prefix += text(m->name.parts[i]);
            }
        } else {
            for (char c : stem) prefix += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
            if (prefix.empty() || std::isdigit(static_cast<unsigned char>(prefix[0]))) prefix = "m" + prefix;
        }
        prog_.prefix = ctx_.arena.copy(prefix);
    }

    std::string_view c_name(std::initializer_list<std::string_view> parts) {
        std::string s(prog_.prefix);
        for (std::string_view p : parts) {
            s += "__";
            s += p;
        }
        return ctx_.arena.copy(s);
    }

    bool is_builtin_name(Name n) const {
        for (Name b : builtin_names_) if (b == n) return true;
        for (Name b : reserved_names_) if (b == n) return true;
        return T.primitive(text(n)) != nullptr;
    }

    void declare_item(Item* it) {
        if (is_builtin_name(it->name)) {
            error(it->name_span, q(text(it->name)) + " is a built-in name");
            return;
        }
        auto [pos, fresh] = items_.emplace(it->name, it);
        if (!fresh) {
            error(it->name_span, q(text(it->name)) + " is already defined");
            note(pos->second->name_span, "the first definition is here");
        }
    }

    void collect(Module* m) {
        for (Item* it : m->items) {
            if (!it->attributes.empty()) {
                error(it->attributes[0].span, "attributes aren't implemented yet");
            }
            switch (it->kind) {
                case ItemKind::fn:
                    if (it->is_extern) {
                        not_yet(it->span, "extern functions");
                        continue;
                    }
                    if (!it->generics.empty()) not_yet(it->generics[0].span, "generic functions");
                    if (!it->body) {
                        error(it->name_span, "function " + q(text(it->name)) + " needs a body");
                        continue;
                    }
                    declare_item(it);
                    it->c_name = c_name({text(it->name)});
                    all_fns_.push_back(it);
                    break;
                case ItemKind::struct_:
                    if (it->is_actor) not_yet(it->span, "actors");
                    if (!it->generics.empty()) not_yet(it->generics[0].span, "generic structs");
                    if (it->is_tuple_struct) not_yet(it->span, "wrapper structs");
                    declare_item(it);
                    it->c_name = c_name({text(it->name)});
                    it->declared = T.make_struct(it, text(it->name));
                    structs_.push_back(it);
                    break;
                case ItemKind::const_:
                    declare_item(it);
                    it->c_name = c_name({text(it->name)});
                    consts_.push_back(it);
                    break;
                case ItemKind::impl:
                    impls_.push_back(it);
                    break;
                case ItemKind::import: not_yet(it->span, "imports"); break;
                case ItemKind::enum_: not_yet(it->span, "enums"); break;
                case ItemKind::interface: not_yet(it->span, "interfaces"); break;
                case ItemKind::type_alias: not_yet(it->span, "type aliases"); break;
                case ItemKind::extern_block: not_yet(it->span, "extern blocks"); break;
                case ItemKind::attribute_block: not_yet(it->span, "attribute blocks"); break;
            }
        }
    }

    void resolve_fields(Item* s) {
        std::unordered_map<Name, Span> seen;
        for (FieldDecl& f : s->fields) {
            if (!seen.emplace(f.name, f.span).second) {
                error(f.span, "field " + q(text(f.name)) + " is already declared");
                note(seen[f.name], "the first declaration is here");
            }
            current_item_ = s;
            f.resolved = resolve_type(f.type);
            current_item_ = nullptr;
        }
        for (FieldDecl& f : s->fields) {
            if (!f.default_value) continue;
            Type* t = check(f.default_value, f.resolved);
            if (!is_bad(t) && !f.default_value->is_const) {
                error(f.default_value->span, "a field's default must be a constant");
            }
        }
    }

    void collect_impl(Item* impl) {
        if (impl->interface) {
            not_yet(impl->interface->span, "interfaces");
            return;
        }
        if (!impl->generics.empty()) not_yet(impl->generics[0].span, "generic impls");
        Type* t = resolve_type(impl->self_type);
        if (is_bad(t)) return;
        if (t->kind != TypeKind::struct_) {
            error(impl->self_type->span, "methods can only be added to structs declared in this module");
            return;
        }
        auto& table = methods_[t];
        for (Item* m : impl->members) {
            if (m->kind != ItemKind::fn) {
                error(m->span, "an impl block holds only functions for now");
                continue;
            }
            if (!m->body) {
                error(m->name_span, "function " + q(text(m->name)) + " needs a body");
                continue;
            }
            if (!m->generics.empty()) not_yet(m->generics[0].span, "generic functions");
            m->impl_type = t;
            m->c_name = c_name({t->name, text(m->name)});
            for (const FieldDecl& f : t->item->fields) {
                if (f.name == m->name) {
                    error(m->name_span, q(text(m->name)) + " is both a field and a method of " + name_of(t));
                    note(f.span, "the field is here");
                }
            }
            auto [pos, fresh] = table.emplace(m->name, m);
            if (!fresh) {
                error(m->name_span, name_of(t) + " already has a function named " + q(text(m->name)));
                note(pos->second->name_span, "the first one is here");
                continue;
            }
            all_fns_.push_back(m);
        }
    }

    // --- types ---------------------------------------------------------------------------

    Type* resolve_type(TypeExpr* t) {
        switch (t->kind) {
            case TypeExprKind::named: {
                if (t->path.parts.size() != 1) {
                    error(t->span, "unknown type " + q(path_text(t->path)));
                    return T.error;
                }
                Name n = t->path.parts[0];
                Type* r = T.primitive(text(n));
                if (!r) {
                    auto it = items_.find(n);
                    if (it != items_.end() && it->second->kind == ItemKind::struct_) {
                        r = it->second->declared;
                        if (it->second != current_item_) it->second->used = true;
                    } else if (it != items_.end()) {
                        error(t->span, q(text(n)) + " isn't a type");
                        return T.error;
                    } else {
                        error(t->span, "unknown type " + q(text(n)));
                        return T.error;
                    }
                }
                if (!t->args.empty()) {
                    error(t->args[0]->span, name_of(r) + " doesn't take generic arguments");
                    return T.error;
                }
                return r;
            }
            case TypeExprKind::tuple:
                if (t->args.empty()) return T.unit;
                not_yet(t->span, "tuples");
                return T.error;
            case TypeExprKind::array: {
                Type* elem = resolve_type(t->inner);
                Type* len_t = check(t->length, T.usize);
                if (is_bad(elem) || is_bad(len_t)) return T.error;
                if (!t->length->has_bits) {
                    error(t->length->span, "an array's length must be a constant");
                    return T.error;
                }
                if (elem->kind == TypeKind::unit || elem->kind == TypeKind::never) {
                    error(t->inner->span, "an array can't hold " + name_of(elem));
                    return T.error;
                }
                return T.array_of(elem, t->length->const_bits);
            }
            case TypeExprKind::self:
                if (current_impl_type_) return current_impl_type_;
                error(t->span, "`Self` only means something inside an impl block");
                return T.error;
            case TypeExprKind::optional: not_yet(t->span, "optional types"); return T.error;
            case TypeExprKind::fn: not_yet(t->span, "function types"); return T.error;
            case TypeExprKind::pointer: not_yet(t->span, "pointers"); return T.error;
            case TypeExprKind::dyn: not_yet(t->span, "`dyn` types"); return T.error;
        }
        return T.error;
    }

    std::string path_text(const Path& p) const {
        std::string s;
        for (std::size_t i = 0; i < p.parts.size(); i++) {
            if (i) s += '.';
            s += text(p.parts[i]);
        }
        return s;
    }

    static bool is_view_type(const Type* t) { return t->kind == TypeKind::struct_ || t->kind == TypeKind::array; }

    void check_signature(Item* f) {
        if (f->checked_signature) return;
        f->checked_signature = true;
        current_impl_type_ = f->impl_type;
        for (std::size_t i = 0; i < f->params.size(); i++) {
            Param& p = f->params[i];
            if (p.default_value) not_yet(p.default_value->span, "default arguments");
            Type* t;
            if (p.is_self) {
                if (!f->impl_type) {
                    error(p.span, "`self` is only allowed in a function in an impl block");
                    t = T.error;
                } else if (i != 0) {
                    error(p.span, "`self` must be the first parameter");
                    t = T.error;
                } else {
                    t = p.type ? resolve_type(p.type) : f->impl_type;
                    if (p.type && !is_bad(t) && t != f->impl_type) {
                        error(p.type->span, "`self` must be " + a_type(f->impl_type));
                    }
                }
            } else {
                t = resolve_type(p.type);
            }
            if (t->kind == TypeKind::unit) error(p.type->span, "a parameter can't be `()`");
            if (i == 0 && f->impl_type && t == f->impl_type) f->is_method = true;
            auto* l = ctx_.arena.make<Local>();
            l->name = p.name;
            l->type = t;
            l->span = p.span;
            l->is_param = true;
            l->mode = p.mode;
            l->is_view = p.mode == ParamMode::mut || (p.mode == ParamMode::read && is_view_type(t));
            p.local = l;
        }
        if (f->error) not_yet(f->error->span, "functions that can fail");
        f->result_type = f->result ? resolve_type(f->result) : T.unit;
        if (f->result && f->result_type->kind == TypeKind::unit) {
            error(f->result->span, "leave out `-> ()`: a function without `->` returns nothing");
        }
        current_impl_type_ = nullptr;
    }

    void check_const(Item* c) {
        if (c->declared) return;
        if (checking_consts_.count(c)) {
            error(c->name_span, "constant " + q(text(c->name)) + " refers to itself");
            c->declared = T.error;
            return;
        }
        if (!c->type) {
            error(c->name_span, "give the constant a type: `const " + std::string(text(c->name)) + ": i32 = ...`");
            c->declared = T.error;
            return;
        }
        checking_consts_.emplace(c, true);
        // A constant sees only module-level names, even when a function body asks for it first.
        std::vector<Local*> saved_visible;
        std::vector<std::size_t> saved_marks;
        saved_visible.swap(visible_);
        saved_marks.swap(scope_marks_);
        Item* saved_fn = fn_;
        Item* saved_item = current_item_;
        Type* saved_impl = current_impl_type_;
        fn_ = nullptr;
        current_item_ = c;
        current_impl_type_ = nullptr;
        Type* t = resolve_type(c->type);
        if (!c->value) {
            error(c->name_span, "constant " + q(text(c->name)) + " needs a value");
        } else if (!is_bad(t)) {
            if (!(is_numeric(t) || t->kind == TypeKind::bool_ || t->kind == TypeKind::char_ ||
                  t->kind == TypeKind::rune)) {
                not_yet(c->type->span, "constants of this type");
            } else {
                Type* vt = check(c->value, t);
                if (!is_bad(vt) && !c->value->is_const) {
                    error(c->value->span, "a constant's value must be known at compile time");
                }
                if (is_float(t)) prog_.consts.push_back(c);
            }
        }
        checking_consts_.erase(c);
        visible_.swap(saved_visible);
        scope_marks_.swap(saved_marks);
        fn_ = saved_fn;
        current_item_ = saved_item;
        current_impl_type_ = saved_impl;
        c->declared = t;
    }

    // --- functions -------------------------------------------------------------------------

    void check_fn(Item* f) {
        fn_ = f;
        current_impl_type_ = f->impl_type;
        visible_.clear();
        scope_marks_.clear();
        loops_.clear();
        push_scope();
        for (Param& p : f->params) {
            if (p.local) declare(p.local);
        }
        Type* body = check_block(f->body, nullptr, BlockUse::fn_body);
        if (f->result_type->kind != TypeKind::unit && !is_bad(f->result_type) && body->kind != TypeKind::never) {
            Span end{f->body->span.file, f->body->span.end - 1, f->body->span.end};
            error(end, "function " + q(text(f->name)) + " can reach its end without returning a value");
        }
        pop_scope();
        prog_.fns.push_back(f);
        fn_ = nullptr;
        current_impl_type_ = nullptr;
    }

    // --- scopes and locals -----------------------------------------------------------------

    void push_scope() { scope_marks_.push_back(visible_.size()); }

    void pop_scope() {
        std::size_t mark = scope_marks_.back();
        scope_marks_.pop_back();
        for (std::size_t i = mark; i < visible_.size(); i++) report_unused(visible_[i]);
        visible_.resize(mark);
    }

    void report_unused(Local* l) {
        std::string_view n = text(l->name);
        if (l->is_param) {
            if (!l->used && !l->mutated && !starts_with_underscore(n)) {
                if (l->name == n_self_) {
                    error(l->span, "`self` is never used; make " + q(text(fn_->name)) +
                                       " a plain function instead of a method");
                } else {
                    error(l->span, "parameter " + q(n) + " is never used; name it " + q("_" + std::string(n)) +
                                       " if that's intended");
                }
            } else if (l->mode == ParamMode::mut && !l->mutated) {
                error(l->span, "parameter " + q(n) + " is `mut` but never changed; take it without `mut`");
            }
            return;
        }
        if (!l->used && !starts_with_underscore(n)) {
            error(l->span, q(n) + " is never used");
        } else if (l->is_var && !l->reassigned && !l->mutated) {
            error(l->span, q(n) + " is declared with `var` but never changed; declare it with `let`");
        }
    }

    Local* find_local(Name n) const {
        for (std::size_t i = visible_.size(); i-- > 0;) {
            if (visible_[i]->name == n) return visible_[i];
        }
        return nullptr;
    }

    // No shadowing (D63): a new local can't take a name that's already visible.
    void declare(Local* l) {
        if (l->name == n_underscore_) return;
        // After a shadowing error, the hidden name counts as used, so it gets no second error.
        if (Local* prev = find_local(l->name)) {
            error(l->span, q(text(l->name)) + " is already defined; Calx doesn't allow shadowing");
            note(prev->span, "the earlier definition is here");
            prev->used = true;
        } else if (auto it = items_.find(l->name); it != items_.end()) {
            error(l->span, q(text(l->name)) + " is already the name of a " + item_kind(it->second) +
                               "; Calx doesn't allow shadowing");
            note(it->second->name_span, "it's defined here");
            it->second->used = true;
        } else if (is_builtin_name(l->name)) {
            error(l->span, q(text(l->name)) + " is a built-in name");
        }
        visible_.push_back(l);
    }

    static const char* item_kind(const Item* it) {
        switch (it->kind) {
            case ItemKind::fn: return "function";
            case ItemKind::struct_: return "struct";
            case ItemKind::const_: return "constant";
            default: return "declaration";
        }
    }

    Local* new_local(Name name, Type* type, Span span, bool is_var) {
        auto* l = ctx_.arena.make<Local>();
        l->name = name;
        l->type = type;
        l->span = span;
        l->is_var = is_var;
        return l;
    }

    // --- blocks and statements -------------------------------------------------------------

    Type* check_block(Block* b, Type* expected, BlockUse use) {
        push_scope();
        bool diverged = false;
        bool reported = false;
        auto unreachable = [&](Span s) {
            if (diverged && !reported) {
                error(s, "this code is never reached");
                reported = true;
            }
        };
        for (Stmt* s : b->stmts) {
            unreachable(s->span);
            if (check_stmt(s)) diverged = true;
        }
        Type* result = T.unit;
        if (b->tail) {
            unreachable(b->tail->span);
            if (use == BlockUse::value) {
                result = check(b->tail, expected);
            } else {
                Type* t = check_expr_stmt(b->tail, use == BlockUse::fn_body);
                if (t->kind == TypeKind::never) diverged = true;
            }
        }
        if (diverged) result = T.never;
        pop_scope();
        return result;
    }

    // An expression on its own line: its value must be `()` (D63).
    Type* check_expr_stmt(Expr* e, bool fn_tail) {
        Type* t;
        switch (e->kind) {
            case ExprKind::if_: t = check_if(e, nullptr, true); break;
            case ExprKind::block: t = check_block(e->block, nullptr, BlockUse::stmt); break;
            case ExprKind::loop: t = check_loop(e, nullptr, true); break;
            default: t = check(e, nullptr); break;
        }
        e->type = t;
        if (t->kind == TypeKind::unit || t->kind == TypeKind::never || is_bad(t)) return t;
        if (fn_tail && fn_ && t == fn_->result_type) {
            error(e->span, "a function gives back its value with `return`: write `return` before this");
            return T.never;  // so there's no second error about a missing return
        } else {
            error(e->span, "this " + name_of(t) + " value is unused; discard it with `_ = ...` if that's intended");
        }
        return T.error;
    }

    // Returns whether the statement diverges: whether code after it can run.
    bool check_stmt(Stmt* s) {
        switch (s->kind) {
            case StmtKind::let: return check_let(s);
            case StmtKind::assign: check_assign(s); return false;
            case StmtKind::expr: return check_expr_stmt(s->value, false)->kind == TypeKind::never;
            case StmtKind::return_: check_return(s); return true;
            case StmtKind::break_: check_break(s); return true;
            case StmtKind::continue_:
                if (loops_.empty()) error(s->span, "`continue` outside a loop");
                else s->loop_id = loops_.back().id;
                return true;
            case StmtKind::while_: check_while(s); return false;
            case StmtKind::for_: check_for(s); return false;
            case StmtKind::fail: not_yet(s->span, "errors"); return true;
            case StmtKind::checkout: not_yet(s->span, "`checkout` statements"); return false;
            case StmtKind::send: not_yet(s->span, "`send` statements"); return false;
            case StmtKind::item: not_yet(s->span, "nested declarations"); return false;
        }
        return false;
    }

    bool check_let(Stmt* s) {
        Pattern* p = s->pattern;
        if (p->kind == PatternKind::wildcard) {
            error(p->span, "write `_ = ...` to discard a value");
            if (s->value) check(s->value, nullptr);
            return false;
        }
        if (p->kind != PatternKind::binding) {
            not_yet(p->span, "patterns in `let`");
            return false;
        }
        if (p->is_mut) {
            error(p->span, "declare a variable that changes with `var`, not `let mut`");
        }
        if (s->else_ || s->else_block) not_yet(s->span, "`let ... else`");
        Type* declared = s->type ? resolve_type(s->type) : nullptr;
        if (!s->value) {
            error(p->span, "give " + q(text(p->name)) + " a value; Calx has no uninitialized variables");
            Local* l = new_local(p->name, declared ? declared : T.error, p->span, s->is_var);
            l->used = l->reassigned = l->mutated = true;
            s->local = l;
            declare(l);
            return false;
        }
        Type* t = check(s->value, declared);
        Type* local_t = declared ? declared : t;
        if (t->kind == TypeKind::unit) {
            error(s->value->span, "this has no value to bind");
            local_t = T.error;
        }
        if (local_t->kind == TypeKind::never) local_t = T.error;
        Local* l = new_local(p->name, local_t, p->span, s->is_var);
        s->local = l;
        declare(l);
        return t->kind == TypeKind::never;
    }

    // `_ = e` discards; anything else assigns to a place.
    void check_assign(Stmt* s) {
        Expr* target = s->target;
        if (!s->compound && target->kind == ExprKind::name && target->path.parts[0] == n_underscore_) {
            Type* t = check(s->value, nullptr);
            if (t->kind == TypeKind::unit) error(s->value->span, "there's no value here to discard");
            return;
        }
        Local* root = nullptr;
        Type* tt = check_place(target, &root);
        bool whole = target->kind == ExprKind::name || target->kind == ExprKind::self;
        if (root) require_writable(target, root, whole);
        if (!s->compound) {
            check(s->value, is_bad(tt) ? nullptr : tt);
            return;
        }
        if (is_bad(tt)) {
            check(s->value, nullptr);
            return;
        }
        // `a op= b` is `a = a op b`, with the same rules.
        Type* vt = check(s->value, is_shift(s->op) ? nullptr : tt);
        // `x += 1` doesn't count as reading `x`: a counter nobody reads is unused.
        check_operands(s->op, s->span, tt, vt, s->value);
    }

    static bool is_shift(Op op) { return op == Op::shl || op == Op::shr || op == Op::ushr; }

    // The type of an assignment's target, without counting it as a read.
    Type* check_place(Expr* e, Local** root) {
        switch (e->kind) {
            case ExprKind::name:
            case ExprKind::self: {
                Name n = e->kind == ExprKind::self ? n_self_ : e->path.parts[0];
                Local* l = find_local(n);
                if (!l) {
                    Type* t = check(e, nullptr);
                    if (!is_bad(t)) error(e->span, "can't assign to " + q(text(n)) + ": it isn't a variable");
                    return T.error;
                }
                e->ref = Ref::local;
                e->local = l;
                e->type = l->type;
                *root = l;
                return l->type;
            }
            case ExprKind::field: {
                Type* base = check_place(e->lhs, root);
                return e->type = field_type(e, base);
            }
            case ExprKind::index: {
                Type* base = check_place(e->lhs, root);
                return e->type = index_type(e, base);
            }
            default:
                if (!is_bad(check(e, nullptr))) error(e->span, "can't assign to this expression");
                return T.error;
        }
    }

    // Checks that the place rooted at `l` can be changed, and records that it was.
    void require_writable(Expr* place, Local* l, bool whole) {
        std::string_view n = text(l->name);
        // After an error, count the write anyway, so the same variable gets no second error.
        if (l->borrowed) {
            error(place->span, q(n) + " can't be changed while a `for` loop goes over it");
            l->used = l->mutated = true;
            return;
        }
        if (l->is_param) {
            if (l->mode != ParamMode::mut) {
                error(place->span, "parameter " + q(n) + " can't be changed; take it as `mut` to change it");
                l->used = true;
                return;
            }
            l->mutated = true;
            return;
        }
        if (l->is_loop_var) {
            error(place->span, "the loop variable " + q(n) + " can't be changed; copy it into a `var`");
            l->used = true;
            return;
        }
        if (!l->is_var) {
            error(place->span, q(n) + " can't be changed; declare it with `var` to change it");
            note(l->span, q(n) + " is declared here");
            l->used = true;
            return;
        }
        if (whole) l->reassigned = true;
        else l->mutated = true;
    }

    // The root variable of a place such as `a.b[i].c`, or null if the expression isn't one.
    static Local* place_root(Expr* e) {
        while (true) {
            if (e->kind == ExprKind::name || e->kind == ExprKind::self) return e->ref == Ref::local ? e->local : nullptr;
            if (e->kind == ExprKind::field || e->kind == ExprKind::index) {
                e = e->lhs;
                continue;
            }
            return nullptr;
        }
    }

    void check_return(Stmt* s) {
        Type* want = fn_->result_type;
        if (!s->value) {
            if (want->kind != TypeKind::unit && !is_bad(want)) {
                error(s->span, "function " + q(text(fn_->name)) + " must return " + a_type(want));
            }
            return;
        }
        if (want->kind == TypeKind::unit) {
            check(s->value, nullptr);
            error(s->value->span, "function " + q(text(fn_->name)) + " doesn't return a value");
            return;
        }
        check(s->value, want);
    }

    void check_break(Stmt* s) {
        if (loops_.empty()) {
            error(s->span, "`break` outside a loop");
            if (s->value) check(s->value, nullptr);
            return;
        }
        LoopCtx& loop = loops_.back();
        s->loop_id = loop.id;
        loop.has_break = true;
        if (!s->value) {
            if (loop.is_loop && !loop.stmt) {
                Type* want = loop.break_type ? loop.break_type : loop.expected;
                if (want && want->kind != TypeKind::unit) error(s->span, "this `break` needs a value");
                if (!loop.break_type) loop.break_type = T.unit;
            }
            return;
        }
        if (!loop.is_loop) {
            error(s->value->span, "only `loop` can break with a value");
            check(s->value, nullptr);
            return;
        }
        if (loop.stmt) {
            check(s->value, nullptr);
            error(s->value->span, "this loop's value is unused, so `break` can't carry one");
            return;
        }
        Type* want = loop.break_type ? loop.break_type : loop.expected;
        Type* t = check(s->value, want);
        if (!loop.break_type && t->kind != TypeKind::never) loop.break_type = want ? want : t;
    }

    void check_while(Stmt* s) {
        check_condition(s->cond);
        if (s->cond->kind == ExprKind::bool_lit && s->cond->bool_value) {
            error(s->cond->span, "write `loop` for a loop that only ends with `break`");
        }
        push_loop(false, false, nullptr);
        s->loop_id = loops_.back().id;
        check_block(s->body, nullptr, BlockUse::stmt);
        loops_.pop_back();
    }

    void check_for(Stmt* s) {
        if (s->is_parallel) not_yet(s->span, "`parallel for` loops");
        if (s->iter_mut) not_yet(s->span, "`for ... in mut` loops");
        Pattern* p = s->pattern;
        if (p->kind != PatternKind::binding && p->kind != PatternKind::wildcard) {
            not_yet(p->span, "patterns in `for`");
            return;
        }
        Expr* it = s->value;
        Type* elem = T.error;
        Local* borrowed = nullptr;
        if (it->kind == ExprKind::range) {
            if (!it->lhs || !it->rhs) {
                error(it->span, "a `for` loop's range needs both ends");
            } else {
                Type* t = check_pair(it->lhs, it->rhs, nullptr);
                if (!is_bad(t) && !is_int(t)) {
                    error(it->span, "a range goes over integers, not " + name_of(t));
                } else {
                    elem = t;
                }
            }
            it->type = elem;
        } else {
            Type* t = check(it, nullptr);
            if (t->kind == TypeKind::array) {
                elem = t->elem;
                borrowed = place_root(it);
            } else if (!is_bad(t)) {
                error(it->span, "a `for` loop goes over a range or an array, not " + name_of(t));
            }
        }
        s->elem_type = elem;
        push_scope();
        if (p->kind == PatternKind::binding) {
            if (p->is_mut) error(p->span, "a `for` loop's variable can't be `mut`; copy it into a `var`");
            Local* l = new_local(p->name, elem, p->span, false);
            l->is_loop_var = true;
            s->local = l;
            declare(l);
        }
        push_loop(false, false, nullptr);
        s->loop_id = loops_.back().id;
        if (borrowed) borrowed->borrowed++;
        check_block(s->body, nullptr, BlockUse::stmt);
        if (borrowed) borrowed->borrowed--;
        loops_.pop_back();
        pop_scope();
    }

    void push_loop(bool is_loop, bool stmt, Type* expected) {
        loops_.push_back(LoopCtx{next_loop_++, is_loop, stmt, expected, nullptr, false});
    }

    void check_condition(Expr* cond) {
        check(cond, T.bool_);
        if (cond->kind == ExprKind::bool_lit && cond->bool_value == false) {
            error(cond->span, "this condition is always false");
        }
    }

    // --- expressions -----------------------------------------------------------------------

    // Checks `e`, which should have type `expected` if that isn't null, and returns its type.
    Type* check(Expr* e, Type* expected) {
        Type* t = infer(e, expected);
        e->type = t;
        if (expected && !is_bad(expected) && !is_bad(t) && t != expected && t->kind != TypeKind::never) {
            mismatch(e, expected, t);
            return T.error;
        }
        return t;
    }

    void mismatch(Expr* e, Type* want, Type* got) {
        std::string msg = "expected " + name_of(want) + ", found " + name_of(got);
        if (is_numeric(want) && (is_numeric(got) || is_integral(got))) {
            if (exact_conversion(got, want)) {
                msg += "; convert it with " + q(std::string(want->name) + "(...)");
            } else {
                msg += "; " + lossy_hint(got, want);
            }
        } else if (is_integral(want) && is_integral(got)) {
            msg += "; " + (exact_conversion(got, want) ? "convert it with " + q(std::string(want->name) + "(...)")
                                                        : lossy_hint(got, want));
        }
        error(e->span, msg);
    }

    std::string lossy_hint(const Type* from, const Type* to) {
        std::string t(to->name);
        if (is_float(from) && is_int(to)) {
            if (to == T.i32 || to == T.i64) return "Java's cast is " + q(t + ".saturate(...)");
            return "convert to i32 first, as Java's cast does: " + q(t + ".wrap(i32.saturate(...))");
        }
        if (is_float(to)) return "round it with " + q(t + ".nearest(...)");
        if (is_integral(from) && is_integral(to)) {
            if (to->kind == TypeKind::rune) return "" + a_type(from) + " may not be a valid rune";
            return "keep the low bits with " + q(t + ".wrap(...)") + ", as Java's cast does";
        }
        return "there's no conversion between them";
    }

    static bool is_untyped_literal(const Expr* e) {
        if (e->kind == ExprKind::int_lit || e->kind == ExprKind::float_lit || e->kind == ExprKind::char_lit) {
            return e->literal.suffix.empty();
        }
        if (e->kind == ExprKind::unary && e->op == Op::neg) return is_untyped_literal(e->lhs);
        return false;
    }

    // Two operands that must have the same type. An untyped literal on the left takes its type
    // from the right, so `1 + x` works like `x + 1`.
    Type* check_pair(Expr* a, Expr* b, Type* want) {
        if (is_untyped_literal(a) && !is_untyped_literal(b)) {
            Type* tb = check(b, want);
            Type* ta = check(a, is_bad(tb) ? want : tb);
            return is_bad(ta) ? ta : tb;
        }
        Type* ta = check(a, want);
        if (is_bad(ta)) {
            check(b, nullptr);
            return T.error;
        }
        Type* tb = check(b, ta->kind == TypeKind::never ? nullptr : ta);
        if (ta->kind == TypeKind::never) return tb;
        return is_bad(tb) ? tb : ta;
    }

    Type* infer(Expr* e, Type* expected) {
        switch (e->kind) {
            case ExprKind::int_lit: return int_literal(e, expected, false);
            case ExprKind::float_lit: return float_literal(e, expected, false);
            case ExprKind::char_lit: return char_literal(e, expected);
            case ExprKind::bool_lit:
                e->is_const = e->has_bits = true;
                e->const_bits = e->bool_value;
                return T.bool_;
            case ExprKind::string_lit: return string_literal(e, false);
            case ExprKind::none_lit: not_yet(e->span, "optionals"); return T.error;
            case ExprKind::name:
            case ExprKind::self: return name_value(e);
            case ExprKind::unary: return unary(e, expected);
            case ExprKind::binary: return binary(e, expected);
            case ExprKind::range:
                error(e->span, "a range can only be used in a `for` loop for now");
                return T.error;
            case ExprKind::call: return call(e, expected);
            case ExprKind::field: return field(e);
            case ExprKind::index: {
                Type* base = check(e->lhs, nullptr);
                return index_type(e, base);
            }
            case ExprKind::struct_lit: return struct_literal(e);
            case ExprKind::array_lit: return array_literal(e, expected);
            case ExprKind::tuple:
                if (e->elems.empty()) return T.unit;
                not_yet(e->span, "tuples");
                return T.error;
            case ExprKind::block: return check_block(e->block, expected, BlockUse::value);
            case ExprKind::if_: return check_if(e, expected, false);
            case ExprKind::loop: return check_loop(e, expected, false);
            case ExprKind::match: not_yet(e->span, "`match` expressions"); return T.error;
            case ExprKind::closure: not_yet(e->span, "closures"); return T.error;
            case ExprKind::optional_field: not_yet(e->span, "optionals"); return T.error;
            case ExprKind::unwrap: not_yet(e->span, "optionals"); return T.error;
            case ExprKind::try_: not_yet(e->span, "errors"); return T.error;
            case ExprKind::unsafe_block: not_yet(e->span, "`unsafe` blocks"); return T.error;
        }
        return T.error;
    }

    // --- literals --------------------------------------------------------------------------

    Type* suffix_type(const Expr* e) {
        std::string_view sfx = e->literal.suffix;
        if (sfx.empty()) return nullptr;
        Type* t = T.primitive(sfx);
        if (!t || !(is_int(t) || is_float(t))) {
            error(e->span, "unknown literal suffix " + q(sfx));
            return T.error;
        }
        return t;
    }

    Type* int_literal(Expr* e, Type* expected, bool negative) {
        Type* t = suffix_type(e);
        if (t && is_bad(t)) return t;
        if (!t) {
            if (expected && is_int(expected)) {
                t = expected;
            } else if (expected && is_float(expected)) {
                error(e->span, "an integer literal isn't " + a_type(expected) + "; write " +
                                   q(std::string(e->literal.digits) + ".0"));
                return T.error;
            } else if (expected && (expected->kind == TypeKind::char_ || expected->kind == TypeKind::rune)) {
                error(e->span, "an integer literal isn't " + a_type(expected) + "; write a character literal such as `'a'`");
                return T.error;
            } else {
                t = T.i32;
            }
        }
        if (e->literal.overflow) {
            error(e->span, "this literal is too large for any integer type");
            return T.error;
        }
        std::uint64_t v = e->literal.int_value;
        IntRange r = t->is_usize ? IntRange{0, 0xFFFFFFFFu} : source_range(t);
        if (negative && !t->is_signed) {
            error(e->span, name_of(t) + " can't be negative");
            return T.error;
        }
        if (negative ? v > r.neg : v > r.pos) {
            std::string src = literal_source(e);
            std::string msg = std::string(negative ? "-" : "") + src + " doesn't fit in " + name_of(t);
            if (t->is_usize) msg += ", which is 32 bits on some targets; use u64 for larger values";
            else msg += " (" + range_text(t) + ")";
            error(e->span, msg);
            bool radix = src.size() > 2 && src[0] == '0' && (src[1] == 'x' || src[1] == 'b' || src[1] == 'o');
            if (radix && t->is_signed && e->literal.suffix.empty() && v <= (t->bits == 64 ? UINT64_MAX : (std::uint64_t{1} << t->bits) - 1)) {
                std::string u = "u" + std::to_string(t->bits);
                note(e->span, "for the bit pattern, as Java reads a hex literal, write " +
                                  q(std::string(t->name) + ".wrap(" + src + u + ")"));
            }
            return T.error;
        }
        e->is_const = e->has_bits = true;
        e->const_bits = negative ? 0 - v : v;
        return t;
    }

    std::string range_text(const Type* t) {
        IntRange r = source_range(t);
        if (!t->is_signed) return "0 to " + std::to_string(r.pos);
        return "-" + std::to_string(r.neg) + " to " + std::to_string(r.pos);
    }

    Type* float_literal(Expr* e, Type* expected, bool negative) {
        Type* t = suffix_type(e);
        if (t && is_bad(t)) return t;
        std::string_view digits = e->literal.digits;
        if (digits.find_first_of(".e") == std::string_view::npos) {
            error(e->span, "write a float literal with a fraction: " + q(std::string(digits) + ".0" + std::string(e->literal.suffix)));
            return T.error;
        }
        if (t && !is_float(t)) {
            error(e->span, "a float literal can't have the suffix " + q(e->literal.suffix));
            return T.error;
        }
        if (!t) {
            if (expected && is_float(expected)) {
                t = expected;
            } else if (expected && is_int(expected)) {
                error(e->span, "a float literal isn't " + a_type(expected));
                return T.error;
            } else {
                t = T.f64;
            }
        }
        std::string s(digits);
        double v;
        bool nonzero = s.find_first_of("123456789") < s.find('e');
        if (t == T.f32) {
            float f = std::strtof(s.c_str(), nullptr);
            if (std::isinf(f)) {
                error(e->span, "this literal is too large for f32");
                return T.error;
            }
            if (f == 0.0f && nonzero) {
                error(e->span, "this literal is too small for f32: it would be 0");
                return T.error;
            }
            v = f;
        } else {
            v = std::strtod(s.c_str(), nullptr);
            if (std::isinf(v)) {
                error(e->span, "this literal is too large for f64");
                return T.error;
            }
            if (v == 0.0 && nonzero) {
                error(e->span, "this literal is too small for f64: it would be 0");
                return T.error;
            }
        }
        if (negative) v = -v;
        e->is_const = e->has_bits = true;
        std::memcpy(&e->const_bits, &v, sizeof v);
        return t;
    }

    Type* char_literal(Expr* e, Type* expected) {
        Type* t = expected && expected->kind == TypeKind::char_ ? T.char_ : T.rune;
        if (expected && !is_bad(expected) && expected->kind != TypeKind::char_ && expected->kind != TypeKind::rune) {
            if (is_int(expected)) {
                error(e->span, "a character literal isn't " + a_type(expected) + "; convert it with " +
                                   q(std::string(expected->name) + "(...)"));
                return T.error;
            }
        }
        if (t == T.char_ && e->literal.int_value > 0xFFFF) {
            error(e->span, "this character doesn't fit in a char, which is one UTF-16 code unit; use a rune");
            return T.error;
        }
        e->is_const = e->has_bits = true;
        e->const_bits = e->literal.int_value;
        return t;
    }

    Type* string_literal(Expr* e, bool allow_interpolation) {
        if (e->literal.name) {
            not_yet(e->span, "string prefixes");
            return T.error;
        }
        if (!e->interpolations.empty() && !allow_interpolation) {
            error(e->span, "interpolated strings need `String`, which isn't implemented yet; they work in "
                           "`print`, `println`, `panic` and `assert` for now");
            return T.error;
        }
        for (Expr* part : e->interpolations) {
            Type* t = check(part, nullptr);
            if (is_bad(t)) continue;
            if (!(is_numeric(t) || t->kind == TypeKind::bool_ || t->kind == TypeKind::char_ ||
                  t->kind == TypeKind::rune || t->kind == TypeKind::str)) {
                error(part->span, name_of(t) + " can't be shown in a string yet");
            }
        }
        e->is_const = e->interpolations.empty();
        return T.str;
    }

    // --- names -----------------------------------------------------------------------------

    enum class NameKind { none, local, item, type, builtin };

    struct Resolved {
        NameKind kind = NameKind::none;
        Local* local = nullptr;
        Item* item = nullptr;
        Type* type = nullptr;
        Builtin builtin = Builtin::none;
    };

    Resolved resolve_name(Name n) {
        Resolved r;
        if (Local* l = find_local(n)) {
            r.kind = NameKind::local;
            r.local = l;
            return r;
        }
        if (auto it = items_.find(n); it != items_.end()) {
            if (it->second->kind == ItemKind::struct_) {
                r.kind = NameKind::type;
                r.type = it->second->declared;
                r.item = it->second;
            } else {
                r.kind = NameKind::item;
                r.item = it->second;
            }
            return r;
        }
        if (n == n_Self_ && current_impl_type_) {
            r.kind = NameKind::type;
            r.type = current_impl_type_;
            r.item = current_impl_type_->item;
            return r;
        }
        if (Type* t = T.primitive(text(n))) {
            r.kind = NameKind::type;
            r.type = t;
            return r;
        }
        std::string_view s = text(n);
        r.builtin = s == "print" ? Builtin::print
                    : s == "println" ? Builtin::println
                    : s == "panic" ? Builtin::panic
                    : s == "assert" ? Builtin::assert_
                                     : Builtin::none;
        if (r.builtin != Builtin::none) r.kind = NameKind::builtin;
        return r;
    }

    void use_item(Item* it) {
        if (fn_ != it && current_item_ != it) it->used = true;
    }

    Type* name_value(Expr* e) {
        Name n = e->kind == ExprKind::self ? n_self_ : e->path.parts[0];
        if (n == n_underscore_) {
            error(e->span, "`_` can only be assigned to, to discard a value: `_ = ...`");
            return T.error;
        }
        Resolved r = resolve_name(n);
        switch (r.kind) {
            case NameKind::local:
                e->ref = Ref::local;
                e->local = r.local;
                r.local->used = true;
                return r.local->type;
            case NameKind::item:
                use_item(r.item);
                if (r.item->kind == ItemKind::const_) {
                    check_const(r.item);
                    e->ref = Ref::const_;
                    e->item = r.item;
                    e->is_const = true;
                    if (r.item->value && r.item->value->has_bits) {
                        e->has_bits = true;
                        e->const_bits = r.item->value->const_bits;
                        e->is_nan_const = r.item->value->is_nan_const;
                    }
                    return r.item->declared;
                }
                error(e->span, "function " + q(text(n)) + " can't be used as a value yet; call it");
                return T.error;
            case NameKind::type:
                error(e->span, q(text(n)) + " is a type, not a value");
                return T.error;
            case NameKind::builtin:
                error(e->span, q(text(n)) + " can only be called");
                return T.error;
            case NameKind::none:
                if (e->kind == ExprKind::self) error(e->span, "`self` only exists in a method");
                else error(e->span, "unknown name " + q(text(n)));
                return T.error;
        }
        return T.error;
    }

    // --- operators ---------------------------------------------------------------------------

    static bool is_bitwise(Op op) { return op == Op::bit_and || op == Op::bit_or || op == Op::bit_xor; }
    static bool is_additive(Op op) { return op == Op::add || op == Op::sub; }

    // D63: mixtures whose precedence surprises people need parentheses.
    void check_mixing(Expr* e) {
        for (Expr* child : {e->lhs, e->rhs}) {
            if (!child || child->kind != ExprKind::binary || child->parens) continue;
            Op a = e->op, b = child->op;
            const char* msg = nullptr;
            if ((a == Op::log_or && b == Op::log_and) || (a == Op::log_and && b == Op::log_or)) {
                msg = "`and` and `or` together need parentheses to show which goes first";
            } else if (is_bitwise(a) && is_bitwise(b) && a != b) {
                msg = "`&`, `|` and `^` together need parentheses to show which goes first";
            } else if ((is_shift(a) && is_additive(b)) || (is_additive(a) && is_shift(b))) {
                msg = "a shift next to `+` or `-` needs parentheses to show which goes first";
            } else if ((a == Op::coalesce) != (b == Op::coalesce)) {
                msg = "`??` next to another operator needs parentheses to show which goes first";
            }
            if (msg) {
                error(child->span, msg);
                return;
            }
        }
    }

    Type* unary(Expr* e, Type* expected) {
        Expr* x = e->lhs;
        switch (e->op) {
            case Op::neg: {
                // `-128i8` is one literal, so it can reach the type's minimum.
                if (x->kind == ExprKind::int_lit) {
                    Type* t = int_literal(x, expected, true);
                    x->type = t;
                    copy_const(e, x);
                    return t;
                }
                if (x->kind == ExprKind::float_lit) {
                    Type* t = float_literal(x, expected, true);
                    x->type = t;
                    copy_const(e, x);
                    return t;
                }
                Type* t = check(x, expected && is_numeric(expected) ? expected : nullptr);
                if (is_bad(t)) return t;
                if (is_int(t) && !t->is_signed) {
                    error(e->span, "an unsigned " + name_of(t) + " can't be negated");
                    return T.error;
                }
                if (!is_numeric(t)) {
                    error(e->span, "`-` needs a number, not " + name_of(t));
                    return T.error;
                }
                if (x->has_bits && is_int(t)) {
                    auto v = static_cast<std::int64_t>(x->const_bits);
                    if (v == min_of(t)) {
                        error(e->span, "the constant overflows " + name_of(t) + "; use `wrapping_sub` from 0 if that's intended");
                        return T.error;
                    }
                    set_int(e, t, 0 - x->const_bits);
                } else if (x->is_const) {
                    e->is_const = true;
                    if (x->has_bits && is_float(t)) {
                        double d;
                        std::memcpy(&d, &x->const_bits, sizeof d);
                        d = -d;
                        e->has_bits = true;
                        std::memcpy(&e->const_bits, &d, sizeof d);
                    }
                }
                return t;
            }
            case Op::bit_not: {
                Type* t = check(x, expected && is_int(expected) ? expected : nullptr);
                if (is_bad(t)) return t;
                if (t->kind == TypeKind::bool_) {
                    error(e->span, "`~` is for integers; negate a bool with `not`");
                    return T.error;
                }
                if (!is_int(t)) {
                    error(e->span, "`~` needs an integer, not " + name_of(t));
                    return T.error;
                }
                if (x->has_bits) set_int(e, t, ~x->const_bits);
                return t;
            }
            case Op::log_not: {
                Type* t = check(x, T.bool_);
                if (!is_bad(t) && x->has_bits) set_bool(e, x->const_bits == 0);
                return T.bool_;
            }
            default: break;
        }
        return T.error;
    }

    void copy_const(Expr* to, const Expr* from) {
        to->is_const = from->is_const;
        to->has_bits = from->has_bits;
        to->const_bits = from->const_bits;
    }

    static std::int64_t min_of(const Type* t) {
        return t->bits == 64 ? INT64_MIN : -(std::int64_t{1} << (t->bits - 1));
    }

    // Stores an integer constant, wrapped to the type's width.
    void set_int(Expr* e, const Type* t, std::uint64_t bits) {
        e->is_const = e->has_bits = true;
        if (t->bits < 64) {
            std::uint64_t mask = (std::uint64_t{1} << t->bits) - 1;
            bits &= mask;
            if (t->is_signed && (bits >> (t->bits - 1)) & 1) bits |= ~mask;
        }
        e->const_bits = bits;
    }

    void set_bool(Expr* e, bool v) {
        e->is_const = e->has_bits = true;
        e->const_bits = v;
    }

    bool fits(const Type* t, std::int64_t v) const {
        IntRange r = source_range(t);
        if (v < 0) return static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(v) <= r.neg;
        return static_cast<std::uint64_t>(v) <= r.pos;
    }

    Type* binary(Expr* e, Type* expected) {
        check_mixing(e);
        Op op = e->op;
        if (op == Op::log_and || op == Op::log_or) {
            check(e->lhs, T.bool_);
            check(e->rhs, T.bool_);
            if (e->lhs->has_bits && e->rhs->has_bits) {
                bool a = e->lhs->const_bits, b = e->rhs->const_bits;
                set_bool(e, op == Op::log_and ? (a && b) : (a || b));
            }
            return T.bool_;
        }
        if (op == Op::coalesce) {
            not_yet(e->span, "optionals");
            return T.error;
        }
        bool compare = op == Op::eq || op == Op::ne || op == Op::lt || op == Op::le || op == Op::gt || op == Op::ge;
        if (compare) {
            Type* t = check_pair(e->lhs, e->rhs, nullptr);
            if (is_bad(t)) return T.bool_;
            bool ordered = op != Op::eq && op != Op::ne;
            bool ok = is_numeric(t) || t->kind == TypeKind::char_ || t->kind == TypeKind::rune ||
                      (!ordered && t->kind == TypeKind::bool_);
            if (!ok) {
                error(e->span, "values of type " + name_of(t) + " can't be compared with " + q(op_text(op)) + " yet");
                return T.bool_;
            }
            if (e->lhs->is_nan_const || e->rhs->is_nan_const) {
                error(e->span, std::string("comparing with NaN is always ") + (op == Op::ne ? "true" : "false") +
                                   "; use `.is_nan()`");
                return T.bool_;
            }
            if (e->lhs->has_bits && e->rhs->has_bits && !is_float(t)) fold_compare(e, t);
            else if (e->lhs->is_const && e->rhs->is_const) e->is_const = true;
            return T.bool_;
        }
        // Arithmetic, bitwise and shifts.
        Type* want = expected && is_numeric(expected) ? expected : nullptr;
        Type* lt;
        Type* rt;
        if (is_shift(op)) {
            lt = check(e->lhs, want);
            rt = check(e->rhs, nullptr);
        } else {
            lt = check_pair(e->lhs, e->rhs, want);
            rt = lt;
        }
        if (is_bad(lt) || is_bad(rt)) return T.error;
        return check_operands(op, e->span, lt, rt, e->rhs, e);
    }

    // The rules for `a op b` where `a` is a `lt` and `b` a `rt`, shared with compound assignment.
    // With `e`, also folds constants. Returns the result type.
    Type* check_operands(Op op, Span span, Type* lt, Type* rt, Expr* rhs, Expr* e = nullptr) {
        if (is_bad(lt) || is_bad(rt)) return T.error;
        std::string o = q(op_text(op));
        if (is_shift(op)) {
            if (!(lt == T.i32 || lt == T.i64 || lt == T.u32 || lt == T.u64)) {
                if (is_int(lt) && !lt->is_usize) {
                    error(span, "shifts work on i32, i64, u32 and u64; convert the " + name_of(lt) + " first, as Java does");
                } else {
                    error(span, o + " needs i32, i64, u32 or u64 on the left, not " + name_of(lt));
                }
                return T.error;
            }
            if (!is_int(rt)) {
                error(rhs->span, "a shift count must be an integer, not " + name_of(rt));
                return T.error;
            }
            if (op == Op::ushr && !lt->is_signed) {
                error(span, "`>>>` is the same as `>>` on an unsigned type; write `>>`");
                return T.error;
            }
            if (rhs->has_bits) {
                auto n = static_cast<std::int64_t>(rhs->const_bits);
                bool negative = rt->is_signed && n < 0;
                if (negative || rhs->const_bits >= lt->bits) {
                    error(rhs->span, "a constant shift count must be from 0 to " + std::to_string(lt->bits - 1) +
                                         " for " + name_of(lt) + ", since Java would mask it silently");
                    return T.error;
                }
                if (e && e->lhs->has_bits) fold_shift(e, lt, op, static_cast<unsigned>(rhs->const_bits));
            }
            return lt;
        }
        if (lt != rt) {
            error(span, o + " needs both sides to have the same type, but they're " + name_of(lt) + " and " + name_of(rt));
            return T.error;
        }
        if (is_bitwise(op)) {
            if (lt->kind == TypeKind::bool_) {
                error(span, std::string(o) + " is for integers; for bools use " +
                                (op == Op::bit_and ? "`and`" : op == Op::bit_or ? "`or`" : "`!=`"));
                return T.error;
            }
            if (!is_int(lt)) {
                error(span, o + " needs integers, not " + name_of(lt));
                return T.error;
            }
        } else if (!is_numeric(lt)) {
            error(span, o + " needs numbers, not " + name_of(lt));
            return T.error;
        }
        if ((op == Op::div || op == Op::rem) && is_int(lt) && rhs->has_bits && rhs->const_bits == 0) {
            error(rhs->span, "division by zero");
            return T.error;
        }
        if (e && e->lhs->has_bits && e->rhs->has_bits && is_int(lt)) {
            fold_int(e, lt, op);
        } else if (e && e->lhs->is_const && e->rhs->is_const) {
            e->is_const = true;
        }
        return lt;
    }

    void fold_int(Expr* e, Type* t, Op op) {
        std::uint64_t a = e->lhs->const_bits, b = e->rhs->const_bits;
        bool overflow = false;
        std::uint64_t r = 0;
        if (t->is_signed) {
            auto x = static_cast<std::int64_t>(a), y = static_cast<std::int64_t>(b);
            std::int64_t v = 0;
            switch (op) {
                case Op::add: overflow = __builtin_add_overflow(x, y, &v); break;
                case Op::sub: overflow = __builtin_sub_overflow(x, y, &v); break;
                case Op::mul: overflow = __builtin_mul_overflow(x, y, &v); break;
                case Op::div:
                    if (x == min_of(t) && y == -1) overflow = true;
                    else v = x / y;
                    break;
                case Op::rem: v = (y == -1) ? 0 : x % y; break;
                case Op::bit_and: v = x & y; break;
                case Op::bit_or: v = x | y; break;
                case Op::bit_xor: v = x ^ y; break;
                default: return;
            }
            if (!overflow && !fits(t, v)) overflow = true;
            r = static_cast<std::uint64_t>(v);
        } else {
            switch (op) {
                case Op::add: overflow = __builtin_add_overflow(a, b, &r); break;
                case Op::sub: overflow = __builtin_sub_overflow(a, b, &r); break;
                case Op::mul: overflow = __builtin_mul_overflow(a, b, &r); break;
                case Op::div: r = a / b; break;
                case Op::rem: r = a % b; break;
                case Op::bit_and: r = a & b; break;
                case Op::bit_or: r = a | b; break;
                case Op::bit_xor: r = a ^ b; break;
                default: return;
            }
            if (!overflow && r > source_range(t).pos) overflow = true;
        }
        if (overflow) {
            if (op == Op::div) {
                error(e->span, "this constant division overflows " + name_of(t) + ": it's MIN / -1");
            } else {
                error(e->span, "this constant expression overflows " + name_of(t) + "; use `wrapping_" +
                                   (op == Op::add ? "add" : op == Op::sub ? "sub" : "mul") + "` if wrapping is intended");
            }
            e->is_const = true;
            return;
        }
        set_int(e, t, r);
    }

    void fold_shift(Expr* e, Type* t, Op op, unsigned n) {
        std::uint64_t a = e->lhs->const_bits;
        std::uint64_t mask = t->bits == 64 ? UINT64_MAX : (std::uint64_t{1} << t->bits) - 1;
        std::uint64_t r;
        if (op == Op::shl) r = a << n;
        else if (op == Op::ushr || !t->is_signed) r = (a & mask) >> n;
        else r = static_cast<std::uint64_t>(static_cast<std::int64_t>(a) >> n);
        set_int(e, t, r);
    }

    void fold_compare(Expr* e, Type* t) {
        std::uint64_t a = e->lhs->const_bits, b = e->rhs->const_bits;
        int c;
        if (t->is_signed) {
            auto x = static_cast<std::int64_t>(a), y = static_cast<std::int64_t>(b);
            c = x < y ? -1 : x > y;
        } else {
            c = a < b ? -1 : a > b;
        }
        bool r = false;
        switch (e->op) {
            case Op::eq: r = c == 0; break;
            case Op::ne: r = c != 0; break;
            case Op::lt: r = c < 0; break;
            case Op::le: r = c <= 0; break;
            case Op::gt: r = c > 0; break;
            case Op::ge: r = c >= 0; break;
            default: break;
        }
        set_bool(e, r);
    }

    // --- fields and indexes ----------------------------------------------------------------

    Type* field(Expr* e) {
        // `i32.MAX` and other names inside a type.
        if (e->lhs->kind == ExprKind::name) {
            Resolved r = resolve_name(e->lhs->path.parts[0]);
            if (r.kind == NameKind::type) {
                e->lhs->ref = Ref::type;
                e->lhs->named_type = r.type;
                return associated(e, r.type);
            }
        }
        if (e->is_tuple_index) {
            Type* base = check(e->lhs, nullptr);
            if (!is_bad(base)) error(e->span, name_of(base) + " has no numbered fields");
            return T.error;
        }
        Type* base = check(e->lhs, nullptr);
        return field_type(e, base);
    }

    Type* field_type(Expr* e, Type* base) {
        if (is_bad(base)) return T.error;
        if (base->kind == TypeKind::struct_) {
            Item* s = base->item;
            for (std::uint32_t i = 0; i < s->fields.size(); i++) {
                if (s->fields[i].name == e->member) {
                    e->field_index = i;
                    return s->fields[i].resolved;
                }
            }
            auto mt = methods_.find(base);
            if (mt != methods_.end() && mt->second.count(e->member)) {
                error(e->span, q(text(e->member)) + " is a method; call it: " + q(std::string(text(e->member)) + "()"));
                return T.error;
            }
            error(e->span, name_of(base) + " has no field " + q(text(e->member)));
            return T.error;
        }
        error(e->span, name_of(base) + " has no fields");
        return T.error;
    }

    Type* associated(Expr* e, Type* t) {
        std::string_view m = text(e->member);
        if (is_int(t) && (m == "MIN" || m == "MAX")) {
            IntRange r = source_range(t);
            if (t->is_usize && m == "MAX") {
                error(e->span, "usize.MAX depends on the target; use u32.MAX or u64.MAX");
                return T.error;
            }
            set_int(e, t, m == "MIN" ? 0 - r.neg : r.pos);
            return t;
        }
        if (is_float(t)) {
            double v = 0;
            bool found = true;
            bool f32 = t == T.f32;
            if (m == "NAN") v = std::nan("");
            else if (m == "INFINITY") v = HUGE_VAL;
            else if (m == "NEG_INFINITY") v = -HUGE_VAL;
            else if (m == "MAX") v = f32 ? 3.4028234663852886e38 : 1.7976931348623157e308;
            else if (m == "MIN_POSITIVE") v = f32 ? 1.1754943508222875e-38 : 2.2250738585072014e-308;
            else found = false;
            if (found) {
                e->is_const = e->has_bits = true;
                std::memcpy(&e->const_bits, &v, sizeof v);
                e->is_nan_const = m == "NAN";
                return t;
            }
        }
        if (t->kind == TypeKind::struct_) {
            auto mt = methods_.find(t);
            if (mt != methods_.end() && mt->second.count(e->member)) {
                error(e->span, q(std::string(t->name) + "." + std::string(m)) + " is a function; call it");
                return T.error;
            }
        }
        error(e->span, name_of(t) + " has nothing named " + q(m));
        return T.error;
    }

    Type* index_type(Expr* e, Type* base) {
        Type* it = check(e->rhs, T.usize);
        if (is_bad(base)) return T.error;
        if (base->kind != TypeKind::array) {
            error(e->lhs->span, name_of(base) + " can't be indexed");
            return T.error;
        }
        if (!is_bad(it) && e->rhs->has_bits && e->rhs->const_bits >= base->length) {
            error(e->rhs->span, "index " + std::to_string(e->rhs->const_bits) + " is out of bounds for " + name_of(base));
        }
        return base->elem;
    }

    // --- literals of structs and arrays ------------------------------------------------------

    Type* struct_literal(Expr* e) {
        if (e->type_path.parts.size() != 1) {
            error(e->type_path.span, "unknown type " + q(path_text(e->type_path)));
            return T.error;
        }
        Resolved r = resolve_name(e->type_path.parts[0]);
        if (r.kind != NameKind::type || r.type->kind != TypeKind::struct_) {
            error(e->type_path.span, q(path_text(e->type_path)) + " isn't a struct");
            for (FieldInit& f : e->fields) {
                if (f.value) check(f.value, nullptr);
                else if (Local* l = find_local(f.name)) l->used = true;
            }
            return T.error;
        }
        Type* t = r.type;
        Item* s = t->item;
        use_item(s);
        std::vector<Expr*> values(s->fields.size(), nullptr);
        std::vector<bool> given(s->fields.size(), false);
        std::vector<Expr*> order;  // the values in the order they're written, which is the order they run
        for (FieldInit& f : e->fields) {
            std::uint32_t index = UINT32_MAX;
            for (std::uint32_t i = 0; i < s->fields.size(); i++) {
                if (s->fields[i].name == f.name) index = i;
            }
            Expr* v = f.value;
            if (!v) {
                // `Point { x, y }` takes the locals of those names.
                v = ctx_.arena.make<Expr>();
                v->kind = ExprKind::name;
                v->span = f.span;
                std::vector<Name> one{f.name};
                v->path.parts = ctx_.arena.copy(one);
                v->path.span = f.span;
                f.value = v;
            }
            if (index == UINT32_MAX) {
                error(f.span, name_of(t) + " has no field " + q(text(f.name)));
                check(v, nullptr);
                continue;
            }
            if (given[index]) {
                error(f.span, "field " + q(text(f.name)) + " is given twice");
                continue;
            }
            given[index] = true;
            check(v, s->fields[index].resolved);
            values[index] = v;
        }
        for (std::uint32_t i = 0; i < s->fields.size(); i++) {
            if (!given[i] && !s->fields[i].default_value) {
                error(e->span, "missing field " + q(text(s->fields[i].name)) + " in " + name_of(t));
            }
        }
        e->elems = ctx_.arena.copy(values);
        return t;
    }

    Type* array_literal(Expr* e, Type* expected) {
        Type* want = expected && expected->kind == TypeKind::array ? expected->elem : nullptr;
        if (e->repeat) {
            Type* elem = check(e->elems[0], want);
            Type* n = check(e->repeat, T.usize);
            if (is_bad(elem) || is_bad(n)) return T.error;
            if (!e->repeat->has_bits) {
                error(e->repeat->span, "the length of `[x; n]` must be a constant");
                return T.error;
            }
            return T.array_of(elem, e->repeat->const_bits);
        }
        if (e->elems.empty()) {
            if (!want) {
                error(e->span, "can't tell what this empty array holds; give it a type");
                return T.error;
            }
            return T.array_of(want, 0);
        }
        Type* elem = check(e->elems[0], want);
        for (std::size_t i = 1; i < e->elems.size(); i++) check(e->elems[i], is_bad(elem) ? nullptr : elem);
        if (is_bad(elem)) return T.error;
        if (elem->kind == TypeKind::unit || elem->kind == TypeKind::never) {
            error(e->span, "an array can't hold " + name_of(elem));
            return T.error;
        }
        return T.array_of(elem, e->elems.size());
    }

    // --- control flow ------------------------------------------------------------------------

    Type* check_if(Expr* e, Type* expected, bool stmt) {
        if (e->let_pattern) {
            not_yet(e->span, "`if let`");
            return T.error;
        }
        check_condition(e->cond);
        if (e->cond->kind == ExprKind::bool_lit && e->cond->bool_value) {
            error(e->cond->span, "this condition is always true");
        }
        if (stmt) {
            Type* a = check_block(e->block, nullptr, BlockUse::stmt);
            if (!e->else_) return T.unit;
            Type* b = e->else_->kind == ExprKind::if_ ? check_if(e->else_, nullptr, true)
                                                      : check_block(e->else_->block, nullptr, BlockUse::stmt);
            e->else_->type = b;
            return a->kind == TypeKind::never && b->kind == TypeKind::never ? T.never : T.unit;
        }
        if (!e->else_) {
            check_block(e->block, expected, BlockUse::value);
            error(e->span, "an `if` used as a value needs an `else`");
            return T.error;
        }
        Type* a = check_block(e->block, expected, BlockUse::value);
        Type* want = expected ? expected : (a->kind == TypeKind::never || is_bad(a) ? nullptr : a);
        Type* b = check(e->else_, want);
        if (is_bad(a) || is_bad(b)) return T.error;
        if (a->kind == TypeKind::never) return b;
        return a;
    }

    Type* check_loop(Expr* e, Type* expected, bool stmt) {
        push_loop(true, stmt, expected);
        e->loop_id = loops_.back().id;
        check_block(e->block, nullptr, BlockUse::stmt);
        LoopCtx loop = loops_.back();
        loops_.pop_back();
        if (!loop.has_break) return T.never;
        if (stmt) return T.unit;
        return loop.break_type ? loop.break_type : T.unit;
    }

    // --- calls -------------------------------------------------------------------------------

    Type* call(Expr* e, Type* expected) {
        Expr* callee = e->callee;
        if (e->trailing) {
            not_yet(e->trailing->span, "closures");
            return T.error;
        }
        if (callee->kind == ExprKind::name) {
            Name n = callee->path.parts[0];
            Resolved r = resolve_name(n);
            switch (r.kind) {
                case NameKind::builtin: return builtin_call(e, r.builtin);
                case NameKind::item:
                    if (r.item->kind != ItemKind::fn) {
                        error(callee->span, q(text(n)) + " isn't a function");
                        return T.error;
                    }
                    use_item(r.item);
                    return fn_call(e, r.item, nullptr);
                case NameKind::type:
                    callee->ref = Ref::type;
                    callee->named_type = r.type;
                    if (r.type->kind == TypeKind::struct_) return construct(e, r.type);
                    return conversion(e, r.type, Conv::exact);
                case NameKind::local:
                    r.local->used = true;
                    error(callee->span, q(text(n)) + " is " + a_type(r.local->type) + ", not a function");
                    return T.error;
                case NameKind::none:
                    error(callee->span, "unknown function " + q(text(n)));
                    for (Arg& a : e->args) check(a.value, nullptr);
                    return T.error;
            }
        }
        if (callee->kind == ExprKind::field && !callee->is_tuple_index) {
            // `T.something(...)`: a conversion or a static function.
            if (callee->lhs->kind == ExprKind::name) {
                Resolved r = resolve_name(callee->lhs->path.parts[0]);
                if (r.kind == NameKind::type) {
                    callee->lhs->ref = Ref::type;
                    callee->lhs->named_type = r.type;
                    return static_call(e, r.type);
                }
            }
            Type* recv = check(callee->lhs, nullptr);
            if (is_bad(recv)) {
                for (Arg& a : e->args) check(a.value, nullptr);
                return T.error;
            }
            if (recv->kind == TypeKind::struct_) {
                auto mt = methods_.find(recv);
                Item* m = nullptr;
                if (mt != methods_.end()) {
                    auto it = mt->second.find(callee->member);
                    if (it != mt->second.end()) m = it->second;
                }
                if (!m) {
                    for (const FieldDecl& f : recv->item->fields) {
                        if (f.name == callee->member) {
                            error(callee->span, q(text(f.name)) + " is a field of " + name_of(recv) + ", not a method");
                            return T.error;
                        }
                    }
                    error(callee->span, name_of(recv) + " has no method " + q(text(callee->member)));
                    return T.error;
                }
                if (!m->is_method) {
                    error(callee->span, q(text(m->name)) + " isn't a method; call it as " +
                                            q(std::string(recv->name) + "." + std::string(text(m->name)) + "(...)"));
                    return T.error;
                }
                use_item(m);
                return fn_call(e, m, callee->lhs);
            }
            return prim_method(e, recv);
        }
        check(callee, nullptr);
        error(callee->span, "this can't be called");
        return T.error;
    }

    Type* static_call(Expr* e, Type* t) {
        std::string_view m = text(e->callee->member);
        if (is_numeric(t) || t->kind == TypeKind::char_ || t->kind == TypeKind::rune) {
            if (m == "wrap") return conversion(e, t, Conv::wrap);
            if (m == "saturate") return conversion(e, t, Conv::saturate);
            if (m == "nearest") return conversion(e, t, Conv::nearest);
            if (m == "try_from") {
                not_yet(e->span, "optionals, which `try_from` returns,");
                return T.error;
            }
        }
        if (t->kind == TypeKind::struct_) {
            auto mt = methods_.find(t);
            if (mt != methods_.end()) {
                auto it = mt->second.find(e->callee->member);
                if (it != mt->second.end()) {
                    Item* f = it->second;
                    if (f->is_method) {
                        error(e->callee->span, q(text(f->name)) + " is a method; call it on a value: " +
                                                   q("x." + std::string(m) + "(...)"));
                        return T.error;
                    }
                    use_item(f);
                    return fn_call(e, f, nullptr);
                }
            }
        }
        error(e->callee->span, name_of(t) + " has no function " + q(m));
        for (Arg& a : e->args) check(a.value, nullptr);
        return T.error;
    }

    // Checks a call's arguments against `f`'s parameters, matching names, and records them in
    // parameter order. `receiver` is the value a method is called on.
    Type* fn_call(Expr* e, Item* f, Expr* receiver) {
        check_signature(f);
        e->call = receiver ? CallKind::method : CallKind::fn;
        e->item = f;
        std::size_t first = receiver ? 1 : 0;
        std::size_t n = f->params.size();
        std::vector<Expr*> ordered(n, nullptr);
        std::vector<bool> is_mut(n, false);
        if (receiver) {
            ordered[0] = receiver;
            const Param& p = f->params[0];
            if (p.mode == ParamMode::mut) {
                Local* root = place_root(receiver);
                if (!root) error(receiver->span, q(text(f->name)) + " changes its receiver, so it must be called on a variable");
                else require_writable(receiver, root, false);
                is_mut[0] = true;
            }
        }
        bool named_seen = false;
        std::size_t next = first;
        bool ok = true;
        for (Arg& a : e->args) {
            std::size_t index;
            if (a.name) {
                named_seen = true;
                index = n;
                for (std::size_t i = first; i < n; i++) {
                    if (f->params[i].name == a.name) index = i;
                }
                if (index == n) {
                    error(a.span, q(text(f->name)) + " has no parameter " + q(text(a.name)));
                    check(a.value, nullptr);
                    ok = false;
                    continue;
                }
            } else {
                if (named_seen) {
                    error(a.span, "unnamed arguments must come before named ones");
                    check(a.value, nullptr);
                    ok = false;
                    continue;
                }
                index = next++;
                if (index >= n) {
                    error(a.span, q(text(f->name)) + " takes " + std::to_string(n - first) + " argument" +
                                      (n - first == 1 ? "" : "s") + ", but more were given");
                    check(a.value, nullptr);
                    ok = false;
                    continue;
                }
            }
            if (ordered[index]) {
                error(a.span, "parameter " + q(text(f->params[index].name)) + " is given twice");
                check(a.value, nullptr);
                ok = false;
                continue;
            }
            const Param& p = f->params[index];
            Type* want = p.local ? p.local->type : T.error;
            check(a.value, want);
            if (p.mode == ParamMode::mut) {
                if (!a.is_mut) {
                    error(a.value->span, q(text(f->name)) + " can change " + q(text(p.name)) + ", so pass it as " +
                                             q("mut ...") + " to show that");
                } else if (Local* root = place_root(a.value)) {
                    require_writable(a.value, root, false);
                } else {
                    error(a.value->span, "only a variable can be passed as `mut`");
                }
                is_mut[index] = true;
            } else if (a.is_mut) {
                error(a.span, q(text(f->name)) + " doesn't change " + q(text(p.name)) + "; remove `mut`");
            }
            ordered[index] = a.value;
        }
        for (std::size_t i = first; i < n && ok; i++) {
            if (!ordered[i]) error(e->span, "missing argument " + q(text(f->params[i].name)) + " for " + q(text(f->name)));
        }
        // D48: a value passed as `mut` can't be passed again in the same call.
        for (std::size_t i = 0; i < n; i++) {
            if (!is_mut[i] || !ordered[i]) continue;
            Local* root = place_root(ordered[i]);
            if (!root) continue;
            for (std::size_t j = 0; j < n; j++) {
                if (j == i || !ordered[j]) continue;
                if (mentions(ordered[j], root)) {
                    error(ordered[j]->span, q(text(root->name)) + " is passed as `mut`, so it can't be used again in the same call");
                    note(ordered[i]->span, "it's passed as `mut` here");
                    break;
                }
            }
        }
        e->ordered_args = ctx_.arena.copy(ordered);
        return f->result_type ? f->result_type : T.error;
    }

    // Whether the expression reads or writes the local anywhere.
    static bool mentions(const Expr* e, const Local* l) {
        if (!e) return false;
        if ((e->kind == ExprKind::name || e->kind == ExprKind::self) && e->local == l) return true;
        if (mentions(e->lhs, l) || mentions(e->rhs, l) || mentions(e->callee, l)) return true;
        for (const Arg& a : e->args) if (mentions(a.value, l)) return true;
        for (const Expr* x : e->elems) if (mentions(x, l)) return true;
        for (const FieldInit& f : e->fields) if (mentions(f.value, l)) return true;
        for (const Expr* x : e->interpolations) if (mentions(x, l)) return true;
        if (mentions(e->repeat, l) || mentions(e->cond, l) || mentions(e->else_, l)) return true;
        if (e->block) return true;  // blocks are rare in arguments; assume they might
        return false;
    }

    Type* construct(Expr* e, Type* t) {
        Item* s = t->item;
        use_item(s);
        e->call = CallKind::construct;
        if (!e->args.empty()) {
            error(e->span, "build " + a_type(t) + " with " + q(std::string(t->name) + " { field: value }") +
                               "; " + q(std::string(t->name) + "()") + " takes no arguments");
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        for (const FieldDecl& f : s->fields) {
            if (!f.default_value) {
                error(e->span, q(std::string(t->name) + "()") + " needs every field to have a default, but " +
                                   q(text(f.name)) + " has none");
                return T.error;
            }
        }
        return t;
    }

    Expr* single_arg(Expr* e, const char* what) {
        if (e->args.size() != 1 || e->args[0].name || e->args[0].is_mut) {
            error(e->span, std::string(what) + " takes one argument");
            for (Arg& a : e->args) check(a.value, nullptr);
            return nullptr;
        }
        return e->args[0].value;
    }

    Type* conversion(Expr* e, Type* to, Conv conv) {
        Expr* a = single_arg(e, "a conversion");
        if (!a) return T.error;
        e->call = CallKind::convert;
        e->conv = conv;
        e->named_type = to;
        Type* from = check(a, nullptr);
        if (is_bad(from)) return T.error;
        std::string target(to->name);
        if (from == to) {
            error(e->span, q(a->kind == ExprKind::name ? std::string(text(a->path.parts[0])) : std::string("this")) +
                               " is already " + a_type(to) + "; remove the conversion");
            return T.error;
        }
        bool convertible = (is_integral(from) || is_float(from)) && (is_integral(to) || is_float(to));
        if (!convertible) {
            if (from->kind == TypeKind::bool_ || to->kind == TypeKind::bool_) {
                error(e->span, "bools don't convert to or from numbers; write `if b { 1 } else { 0 }`");
            } else {
                error(e->span, "there's no conversion from " + name_of(from) + " to " + name_of(to));
            }
            return T.error;
        }
        bool exact = exact_conversion(from, to);
        switch (conv) {
            case Conv::exact:
                if (!exact) {
                    error(e->span, name_of(from) + " to " + name_of(to) + " can lose information; " + lossy_hint(from, to));
                    if (a->kind == ExprKind::int_lit && is_int(to)) {
                        note(a->span, "for a literal, write it with a suffix: " + q(std::string(a->literal.digits) + target));
                    }
                    return T.error;
                }
                break;
            case Conv::wrap:
                if (!is_integral(from) || !(is_int(to) || to->kind == TypeKind::char_)) {
                    error(e->span, q(target + ".wrap") + " converts between integer types; " + lossy_hint(from, to));
                    return T.error;
                }
                if (exact) {
                    error(e->span, name_of(from) + " to " + name_of(to) + " can't lose information; write " + q(target + "(...)"));
                    return T.error;
                }
                break;
            case Conv::saturate:
                if (!(to == T.i32 || to == T.i64)) {
                    error(e->span, "only `i32.saturate` and `i64.saturate` exist, matching Java's casts; Java's "
                                   "`(byte) f` is `i8.wrap(i32.saturate(f))`");
                    return T.error;
                }
                if (!is_float(from)) {
                    error(e->span, q(target + ".saturate") + " converts a float; " +
                                       (exact ? "write " + q(target + "(...)") : lossy_hint(from, to)));
                    return T.error;
                }
                break;
            case Conv::nearest:
                if (!is_float(to)) {
                    error(e->span, q(target + ".nearest") + " doesn't exist; it rounds to a float type");
                    return T.error;
                }
                if (exact) {
                    error(e->span, name_of(from) + " to " + name_of(to) + " is exact; write " + q(target + "(...)"));
                    return T.error;
                }
                break;
            case Conv::none: break;
        }
        if (a->has_bits && is_integral(from) && is_integral(to)) {
            set_int(e, to, a->const_bits);
        } else if (a->is_const) {
            e->is_const = true;
        }
        return to;
    }

    Type* builtin_call(Expr* e, Builtin b) {
        e->call = CallKind::builtin;
        e->builtin = b;
        for (Arg& a : e->args) {
            if (a.name || a.is_mut) error(a.span, "this argument can't be named or `mut`");
        }
        auto message = [&](Expr* m) {
            if (m->kind == ExprKind::string_lit) {
                m->type = string_literal(m, true);
            } else {
                check(m, T.str);
            }
        };
        switch (b) {
            case Builtin::print:
            case Builtin::println:
                if (e->args.size() > 1 || (b == Builtin::print && e->args.empty())) {
                    error(e->span, std::string(b == Builtin::print ? "`print`" : "`println`") + " takes " +
                                       (b == Builtin::print ? "one string" : "one string, or nothing"));
                    return T.unit;
                }
                if (!e->args.empty()) message(e->args[0].value);
                return T.unit;
            case Builtin::panic:
                if (e->args.size() != 1) {
                    error(e->span, "`panic` takes one string: the message");
                    return T.never;
                }
                message(e->args[0].value);
                return T.never;
            case Builtin::assert_:
                if (e->args.empty() || e->args.size() > 2) {
                    error(e->span, "`assert` takes a condition, and optionally a message");
                    return T.unit;
                }
                check(e->args[0].value, T.bool_);
                if (e->args[0].value->kind == ExprKind::bool_lit) {
                    error(e->args[0].value->span, e->args[0].value->bool_value ? "this assertion always holds"
                                                                               : "write `panic(...)` for an assertion that always fails");
                }
                if (e->args.size() == 2) message(e->args[1].value);
                return T.unit;
            case Builtin::none: break;
        }
        return T.error;
    }

    Type* prim_method(Expr* e, Type* recv) {
        std::string_view m = text(e->callee->member);
        e->call = CallKind::prim_method;
        PrimMethod pm = PrimMethod::none;
        int args = 0;
        bool num = is_numeric(recv);
        if (m == "abs" && num) pm = PrimMethod::abs;
        else if (m == "min" && num) pm = PrimMethod::min, args = 1;
        else if (m == "max" && num) pm = PrimMethod::max, args = 1;
        else if (m == "is_nan" && is_float(recv)) pm = PrimMethod::is_nan;
        else if (m == "sqrt" && is_float(recv)) pm = PrimMethod::sqrt;
        else if (m == "floor" && is_float(recv)) pm = PrimMethod::floor;
        else if (m == "ceil" && is_float(recv)) pm = PrimMethod::ceil;
        else if (m == "wrapping_add" && is_int(recv)) pm = PrimMethod::wrapping_add, args = 1;
        else if (m == "wrapping_sub" && is_int(recv)) pm = PrimMethod::wrapping_sub, args = 1;
        else if (m == "wrapping_mul" && is_int(recv)) pm = PrimMethod::wrapping_mul, args = 1;
        else if (m == "floor_div" && is_int(recv) && recv->is_signed) pm = PrimMethod::floor_div, args = 1;
        else if (m == "floor_mod" && is_int(recv) && recv->is_signed) pm = PrimMethod::floor_mod, args = 1;
        if (pm == PrimMethod::none) {
            error(e->callee->span, name_of(recv) + " has no method " + q(m));
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        if (pm == PrimMethod::abs && is_int(recv) && !recv->is_signed) {
            error(e->callee->span, "an unsigned " + name_of(recv) + " is never negative; remove `.abs()`");
            return T.error;
        }
        e->prim = pm;
        if (static_cast<int>(e->args.size()) != args) {
            error(e->span, q(m) + " takes " + (args ? "one argument" : "no arguments"));
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        for (Arg& a : e->args) {
            if (a.name || a.is_mut) error(a.span, "this argument can't be named or `mut`");
            check(a.value, recv);
        }
        if (pm == PrimMethod::is_nan) return T.bool_;
        Expr* lhs = e->callee->lhs;
        if (args == 1 && lhs->has_bits && e->args[0].value->has_bits && is_int(recv)) {
            if (pm == PrimMethod::wrapping_add) set_int(e, recv, lhs->const_bits + e->args[0].value->const_bits);
            else if (pm == PrimMethod::wrapping_sub) set_int(e, recv, lhs->const_bits - e->args[0].value->const_bits);
            else if (pm == PrimMethod::wrapping_mul) set_int(e, recv, lhs->const_bits * e->args[0].value->const_bits);
        }
        return recv;
    }

    // --- after the bodies ----------------------------------------------------------------------

    // Puts structs and arrays in an order where each comes after the types it holds by value,
    // and rejects a struct that holds itself.
    void order_types() {
        std::unordered_map<Type*, int> state;  // 1 visiting, 2 done
        std::vector<Type*> stack;
        auto visit = [&](auto& self, Type* t) -> void {
            if (t->kind != TypeKind::struct_ && t->kind != TypeKind::array) return;
            int& st = state[t];
            if (st == 2) return;
            if (st == 1) {
                if (t->kind == TypeKind::struct_) {
                    error(t->item->name_span, name_of(t) + " holds itself, so it would have no end");
                }
                return;
            }
            st = 1;
            if (t->kind == TypeKind::array) {
                self(self, t->elem);
            } else {
                for (const FieldDecl& f : t->item->fields) {
                    if (f.resolved) self(self, f.resolved);
                }
            }
            state[t] = 2;
            prog_.type_order.push_back(t);
        };
        for (Item* s : structs_) visit(visit, s->declared);
        for (Type* a : T.arrays()) visit(visit, a);
    }

    void check_unused_items() {
        for (Item* it : module_->items) {
            if (it->kind == ItemKind::impl) {
                for (Item* m : it->members) {
                    if (m->kind == ItemKind::fn && !m->is_pub && !m->used && m->impl_type) {
                        error(m->name_span, "function " + q(text(m->name)) + " is never used");
                    }
                }
                continue;
            }
            if (it->is_pub || it->used) continue;
            if (it->kind == ItemKind::fn && text(it->name) == "main") continue;
            if (it->kind == ItemKind::fn || it->kind == ItemKind::struct_ || it->kind == ItemKind::const_) {
                if (it->kind == ItemKind::fn && !it->body) continue;
                error(it->name_span, std::string(item_kind(it)) + " " + q(text(it->name)) + " is never used");
            }
        }
    }

    void find_main(Module* m) {
        auto it = items_.find(ctx_.names.intern("main"));
        if (it == items_.end() || it->second->kind != ItemKind::fn) {
            error(Span{m->file, 0, 0}, "a program needs a `fn main()`");
            return;
        }
        Item* f = it->second;
        if (!f->params.empty() || (f->result_type && f->result_type->kind != TypeKind::unit)) {
            error(f->name_span, "`main` takes no parameters and returns nothing");
        }
        prog_.main = f;
    }

    CheckContext ctx_;
    Types& T;
    Program& prog_;
    Module* module_ = nullptr;
    Name n_self_, n_underscore_, n_Self_;
    std::vector<Name> builtin_names_, reserved_names_;
    std::unordered_map<Name, Item*> items_;
    std::vector<Item*> structs_, consts_, impls_, all_fns_;
    std::unordered_map<Type*, std::unordered_map<Name, Item*>> methods_;
    std::unordered_map<Item*, bool> checking_consts_;
    Item* current_item_ = nullptr;
    Type* current_impl_type_ = nullptr;
    Item* fn_ = nullptr;
    std::vector<Local*> visible_;
    std::vector<std::size_t> scope_marks_;
    std::vector<LoopCtx> loops_;
    std::uint32_t next_loop_ = 1;
};

}  // namespace

void check_module(CheckContext ctx, Module* module, Program& program, bool need_main) {
    Checker c(ctx, program);
    c.run(module, need_main);
}

}  // namespace calx
