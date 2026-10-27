#include "check.h"
#include "moves.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
        for (const char* b : {"some", "ok", "err", "List", "Copy"}) reserved_names_.push_back(ctx.names.intern(b));
        n_List_ = ctx.names.intern("List");
        n_Copy_ = ctx.names.intern("Copy");
        n_copy_ = ctx.names.intern("copy");
    }

    void run(const std::vector<Module*>& modules, bool need_main) {
        prog_.modules = modules;
        for (Module* m : modules) {
            auto scope = std::make_unique<Scope>();
            scope->module = m;
            scope_ = scope.get();
            scopes_.emplace(m, std::move(scope));
            set_prefix(m);
            collect(m);
        }
        for (Module* m : modules) resolve_imports(m);
        resolve_params();
        // Bounds are checked once every impl is known, since a type's fields can need one.
        defer_bounds_ = true;
        for (Item* it : structs_) resolve_fields(it);
        for (Item* it : enums_) resolve_enum(it);
        for (Item* it : interfaces_) collect_interface(it);
        for (Item* it : impls_) collect_impl(it);
        check_one_name();
        defer_bounds_ = false;
        for (const DeferredBounds& d : deferred_bounds_) check_bounds(d.params, d.args, d.span, d.what);
        // Defaults come after the enums, since a default can be a variant, whose tag the enum sets.
        for (Item* it : structs_) check_defaults(it);
        defaults_ready_ = true;
        for (Type* t : pending_defaults_) check_instance_defaults(t);
        for (Item* f : all_fns_) check_signature(f);
        for (Item* c : consts_) check_const(c);
        for (Item* f : all_fns_) {
            std::size_t before = ctx_.diags.error_count();
            check_fn(f);
            if (!f->type_params.empty() && ctx_.diags.error_count() != before) failed_generics_.insert(f);
        }
        // Each use of a generic function with real types makes an instance, checked here, which
        // can need more instances. A definition with errors has had them reported once, so its
        // instances aren't checked again.
        instance_mode_ = true;
        for (std::size_t i = 0; i < pending_fns_.size(); i++) {
            Item* f = pending_fns_[i];
            if (failed_generics_.count(f->origin)) continue;
            // Copied now, after its definition was checked, so the copy keeps what that check
            // worked out, such as which interface each call through a bound keeps to.
            f->body = clone(f->origin->body);
            check_fn(f);
        }
        instance_mode_ = false;
        order_types();
        check_unused_items();
        // Ownership follows the types, so it waits for a program without type errors, as Rust's
        // borrow checker does. An instance's errors were its definition's (D76).
        if (ctx_.diags.error_count() == 0) {
            MoveContext mc{ctx_.names, ctx_.arena, ctx_.diags};
            for (Item* f : all_fns_) check_moves(mc, f, true);
            for (Item* f : pending_fns_) check_moves(mc, f, false);
        }
        if (need_main) find_main(modules.back());
    }

private:
    // What each generic parameter stands for, in one use.
    using Subst = std::unordered_map<Type*, Type*>;

    // A module's names: what it declares, and what it imports (D73).
    struct Scope {
        Module* module = nullptr;
        std::unordered_map<Name, Item*> items;
        struct Import {
            Item* item;
            Span span;
            bool used = false;
        };
        std::unordered_map<Name, Import> imports;
    };

    // Work on an item happens among its own module's names. Asked for from another module, say
    // to resolve an imported struct's fields in the middle of a function, it also sets aside that
    // function's locals and generic parameters, which mean nothing where the item is.
    class InModule {
    public:
        InModule(Checker& c, const Item* it) : c_(c) {
            if (!it || !it->module) return;
            Scope* s = c.scopes_.at(it->module).get();
            if (s == c.scope_) return;
            switched_ = true;
            scope_ = c.scope_;
            c.scope_ = s;
            env_.swap(c.env_);
            visible_.swap(c.visible_);
            marks_.swap(c.scope_marks_);
            loops_.swap(c.loops_);
            fn_ = c.fn_;
            item_ = c.current_item_;
            impl_ = c.current_impl_type_;
            c.fn_ = nullptr;
            c.current_item_ = nullptr;
            c.current_impl_type_ = nullptr;
        }
        ~InModule() {
            if (!switched_) return;
            c_.scope_ = scope_;
            c_.env_.swap(env_);
            c_.visible_.swap(visible_);
            c_.scope_marks_.swap(marks_);
            c_.loops_.swap(loops_);
            c_.fn_ = fn_;
            c_.current_item_ = item_;
            c_.current_impl_type_ = impl_;
        }
        InModule(const InModule&) = delete;
        InModule& operator=(const InModule&) = delete;

    private:
        Checker& c_;
        bool switched_ = false;
        Scope* scope_ = nullptr;
        std::vector<std::pair<Name, Type*>> env_;
        std::vector<Local*> visible_;
        std::vector<std::size_t> marks_;
        std::vector<LoopCtx> loops_;
        Item* fn_ = nullptr;
        Item* item_ = nullptr;
        Type* impl_ = nullptr;
    };

    // The item `n` names in the module being worked on: one of its own, or one it imports.
    Item* find_item(Name n) {
        if (auto it = scope_->items.find(n); it != scope_->items.end()) return it->second;
        if (auto it = scope_->imports.find(n); it != scope_->imports.end()) {
            it->second.used = true;
            return it->second.item;
        }
        return nullptr;
    }

    std::string module_name(const Module* m) const {
        std::string s;
        for (std::size_t i = 0; i < m->name.parts.size(); i++) {
            if (i) s += '.';
            s += text(m->name.parts[i]);
        }
        return s;
    }

    // Whether code in the module being worked on can see `it`: it's `pub`, or it's here.
    bool visible(const Item* it) const { return it->is_pub || it->module == scope_->module; }

    bool field_visible(const Item* s, const FieldDecl& f) const { return f.is_pub || s->module == scope_->module; }

    void private_field(Span at, const Item* s, const FieldDecl& f) {
        error(at, "field " + q(text(f.name)) + " of " + q(text(s->name)) + " is private to " + q(module_name(s->module)));
    }

    void private_member(Span at, const Item* f) {
        error(at, q(text(f->name)) + " is private to " + q(module_name(f->module)));
        note(f->name_span, "it's declared here");
    }

    // Whether other modules can call `f`, so that its signature must show only `pub` types.
    static bool exported(const Item* f) {
        if (f->origin) return false;
        if (f->interface_item && f->impl_type && f->impl_type->kind == TypeKind::param) return f->interface_item->is_pub;
        if (!f->is_pub) return false;
        if (!f->impl_type) return true;
        const Item* owner = f->impl_type->generic ? f->impl_type->generic : f->impl_type->item;
        return !owner || owner->is_pub;
    }

    // The first type `t` mentions that isn't `pub`, which a `pub` signature or field can't show.
    static const Item* private_part(const Type* t) {
        switch (t->kind) {
            case TypeKind::struct_:
            case TypeKind::enum_: {
                const Item* it = t->generic ? t->generic : t->item;
                if (it && !it->is_pub) return it;
                for (const Type* a : t->args) if (const Item* p = private_part(a)) return p;
                return nullptr;
            }
            case TypeKind::optional:
            case TypeKind::array:
            case TypeKind::list: return private_part(t->elem);
            case TypeKind::tuple:
                for (const Type* e : t->elems) if (const Item* p = private_part(e)) return p;
                return nullptr;
            default: return nullptr;
        }
    }

    void public_type(const Type* t, Span at, const char* why) {
        if (const Item* it = private_part(t)) error(at, q(text(it->name)) + " isn't `pub`, so " + why);
    }

    // `import a.b.{X, Y}` brings `pub` items of `a.b` in by name.
    void resolve_imports(Module* m) {
        Scope* here = scopes_.at(m).get();
        for (Item* it : m->items) {
            if (it->kind != ItemKind::import || !it->imported) continue;
            Scope* from = scopes_.at(it->imported).get();
            std::string from_name = module_name(it->imported);
            if (it->import_names.empty()) {
                error(it->import_path.span, "say what to import from " + q(from_name) + ", as in " +
                                                q("import " + from_name + ".{Name}"));
                continue;
            }
            for (const ImportName& n : it->import_names) {
                auto found = from->items.find(n.name);
                if (found == from->items.end()) {
                    error(n.span, q(from_name) + " has no " + q(text(n.name)));
                    continue;
                }
                Item* target = found->second;
                if (!target->is_pub) {
                    error(n.span, q(text(n.name)) + " isn't `pub` in " + q(from_name));
                    note(target->name_span, "it's declared here");
                    continue;
                }
                if (auto own = here->items.find(n.name); own != here->items.end()) {
                    error(n.span, q(text(n.name)) + " is already the name of a " + item_kind(own->second) +
                                      " here; Calx doesn't allow shadowing");
                    continue;
                }
                if (auto prev = here->imports.find(n.name); prev != here->imports.end()) {
                    error(n.span, q(text(n.name)) + " is already imported");
                    note(prev->second.span, "it's imported here");
                    continue;
                }
                here->imports.emplace(n.name, Scope::Import{target, n.span, false});
            }
        }
    }

    // --- reporting -----------------------------------------------------------------------

    void error(Span s, std::string msg) {
        ctx_.diags.error(s, std::move(msg));
        // An instance's errors are ones its generic definition didn't have, so say which use it was.
        if (instance_mode_ && fn_ && fn_->origin) note(fn_->instance_site, "in " + q(instance_name(fn_)) + ", needed here");
    }
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

    // A module's items are named in C after its path: `geo.shapes` gives `geo_shapes__Square`.
    void set_prefix(Module* m) {
        std::string prefix;
        for (std::size_t i = 0; i < m->name.parts.size(); i++) {
            if (i) prefix += '_';
            prefix += text(m->name.parts[i]);
        }
        m->prefix = ctx_.arena.copy(prefix);
    }

    std::string_view c_name(std::initializer_list<std::string_view> parts) { return c_name_in(scope_->module, parts); }

    std::string_view c_name_in(const Module* m, std::initializer_list<std::string_view> parts) {
        std::string s(m->prefix);
        for (std::string_view p : parts) {
            s += "__";
            s += p;
        }
        // Instances' names are made up, so they could meet a written one.
        if (!c_names_.insert(s).second) {
            for (int i = 2;; i++) {
                std::string t = s + "_" + std::to_string(i);
                if (c_names_.insert(t).second) {
                    s = t;
                    break;
                }
            }
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
            it->used = true;  // nothing can name it, so "never used" would only repeat the error
            return;
        }
        auto [pos, fresh] = scope_->items.emplace(it->name, it);
        if (!fresh) {
            error(it->name_span, q(text(it->name)) + " is already defined");
            note(pos->second->name_span, "the first definition is here");
        }
    }

    void collect(Module* m) {
        for (Item* it : m->items) {
            it->module = m;
            for (Item* member : it->members) member->module = m;
            if (!it->attributes.empty()) {
                error(it->attributes[0].span, "attributes aren't implemented yet");
            }
            switch (it->kind) {
                case ItemKind::fn:
                    if (it->is_extern) {
                        not_yet(it->span, "extern functions");
                        continue;
                    }
                    it->type_params = make_params(it);
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
                    if (it->is_tuple_struct) not_yet(it->span, "wrapper structs");
                    declare_item(it);
                    it->c_name = c_name({text(it->name)});
                    it->type_params = make_params(it);
                    it->declared = T.make_struct(it, generic_name(it));
                    make_identity(it);
                    structs_.push_back(it);
                    break;
                case ItemKind::const_:
                    declare_item(it);
                    it->c_name = c_name({text(it->name)});
                    consts_.push_back(it);
                    break;
                case ItemKind::impl:
                    it->type_params = make_params(it);
                    for (Item* m : it->members) {
                        if (m->kind != ItemKind::fn) continue;
                        // A method's own parameters come after the impl's, and can't reuse their names.
                        std::vector<Type*> all(it->type_params.begin(), it->type_params.end());
                        for (const GenericParam& g : m->generics) {
                            for (const GenericParam& outer : it->generics) {
                                if (g.name == outer.name) {
                                    error(g.span, q(text(g.name)) + " is already a parameter of the impl; Calx doesn't allow shadowing");
                                }
                            }
                        }
                        for (Type* own : make_params(m)) all.push_back(own);
                        m->type_params = ctx_.arena.copy(all);
                    }
                    impls_.push_back(it);
                    break;
                case ItemKind::import: break;  // resolve_imports
                case ItemKind::enum_:
                    declare_item(it);
                    it->c_name = c_name({text(it->name)});
                    it->type_params = make_params(it);
                    it->declared = T.make_enum(it, generic_name(it));
                    make_identity(it);
                    enums_.push_back(it);
                    break;
                case ItemKind::interface: {
                    if (!it->generics.empty()) not_yet(it->generics[0].span, "generic interfaces");
                    declare_item(it);
                    // `Self` is a parameter of every function in the interface, bounded by it.
                    Type* self = T.make_param("Self");
                    param_names_[self] = n_Self_;
                    std::vector<Item*> bound{it};
                    self->bounds = ctx_.arena.copy(bound);
                    std::vector<Type*> params{self};
                    it->type_params = ctx_.arena.copy(params);
                    interfaces_.push_back(it);
                    break;
                }
                case ItemKind::type_alias: not_yet(it->span, "type aliases"); break;
                case ItemKind::extern_block: not_yet(it->span, "extern blocks"); break;
                case ItemKind::attribute_block: not_yet(it->span, "attribute blocks"); break;
            }
        }
    }

    void resolve_fields(Item* s) {
        InModule in(*this, s);
        if (s->resolved || s->resolving) return;
        s->resolving = true;
        Item* saved = current_item_;
        std::size_t mark = push_env(s);
        std::unordered_map<Name, Span> seen;
        for (FieldDecl& f : s->fields) {
            if (!seen.emplace(f.name, f.span).second) {
                error(f.span, "field " + q(text(f.name)) + " is already declared");
                note(seen[f.name], "the first declaration is here");
            }
            current_item_ = s;
            f.resolved = resolve_type(f.type);
            if (s->is_pub && f.is_pub) public_type(f.resolved, f.type->span, "a `pub` field of a `pub` struct can't hold it");
        }
        pop_env(mark);
        current_item_ = saved;
        s->resolving = false;
        s->resolved = true;
        report_unused_params(s, "isn't used by any field");
        fill_unfilled(s);
    }

    void check_defaults(Item* s) {
        InModule in(*this, s);
        std::size_t before = ctx_.diags.error_count();
        std::size_t mark = push_env(s);
        for (FieldDecl& f : s->fields) {
            if (!f.default_value) continue;
            Type* t = check(f.default_value, f.resolved);
            if (!is_bad(t) && !f.default_value->is_const) {
                error(f.default_value->span, "a field's default must be a constant");
            }
        }
        pop_env(mark);
        if (!s->type_params.empty() && ctx_.diags.error_count() != before) failed_generics_.insert(s);
    }

    // An instance's copies of the defaults. Its generic struct's defaults were checked first, so a
    // struct with errors has them reported once.
    void check_instance_defaults(Type* t) {
        if (failed_generics_.count(t->generic)) return;
        InModule in(*this, t->item);
        std::size_t mark = push_env(t->item);
        for (FieldDecl& f : t->item->fields) {
            if (f.default_value) check(f.default_value, f.resolved);
        }
        pop_env(mark);
    }

    static bool is_type_item(const Item* it) { return it->kind == ItemKind::struct_ || it->kind == ItemKind::enum_; }

    void resolve_enum(Item* en) {
        InModule in(*this, en);
        if (en->resolved || en->resolving) return;
        en->resolving = true;
        Item* saved_item = current_item_;
        std::size_t mark = push_env(en);
        Type* t = en->declared;
        if (en->variants.empty()) error(en->name_span, "an enum needs at least one variant");
        std::unordered_map<Name, Span> seen;
        bool has_data = false;
        current_item_ = en;
        for (VariantDecl& v : en->variants) {
            if (!seen.emplace(v.name, v.span).second) {
                error(v.span, "variant " + q(text(v.name)) + " is already declared");
                note(seen[v.name], "the first declaration is here");
            }
            std::vector<Type*> types;
            for (TypeExpr* p : v.payload) {
                types.push_back(resolve_type(p));
                if (en->is_pub) public_type(types.back(), p->span, "a `pub` enum's variants can't hold it");
            }
            std::unordered_map<Name, Span> fields;
            for (FieldDecl& f : v.fields) {
                if (!fields.emplace(f.name, f.span).second) {
                    error(f.span, "field " + q(text(f.name)) + " is already declared");
                }
                f.resolved = resolve_type(f.type);
                if (en->is_pub) public_type(f.resolved, f.type->span, "a `pub` enum's variants can't hold it");
                types.push_back(f.resolved);
                if (f.default_value) error(f.default_value->span, "a variant's fields can't have defaults");
            }
            if (v.has_fields && v.fields.empty()) error(v.span, "a variant without data needs no braces");
            for (std::size_t i = 0; i < types.size(); i++) {
                if (types[i]->kind == TypeKind::unit || types[i]->kind == TypeKind::never) {
                    error(v.span, "a variant can't hold " + name_of(types[i]));
                    types[i] = T.error;
                }
            }
            v.types = ctx_.arena.copy(types);
            if (!types.empty()) has_data = true;
        }
        pop_env(mark);
        current_item_ = saved_item;
        t->has_data = has_data;
        if (en->repr) {
            Type* r = resolve_type(en->repr);
            if (!is_bad(r) && (!is_int(r) || r->is_usize)) {
                error(en->repr->span, "an enum's representation must be an integer type such as `u8`");
            } else if (!is_bad(r) && has_data) {
                error(en->repr->span, "only an enum without data can have a representation");
            } else if (!is_bad(r)) {
                t->repr = r;
            }
        }
        bool explicit_repr = t->repr != nullptr;
        if (!t->repr) {
            std::size_t n = en->variants.size();
            t->repr = n <= 256 ? T.u8 : n <= 65536 ? T.u16 : T.u32;
        }
        // The tags count up from 0, or from the last value given, as in C.
        std::uint64_t next = 0;
        bool next_ok = true;
        std::unordered_map<std::uint64_t, const VariantDecl*> values;
        for (VariantDecl& v : en->variants) {
            if (v.value) {
                if (!explicit_repr) {
                    error(v.value->span, "give the enum a representation, such as " +
                                             q("enum " + std::string(text(en->name)) + ": u8") + ", to set its values");
                    check(v.value, nullptr);
                } else {
                    Type* vt = check(v.value, t->repr);
                    if (!is_bad(vt) && !v.value->has_bits) {
                        error(v.value->span, "a variant's value must be a constant");
                    } else if (!is_bad(vt)) {
                        next = v.value->const_bits;
                        next_ok = true;
                    }
                }
            } else if (!next_ok) {
                error(v.span, "the value of " + q(text(v.name)) + " would overflow " + name_of(t->repr));
            }
            v.tag = next;
            auto [pos, fresh] = values.emplace(next, &v);
            if (!fresh && next_ok) {
                error(v.span, q(text(v.name)) + " has the same value as " + q(text(pos->second->name)));
            }
            if (next == source_range(t->repr).pos) {
                next_ok = false;
            } else {
                std::uint64_t mask = t->repr->bits == 64 ? UINT64_MAX : (std::uint64_t{1} << t->repr->bits) - 1;
                next = ((next + 1) & mask);
                if (t->repr->is_signed && (next >> (t->repr->bits - 1)) & 1) next |= ~mask;
            }
        }
        en->resolving = false;
        en->resolved = true;
        report_unused_params(en, "isn't used by any variant");
        fill_unfilled(en);
    }

    static std::uint32_t find_variant(const Type* t, Name n) {
        for (std::uint32_t i = 0; i < t->item->variants.size(); i++) {
            if (t->item->variants[i].name == n) return i;
        }
        return UINT32_MAX;
    }

    std::string variant_text(const Type* t, std::uint32_t i) const {
        std::string_view owner = t->generic && t->item == t->generic ? text(t->generic->name) : t->name;
        return std::string(owner) + "." + std::string(text(t->item->variants[i].name));
    }

    // An interface's functions are generic over `Self`. One with a body is a default, which is
    // checked once for every `Self` and copied for each type that doesn't write its own.
    void collect_interface(Item* it) {
        InModule in(*this, it);
        Type* self = it->type_params[0];
        std::unordered_map<Name, Item*> seen;
        for (Item* m : it->members) {
            if (m->kind != ItemKind::fn && m->kind != ItemKind::const_) {
                error(m->span, "an interface holds functions and constants");
                continue;
            }
            if (!seen.emplace(m->name, m).second) {
                error(m->name_span, q(text(m->name)) + " is already in " + q(text(it->name)));
                continue;
            }
            m->interface_item = it;
            m->impl_type = self;
            if (m->kind == ItemKind::const_) {
                if (m->value) not_yet(m->value->span, "default values for an interface's constants");
                if (!m->type) {
                    error(m->name_span, "give the constant a type: " + q("const " + std::string(text(m->name)) + ": i32"));
                    m->declared = T.error;
                    continue;
                }
                Type* saved = current_impl_type_;
                current_impl_type_ = self;
                m->declared = resolve_type(m->type);
                current_impl_type_ = saved;
                continue;
            }
            std::size_t mark = env_.size();
            if (!m->generics.empty()) {
                not_yet(m->generics[0].span, "generic functions in interfaces");
                // Its own parameters stand for nothing, so its signature has no further errors.
                for (const GenericParam& g : m->generics) env_.push_back({g.name, T.error});
            }
            m->type_params = it->type_params;
            m->c_name = c_name({text(it->name), text(m->name)});
            check_signature(m);
            pop_env(mark);
            if (m->body && m->generics.empty()) all_fns_.push_back(m);
        }
    }

    // `fn area(self) -> f64`, with `Self` as `t`, for messages.
    std::string signature_text(const Item* f, const Subst& s) {
        std::string out = "fn " + std::string(text(f->name)) + "(";
        for (std::size_t i = 0; i < f->params.size(); i++) {
            const Param& p = f->params[i];
            if (i) out += ", ";
            const char* mode = p.mode == ParamMode::mut ? "mut " : p.mode == ParamMode::own ? "own " : "";
            Type* t = p.local ? subst(p.local->type, s) : T.error;
            if (p.is_self) out += p.mode == ParamMode::read ? std::string("self") : "self: " + std::string(mode) + std::string(t->name);
            else out += std::string(text(p.name)) + ": " + mode + std::string(t->name);
        }
        out += ")";
        Type* r = subst(f->result_type, s);
        if (r && r->kind != TypeKind::unit) out += " -> " + std::string(r->name);
        return out;
    }

    // An impl's function must have exactly the interface's signature, with its type for `Self`,
    // parameter names included, since calls can name their arguments.
    void check_conforms(Item* m, Item* want, Type* t, Item* iface) {
        Subst s{{iface->type_params[0], t}};
        bool ok = m->params.size() == want->params.size();
        for (std::size_t i = 0; ok && i < m->params.size(); i++) {
            const Param& a = m->params[i];
            const Param& b = want->params[i];
            ok = a.mode == b.mode && a.is_self == b.is_self && a.name == b.name && a.local && b.local &&
                 (is_bad(a.local->type) || is_bad(b.local->type) || a.local->type == subst(b.local->type, s));
        }
        Type* r = subst(want->result_type, s);
        ok = ok && (m->result_type == r || is_bad(m->result_type) || is_bad(r));
        if (!ok) {
            error(m->name_span, q(text(m->name)) + " doesn't match " + q(text(iface->name)) + ", which declares it as " +
                                    q(signature_text(want, s)));
            note(want->name_span, "it's declared here");
        }
    }

    void collect_impl(Item* impl) {
        InModule in(*this, impl);
        Item* iface = nullptr;
        if (impl->interface) {
            iface = resolve_interface(impl->interface);
            if (!iface) return;
        }
        std::size_t mark = push_env(impl);
        Type* t = resolve_type(impl->self_type);
        pop_env(mark);
        if (is_bad(t)) return;
        // Every parameter must be in the type, or a use couldn't tell what it stands for.
        for (Type* p : impl->type_params) {
            std::vector<Type*> one{p};
            if (!mentions_any(t, Slice<Type*>(ctx_.arena.copy(one)))) {
                error(param_spans_[p], "generic parameter " + q(p->name) + " must appear in the impl's type, " + name_of(t));
            }
        }
        bool nominal = t->kind == TypeKind::struct_ || t->kind == TypeKind::enum_;
        if (!iface && !nominal) {
            error(impl->self_type->span, "methods can only be added to structs and enums declared in this module");
            return;
        }
        // An impl is in the module of its type or its interface, so there's one place to look
        // for it, and two modules can't both implement an interface for a type.
        const Item* owner = nominal ? (t->generic ? t->generic : t->item) : nullptr;
        bool type_here = owner && owner->module == scope_->module;
        if (!iface && nominal && !type_here) {
            error(impl->self_type->span, "methods for " + name_of(t) + " go in its module, " + q(module_name(owner->module)));
            return;
        }
        if (iface && !type_here && iface->module != scope_->module) {
            error(impl->self_type->span, "an impl of " + q(text(iface->name)) + " for " + name_of(t) + " goes in " +
                                             q(text(iface->name)) + "'s module, " + q(module_name(iface->module)) +
                                             (owner ? ", or in " + name_of(t) + "'s, " + q(module_name(owner->module)) : std::string()));
            return;
        }
        if (iface && !nominal && !is_numeric(t) && t->kind != TypeKind::bool_ && t->kind != TypeKind::char_ &&
            t->kind != TypeKind::rune && t->kind != TypeKind::str) {
            not_yet(impl->self_type->span, "interfaces for this kind of type");
            return;
        }
        auto& same_head = impls_by_head_[head(t)];
        if (iface) {
            for (ImplInfo* other : same_head) {
                if (other->iface != iface) continue;
                Subst a, b;
                if (unify(other->self, t, a, other->impl->type_params) || unify(t, other->self, b, impl->type_params)) {
                    error(impl->self_type->span, q(text(iface->name)) + " is already implemented for " + name_of(t));
                    note(other->impl->self_type->span, "the other impl is here");
                    return;
                }
            }
        }
        impl_infos_.push_back(std::make_unique<ImplInfo>());
        ImplInfo* info = impl_infos_.back().get();
        info->impl = impl;
        info->self = t;
        info->iface = iface;
        impl->interface_item = iface;
        for (Item* m : impl->members) {
            Item* want = nullptr;
            if (iface) {
                if (m->kind != ItemKind::fn && m->kind != ItemKind::const_) {
                    error(m->span, "an impl of an interface holds its functions and constants");
                    continue;
                }
                want = interface_member(iface, m->name);
                if (!want) {
                    error(m->name_span, q(text(m->name)) + " isn't part of " + q(text(iface->name)) + "; put it in a separate " +
                                            q("impl " + std::string(t->name)) + " block");
                    continue;
                }
                if (want->kind != m->kind) {
                    error(m->name_span, q(text(m->name)) + " is a " + (want->kind == ItemKind::fn ? "function" : "constant") +
                                            " in " + q(text(iface->name)));
                    continue;
                }
                m->interface_item = iface;
            } else if (m->kind != ItemKind::fn) {
                error(m->span, "an impl block holds only functions for now");
                continue;
            }
            if (info->members.count(m->name)) {
                error(m->name_span, name_of(t) + " already has " + (m->kind == ItemKind::fn ? "a function" : "a constant") +
                                        " named " + q(text(m->name)) + " here");
                continue;
            }
            // An interface's members carry its name in C, so a profile or a stack trace says which
            // impl ran: `Lift__Up__go` and `Lift__Down__go`.
            std::string owner = mangle(t);
            if (iface) owner += "__" + std::string(text(iface->name));
            if (m->kind == ItemKind::const_) {
                m->c_name = c_name({owner, text(m->name)});
                m->impl_type = t;
                m->result_type = subst(want->declared, Subst{{iface->type_params[0], t}});
                info->members.emplace(m->name, m);
                consts_.push_back(m);
                continue;
            }
            if (!m->body) {
                error(m->name_span, "function " + q(text(m->name)) + " needs a body");
                continue;
            }
            if (iface && !m->generics.empty()) not_yet(m->generics[0].span, "generic functions in interfaces");
            m->impl_type = t;
            m->c_name = c_name({owner, text(m->name)});
            if (t->kind == TypeKind::struct_) {
                for (const FieldDecl& f : t->item->fields) {
                    if (f.name == m->name) {
                        error(m->name_span, q(text(m->name)) + " is both a field and a method of " + name_of(t));
                        note(f.span, "the field is here");
                    }
                }
            }
            if (t->kind == TypeKind::enum_ && find_variant(t, m->name) != UINT32_MAX) {
                error(m->name_span, q(text(m->name)) + " is both a variant and a function of " + name_of(t));
            }
            // Two blocks for overlapping types can't both have the name. Against the type's
            // interfaces, that's checked once every impl is known (check_one_name).
            bool clash = false;
            for (ImplInfo* other : same_head) {
                if (iface || other->iface || !other->members.count(m->name)) continue;
                Subst a, b;
                if (unify(other->self, t, a, other->impl->type_params) || unify(t, other->self, b, impl->type_params)) {
                    error(m->name_span, name_of(t) + " already has a function named " + q(text(m->name)));
                    note(other->members[m->name]->name_span, "the first one is here");
                    clash = true;
                }
            }
            if (clash) continue;
            info->members.emplace(m->name, m);
            check_signature(m);
            if (want) check_conforms(m, want, t, iface);
            all_fns_.push_back(m);
        }
        if (iface) {
            std::string missing;
            for (Item* w : iface->members) {
                if ((w->kind == ItemKind::fn && !w->body) || w->kind == ItemKind::const_) {
                    if (!info->members.count(w->name)) missing += (missing.empty() ? "" : ", ") + q(text(w->name));
                }
            }
            if (!missing.empty()) {
                error(impl->self_type->span, "the impl of " + q(text(iface->name)) + " for " + name_of(t) + " is missing " + missing);
            }
        }
        same_head.push_back(info);
    }

    // A type has one function of each name, as in Java: its own impls can't reuse a name an
    // interface it implements gives it, a default included, so `x.name()` means one thing.
    // Replacing a default goes in the interface's impl.
    void check_one_name() {
        for (const auto& own : impl_infos_) {
            if (own->iface) continue;
            for (ImplInfo* other : impls_by_head_[head(own->self)]) {
                if (!other->iface) continue;
                Subst a, b;
                if (!unify(other->self, own->self, a, other->impl->type_params) &&
                    !unify(own->self, other->self, b, own->impl->type_params)) {
                    continue;
                }
                for (Item* m : own->impl->members) {
                    auto mine = own->members.find(m->name);
                    if (m->kind != ItemKind::fn || mine == own->members.end() || mine->second != m) continue;
                    Item* theirs = nullptr;
                    if (auto f = other->members.find(m->name); f != other->members.end()) theirs = f->second;
                    Item* d = interface_member(other->iface, m->name);
                    bool is_default = !theirs && d && d->kind == ItemKind::fn && d->body;
                    if (is_default) theirs = d;
                    if (!theirs) continue;
                    std::string iface = std::string(text(other->iface->name));
                    error(m->name_span, name_of(own->self) + " gets " + q(text(m->name)) + " from " + q(iface) +
                                            ", so its own impl can't have one; " +
                                            (is_default ? "to replace the default, put it in the " +
                                                              q("impl " + iface + " for " + std::string(own->self->name)) + " block"
                                                        : std::string("rename one of them")));
                    note(theirs->name_span, "this is the other");
                }
            }
        }
    }

    // --- generics ------------------------------------------------------------------------
    //
    // A generic item is checked once, with each parameter a type of its own that allows only what
    // its bounds give it. Each use with real types makes an instance: a copy of the item with the
    // types in place, which is checked again and emitted like any other item (D26).

    struct ImplInfo {
        Item* impl = nullptr;
        Type* self = nullptr;   // the type it's for, which can mention the impl's parameters
        Item* iface = nullptr;  // null for an impl without an interface
        std::unordered_map<Name, Item*> members;
        bool used = false;
    };

    // What a name on a type, `t.name` or `T.name`, turned out to be.
    struct Member {
        Item* fn = nullptr;
        Item* konst = nullptr;
        Subst s;  // what the member's parameters stand for, as far as the type says
        bool ambiguous = false;
        Item* iface = nullptr;  // the interface it's from, or null for the type's own impls
    };

    // A function from the type's own impls is seen where it's `pub` or written; one from an
    // interface, wherever the interface is.
    bool member_visible(const Member& m) const { return m.iface || !m.fn || visible(m.fn); }

    struct DeferredBounds {
        Slice<Type*> params;
        std::vector<Type*> args;
        Span span;
        std::string what;
    };

    Slice<Type*> make_params(Item* it) {
        std::vector<Type*> out;
        for (GenericParam& g : it->generics) {
            Type* p = T.make_param(text(g.name));
            param_names_[p] = g.name;
            param_spans_[p] = g.span;
            param_owner_[p] = it;
            param_decls_.push_back({p, &g});
            out.push_back(p);
        }
        return ctx_.arena.copy(out);
    }

    std::string_view generic_name(const Item* it) {
        if (it->type_params.empty()) return text(it->name);
        std::string s(text(it->name));
        s += '<';
        for (std::size_t i = 0; i < it->type_params.size(); i++) {
            if (i) s += ", ";
            s += it->type_params[i]->name;
        }
        s += '>';
        return ctx_.arena.copy(s);
    }

    // A generic struct or enum's own type is its parameters applied to itself, `Pair<A, B>`. It
    // appears only inside the declaration; uses elsewhere are instances.
    void make_identity(Item* it) {
        if (it->type_params.empty()) return;
        Type* t = it->declared;
        t->generic = it;
        t->args = it->type_params;
        t->open = true;
        std::vector<Type*> key(it->type_params.begin(), it->type_params.end());
        type_instances_.emplace(std::make_pair(it, key), t);
    }

    bool is_identity(const Type* t) const { return t->generic && t->item == t->generic; }

    // Every generic parameter's name and bounds, once every interface is declared.
    void resolve_params() {
        std::unordered_map<Item*, std::unordered_map<Name, Span>> seen;
        for (auto& [p, g] : param_decls_) {
            InModule in(*this, param_owner_[p]);
            auto& names = seen[param_owner_[p]];
            if (!names.emplace(g->name, g->span).second) {
                error(g->span, "generic parameter " + q(text(g->name)) + " is already declared");
                note(names[g->name], "the first declaration is here");
                // Either one could be the one meant, so neither is reported as unused.
                for (Type* other : param_owner_[p]->type_params) {
                    if (param_names_[other] == g->name) param_used_[other] = true;
                }
            } else if (Item* it = find_item(g->name)) {
                error(g->span, q(text(g->name)) + " is already the name of a " + item_kind(it) +
                                   "; Calx doesn't allow shadowing");
            } else if (is_builtin_name(g->name)) {
                error(g->span, q(text(g->name)) + " is a built-in name");
            }
            std::vector<Item*> bounds;
            for (TypeExpr* b : g->bounds) {
                if (b->kind == TypeExprKind::named && b->path.parts.size() == 1 && b->path.parts[0] == n_Copy_ &&
                    b->args.empty()) {
                    if (p->copy_bound) error(b->span, "`Copy` is already a bound of " + q(text(g->name)));
                    p->copy_bound = true;
                    continue;
                }
                Item* iface = resolve_interface(b);
                if (!iface) continue;
                if (std::find(bounds.begin(), bounds.end(), iface) != bounds.end()) {
                    error(b->span, q(text(iface->name)) + " is already a bound of " + q(text(g->name)));
                    continue;
                }
                bounds.push_back(iface);
            }
            p->bounds = ctx_.arena.copy(bounds);
        }
    }

    Item* resolve_interface(TypeExpr* t) {
        if (t->kind != TypeExprKind::named || t->path.parts.size() != 1) {
            error(t->span, "a bound must be an interface");
            return nullptr;
        }
        Item* it = find_item(t->path.parts[0]);
        if (!it || it->kind != ItemKind::interface) {
            error(t->span, q(path_text(t->path)) + (!it ? " isn't defined" : " isn't an interface"));
            return nullptr;
        }
        if (!t->args.empty()) not_yet(t->args[0]->span, "generic interfaces");
        it->used = true;
        return it;
    }

    // A parameter that nothing mentions can't be worked out from a use, so it's an error (D63).
    void report_unused_params(Item* it, const char* why) {
        if (it->origin) return;
        for (Type* p : it->type_params) {
            if (param_owner_[p] != it || param_used_[p]) continue;
            error(param_spans_[p], "generic parameter " + q(p->name) + " " + why);
        }
    }

    std::size_t push_env(const Item* it) {
        std::size_t mark = env_.size();
        const Item* o = it->origin ? it->origin : it;
        for (std::size_t i = 0; i < o->type_params.size(); i++) {
            Type* p = o->type_params[i];
            env_.push_back({param_names_[p], it->origin ? it->type_args[i] : p});
        }
        return mark;
    }

    void pop_env(std::size_t mark) { env_.resize(mark); }

    Type* env_type(Name n) {
        for (std::size_t i = env_.size(); i-- > 0;) {
            if (env_[i].first != n) continue;
            Type* t = env_[i].second;
            if (t->kind == TypeKind::param) param_used_[t] = true;
            return t;
        }
        return nullptr;
    }

    static bool contains(const Slice<Type*>& vars, const Type* t) {
        for (Type* v : vars) if (v == t) return true;
        return false;
    }

    // Whether `t` mentions one of `vars` that `s` doesn't give yet.
    static bool unresolved(const Type* t, const Slice<Type*>& vars, const Subst& s) {
        if (!t->open) return false;
        switch (t->kind) {
            case TypeKind::param: return contains(vars, t) && !s.count(const_cast<Type*>(t));
            case TypeKind::optional: case TypeKind::array: case TypeKind::list: return unresolved(t->elem, vars, s);
            case TypeKind::tuple:
                for (Type* e : t->elems) if (unresolved(e, vars, s)) return true;
                return false;
            case TypeKind::struct_: case TypeKind::enum_:
                for (Type* a : t->args) if (unresolved(a, vars, s)) return true;
                return false;
            default: return false;
        }
    }

    static bool mentions_any(const Type* t, const Slice<Type*>& vars) { return unresolved(t, vars, Subst{}); }

    Subst binding(const Slice<Type*>& params, const std::vector<Type*>& args) {
        Subst s;
        for (std::size_t i = 0; i < params.size() && i < args.size(); i++) s.emplace(params[i], args[i]);
        return s;
    }

    // `t` with the parameters in `s` replaced.
    Type* subst(Type* t, const Subst& s) {
        if (!t || !t->open || s.empty()) return t;
        switch (t->kind) {
            case TypeKind::param: {
                auto it = s.find(t);
                return it == s.end() ? t : it->second;
            }
            case TypeKind::optional: return T.optional_of(subst(t->elem, s));
            case TypeKind::array: return T.array_of(subst(t->elem, s), t->length);
            case TypeKind::list: return T.list_of(subst(t->elem, s));
            case TypeKind::tuple: {
                std::vector<Type*> elems;
                for (Type* e : t->elems) elems.push_back(subst(e, s));
                return T.tuple_of(elems);
            }
            case TypeKind::struct_: case TypeKind::enum_: {
                std::vector<Type*> args;
                for (Type* a : t->args) args.push_back(subst(a, s));
                return instance_type(t->generic, args);
            }
            default: return t;
        }
    }

    // Matches `pattern`, which can mention the parameters in `vars`, against `actual`, binding them
    // in `s`. A parameter already bound must match what it's bound to.
    bool unify(Type* pattern, Type* actual, Subst& s, const Slice<Type*>& vars) {
        if (is_bad(actual) || is_bad(pattern) || actual->kind == TypeKind::never) return true;
        if (pattern->kind == TypeKind::param && contains(vars, pattern)) {
            auto it = s.find(pattern);
            if (it == s.end()) {
                s.emplace(pattern, actual);
                return true;
            }
            return it->second == actual;
        }
        if (!pattern->open) return pattern == actual;
        if (pattern->kind != actual->kind) return false;
        switch (pattern->kind) {
            case TypeKind::optional: case TypeKind::list: return unify(pattern->elem, actual->elem, s, vars);
            case TypeKind::array:
                return pattern->length == actual->length && unify(pattern->elem, actual->elem, s, vars);
            case TypeKind::tuple:
                if (pattern->elems.size() != actual->elems.size()) return false;
                for (std::size_t i = 0; i < pattern->elems.size(); i++) {
                    if (!unify(pattern->elems[i], actual->elems[i], s, vars)) return false;
                }
                return true;
            case TypeKind::struct_: case TypeKind::enum_:
                if (pattern->generic != actual->generic) return false;
                for (std::size_t i = 0; i < pattern->args.size(); i++) {
                    if (!unify(pattern->args[i], actual->args[i], s, vars)) return false;
                }
                return true;
            default: return pattern == actual;
        }
    }

    // `Pair<i32, str>` as written: the instance, after checking the arguments.
    Type* generic_type(Item* g, Slice<TypeExpr*> written, Span span) {
        std::size_t n = g->type_params.size();
        if (written.size() != n) {
            if (written.empty()) {
                error(span, q(text(g->name)) + " is generic; give its " + (n == 1 ? "type" : "types") + ", as in " +
                                q(std::string(text(g->name)) + "<...>"));
            } else {
                error(span, q(text(g->name)) + " takes " + std::to_string(n) + (n == 1 ? " type" : " types") + ", but " +
                                std::to_string(written.size()) + (written.size() == 1 ? " was given" : " were given"));
            }
            for (TypeExpr* a : written) resolve_type(a);
            return T.error;
        }
        std::vector<Type*> args;
        bool bad = false;
        for (TypeExpr* a : written) {
            Type* x = resolve_type(a);
            if (x->kind == TypeKind::unit || x->kind == TypeKind::never) {
                error(a->span, name_of(x) + " can't be a generic argument");
                x = T.error;
            }
            bad = bad || is_bad(x);
            args.push_back(x);
        }
        if (bad || !check_bounds(g->type_params, args, span, q(text(g->name)))) return T.error;
        return instance_type(g, args);
    }

    // Whether each argument implements its parameter's bounds. While the impls are still being
    // collected, the check waits until they all are.
    bool check_bounds(const Slice<Type*>& params, const std::vector<Type*>& args, Span span, const std::string& what) {
        if (defer_bounds_) {
            deferred_bounds_.push_back({params, args, span, what});
            return true;
        }
        bool ok = true;
        for (std::size_t i = 0; i < params.size() && i < args.size(); i++) {
            if (params[i]->copy_bound && !is_bad(args[i]) && !is_copy(args[i])) {
                error(span, name_of(args[i]) + " isn't `Copy`, which " + what + " needs for " + q(params[i]->name));
                ok = false;
            }
            for (Item* b : params[i]->bounds) {
                if (satisfies(args[i], b)) continue;
                error(span, name_of(args[i]) + " doesn't implement " + q(text(b->name)) + ", which " + what +
                                " needs for " + q(params[i]->name));
                ok = false;
            }
        }
        return ok;
    }

    bool bounds_hold(const Slice<Type*>& params, const Subst& s) {
        for (Type* p : params) {
            auto it = s.find(p);
            if (it == s.end()) return false;
            if (p->copy_bound && !is_copy(it->second)) return false;
            for (Item* b : p->bounds) {
                if (!satisfies(it->second, b)) return false;
            }
        }
        return true;
    }

    static const void* head(const Type* t) {
        if (t->generic) return t->generic;
        if (t->item) return t->item;
        return t;
    }

    bool satisfies(Type* t, Item* iface) {
        if (is_bad(t)) return true;
        if (t->kind == TypeKind::param) {
            for (Item* b : t->bounds) if (b == iface) return true;
            return false;
        }
        auto it = impls_by_head_.find(head(t));
        if (it == impls_by_head_.end()) return false;
        for (ImplInfo* info : it->second) {
            if (info->iface != iface) continue;
            Subst s;
            if (unify(info->self, t, s, info->impl->type_params) && bounds_hold(info->impl->type_params, s)) {
                info->used = true;
                return true;
            }
        }
        return false;
    }

    Item* interface_member(const Item* iface, Name n) const {
        for (Item* m : iface->members) {
            if (m->name == n && (m->kind == ItemKind::fn || m->kind == ItemKind::const_)) return m;
        }
        return nullptr;
    }

    // The function or constant `name` on type `t`: from the bounds of a parameter, or from an impl
    // block, with the type's own blocks before its interfaces'. A generic type written without its
    // arguments, `Pair.make(...)`, matches any of its impls, and the call works out the arguments.
    Member lookup(Type* t, Name n, Item* only = nullptr) {
        Member m;
        if (is_bad(t)) return m;
        auto take = [&](Item* x, Subst s, Item* iface) {
            if (m.fn || m.konst) {
                m.ambiguous = true;
                return;
            }
            (x->kind == ItemKind::fn ? m.fn : m.konst) = x;
            m.s = std::move(s);
            m.iface = iface;
        };
        if (t->kind == TypeKind::param) {
            for (Item* b : t->bounds) {
                if (only && b != only) continue;
                if (Item* x = interface_member(b, n)) take(x, Subst{{b->type_params[0], t}}, b);
            }
            return m;
        }
        auto found = impls_by_head_.find(head(t));
        if (found == impls_by_head_.end()) return m;
        bool identity = is_identity(t);
        for (ImplInfo* info : found->second) {
            if (info->iface || only) continue;
            auto f = info->members.find(n);
            if (f == info->members.end()) continue;
            Subst s;
            if (!identity && (!unify(info->self, t, s, info->impl->type_params) || !bounds_hold(info->impl->type_params, s))) {
                continue;
            }
            info->used = true;
            take(f->second, std::move(s), nullptr);
        }
        if (m.fn || m.konst || identity) return m;
        for (ImplInfo* info : found->second) {
            if (!info->iface || (only && info->iface != only) || !visible(info->iface)) continue;
            Subst s;
            if (!unify(info->self, t, s, info->impl->type_params) || !bounds_hold(info->impl->type_params, s)) continue;
            Item* x = nullptr;
            if (auto f = info->members.find(n); f != info->members.end()) {
                x = f->second;
            } else if (Item* d = interface_member(info->iface, n); d && d->kind == ItemKind::fn && d->body) {
                x = d;  // the interface's default, for this `Self`
                s = Subst{{info->iface->type_params[0], t}};
            }
            if (!x) continue;
            info->used = true;
            take(x, std::move(s), info->iface);
        }
        return m;
    }

    std::string mangle(const Type* t) const {
        switch (t->kind) {
            case TypeKind::struct_: case TypeKind::enum_: {
                const Item* it = t->generic ? t->generic : t->item;
                std::string s(text(it->name));
                for (Type* a : t->args) s += "_" + mangle(a);
                return s;
            }
            case TypeKind::optional: return mangle(t->elem) + "_opt";
            case TypeKind::array: return mangle(t->elem) + "_arr" + std::to_string(t->length);
            case TypeKind::list: return mangle(t->elem) + "_list";
            case TypeKind::tuple: {
                std::string s = "tup";
                for (Type* e : t->elems) s += "_" + mangle(e);
                return s;
            }
            default: return std::string(t->name);
        }
    }

    std::string instance_name(const Item* f) const {
        const Item* o = f->origin ? f->origin : f;
        std::string s = f->impl_type ? std::string(f->impl_type->name) + "." : "";
        s += text(f->name);
        std::size_t own = o->generics.size();
        if (own && f->type_args.size() >= own) {
            s += '<';
            for (std::size_t k = f->type_args.size() - own; k < f->type_args.size(); k++) {
                if (k != f->type_args.size() - own) s += ", ";
                s += f->type_args[k]->name;
            }
            s += '>';
        }
        return s;
    }

    // The instance of a generic struct or enum for `args`: a copy of the item whose fields or
    // variants have the arguments' types.
    Type* instance_type(Item* g, const std::vector<Type*>& args) {
        auto key = std::make_pair(g, args);
        if (auto it = type_instances_.find(key); it != type_instances_.end()) return it->second;
        std::string name = std::string(text(g->name)) + "<";
        for (std::size_t i = 0; i < args.size(); i++) {
            if (i) name += ", ";
            name += args[i]->name;
        }
        name += ">";
        std::string_view nm = ctx_.arena.copy(name);
        Type* t = g->kind == ItemKind::struct_ ? T.make_struct(nullptr, nm) : T.make_enum(nullptr, nm);
        Item* it = ctx_.arena.make<Item>(*g);
        it->origin = g;
        it->type_args = ctx_.arena.copy(args);
        it->type_params = {};
        it->declared = t;
        it->resolved = it->resolving = false;
        t->item = it;
        t->generic = g;
        t->args = it->type_args;
        for (Type* a : args) t->open = t->open || a->open;
        it->c_name = c_name_in(g->module, {mangle(t)});
        type_instances_.emplace(key, t);
        instances_.push_back(t);
        if (!g->resolved && !g->resolving) {
            if (g->kind == ItemKind::struct_) resolve_fields(g);
            else resolve_enum(g);
        }
        if (g->resolved) fill_instance(t);
        else unfilled_[g].push_back(t);
        return t;
    }

    void fill_instance(Type* t) {
        Item* g = t->generic;
        Item* it = t->item;
        std::vector<Type*> args(t->args.begin(), t->args.end());
        Subst s = binding(g->type_params, args);
        auto fields = [&](Slice<FieldDecl> from) {
            std::vector<FieldDecl> fs(from.begin(), from.end());
            for (FieldDecl& f : fs) {
                f.resolved = f.resolved ? subst(f.resolved, s) : T.error;
                // A default can have the parameters' types, as `none` for a `T?`, or mention them,
                // as `T.sides`, so a real instance checks its own copy.
                if (f.default_value && !t->open) f.default_value = clone(f.default_value);
            }
            return Slice<FieldDecl>(ctx_.arena.copy(fs));
        };
        if (g->kind == ItemKind::struct_) {
            it->fields = fields(g->fields);
            if (!t->open) {
                if (defaults_ready_) check_instance_defaults(t);
                else pending_defaults_.push_back(t);
            }
        } else {
            std::vector<VariantDecl> vs(g->variants.begin(), g->variants.end());
            for (VariantDecl& v : vs) {
                std::vector<Type*> ts;
                for (Type* x : v.types) ts.push_back(subst(x, s));
                v.types = ctx_.arena.copy(ts);
                v.fields = fields(v.fields);
            }
            it->variants = ctx_.arena.copy(vs);
            t->has_data = g->declared->has_data;
            t->repr = g->declared->repr;
        }
        it->resolved = true;
    }

    void fill_unfilled(Item* g) {
        auto it = unfilled_.find(g);
        if (it == unfilled_.end()) return;
        std::vector<Type*> ts = std::move(it->second);
        unfilled_.erase(it);
        for (Type* t : ts) fill_instance(t);
    }

    // The instance of a generic function for `args`, in `type_params` order: a copy of its body,
    // checked once the bodies written in the module are.
    Item* instance_fn(Item* f, const std::vector<Type*>& args, Span site) {
        auto key = std::make_pair(f, args);
        if (auto it = fn_instances_.find(key); it != fn_instances_.end()) return it->second;
        std::uint32_t depth = (fn_ ? fn_->instance_depth : 0) + 1;
        if (depth > 64) {
            ctx_.diags.error(site, "the instances of " + q(text(f->name)) + " never end: each one needs another, with bigger types");
            return nullptr;
        }
        Item* it = ctx_.arena.make<Item>(*f);
        it->origin = f;
        it->type_args = ctx_.arena.copy(args);
        it->type_params = {};
        it->generics = {};
        std::vector<Param> ps(f->params.begin(), f->params.end());
        for (Param& p : ps) {
            p.local = nullptr;
            p.default_value = clone(p.default_value);
        }
        it->params = ctx_.arena.copy(ps);
        it->body = nullptr;  // copied when it's checked
        it->checked_signature = false;
        it->is_method = false;
        it->used = true;
        it->instance_site = site;
        it->instance_depth = depth;
        Subst s = binding(f->type_params, args);
        it->impl_type = f->impl_type ? subst(f->impl_type, s) : nullptr;
        std::string base;
        if (it->impl_type) {
            base = mangle(it->impl_type) + "__";
            if (f->interface_item) base += std::string(text(f->interface_item->name)) + "__";
        }
        base += text(f->name);
        std::size_t own = f->generics.size();
        for (std::size_t k = args.size() - own; k < args.size(); k++) base += "__" + mangle(args[k]);
        it->c_name = c_name_in(f->module, {base});
        fn_instances_.emplace(key, it);
        check_signature(it);
        pending_fns_.push_back(it);
        return it;
    }

    // Deep copies of a body, for an instance to check on its own. The checker's results are left
    // out, since the instance gets its own.
    Block* clone(const Block* b) {
        if (!b) return nullptr;
        Block* c = ctx_.arena.make<Block>(*b);
        std::vector<Stmt*> ss;
        for (Stmt* x : b->stmts) ss.push_back(clone(x));
        c->stmts = ctx_.arena.copy(ss);
        c->tail = clone(b->tail);
        return c;
    }

    Stmt* clone(const Stmt* s) {
        if (!s) return nullptr;
        Stmt* c = ctx_.arena.make<Stmt>(*s);
        c->pattern = clone(s->pattern);
        c->value = clone(s->value);
        c->target = clone(s->target);
        c->else_ = clone(s->else_);
        c->else_block = clone(s->else_block);
        c->cond = clone(s->cond);
        c->body = clone(s->body);
        c->pool = clone(s->pool);
        c->local = nullptr;
        c->let_unwraps = false;
        c->loop_id = 0;
        c->elem_type = nullptr;
        return c;
    }

    Pattern* clone(const Pattern* p) {
        if (!p) return nullptr;
        Pattern* c = ctx_.arena.make<Pattern>(*p);
        std::vector<Pattern*> es;
        for (Pattern* x : p->elems) es.push_back(clone(x));
        c->elems = ctx_.arena.copy(es);
        std::vector<FieldPattern> fs(p->fields.begin(), p->fields.end());
        for (FieldPattern& f : fs) {
            f.pattern = clone(f.pattern);
            f.index = 0;
        }
        c->fields = ctx_.arena.copy(fs);
        c->value = clone(p->value);
        c->end = clone(p->end);
        c->type = nullptr;
        c->local = nullptr;
        c->variant = 0;
        return c;
    }

    Slice<Expr*> clone(const Slice<Expr*>& xs) {
        std::vector<Expr*> out;
        for (Expr* x : xs) out.push_back(clone(x));
        return ctx_.arena.copy(out);
    }

    Expr* clone(const Expr* e) {
        if (!e) return nullptr;
        Expr* c = ctx_.arena.make<Expr>(*e);
        c->interpolations = clone(e->interpolations);
        c->lhs = clone(e->lhs);
        c->rhs = clone(e->rhs);
        c->callee = clone(e->callee);
        c->trailing = clone(e->trailing);
        std::vector<Arg> args(e->args.begin(), e->args.end());
        for (Arg& a : args) a.value = clone(a.value);
        c->args = ctx_.arena.copy(args);
        std::vector<FieldInit> fields(e->fields.begin(), e->fields.end());
        for (FieldInit& f : fields) f.value = clone(f.value);
        c->fields = ctx_.arena.copy(fields);
        // A struct literal's `elems` are its field values, which the checker fills in again.
        c->elems = e->kind == ExprKind::struct_lit ? Slice<Expr*>{} : clone(e->elems);
        c->repeat = clone(e->repeat);
        c->block = clone(e->block);
        c->cond = clone(e->cond);
        c->let_pattern = clone(e->let_pattern);
        c->else_ = clone(e->else_);
        c->scrutinee = clone(e->scrutinee);
        std::vector<MatchArm> arms(e->arms.begin(), e->arms.end());
        for (MatchArm& a : arms) {
            a.pattern = clone(a.pattern);
            a.guard = clone(a.guard);
            a.body = clone(a.body);
        }
        c->arms = ctx_.arena.copy(arms);
        std::vector<Param> params(e->params.begin(), e->params.end());
        for (Param& p : params) {
            p.local = nullptr;
            p.default_value = clone(p.default_value);
        }
        c->params = ctx_.arena.copy(params);
        c->body = clone(e->body);
        c->type = nullptr;
        c->ref = Ref::none;
        c->local = nullptr;
        c->item = nullptr;
        c->named_type = nullptr;
        c->call = CallKind::none;
        c->builtin = Builtin::none;
        c->conv = Conv::none;
        c->prim = PrimMethod::none;
        c->field_index = 0;
        c->ordered_args = {};
        c->is_const = c->has_bits = false;
        c->const_bits = 0;
        c->is_nan_const = false;
        c->loop_id = 0;
        c->let_unwraps = false;
        return c;
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
                if (n == n_List_ && !env_type(n)) {
                    if (t->args.size() != 1) {
                        error(t->span, "`List` takes one generic argument, the type of its elements: `List<i32>`");
                        return T.error;
                    }
                    Type* elem = resolve_type(t->args[0]);
                    if (is_bad(elem)) return T.error;
                    if (elem->kind == TypeKind::unit || elem->kind == TypeKind::never) {
                        error(t->args[0]->span, "a list can't hold " + name_of(elem));
                        return T.error;
                    }
                    return T.list_of(elem);
                }
                if (Type* p = env_type(n)) {
                    if (!t->args.empty()) {
                        error(t->args[0]->span, q(text(n)) + " doesn't take generic arguments");
                        return T.error;
                    }
                    return p;
                }
                Type* r = T.primitive(text(n));
                if (!r) {
                    Item* it = n == n_Self_ && current_impl_type_ ? nullptr : find_item(n);
                    if (n == n_Self_ && current_impl_type_) {
                        r = current_impl_type_;
                    } else if (it && is_type_item(it)) {
                        r = it->declared;
                        if (it != current_item_) it->used = true;
                        if (!it->type_params.empty()) return generic_type(it, t->args, t->span);
                    } else if (it && it->kind == ItemKind::interface) {
                        it->used = true;
                        error(t->span, q(text(n)) + " is an interface, not a type; take a generic parameter with it as a "
                                                    "bound, such as " + q("<T: " + std::string(text(n)) + ">"));
                        return T.error;
                    } else if (it) {
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
            case TypeExprKind::tuple: {
                if (t->args.empty()) return T.unit;
                std::vector<Type*> elems;
                bool bad = false;
                for (TypeExpr* a : t->args) {
                    Type* et = resolve_type(a);
                    if (et->kind == TypeKind::unit || et->kind == TypeKind::never) {
                        error(a->span, "a tuple can't hold " + name_of(et));
                        bad = true;
                    }
                    bad = bad || is_bad(et);
                    elems.push_back(et);
                }
                if (elems.size() < 2) {
                    error(t->span, "a tuple needs at least two elements");
                    return T.error;
                }
                return bad ? T.error : T.tuple_of(elems);
            }
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
            case TypeExprKind::optional: {
                Type* inner = resolve_type(t->inner);
                if (is_bad(inner)) return T.error;
                // `T?` is fine whatever `T` stands for, even an optional: the nesting comes from the
                // generic code, not from writing `i32??`.
                if (t->inner->kind == TypeExprKind::named && t->inner->path.parts.size() == 1 &&
                    env_type(t->inner->path.parts[0])) {
                    return T.optional_of(inner);
                }
                return optional_type(inner, t->span);
            }
            case TypeExprKind::fn: not_yet(t->span, "function types"); return T.error;
            case TypeExprKind::pointer: not_yet(t->span, "pointers"); return T.error;
            case TypeExprKind::dyn: not_yet(t->span, "`dyn` types"); return T.error;
        }
        return T.error;
    }

    // `T?`, or an error for the types an optional can't hold.
    Type* optional_type(Type* inner, Span span) {
        if (inner->kind == TypeKind::optional && !instance_mode_) {
            error(span, "an optional can't hold another optional; use an enum to tell the cases apart");
            return T.error;
        }
        if (inner->kind == TypeKind::unit || inner->kind == TypeKind::never) {
            error(span, "an optional can't hold " + name_of(inner) + "; use a `bool`");
            return T.error;
        }
        return T.optional_of(inner);
    }

    std::string path_text(const Path& p) const {
        std::string s;
        for (std::size_t i = 0; i < p.parts.size(); i++) {
            if (i) s += '.';
            s += text(p.parts[i]);
        }
        return s;
    }

    // Read parameters of these types are passed as views, since copying them could be costly.
    static bool is_view_type(const Type* t) {
        switch (t->kind) {
            case TypeKind::struct_: case TypeKind::array: case TypeKind::tuple: case TypeKind::string:
            case TypeKind::list: case TypeKind::param:
                return true;
            case TypeKind::enum_: return t->has_data;
            case TypeKind::optional: return is_view_type(t->elem);
            default: return false;
        }
    }

    void check_signature(Item* f) {
        if (f->checked_signature) return;
        f->checked_signature = true;
        InModule in(*this, f);
        Type* saved_impl = current_impl_type_;
        std::size_t mark = push_env(f);
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
        if (exported(f)) {
            for (const Param& p : f->params) {
                if (p.local && p.type) public_type(p.local->type, p.type->span, "a `pub` function can't take it");
            }
            if (f->result) public_type(f->result_type, f->result->span, "a `pub` function can't return it");
        }
        if (f->result && f->result_type->kind == TypeKind::unit) {
            error(f->result->span, "leave out `-> ()`: a function without `->` returns nothing");
        }
        pop_env(mark);
        current_impl_type_ = saved_impl;
    }

    void check_const(Item* c) {
        if (c->declared) return;
        InModule in(*this, c);
        if (checking_consts_.count(c)) {
            error(c->name_span, "constant " + q(text(c->name)) + " refers to itself");
            c->declared = T.error;
            return;
        }
        if (!c->type && !c->result_type) {
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
        current_impl_type_ = c->impl_type;
        Type* t = c->type ? resolve_type(c->type) : c->result_type;
        if (c->type && c->result_type && !is_bad(t) && !is_bad(c->result_type) && t != c->result_type) {
            error(c->type->span, q(text(c->name)) + " is " + a_type(c->result_type) + " in " + q(text(c->interface_item->name)));
            t = T.error;
        }
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
        InModule in(*this, f);
        fn_ = f;
        current_impl_type_ = f->impl_type;
        std::size_t mark = push_env(f);
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
        pop_env(mark);
        // A generic function is only checked here. Its instances are what's emitted.
        if (f->type_params.empty()) prog_.fns.push_back(f);
        else report_unused_params(f, "is never used");
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
        if (instance_mode_) return;  // the generic definition reported these once
        std::string_view n = text(l->name);
        if (l->is_param) {
            // An interface fixes its functions' parameters, names included, so one impl not using
            // a parameter is fine.
            if (fn_ && fn_->interface_item) return;
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
        } else if (Item* it = find_item(l->name)) {
            error(l->span, q(text(l->name)) + " is already the name of a " + item_kind(it) +
                               "; Calx doesn't allow shadowing");
            note(it->name_span, "it's defined here");
            it->used = true;
        } else if (is_builtin_name(l->name)) {
            error(l->span, q(text(l->name)) + " is a built-in name");
        }
        visible_.push_back(l);
    }

    static const char* item_kind(const Item* it) {
        switch (it->kind) {
            case ItemKind::fn: return "function";
            case ItemKind::struct_: return "struct";
            case ItemKind::enum_: return "enum";
            case ItemKind::const_: return "constant";
            case ItemKind::interface: return "interface";
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
            case ExprKind::match: t = check_match(e, nullptr, true); break;
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
        bool has_else = s->else_ || s->else_block;
        if (p->kind == PatternKind::wildcard) {
            error(p->span, has_else ? "check for `none` with `if x == none { ... }`" : "write `_ = ...` to discard a value");
            if (s->value) check(s->value, nullptr);
            return false;
        }
        if (p->is_mut) {
            error(p->span, "declare a variable that changes with `var`, not `let mut`");
        }
        // A capitalized name can be a variant (`let Empty = s else return`), unless it's already
        // declared, which gets the clearer error about reusing the name.
        bool plain = p->kind == PatternKind::binding && (!std::isupper(static_cast<unsigned char>(text(p->name)[0])) ||
                                                         resolve_name(p->name).kind != NameKind::none);
        if (has_else || !plain) return check_let_pattern(s);
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

    // `let p = e`, where `p` is more than a name, and `let p = e else <leave>`.
    bool check_let_pattern(Stmt* s) {
        Pattern* p = s->pattern;
        bool has_else = s->else_ || s->else_block;
        Type* declared = s->type ? resolve_type(s->type) : nullptr;
        if (!s->value) {
            error(p->span, "give the pattern a value: `let ... = value`");
            return false;
        }
        Type* vt;
        if (has_else && declared && !is_bad(declared)) {
            // The type given is the pattern's: the optional's value, for an optional.
            vt = check(s->value, nullptr);
            if (!is_bad(vt) && vt != declared && !(vt->kind == TypeKind::optional && vt->elem == declared)) {
                error(s->value->span, "expected " + name_of(declared) + " or " + name_of(T.optional_of(declared)) +
                                          ", found " + name_of(vt));
                vt = T.error;
            }
        } else {
            vt = check(s->value, declared);
        }
        if (vt->kind == TypeKind::never) vt = T.error;
        if (vt->kind == TypeKind::unit) {
            error(s->value->span, "this has no value to bind");
            vt = T.error;
        }
        Type* pt = vt;
        if (has_else && vt->kind == TypeKind::optional) {
            s->let_unwraps = true;
            pt = vt->elem;
            if (reject_optional_pattern(p, "`let x = opt else` already unwraps the optional; match its value directly")) {
                pt = T.error;  // its names are declared without another error
            }
        }
        // The `else` can't see the pattern's names, so it's checked first.
        if (has_else) {
            Type* et = s->else_block ? check_block(s->else_block, nullptr, BlockUse::stmt) : check_expr_stmt(s->else_, false);
            if (et->kind != TypeKind::never && !is_bad(et)) {
                Span es = s->else_block ? s->else_block->span : s->else_->span;
                error(es, "the `else` of `let ... else` must leave: `return`, `break`, `continue` or a panic");
            }
        }
        pattern_var_ = s->is_var;
        bool ok = check_pattern(p, pt);
        pattern_var_ = false;
        if (p->kind == PatternKind::binding) s->local = p->local;
        if (!ok || is_bad(pt)) return false;
        std::string missing;
        bool all = irrefutable(p, pt, &missing);
        if (!has_else && !all) {
            error(p->span, "this pattern doesn't match every " + name_of(pt) + ", such as " + q(missing) +
                               "; add `else` to leave when it doesn't");
        } else if (has_else && all && !s->let_unwraps) {
            error(p->span, "this pattern matches every " + name_of(pt) + "; remove the `else`");
        }
        return false;
    }

    // `some(x)` and `none` in a pattern that already unwraps an optional.
    bool reject_optional_pattern(Pattern* p, const char* msg) {
        bool some = p->kind == PatternKind::variant && p->path.parts.size() == 1 && text(p->path.parts[0]) == "some";
        if (some || p->kind == PatternKind::none) error(p->span, msg);
        return some || p->kind == PatternKind::none;
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
            if (l->mode == ParamMode::own) {
                if (whole) l->reassigned = true;
                else l->mutated = true;
                return;
            }
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
            if (t->kind == TypeKind::array || t->kind == TypeKind::list) {
                elem = t->elem;
                borrowed = place_root(it);
            } else if (!is_bad(t)) {
                error(it->span, "a `for` loop goes over a range, an array or a list, not " + name_of(t));
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
        } else if (p->kind != PatternKind::wildcard) {
            if (check_pattern(p, elem) && !is_bad(elem)) {
                std::string missing;
                if (!irrefutable(p, elem, &missing)) {
                    error(p->span, "a `for` loop's pattern must match every element, but this one misses " + q(missing));
                }
            }
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
        } else if (want == T.string && got == T.str) {
            msg += "; make one with " + q("String(...)");
        } else if (want == T.str && got == T.string) {
            msg += "; take a `String` here instead";
        } else if (want->kind == TypeKind::optional && want->elem == got) {
            msg += "; a value doesn't become an optional by itself: write " + q("some(...)");
        } else if (got->kind == TypeKind::optional && got->elem == want) {
            msg += is_some_call(e) ? std::string("; `some(...)` makes an optional, so remove it")
                                   : std::string("; unwrap it with `??`, `!`, `if let` or `let ... else`");
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

    bool is_untyped_literal(const Expr* e) const {
        if (e->kind == ExprKind::int_lit || e->kind == ExprKind::float_lit || e->kind == ExprKind::char_lit) {
            return e->literal.suffix.empty();
        }
        if (e->kind == ExprKind::unary && e->op == Op::neg) return is_untyped_literal(e->lhs);
        if (e->kind == ExprKind::none_lit) return true;
        if (is_some_call(e)) return is_untyped_literal(e->args[0].value);
        return false;
    }

    bool is_some_call(const Expr* e) const {
        return e->kind == ExprKind::call && e->callee->kind == ExprKind::name && e->args.size() == 1 &&
               text(e->callee->path.parts[0]) == "some" && !find_local(e->callee->path.parts[0]);
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
            case ExprKind::string_lit: return string_literal(e, expected, false);
            case ExprKind::none_lit:
                e->is_const = true;
                if (expected && expected->kind == TypeKind::optional) return expected;
                if (expected && !is_bad(expected)) {
                    error(e->span, "expected " + name_of(expected) + ", found `none`, which is an optional");
                } else if (!expected) {
                    error(e->span, "can't tell what kind of optional `none` is; give it a type, such as "
                                   "`let x: i32? = none`");
                }
                return T.error;
            case ExprKind::name:
            case ExprKind::self: return name_value(e);
            case ExprKind::unary: return unary(e, expected);
            case ExprKind::binary: return binary(e, expected);
            case ExprKind::range:
                error(e->span, "a range can only be used in a `for` loop for now");
                return T.error;
            case ExprKind::call: return call(e, expected);
            case ExprKind::field: return field(e, expected);
            case ExprKind::index: {
                Type* base = check(e->lhs, nullptr);
                return index_type(e, base);
            }
            case ExprKind::struct_lit: return struct_literal(e, expected);
            case ExprKind::array_lit: return array_literal(e, expected);
            case ExprKind::tuple: return tuple_literal(e, expected);
            case ExprKind::block: return check_block(e->block, expected, BlockUse::value);
            case ExprKind::if_: return check_if(e, expected, false);
            case ExprKind::loop: return check_loop(e, expected, false);
            case ExprKind::match: return check_match(e, expected, false);
            case ExprKind::closure: not_yet(e->span, "closures"); return T.error;
            case ExprKind::optional_field: return optional_field(e);
            case ExprKind::unwrap: {
                Type* t = check(e->lhs, nullptr);
                if (is_bad(t)) return T.error;
                if (t->kind != TypeKind::optional) {
                    error(e->span, "`!` unwraps an optional, but this is " + a_type(t));
                    return T.error;
                }
                return t->elem;
            }
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

    // A string literal is a `str`, or a `String` where one is expected, which costs nothing since
    // the `String` doesn't own the literal's bytes. One with `{...}` parts is a `String` built when
    // it runs, except as the message of `print` and the others, which print the parts (`printed`).
    Type* string_literal(Expr* e, Type* expected, bool printed) {
        if (e->literal.name) {
            not_yet(e->span, "string prefixes");
            return T.error;
        }
        for (Expr* part : e->interpolations) {
            Type* t = check(part, nullptr);
            if (!is_bad(t) && !printable(t)) error(part->span, name_of(t) + " can't be shown in a string yet");
        }
        e->is_const = e->interpolations.empty();
        if (printed) return T.str;
        if (!e->interpolations.empty() || expected == T.string) return T.string;
        return T.str;
    }

    // What `{x}` in a string can show.
    static bool printable(const Type* t) {
        switch (t->kind) {
            case TypeKind::int_: case TypeKind::float_: case TypeKind::bool_: case TypeKind::char_:
            case TypeKind::rune: case TypeKind::str: case TypeKind::string: return true;
            case TypeKind::enum_: return !t->has_data;
            case TypeKind::list: return printable(t->elem);
            default: return false;
        }
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
        if (Type* p = env_type(n)) {
            r.kind = NameKind::type;
            r.type = p;
            return r;
        }
        if (Item* it = find_item(n)) {
            if (is_type_item(it)) {
                r.kind = NameKind::type;
                r.type = it->declared;
                r.item = it;
            } else {
                r.kind = NameKind::item;
                r.item = it;
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
        if (it->origin) it = it->origin;  // using an instance uses its generic item
        if (fn_ != it && current_item_ != it && !(fn_ && fn_->origin == it)) it->used = true;
    }

    Type* const_ref(Expr* e, Item* c) {
        check_const(c);
        e->ref = Ref::const_;
        e->item = c;
        e->is_const = true;
        if (c->value && c->value->has_bits) {
            e->has_bits = true;
            e->const_bits = c->value->const_bits;
            e->is_nan_const = c->value->is_nan_const;
        }
        return c->declared;
    }

    // A type name with generic arguments, `Pair<i32, str>`, as an expression's start.
    Type* type_of_name(Expr* n, Type* t) {
        if (n->type_args.empty()) return t;
        if (!is_identity(t)) {
            error(n->type_args[0]->span, name_of(t) + " doesn't take generic arguments");
            return T.error;
        }
        return generic_type(t->generic, n->type_args, n->span);
    }

    // After inference: the instance of `g` that `s` gives, or an error if a parameter is unknown.
    Type* inferred_instance(Item* g, const Subst& s, Span span, const std::string& example) {
        std::vector<Type*> args;
        for (Type* p : g->type_params) {
            auto it = s.find(p);
            if (it == s.end()) {
                error(span, "can't tell what " + q(p->name) + " is in this " + q(text(g->name)) + "; give its types, as in " +
                                q(example) + ", or give the variable a type");
                return T.error;
            }
            args.push_back(it->second);
        }
        if (!check_bounds(g->type_params, args, span, q(text(g->name)))) return T.error;
        return instance_type(g, args);
    }

    Type* name_value(Expr* e) {
        Name n = e->kind == ExprKind::self ? n_self_ : e->path.parts[0];
        if (n == n_underscore_) {
            error(e->span, "`_` can only be assigned to, to discard a value: `_ = ...`");
            return T.error;
        }
        Resolved r = resolve_name(n);
        if (!e->type_args.empty() && r.kind != NameKind::type) {
            error(e->type_args[0]->span, q(text(n)) + " doesn't take generic arguments here");
            return T.error;
        }
        switch (r.kind) {
            case NameKind::local:
                e->ref = Ref::local;
                e->local = r.local;
                r.local->used = true;
                return r.local->type;
            case NameKind::item:
                use_item(r.item);
                if (r.item->kind == ItemKind::const_) return const_ref(e, r.item);
                if (r.item->kind == ItemKind::interface) {
                    error(e->span, q(text(n)) + " is an interface, not a value");
                    return T.error;
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

    // Whether `==` and `!=` work on the type.
    static bool equatable(const Type* t) {
        switch (t->kind) {
            case TypeKind::bool_: case TypeKind::int_: case TypeKind::float_: case TypeKind::char_:
            case TypeKind::rune: case TypeKind::str: case TypeKind::string: return true;
            case TypeKind::enum_: return !t->has_data;
            case TypeKind::optional: return equatable(t->elem);
            default: return false;
        }
    }

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
        if (op == Op::coalesce) return coalesce(e, expected);
        bool compare = op == Op::eq || op == Op::ne || op == Op::lt || op == Op::le || op == Op::gt || op == Op::ge;
        if (compare) {
            Type* t = check_pair(e->lhs, e->rhs, nullptr);
            if (is_bad(t)) return T.bool_;
            bool ordered = op != Op::eq && op != Op::ne;
            bool ok = is_numeric(t) || t->kind == TypeKind::char_ || t->kind == TypeKind::rune ||
                      (!ordered && equatable(t));
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

    Type* field(Expr* e, Type* expected) {
        // `i32.MAX` and other names inside a type.
        if (e->lhs->kind == ExprKind::name) {
            Resolved r = resolve_name(e->lhs->path.parts[0]);
            if (r.kind == NameKind::type) {
                Type* t = type_of_name(e->lhs, r.type);
                if (is_bad(t)) return T.error;
                e->lhs->ref = Ref::type;
                e->lhs->named_type = t;
                return associated(e, t, expected);
            }
        }
        Type* base = check(e->lhs, nullptr);
        return field_type(e, base);
    }

    Type* field_type(Expr* e, Type* base) {
        if (is_bad(base)) return T.error;
        if (e->is_tuple_index) {
            if (base->kind != TypeKind::tuple) {
                error(e->span, name_of(base) + " has no numbered fields");
                return T.error;
            }
            if (e->tuple_index >= base->elems.size()) {
                error(e->span, name_of(base) + " has no element " + std::to_string(e->tuple_index));
                return T.error;
            }
            e->field_index = e->tuple_index;
            return base->elems[e->tuple_index];
        }
        if (base->kind == TypeKind::optional) {
            error(e->span, name_of(base) + " is an optional; read its field with `?.`, or unwrap it first");
            return T.error;
        }
        if (base->kind == TypeKind::struct_) {
            Item* s = base->item;
            for (std::uint32_t i = 0; i < s->fields.size(); i++) {
                if (s->fields[i].name == e->member) {
                    e->field_index = i;
                    if (!field_visible(s, s->fields[i])) private_field(e->span, s, s->fields[i]);
                    return s->fields[i].resolved;
                }
            }
            if (lookup(base, e->member).fn) {
                error(e->span, q(text(e->member)) + " is a method; call it: " + q(std::string(text(e->member)) + "()"));
                return T.error;
            }
            error(e->span, name_of(base) + " has no field " + q(text(e->member)));
            return T.error;
        }
        error(e->span, name_of(base) + " has no fields");
        return T.error;
    }

    Type* associated(Expr* e, Type* t, Type* expected) {
        std::string_view m = text(e->member);
        // A data-less variant of a generic enum, `Either.Nothing`, takes its types from where it's used.
        if (t->kind == TypeKind::enum_ && is_identity(t)) {
            std::uint32_t i = find_variant(t, e->member);
            if (i != UINT32_MAX && t->item->variants[i].types.empty()) {
                if (!expected || expected->generic != t->generic) {
                    error(e->span, "can't tell what types this " + q(text(t->generic->name)) + " has; give them, as in " +
                                       q(std::string(text(t->generic->name)) + "<...>." + std::string(m)) +
                                       ", or give the variable a type");
                    return T.error;
                }
                t = expected;
            }
        }
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
        if (t->kind == TypeKind::enum_) {
            std::uint32_t i = find_variant(t, e->member);
            if (i != UINT32_MAX) {
                use_item(t->item);
                const VariantDecl& v = t->item->variants[i];
                if (!v.types.empty()) {
                    error(e->span, q(variant_text(t, i)) + " holds data; write " +
                                       q(variant_text(t, i) + (v.has_fields ? " { ... }" : "(...)")));
                    return T.error;
                }
                e->ref = Ref::variant;
                e->field_index = i;
                if (!t->has_data) {
                    e->is_const = e->has_bits = true;
                    e->const_bits = v.tag;
                }
                return t;
            }
        }
        Member mem = lookup(t, e->member, e->only_interface);
        if (t->kind == TypeKind::param && mem.konst && !mem.ambiguous) e->only_interface = mem.iface;
        if (mem.ambiguous) {
            error(e->span, q(m) + " is in more than one interface " + name_of(t) + " implements");
            return T.error;
        }
        if (mem.konst) {
            if (t->kind == TypeKind::param) {
                // Its value is known only once there's a type for `T`, but it's a constant in
                // every instance, so it can be a field's default.
                e->is_const = true;
                return subst(mem.konst->declared, mem.s);
            }
            use_item(mem.konst);
            return const_ref(e, mem.konst);
        }
        if (mem.fn) {
            error(e->span, q(std::string(t->name) + "." + std::string(m)) + " is a function; call it");
            return T.error;
        }
        error(e->span, name_of(t) + (t->kind == TypeKind::enum_ ? " has no variant " : " has nothing named ") + q(m));
        return T.error;
    }

    Type* index_type(Expr* e, Type* base) {
        Type* it = check(e->rhs, T.usize);
        if (is_bad(base)) return T.error;
        if (base->kind == TypeKind::list) return base->elem;
        if (base->kind != TypeKind::array) {
            error(e->lhs->span, name_of(base) + " can't be indexed");
            return T.error;
        }
        if (!is_bad(it) && e->rhs->has_bits && e->rhs->const_bits >= base->length) {
            error(e->rhs->span, "index " + std::to_string(e->rhs->const_bits) + " is out of bounds for " + name_of(base));
        }
        return base->elem;
    }

    // --- optionals and tuples -------------------------------------------------------------------

    Type* tuple_literal(Expr* e, Type* expected) {
        if (e->elems.empty()) return T.unit;
        if (e->elems.size() < 2) {
            error(e->span, "a tuple needs at least two elements");
            for (Expr* x : e->elems) check(x, nullptr);
            return T.error;
        }
        bool fit = expected && expected->kind == TypeKind::tuple && expected->elems.size() == e->elems.size();
        std::vector<Type*> types;
        bool bad = false;
        for (std::size_t i = 0; i < e->elems.size(); i++) {
            Type* t = check(e->elems[i], fit ? expected->elems[i] : nullptr);
            if (t->kind == TypeKind::unit || t->kind == TypeKind::never) {
                error(e->elems[i]->span, "a tuple can't hold " + name_of(t));
                bad = true;
            }
            bad = bad || is_bad(t);
            types.push_back(t);
        }
        if (bad) return T.error;
        return T.tuple_of(types);
    }

    Type* some_call(Expr* e, Type* expected) {
        e->call = CallKind::some;
        if (e->args.size() != 1 || e->args[0].name || e->args[0].is_mut) {
            error(e->span, "`some` takes one value");
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        Type* want = expected && expected->kind == TypeKind::optional ? expected->elem : nullptr;
        Type* t = check(e->args[0].value, want);
        if (is_bad(t)) return T.error;
        if (t->kind == TypeKind::never) return want ? expected : T.error;
        e->is_const = e->args[0].value->is_const;
        return optional_type(t, e->span);
    }

    // `a ?? b`: the optional's value, or `b` when it's empty. `b` runs only then.
    Type* coalesce(Expr* e, Type* expected) {
        if (expected && (is_bad(expected) || expected->kind == TypeKind::never || expected->kind == TypeKind::unit)) {
            expected = nullptr;
        }
        Type* lhs_want = nullptr;
        if (expected) lhs_want = expected->kind == TypeKind::optional ? expected : T.optional_of(expected);
        Type* lt = check(e->lhs, lhs_want);
        if (is_bad(lt)) {
            check(e->rhs, nullptr);
            return T.error;
        }
        if (lt->kind != TypeKind::optional) {
            error(e->lhs->span, "`??` needs an optional on the left, but this is " + a_type(lt));
            check(e->rhs, nullptr);
            return T.error;
        }
        Type* rhs_want = expected;
        if (!rhs_want && e->rhs->kind == ExprKind::string_lit && lt->elem->kind == TypeKind::string) {
            rhs_want = lt->elem;  // a literal is a `String` where one is wanted
        } else if (!rhs_want && is_untyped_literal(e->rhs)) {
            // `none` or `some(...)` on the right chains to another optional, unless the value is
            // itself an optional (from generic code), where flattening is the only use: `r ?? none`.
            bool optional_literal = e->rhs->kind == ExprKind::none_lit || is_some_call(e->rhs);
            rhs_want = optional_literal && lt->elem->kind != TypeKind::optional ? lt : lt->elem;
        }
        Type* rt = check(e->rhs, rhs_want);
        if (is_bad(rt)) return T.error;
        if (rt->kind == TypeKind::never || rt == lt->elem) return lt->elem;
        if (rt == lt) return lt;
        error(e->rhs->span, "`??` needs " + a_type(lt->elem) + " or " + a_type(lt) + " on the right, not " +
                                a_type(rt));
        return T.error;
    }

    // `a?.b`: the field of the value, or `none`. A field that's already an optional isn't wrapped again.
    Type* optional_field(Expr* e) {
        Type* t = check(e->lhs, nullptr);
        if (is_bad(t)) return T.error;
        if (t->kind != TypeKind::optional) {
            error(e->span, "`?.` reads a field of an optional, but this is " + a_type(t) + "; write `.`");
            return T.error;
        }
        Type* ft = field_type(e, t->elem);
        if (is_bad(ft)) return T.error;
        return ft->kind == TypeKind::optional ? ft : optional_type(ft, e->span);
    }

    // --- literals of structs and arrays ------------------------------------------------------

    Type* struct_literal(Expr* e, Type* expected) {
        if (e->type_path.parts.size() == 2) {
            Resolved r = resolve_name(e->type_path.parts[0]);
            if (r.kind == NameKind::type && r.type->kind == TypeKind::enum_) {
                if (!e->type_args.empty()) {
                    r.type = is_identity(r.type) ? generic_type(r.type->generic, e->type_args, e->type_path.span) : T.error;
                    if (is_bad(r.type)) {
                        for (FieldInit& f : e->fields) if (f.value) check(f.value, nullptr);
                        return T.error;
                    }
                }
                std::uint32_t i = find_variant(r.type, e->type_path.parts[1]);
                if (i != UINT32_MAX && r.type->item->variants[i].has_fields) return record_literal(e, r.type, i, expected);
                error(e->type_path.span, i == UINT32_MAX ? name_of(r.type) + " has no variant " + q(text(e->type_path.parts[1]))
                                                         : q(variant_text(r.type, i)) + " has no named fields");
                for (FieldInit& f : e->fields) if (f.value) check(f.value, nullptr);
                return T.error;
            }
        }
        if (e->type_path.parts.size() != 1) {
            error(e->type_path.span, "unknown type " + q(path_text(e->type_path)));
            return T.error;
        }
        Resolved r = resolve_name(e->type_path.parts[0]);
        if (r.kind == NameKind::type && r.type->kind == TypeKind::enum_) {
            error(e->type_path.span, "name the variant: " + q(std::string(r.type->name) + ".Variant { ... }"));
            for (FieldInit& f : e->fields) if (f.value) check(f.value, nullptr);
            return T.error;
        }
        if (r.kind != NameKind::type || r.type->kind != TypeKind::struct_) {
            error(e->type_path.span, q(path_text(e->type_path)) + " isn't a struct");
            for (FieldInit& f : e->fields) {
                if (f.value) check(f.value, nullptr);
                else if (Local* l = find_local(f.name)) l->used = true;
            }
            return T.error;
        }
        Type* t = r.type;
        if (!e->type_args.empty()) {
            t = is_identity(t) ? generic_type(t->generic, e->type_args, e->type_path.span) : T.error;
            if (is_bad(t)) {
                if (!is_identity(r.type)) error(e->type_args[0]->span, name_of(r.type) + " doesn't take generic arguments");
                for (FieldInit& f : e->fields) if (f.value) check(f.value, nullptr);
                return T.error;
            }
        }
        Item* s = t->item;
        use_item(s->origin ? s->origin : s);
        // `Pair { a: 1, b: "x" }`: the types come from the fields, or from where the value goes.
        bool infer = is_identity(t);
        Subst sub;
        Slice<Type*> vars = infer ? s->type_params : Slice<Type*>{};
        if (infer && expected && expected->generic == s) unify(t, expected, sub, vars);
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
                error(f.span, q(is_identity(t) ? text(s->name) : t->name) + " has no field " + q(text(f.name)));
                if (v->kind != ExprKind::none_lit) check(v, nullptr);  // `none` alone would be a second error
                continue;
            }
            if (given[index]) {
                error(f.span, "field " + q(text(f.name)) + " is given twice");
                continue;
            }
            given[index] = true;
            if (!field_visible(s, s->fields[index])) private_field(f.span, s, s->fields[index]);
            check_inferred(v, s->fields[index].resolved, sub, vars);
            values[index] = v;
        }
        bool missing = false;
        for (std::uint32_t i = 0; i < s->fields.size(); i++) {
            if (!given[i] && !s->fields[i].default_value) {
                if (!field_visible(s, s->fields[i])) {
                    error(e->span, q(text(s->name)) + " can only be built in " + q(module_name(s->module)) + ", since its field " +
                                       q(text(s->fields[i].name)) + " is private there and has no default");
                } else {
                    error(e->span, "missing field " + q(text(s->fields[i].name)) + " in " + q(text(s->name)));
                }
                missing = true;
            }
        }
        e->elems = ctx_.arena.copy(values);
        if (infer && missing) return T.error;
        if (infer) return inferred_instance(s, sub, e->type_path.span, std::string(text(s->name)) + "<...> { ... }");
        return t;
    }

    // Checks `v` against `want`, which can mention the parameters in `vars` that are being worked
    // out. A parameter `s` already gives becomes the expected type; one it doesn't is learned from `v`.
    Type* check_inferred(Expr* v, Type* want, Subst& s, const Slice<Type*>& vars) {
        if (vars.empty() || !mentions_any(want, vars)) return check(v, want);
        Type* w = subst(want, s);
        if (!unresolved(w, vars, s)) return check(v, w);
        Type* got = check(v, nullptr);
        if (!is_bad(got) && !unify(want, got, s, vars)) {
            mismatch(v, subst(want, s), got);
            return T.error;
        }
        return got;
    }

    // `Shape.Rect { w: 1.0, h: 2.0 }`: like a struct literal, against the variant's fields.
    Type* record_literal(Expr* e, Type* t, std::uint32_t vi, Type* expected) {
        use_item(t->item->origin ? t->item->origin : t->item);
        bool infer = is_identity(t);
        Subst sub;
        Slice<Type*> vars = infer ? t->item->type_params : Slice<Type*>{};
        if (infer && expected && expected->generic == t->generic) unify(t, expected, sub, vars);
        const VariantDecl& v = t->item->variants[vi];
        e->ref = Ref::variant;
        e->field_index = vi;
        std::vector<Expr*> values(v.fields.size(), nullptr);
        for (FieldInit& f : e->fields) {
            std::uint32_t index = UINT32_MAX;
            for (std::uint32_t i = 0; i < v.fields.size(); i++) {
                if (v.fields[i].name == f.name) index = i;
            }
            if (!f.value) {
                Expr* x = ctx_.arena.make<Expr>();
                x->kind = ExprKind::name;
                x->span = f.span;
                std::vector<Name> one{f.name};
                x->path.parts = ctx_.arena.copy(one);
                x->path.span = f.span;
                f.value = x;
            }
            if (index == UINT32_MAX) {
                error(f.span, q(variant_text(t, vi)) + " has no field " + q(text(f.name)));
                check(f.value, nullptr);
                continue;
            }
            if (values[index]) {
                error(f.span, "field " + q(text(f.name)) + " is given twice");
                continue;
            }
            check_inferred(f.value, v.fields[index].resolved, sub, vars);
            values[index] = f.value;
        }
        bool missing = false;
        for (std::uint32_t i = 0; i < v.fields.size(); i++) {
            if (!values[i]) {
                error(e->span, "missing field " + q(text(v.fields[i].name)) + " in " + q(variant_text(t, vi)));
                missing = true;
            }
        }
        e->elems = ctx_.arena.copy(values);
        if (infer && missing) return T.error;
        if (infer) {
            return inferred_instance(t->generic, sub, e->type_path.span,
                                     std::string(text(t->generic->name)) + "<...>." + std::string(text(v.name)) + " { ... }");
        }
        return t;
    }

    Type* array_literal(Expr* e, Type* expected) {
        if (expected && expected->kind == TypeKind::list) return list_literal(e, expected);
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

    // `[a, b]` where a list is expected. `[]` is an empty list, which allocates nothing.
    Type* list_literal(Expr* e, Type* list) {
        Type* want = list->elem;
        if (e->repeat) {
            Type* elem = check(e->elems[0], want);
            check(e->repeat, T.usize);
            if (!is_bad(elem) && !is_copy(elem) && !instance_mode_) {
                error(e->elems[0]->span, "`[x; n]` copies `x`, but " + name_of(elem) +
                                             " isn't `Copy`; push copies in a loop instead");
            }
            return list;
        }
        for (Expr* x : e->elems) check(x, want);
        e->is_const = e->elems.empty();
        return list;
    }

    // --- control flow ------------------------------------------------------------------------

    Type* check_if(Expr* e, Type* expected, bool stmt) {
        Type* a = nullptr;
        if (e->let_pattern) {
            // `if let p = v`: the pattern's names are visible in the first block only.
            Pattern* p = e->let_pattern;
            Type* vt = check(e->cond, nullptr);
            if (vt->kind == TypeKind::never) vt = T.error;
            Type* pt = vt;
            if (vt->kind == TypeKind::optional) {
                e->let_unwraps = true;
                pt = vt->elem;
                if (reject_optional_pattern(p, "`if let x = opt` already unwraps the optional; match its value directly")) {
                    pt = T.error;
                }
            }
            push_scope();
            if (check_pattern(p, pt) && !is_bad(pt) && !e->let_unwraps && irrefutable(p, pt, nullptr)) {
                error(p->span, "this pattern matches every " + name_of(pt) + "; use `let`");
            }
            a = check_block(e->block, stmt ? nullptr : expected, stmt ? BlockUse::stmt : BlockUse::value);
            pop_scope();
        } else {
            check_condition(e->cond);
            if (e->cond->kind == ExprKind::bool_lit && e->cond->bool_value) {
                error(e->cond->span, "this condition is always true");
            }
        }
        if (stmt) {
            if (!a) a = check_block(e->block, nullptr, BlockUse::stmt);
            if (!e->else_) return T.unit;
            Type* b = e->else_->kind == ExprKind::if_ ? check_if(e->else_, nullptr, true)
                                                      : check_block(e->else_->block, nullptr, BlockUse::stmt);
            e->else_->type = b;
            return a->kind == TypeKind::never && b->kind == TypeKind::never ? T.never : T.unit;
        }
        if (!e->else_) {
            if (!a) check_block(e->block, expected, BlockUse::value);
            error(e->span, "an `if` used as a value needs an `else`");
            return T.error;
        }
        if (!a) a = check_block(e->block, expected, BlockUse::value);
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

    // --- match ------------------------------------------------------------------------------

    Type* check_match(Expr* e, Type* expected, bool stmt) {
        Type* st = check(e->scrutinee, nullptr);
        if (st->kind == TypeKind::never) st = T.error;
        e->loop_id = next_loop_++;
        if (e->arms.empty()) {
            error(e->span, "a `match` needs at least one arm");
            return T.error;
        }
        bool patterns_ok = !is_bad(st);
        Type* result = nullptr;
        bool all_never = true, any_bad = false;
        for (MatchArm& arm : e->arms) {
            push_scope();
            patterns_ok = check_pattern(arm.pattern, st) && patterns_ok;
            if (arm.guard) check_condition(arm.guard);
            Type* t;
            if (stmt) {
                t = check_expr_stmt(arm.body, false);
            } else {
                t = check(arm.body, expected ? expected : result);
                if (!result && !is_bad(t) && t->kind != TypeKind::never) result = t;
            }
            if (is_bad(t)) any_bad = true;
            if (t->kind != TypeKind::never) all_never = false;
            pop_scope();
        }
        if (patterns_ok) check_arms(e, st);
        if (stmt) return all_never ? T.never : T.unit;
        if (any_bad) return T.error;
        if (all_never) return T.never;
        return result ? result : T.error;
    }

    // Reports arms that can never match, and a `match` that misses a case (D63).
    void check_arms(Expr* e, Type* st) {
        pool_.clear();
        std::vector<Row> rows;
        std::vector<Type*> types{st};
        for (MatchArm& arm : e->arms) {
            Row q{reduce(arm.pattern)};
            if (!useful(rows, q, types, nullptr)) {
                error(arm.pattern->span, "this arm never matches: the arms before it cover every value it would");
            }
            if (!arm.guard) rows.push_back(q);
        }
        Row w;
        if (useful(rows, Row{wild()}, types, &w)) {
            Span at{e->span.file, e->span.begin, e->scrutinee->span.end};
            error(at, "this `match` doesn't cover " + q(render(w[0], st)) + "; add an arm for it, or `_ =>` for the rest");
        }
    }

    // Whether the pattern matches every value of `t`. If not, `missing` gets one it misses.
    bool irrefutable(Pattern* p, Type* t, std::string* missing) {
        pool_.clear();
        std::vector<Row> rows{Row{reduce(p)}};
        Row w;
        if (!useful(rows, Row{wild()}, std::vector<Type*>{t}, missing ? &w : nullptr)) return true;
        if (missing) *missing = render(w[0], t);
        return false;
    }

    // --- patterns ----------------------------------------------------------------------------

    // Checks a pattern against a value of type `t`, and declares its names in the current scope.
    // Returns false after an error, when there's no point judging exhaustiveness.
    bool check_pattern(Pattern* p, Type* t) {
        p->type = t;
        if (is_bad(t)) {
            bind_names(p);
            return false;
        }
        switch (p->kind) {
            case PatternKind::wildcard: return true;
            case PatternKind::binding: {
                if (t->kind == TypeKind::enum_) {
                    std::uint32_t i = find_variant(t, p->name);
                    if (i != UINT32_MAX) {
                        p->kind = PatternKind::variant;
                        use_item(t->item);
                        return check_variant_pattern(p, t, i);
                    }
                }
                std::string_view n = text(p->name);
                if (std::isupper(static_cast<unsigned char>(n[0]))) {
                    Resolved r = resolve_name(p->name);
                    if (r.kind == NameKind::item && r.item->kind == ItemKind::const_) return constant_pattern(p, t);
                    error(p->span, q(n) + " isn't " + (t->kind == TypeKind::enum_ ? "a variant of " + name_of(t) : std::string("a variant")) +
                                       "; a name that binds a value starts with a lowercase letter");
                    return false;
                }
                if (p->is_mut) error(p->span, "a pattern binds a copy; to change it, copy it into a `var`");
                Local* l = new_local(p->name, t, p->span, pattern_var_);
                p->local = l;
                declare(l);
                return true;
            }
            case PatternKind::literal: return check_literal_pattern(p, t);
            case PatternKind::range: return check_range_pattern(p, t);
            case PatternKind::none:
                if (t->kind != TypeKind::optional) {
                    error(p->span, "`none` only matches an optional, and this is " + a_type(t));
                    return false;
                }
                p->variant = 0;
                return true;
            case PatternKind::tuple: {
                if (t->kind != TypeKind::tuple) {
                    error(p->span, "a tuple pattern can't match " + a_type(t));
                    bind_names(p);
                    return false;
                }
                if (p->elems.size() != t->elems.size()) {
                    error(p->span, name_of(t) + " has " + std::to_string(t->elems.size()) + " elements, but the pattern has " +
                                       std::to_string(p->elems.size()));
                    bind_names(p);
                    return false;
                }
                bool ok = true;
                for (std::size_t i = 0; i < p->elems.size(); i++) ok = check_pattern(p->elems[i], t->elems[i]) && ok;
                return ok;
            }
            case PatternKind::variant: {
                const Path& path = p->path;
                std::string_view first = text(path.parts[0]);
                if (path.parts.size() == 1 && first == "some") {
                    if (t->kind != TypeKind::optional) {
                        error(p->span, "`some` only matches an optional, and this is " + a_type(t));
                        bind_names(p);
                        return false;
                    }
                    if (p->elems.size() != 1) {
                        error(p->span, "`some` holds one value");
                        bind_names(p);
                        return false;
                    }
                    p->variant = 1;
                    return check_pattern(p->elems[0], t->elem);
                }
                if (path.parts.size() == 1 && (first == "ok" || first == "err")) {
                    not_yet(p->span, "errors");
                    bind_names(p);
                    return false;
                }
                if (path.parts.size() >= 2 && p->elems.empty()) {
                    Resolved r = resolve_name(path.parts[0]);
                    if (r.kind == NameKind::type && r.type->kind != TypeKind::enum_) return constant_pattern(p, t);
                }
                if (t->kind != TypeKind::enum_) {
                    error(p->span, q(path_text(path)) + " can't match " + a_type(t));
                    bind_names(p);
                    return false;
                }
                std::uint32_t i = variant_of_path(path, t, p->span);
                if (i == UINT32_MAX) {
                    bind_names(p);
                    return false;
                }
                return check_variant_pattern(p, t, i);
            }
            case PatternKind::record: return check_record_pattern(p, t);
            case PatternKind::or_: {
                bool ok = true, bound = false;
                for (Pattern* a : p->elems) {
                    if (binds_lowercase(a)) {
                        // Once, and its names declared once, however many alternatives bind.
                        if (!bound) {
                            error(a->span, "a pattern with `|` can't bind names yet; give each case its own arm");
                            bind_names(a);
                        }
                        bound = true;
                        ok = false;
                        continue;
                    }
                    ok = check_pattern(a, t) && ok;
                }
                return ok;
            }
        }
        return false;
    }

    // Whether the pattern has a name that would bind a value.
    bool binds_lowercase(const Pattern* p) const {
        if (p->kind == PatternKind::binding) return !std::isupper(static_cast<unsigned char>(text(p->name)[0]));
        for (const Pattern* x : p->elems) if (binds_lowercase(x)) return true;
        for (const FieldPattern& f : p->fields) if (!f.pattern || binds_lowercase(f.pattern)) return true;
        return false;
    }

    // `LIMIT` or `i32.MAX` in a pattern: a constant, matched as its value is.
    bool constant_pattern(Pattern* p, Type* t) {
        Expr* e = ctx_.arena.make<Expr>();
        e->kind = ExprKind::name;
        e->span = p->span;
        std::vector<Name> first{p->path.parts[0]};
        e->path.parts = ctx_.arena.copy(first);
        e->path.span = p->span;
        for (std::size_t i = 1; i < p->path.parts.size(); i++) {
            Expr* f = ctx_.arena.make<Expr>();
            f->kind = ExprKind::field;
            f->span = p->span;
            f->lhs = e;
            f->member = p->path.parts[i];
            e = f;
        }
        p->kind = PatternKind::literal;
        p->value = e;
        return check_literal_pattern(p, t);
    }

    // The variant a pattern's path names: `Circle` or `Shape.Circle`, for the enum `t`.
    std::uint32_t variant_of_path(const Path& path, Type* t, Span span) {
        Name v;
        if (path.parts.size() == 2) {
            Resolved r = resolve_name(path.parts[0]);
            if (r.kind != NameKind::type || r.type != t) {
                error(span, q(text(path.parts[0])) + " isn't " + name_of(t) + ", the type being matched");
                return UINT32_MAX;
            }
            v = path.parts[1];
        } else if (path.parts.size() == 1) {
            v = path.parts[0];
        } else {
            error(span, "unknown variant " + q(path_text(path)));
            return UINT32_MAX;
        }
        std::uint32_t i = find_variant(t, v);
        if (i == UINT32_MAX) error(span, name_of(t) + " has no variant " + q(text(v)));
        else use_item(t->item);
        return i;
    }

    bool check_variant_pattern(Pattern* p, Type* t, std::uint32_t i) {
        p->variant = i;
        const VariantDecl& v = t->item->variants[i];
        if (v.has_fields) {
            error(p->span, q(variant_text(t, i)) + " has named fields; match it with " + q(variant_text(t, i) + " { ... }"));
            bind_names(p);
            return false;
        }
        if (p->elems.size() != v.types.size()) {
            if (v.types.empty()) {
                error(p->span, q(variant_text(t, i)) + " holds no data; match it without parentheses");
            } else {
                error(p->span, q(variant_text(t, i)) + " holds " + std::to_string(v.types.size()) +
                                   (v.types.size() == 1 ? " value" : " values") + ", but the pattern has " +
                                   std::to_string(p->elems.size()));
            }
            bind_names(p);
            return false;
        }
        bool ok = true;
        for (std::size_t k = 0; k < p->elems.size(); k++) ok = check_pattern(p->elems[k], v.types[k]) && ok;
        return ok;
    }

    bool check_record_pattern(Pattern* p, Type* t) {
        Slice<FieldDecl> fields;
        std::string what;
        if (t->kind == TypeKind::enum_) {
            std::uint32_t i = variant_of_path(p->path, t, p->span);
            if (i == UINT32_MAX) {
                bind_names(p);
                return false;
            }
            const VariantDecl& v = t->item->variants[i];
            if (!v.has_fields) {
                error(p->span, q(variant_text(t, i)) + (v.types.empty() ? " holds no data; match it without braces"
                                                                       : " holds unnamed values; match it with " +
                                                                             q(variant_text(t, i) + "(...)")));
                bind_names(p);
                return false;
            }
            p->variant = i;
            fields = v.fields;
            what = variant_text(t, i);
        } else if (t->kind == TypeKind::struct_) {
            Resolved r;
            if (p->path.parts.size() == 1) r = resolve_name(p->path.parts[0]);
            if (r.kind != NameKind::type || r.type != t) {
                error(p->span, q(path_text(p->path)) + " isn't " + name_of(t) + ", the type being matched");
                bind_names(p);
                return false;
            }
            use_item(t->item);
            fields = t->item->fields;
            what = t->name;
        } else {
            error(p->span, "a pattern with fields can't match " + a_type(t));
            bind_names(p);
            return false;
        }
        std::vector<bool> seen(fields.size(), false);
        bool ok = true;
        for (FieldPattern& f : p->fields) {
            std::uint32_t index = UINT32_MAX;
            for (std::uint32_t i = 0; i < fields.size(); i++) {
                if (fields[i].name == f.name) index = i;
            }
            if (index == UINT32_MAX || seen[index]) {
                error(f.span, index == UINT32_MAX ? q(what) + " has no field " + q(text(f.name))
                                                  : "field " + q(text(f.name)) + " is matched twice");
                if (f.pattern) bind_names(f.pattern);
                ok = false;
                continue;
            }
            seen[index] = true;
            f.index = index;
            if (!f.pattern) {
                Pattern* b = ctx_.arena.make<Pattern>();
                b->kind = PatternKind::binding;
                b->span = f.span;
                b->name = f.name;
                f.pattern = b;
            }
            ok = check_pattern(f.pattern, fields[index].resolved) && ok;
        }
        if (!p->has_rest) {
            for (std::uint32_t i = 0; i < fields.size(); i++) {
                if (!seen[i]) {
                    error(p->span, "the pattern leaves out field " + q(text(fields[i].name)) +
                                       "; match it, or end with `..` to skip the rest");
                    return false;
                }
            }
        }
        return ok;
    }

    bool check_literal_pattern(Pattern* p, Type* t) {
        if (is_float(t)) {
            error(p->span, "match a float with comparisons in an `if`; NaN and -0.0 make float patterns misleading");
            return false;
        }
        bool text = t->kind == TypeKind::str || t->kind == TypeKind::string;
        if (!(is_integral(t) || t->kind == TypeKind::bool_ || text)) {
            error(p->span, "a literal can't match " + a_type(t));
            check(p->value, nullptr);
            return false;
        }
        Type* lt = check(p->value, t);
        if (is_bad(lt)) return false;
        if (text) {
            if (p->value->kind != ExprKind::string_lit || !p->value->interpolations.empty()) {
                error(p->value->span, "a string pattern must be a plain string literal");
                return false;
            }
            return true;
        }
        if (!p->value->has_bits) {
            error(p->value->span, "a pattern must be a constant");
            return false;
        }
        return true;
    }

    bool check_range_pattern(Pattern* p, Type* t) {
        if (!is_integral(t)) {
            error(p->span, "a range pattern matches integers and characters, not " + name_of(t));
            return false;
        }
        Type* a = check(p->value, t);
        Type* b = check(p->end, t);
        if (is_bad(a) || is_bad(b)) return false;
        if (!p->value->has_bits || !p->end->has_bits) {
            error(p->span, "a range pattern's ends must be constants");
            return false;
        }
        std::uint64_t lo = rank(t, p->value->const_bits), hi = rank(t, p->end->const_bits);
        if (p->inclusive ? lo > hi : lo >= hi) {
            error(p->span, "this range is empty");
            return false;
        }
        return true;
    }

    // After an error, declares a pattern's names anyway, so their uses aren't reported again.
    void bind_names(Pattern* p) {
        if (!p) return;
        if (p->kind == PatternKind::binding) {
            std::string_view n = text(p->name);
            if (n.empty() || std::isupper(static_cast<unsigned char>(n[0]))) return;
            Local* l = new_local(p->name, T.error, p->span, false);
            l->used = l->reassigned = l->mutated = true;
            p->local = l;
            declare(l);
            return;
        }
        if (p->kind == PatternKind::or_) {
            if (!p->elems.empty()) bind_names(p->elems[0]);
            return;
        }
        for (Pattern* x : p->elems) bind_names(x);
        for (FieldPattern& f : p->fields) {
            if (f.pattern) {
                bind_names(f.pattern);
            } else {
                Local* l = new_local(f.name, T.error, f.span, false);
                l->used = l->reassigned = l->mutated = true;
                declare(l);
            }
        }
    }

    // --- exhaustiveness ----------------------------------------------------------------------
    // Maranget's usefulness algorithm, on patterns reduced to constructors. An integer or
    // character is split into intervals, so ranges can cover a type completely.

    struct DPat {
        enum Kind : std::uint8_t { wild, ctor, range, str, alt } kind = wild;
        std::uint32_t index = 0;      // a variant, `false` 0 and `true` 1, `none` 0 and `some` 1
        std::vector<DPat*> args;
        std::uint64_t lo = 0, hi = 0;  // range: inclusive, as ranks
        std::string_view string;
    };
    using Row = std::vector<DPat*>;

    DPat* make_dpat(DPat::Kind kind) {
        pool_.push_back(std::make_unique<DPat>());
        pool_.back()->kind = kind;
        return pool_.back().get();
    }
    DPat* wild() { return make_dpat(DPat::wild); }

    // Integers and characters as unsigned numbers in the same order, so intervals compare simply.
    static std::uint64_t rank(const Type* t, std::uint64_t bits) {
        return t->is_signed ? bits ^ (std::uint64_t{1} << 63) : bits;
    }
    static std::uint64_t unrank(const Type* t, std::uint64_t r) {
        return t->is_signed ? r ^ (std::uint64_t{1} << 63) : r;
    }

    // The values of an integral type, as intervals of ranks. A rune can't be a surrogate.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> domain(const Type* t) const {
        if (t->kind == TypeKind::rune) return {{0, 0xD7FF}, {0xE000, 0x10FFFF}};
        IntRange r = source_range(t);
        if (t->is_signed) return {{rank(t, 0 - r.neg), rank(t, r.pos)}};
        return {{0, r.pos}};
    }

    DPat* reduce(Pattern* p) {
        Type* t = p->type;
        switch (p->kind) {
            case PatternKind::wildcard:
            case PatternKind::binding: return wild();
            case PatternKind::literal: {
                if (t->kind == TypeKind::bool_) {
                    DPat* d = make_dpat(DPat::ctor);
                    d->index = static_cast<std::uint32_t>(p->value->const_bits);
                    return d;
                }
                if (t->kind == TypeKind::str || t->kind == TypeKind::string) {
                    DPat* d = make_dpat(DPat::str);
                    for (const StringPart& part : p->value->literal.parts) {
                        if (!part.is_expr) d->string = part.text;
                    }
                    return d;
                }
                DPat* d = make_dpat(DPat::range);
                d->lo = d->hi = rank(t, p->value->const_bits);
                return d;
            }
            case PatternKind::range: {
                DPat* d = make_dpat(DPat::range);
                d->lo = rank(t, p->value->const_bits);
                d->hi = rank(t, p->end->const_bits) - (p->inclusive ? 0 : 1);
                return d;
            }
            case PatternKind::none: return make_dpat(DPat::ctor);
            case PatternKind::variant: {
                DPat* d = make_dpat(DPat::ctor);
                d->index = p->variant;
                for (Pattern* x : p->elems) d->args.push_back(reduce(x));
                return d;
            }
            case PatternKind::record: {
                DPat* d = make_dpat(DPat::ctor);
                d->index = t->kind == TypeKind::enum_ ? p->variant : 0;
                std::size_t n = ctor_types(t, d->index).size();
                for (std::size_t i = 0; i < n; i++) d->args.push_back(wild());
                for (FieldPattern& f : p->fields) d->args[f.index] = reduce(f.pattern);
                return d;
            }
            case PatternKind::tuple: {
                DPat* d = make_dpat(DPat::ctor);
                for (Pattern* x : p->elems) d->args.push_back(reduce(x));
                return d;
            }
            case PatternKind::or_: {
                DPat* d = make_dpat(DPat::alt);
                for (Pattern* x : p->elems) d->args.push_back(reduce(x));
                return d;
            }
        }
        return wild();
    }

    // A row starting with `A | B` stands for a row for each alternative.
    static void expand(const Row& r, std::vector<Row>& out) {
        if (r[0]->kind != DPat::alt) {
            out.push_back(r);
            return;
        }
        for (DPat* a : r[0]->args) {
            Row n = r;
            n[0] = a;
            expand(n, out);
        }
    }

    // How many constructors a type has, or -1 for one with too many to list.
    static int ctor_count(const Type* t) {
        switch (t->kind) {
            case TypeKind::enum_: return static_cast<int>(t->item->variants.size());
            case TypeKind::optional: case TypeKind::bool_: return 2;
            case TypeKind::tuple: case TypeKind::struct_: return 1;
            default: return -1;
        }
    }

    // The types a constructor holds.
    std::vector<Type*> ctor_types(const Type* t, std::uint32_t c) const {
        switch (t->kind) {
            case TypeKind::enum_: {
                const VariantDecl& v = t->item->variants[c];
                return std::vector<Type*>(v.types.begin(), v.types.end());
            }
            case TypeKind::optional: return c == 1 ? std::vector<Type*>{t->elem} : std::vector<Type*>{};
            case TypeKind::tuple: return std::vector<Type*>(t->elems.begin(), t->elems.end());
            case TypeKind::struct_: {
                std::vector<Type*> out;
                for (const FieldDecl& f : t->item->fields) out.push_back(f.resolved);
                return out;
            }
            default: return {};
        }
    }

    static bool covers(const DPat* h, const DPat* c) {
        switch (h->kind) {
            case DPat::wild: return true;
            case DPat::ctor: return c->kind == DPat::ctor && h->index == c->index;
            case DPat::range: return c->kind == DPat::range && h->lo <= c->lo && c->hi <= h->hi;
            case DPat::str: return c->kind == DPat::str && h->string == c->string;
            case DPat::alt: return false;  // expanded before this is asked
        }
        return false;
    }

    // Whether `q` matches some value no row matches. If so, `witness` gets such a value.
    bool useful(const std::vector<Row>& rows, const Row& q, const std::vector<Type*>& types, Row* witness) {
        if (q.empty()) {
            if (!rows.empty()) return false;
            if (witness) witness->clear();
            return true;
        }
        for (const Row& r : rows) {
            if (r[0]->kind != DPat::alt) continue;
            std::vector<Row> expanded;
            for (const Row& x : rows) expand(x, expanded);
            return useful(expanded, q, types, witness);
        }
        if (q[0]->kind == DPat::alt) {
            for (DPat* a : q[0]->args) {
                Row n = q;
                n[0] = a;
                if (useful(rows, n, types, witness)) return true;
            }
            return false;
        }
        Type* t = types[0];
        std::vector<Type*> rest(types.begin() + 1, types.end());
        Row qrest(q.begin() + 1, q.end());
        DPat* h = q[0];
        if (h->kind == DPat::ctor || h->kind == DPat::str) return useful_ctor(rows, h, qrest, t, rest, witness);
        if (is_integral(t)) {
            for (auto [lo, hi] : split(rows, t, h)) {
                DPat* c = make_dpat(DPat::range);
                c->lo = lo;
                c->hi = hi;
                if (useful_ctor(rows, c, qrest, t, rest, witness)) return true;
            }
            return false;
        }
        int n = ctor_count(t);
        std::vector<bool> used(n > 0 ? static_cast<std::size_t>(n) : 0, false);
        for (const Row& r : rows) {
            if (n > 0 && r[0]->kind == DPat::ctor) used[r[0]->index] = true;
        }
        bool complete = n > 0 && std::all_of(used.begin(), used.end(), [](bool b) { return b; });
        if (complete) {
            for (int c = 0; c < n; c++) {
                DPat* k = make_dpat(DPat::ctor);
                k->index = static_cast<std::uint32_t>(c);
                for (std::size_t i = 0; i < ctor_types(t, k->index).size(); i++) k->args.push_back(wild());
                if (useful_ctor(rows, k, qrest, t, rest, witness)) return true;
            }
            return false;
        }
        // Some constructor isn't covered: only rows starting with a wildcard can match it.
        std::vector<Row> d;
        for (const Row& r : rows) {
            if (r[0]->kind == DPat::wild) d.push_back(Row(r.begin() + 1, r.end()));
        }
        Row w;
        if (!useful(d, qrest, rest, witness ? &w : nullptr)) return false;
        if (witness) {
            DPat* k = wild();
            if (n > 0) {
                k = make_dpat(DPat::ctor);
                while (used[k->index]) k->index++;
                for (std::size_t i = 0; i < ctor_types(t, k->index).size(); i++) k->args.push_back(wild());
            }
            witness->clear();
            witness->push_back(k);
            witness->insert(witness->end(), w.begin(), w.end());
        }
        return true;
    }

    bool useful_ctor(const std::vector<Row>& rows, DPat* c, const Row& qrest, Type* t, const std::vector<Type*>& rest,
                     Row* witness) {
        std::vector<Type*> sub = c->kind == DPat::ctor ? ctor_types(t, c->index) : std::vector<Type*>{};
        std::size_t arity = sub.size();
        std::vector<Row> srows;
        for (const Row& r : rows) {
            DPat* h = r[0];
            if (!covers(h, c)) continue;
            Row n;
            if (h->kind == DPat::wild) {
                for (std::size_t i = 0; i < arity; i++) n.push_back(wild());
            } else if (h->kind == DPat::ctor) {
                n = h->args;
            }
            n.insert(n.end(), r.begin() + 1, r.end());
            srows.push_back(std::move(n));
        }
        Row nq;
        if (c->kind == DPat::ctor) nq = c->args;
        while (nq.size() < arity) nq.push_back(wild());
        nq.insert(nq.end(), qrest.begin(), qrest.end());
        std::vector<Type*> ntypes = sub;
        ntypes.insert(ntypes.end(), rest.begin(), rest.end());
        Row w;
        if (!useful(srows, nq, ntypes, witness ? &w : nullptr)) return false;
        if (witness) {
            DPat* k = make_dpat(c->kind);
            k->index = c->index;
            k->lo = c->lo;
            k->hi = c->hi;
            k->string = c->string;
            k->args.assign(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(arity));
            witness->clear();
            witness->push_back(k);
            witness->insert(witness->end(), w.begin() + static_cast<std::ptrdiff_t>(arity), w.end());
        }
        return true;
    }

    // The values `h` stands for (the whole domain for a wildcard), cut into intervals at every
    // boundary of the ranges in the column, so each interval is covered by a range or not at all.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> split(const std::vector<Row>& rows, const Type* t, const DPat* h) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> base;
        for (auto [lo, hi] : domain(t)) {
            if (h->kind == DPat::range) {
                lo = std::max(lo, h->lo);
                hi = std::min(hi, h->hi);
                if (lo > hi) continue;
            }
            base.push_back({lo, hi});
        }
        std::vector<std::uint64_t> cuts;
        for (const Row& r : rows) {
            if (r[0]->kind != DPat::range) continue;
            cuts.push_back(r[0]->lo);
            if (r[0]->hi != UINT64_MAX) cuts.push_back(r[0]->hi + 1);
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
        for (auto [lo, hi] : base) {
            std::uint64_t start = lo;
            for (std::uint64_t c : cuts) {
                if (c > start && c <= hi) {
                    out.push_back({start, c - 1});
                    start = c;
                }
            }
            out.push_back({start, hi});
        }
        return out;
    }

    // A value a witness stands for, as Calx source.
    std::string render(const DPat* d, const Type* t) const {
        if (d->kind == DPat::wild) return "_";
        auto all_wild = [&] {
            for (const DPat* a : d->args) if (a->kind != DPat::wild) return false;
            return true;
        };
        switch (t->kind) {
            case TypeKind::bool_: return d->index ? "true" : "false";
            case TypeKind::optional: return d->index == 0 ? "none" : "some(" + render(d->args[0], t->elem) + ")";
            case TypeKind::enum_: {
                const VariantDecl& v = t->item->variants[d->index];
                std::string name = variant_text(t, d->index);
                if (v.types.empty()) return name;
                if (v.has_fields) {
                    if (all_wild()) return name + " { .. }";
                    std::string s = name + " {";
                    for (std::size_t i = 0; i < d->args.size(); i++) {
                        s += (i ? ", " : " ") + std::string(text(v.fields[i].name)) + ": " + render(d->args[i], v.types[i]);
                    }
                    return s + " }";
                }
                std::string s = name + "(";
                for (std::size_t i = 0; i < d->args.size(); i++) s += (i ? ", " : "") + render(d->args[i], v.types[i]);
                return s + ")";
            }
            case TypeKind::tuple: {
                std::string s = "(";
                for (std::size_t i = 0; i < d->args.size(); i++) s += (i ? ", " : "") + render(d->args[i], t->elems[i]);
                return s + ")";
            }
            case TypeKind::struct_: {
                if (all_wild()) return std::string(t->name) + " { .. }";
                std::string s = std::string(t->name) + " {";
                for (std::size_t i = 0; i < d->args.size(); i++) {
                    s += (i ? ", " : " ") + std::string(text(t->item->fields[i].name)) + ": " +
                         render(d->args[i], t->item->fields[i].resolved);
                }
                return s + " }";
            }
            default: break;
        }
        if (d->kind == DPat::range) {
            auto all = domain(t);
            if (all.size() == 1 && d->lo == all[0].first && d->hi == all[0].second) return "_";
            if (d->lo == d->hi) return render_value(t, d->lo);
            return render_value(t, d->lo) + "..=" + render_value(t, d->hi);
        }
        return "_";
    }

    std::string render_value(const Type* t, std::uint64_t r) const {
        std::uint64_t bits = unrank(t, r);
        if (t->kind == TypeKind::char_ || t->kind == TypeKind::rune) {
            if (bits >= 0x20 && bits < 0x7F && bits != '\\' && bits != '\'') return "'" + std::string(1, static_cast<char>(bits)) + "'";
            char buf[16];
            std::snprintf(buf, sizeof buf, "'\\u{%llx}'", static_cast<unsigned long long>(bits));
            return buf;
        }
        IntRange range = source_range(t);
        if (t->is_signed && bits == 0 - range.neg) return std::string(t->name) + ".MIN";
        if (bits == range.pos && !t->is_usize) return std::string(t->name) + ".MAX";
        if (t->is_signed) return std::to_string(static_cast<std::int64_t>(bits));
        return std::to_string(bits);
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
            if (text(n) == "some" && !find_local(n)) return some_call(e, expected);
            if (n == n_List_ && !find_local(n)) return list_call(e, expected);
            if ((text(n) == "ok" || text(n) == "err") && !find_local(n)) {
                not_yet(e->span, "errors");
                return T.error;
            }
            Resolved r = resolve_name(n);
            switch (r.kind) {
                case NameKind::builtin: return builtin_call(e, r.builtin);
                case NameKind::item:
                    if (r.item->kind != ItemKind::fn) {
                        error(callee->span, q(text(n)) + " isn't a function");
                        return T.error;
                    }
                    use_item(r.item);
                    return fn_call(e, r.item, nullptr, expected, Subst{}, callee->type_args);
                case NameKind::type: {
                    Type* t = type_of_name(callee, r.type);
                    if (is_bad(t)) {
                        for (Arg& a : e->args) check(a.value, nullptr);
                        return T.error;
                    }
                    callee->ref = Ref::type;
                    callee->named_type = t;
                    if (t->kind == TypeKind::struct_) return construct(e, t, expected);
                    if (t == T.string) return string_call(e);
                    if (t->kind == TypeKind::enum_) {
                        error(callee->span, "build " + a_type(t) + " from one of its variants, such as " + q(variant_text(t, 0)));
                        for (Arg& a : e->args) check(a.value, nullptr);
                        return T.error;
                    }
                    if (t->kind == TypeKind::param) {
                        error(callee->span, q(t->name) + " is a generic parameter, so there's nothing to build or convert to");
                        for (Arg& a : e->args) check(a.value, nullptr);
                        return T.error;
                    }
                    return conversion(e, t, Conv::exact);
                }
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
        if (callee->kind == ExprKind::optional_field) {
            not_yet(callee->span, "`?.` method calls");
            check(callee->lhs, nullptr);
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        if (callee->kind == ExprKind::field && !callee->is_tuple_index && callee->lhs->kind == ExprKind::name &&
            !e->only_interface) {
            Resolved r = resolve_name(callee->lhs->path.parts[0]);
            if (r.kind == NameKind::item && r.item->kind == ItemKind::interface) return interface_call(e, r.item);
        }
        if (callee->kind == ExprKind::field && !callee->is_tuple_index) {
            // `T.something(...)`: a conversion or a static function.
            if (callee->lhs->kind == ExprKind::name) {
                Resolved r = resolve_name(callee->lhs->path.parts[0]);
                if (r.kind == NameKind::type) {
                    Type* t = type_of_name(callee->lhs, r.type);
                    if (is_bad(t)) {
                        for (Arg& a : e->args) check(a.value, nullptr);
                        return T.error;
                    }
                    callee->lhs->ref = Ref::type;
                    callee->lhs->named_type = t;
                    return static_call(e, t, expected);
                }
            }
            Type* recv = check(callee->lhs, nullptr);
            if (is_bad(recv)) {
                for (Arg& a : e->args) check(a.value, nullptr);
                return T.error;
            }
            if (callee->member == n_copy_ && !e->only_interface) return copy_call(e, recv);
            Member mem = lookup(recv, callee->member, e->only_interface);
            // Through a parameter, the bound says which interface answers, and an instance, whose
            // type could have the name in another interface too, keeps to it.
            if (recv->kind == TypeKind::param && mem.fn && !mem.ambiguous) e->only_interface = mem.iface;
            if (mem.ambiguous) {
                error(callee->span, q(text(callee->member)) + " is in more than one interface " + name_of(recv) +
                                        " implements; say which, as in " + q(ambiguity_example(recv, callee->member)));
                for (Arg& a : e->args) check(a.value, nullptr);
                return T.error;
            }
            if (!mem.fn && e->only_interface) {
                error(callee->lhs->span, name_of(recv) + " doesn't implement " + q(text(e->only_interface->name)));
                for (Arg& a : e->args) check(a.value, nullptr);
                return T.error;
            }
            if (mem.fn) {
                if (!member_visible(mem)) private_member(callee->span, mem.fn);
                if (!mem.fn->is_method) {
                    error(callee->span, q(text(mem.fn->name)) + " isn't a method; call it as " +
                                            q(std::string(recv->name) + "." + std::string(text(mem.fn->name)) + "(...)"));
                    for (Arg& a : e->args) check(a.value, nullptr);
                    return T.error;
                }
                use_item(mem.fn);
                return fn_call(e, mem.fn, callee->lhs, expected, std::move(mem.s));
            }
            if (recv->kind == TypeKind::param) {
                error(callee->span, name_of(recv) + " has no method " + q(text(callee->member)) +
                                        (recv->bounds.empty() ? "; give it a bound that has one, such as " + q("<" + std::string(recv->name) + ": Shape>")
                                                              : std::string("")));
                for (Arg& a : e->args) check(a.value, nullptr);
                return T.error;
            }
            if (recv->kind == TypeKind::struct_ || recv->kind == TypeKind::enum_) {
                Item* m = nullptr;
                if (!m) {
                    if (recv->kind == TypeKind::enum_) {
                        error(callee->span, name_of(recv) + " has no method " + q(text(callee->member)));
                        return T.error;
                    }
                    for (const FieldDecl& f : recv->item->fields) {
                        if (f.name == callee->member) {
                            error(callee->span, q(text(f.name)) + " is a field of " + name_of(recv) + ", not a method");
                            return T.error;
                        }
                    }
                    error(callee->span, name_of(recv) + " has no method " + q(text(callee->member)));
                    return T.error;
                }
            }
            if (recv->kind == TypeKind::string || recv->kind == TypeKind::list) return builtin_method(e, recv);
            return prim_method(e, recv);
        }
        check(callee, nullptr);
        error(callee->span, "this can't be called");
        return T.error;
    }

    // `Shape.area(x)`: the method `area` of `Shape`, on `x`, for when two interfaces have the
    // name. It's rewritten as `x.area()` that looks only in `Shape`.
    Type* interface_call(Expr* e, Item* iface) {
        Expr* callee = e->callee;
        use_item(iface);
        Item* member = interface_member(iface, callee->member);
        if (!member || member->kind != ItemKind::fn) {
            error(callee->span, q(text(iface->name)) + " has no function " + q(text(callee->member)));
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        if (!member->is_method) {
            error(callee->span, "call " + q(text(member->name)) + " on a type that implements " + q(text(iface->name)) +
                                    ", as in " + q("T." + std::string(text(member->name)) + "(...)"));
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        if (e->args.empty() || e->args[0].name) {
            error(e->span, q(std::string(text(iface->name)) + "." + std::string(text(member->name))) +
                               " takes the value to call it on first");
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        Expr* recv = e->args[0].value;
        Expr* f = ctx_.arena.make<Expr>(*callee);
        f->lhs = recv;
        f->span = callee->span;
        std::vector<Arg> rest(e->args.begin() + 1, e->args.end());
        e->callee = f;
        e->args = ctx_.arena.copy(rest);
        e->only_interface = iface;
        return call(e, nullptr);
    }

    std::string ambiguity_example(Type* t, Name member) {
        for (Item* b : t->bounds) {
            if (interface_member(b, member)) return std::string(text(b->name)) + "." + std::string(text(member)) + "(x)";
        }
        auto found = impls_by_head_.find(head(t));
        if (found != impls_by_head_.end()) {
            for (ImplInfo* info : found->second) {
                if (info->iface && interface_member(info->iface, member)) {
                    return std::string(text(info->iface->name)) + "." + std::string(text(member)) + "(x)";
                }
            }
        }
        return "Interface." + std::string(text(member)) + "(x)";
    }

    Type* static_call(Expr* e, Type* t, Type* expected) {
        std::string_view m = text(e->callee->member);
        if (is_numeric(t) || t->kind == TypeKind::char_ || t->kind == TypeKind::rune) {
            if (m == "wrap") return conversion(e, t, Conv::wrap);
            if (m == "saturate") return conversion(e, t, Conv::saturate);
            if (m == "nearest") return conversion(e, t, Conv::nearest);
            if (m == "try_from") return try_from(e, t);
        }
        if (t->kind == TypeKind::enum_) {
            std::uint32_t i = find_variant(t, e->callee->member);
            if (i != UINT32_MAX) return variant_call(e, t, i, expected);
            if (m == "try_from") return try_from(e, t);
        }
        Member mem = lookup(t, e->callee->member, e->only_interface);
        if (t->kind == TypeKind::param && mem.fn && !mem.ambiguous) e->only_interface = mem.iface;
        if (mem.ambiguous) {
            error(e->callee->span, q(m) + " is in more than one " + (is_identity(t) ? "impl of " : "interface ") + name_of(t) +
                                       (is_identity(t) ? "; give its types, as in " + q(std::string(text(t->generic->name)) + "<...>." + std::string(m) + "(...)")
                                                       : std::string(" implements")));
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        if (mem.fn) {
            if (!member_visible(mem)) private_member(e->callee->span, mem.fn);
            if (mem.fn->is_method) {
                error(e->callee->span, q(text(mem.fn->name)) + " is a method; call it on a value: " + q("x." + std::string(m) + "(...)"));
                for (Arg& a : e->args) check(a.value, nullptr);
                return T.error;
            }
            use_item(mem.fn);
            return fn_call(e, mem.fn, nullptr, expected, std::move(mem.s));
        }
        error(e->callee->span, name_of(t) + " has no function " + q(m));
        for (Arg& a : e->args) check(a.value, nullptr);
        return T.error;
    }

    Type* variant_call(Expr* e, Type* t, std::uint32_t i, Type* expected) {
        use_item(t->item->origin ? t->item->origin : t->item);
        const VariantDecl& v = t->item->variants[i];
        e->call = CallKind::variant;
        e->field_index = i;
        if (v.types.empty()) {
            error(e->span, q(variant_text(t, i)) + " holds no data; write it without parentheses");
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        if (v.has_fields) {
            error(e->span, q(variant_text(t, i)) + " has named fields; write " + q(variant_text(t, i) + " { ... }"));
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        if (e->args.size() != v.types.size()) {
            error(e->span, q(variant_text(t, i)) + " holds " + std::to_string(v.types.size()) +
                               (v.types.size() == 1 ? " value" : " values") + ", but " +
                               std::to_string(e->args.size()) + " were given");
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        // `Either.Left(1)`: a generic enum's types come from the values, or from where it goes.
        bool infer = is_identity(t);
        Subst sub;
        Slice<Type*> vars = infer ? t->item->type_params : Slice<Type*>{};
        if (infer && expected && expected->generic == t->generic) unify(t, expected, sub, vars);
        std::vector<Expr*> ordered;
        for (std::size_t k = 0; k < e->args.size(); k++) {
            Arg& a = e->args[k];
            if (a.name || a.is_mut) error(a.span, "a variant's values can't be named or `mut`");
            check_inferred(a.value, v.types[k], sub, vars);
            ordered.push_back(a.value);
        }
        e->ordered_args = ctx_.arena.copy(ordered);
        if (infer) {
            return inferred_instance(t->generic, sub, e->callee->span,
                                     std::string(text(t->generic->name)) + "<...>." + std::string(text(v.name)) + "(...)");
        }
        return t;
    }

    // `T.try_from(x)`: `x` as a `T`, or `none` if it doesn't fit.
    Type* try_from(Expr* e, Type* to) {
        Expr* a = single_arg(e, "`try_from`");
        if (!a) return T.error;
        e->call = CallKind::try_from;
        e->named_type = to;
        Type* from = check(a, to->kind == TypeKind::enum_ ? to->repr : nullptr);
        if (is_bad(from)) return T.error;
        if (to->kind == TypeKind::enum_) {
            use_item(to->item);
            if (to->has_data || !to->item->repr) {
                error(e->span, name_of(to) + " has no representation to convert from; give it one, such as " +
                                   q("enum " + std::string(to->name) + ": u8"));
                return T.error;
            }
            return T.optional_of(to);
        }
        if (!is_integral(from) || !is_integral(to)) {
            error(e->span, "`try_from` converts between integer types" +
                               std::string(is_float(from) ? "; for a float, Java's cast is `saturate`" : ""));
            return T.error;
        }
        if (from == to) {
            error(e->span, "this is already " + a_type(to) + "; remove the conversion");
            return T.error;
        }
        if (exact_conversion(from, to)) {
            error(e->span, name_of(from) + " always fits in " + name_of(to) + "; write " + q(std::string(to->name) + "(...)"));
            return T.error;
        }
        return T.optional_of(to);
    }

    // Checks a call's arguments against `f`'s parameters, matching names, and records them in
    // parameter order. `receiver` is the value a method is called on.
    Type* fn_call(Expr* e, Item* f, Expr* receiver, Type* expected = nullptr, Subst s = {},
                  Slice<TypeExpr*> type_args = {}) {
        check_signature(f);
        e->call = receiver ? CallKind::method : CallKind::fn;
        e->item = f;
        // A generic function's types come from `f<...>`, then the receiver, the arguments in the
        // order they're written, and where the result goes.
        const Slice<Type*>& vars = f->type_params;
        bool generic = !vars.empty();
        if (!type_args.empty()) {
            std::size_t own = f->generics.size();
            if (!own) {
                error(type_args[0]->span, q(text(f->name)) + " isn't generic");
            } else if (type_args.size() != own) {
                error(type_args[0]->span, q(text(f->name)) + " takes " + std::to_string(own) + (own == 1 ? " type" : " types") +
                                              ", but " + std::to_string(type_args.size()) +
                                              (type_args.size() == 1 ? " was given" : " were given"));
            } else {
                for (std::size_t k = 0; k < own; k++) {
                    Type* a = resolve_type(type_args[k]);
                    if (is_bad(a)) return T.error;
                    s[vars[vars.size() - own + k]] = a;
                }
            }
        }
        if (generic && receiver && !f->params.empty() && f->params[0].local) {
            unify(f->params[0].local->type, receiver->type, s, vars);
        }
        if (generic && expected && !is_bad(expected) && f->result_type && mentions_any(f->result_type, vars)) {
            Subst trial = s;
            if (unify(f->result_type, expected, trial, vars)) s = std::move(trial);
        }
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
        bool bad_arg = false;
        std::vector<std::pair<Expr*, Type*>> later;
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
            // An untyped literal, such as `1` or `none`, waits until the other arguments have said
            // what the parameters are, so `pick(c, 1, 2i64)` picks between `i64`s.
            if (generic && is_untyped_literal(a.value) && unresolved(subst(want, s), vars, s)) {
                later.push_back({a.value, want});
            } else {
                Type* got = generic ? check_inferred(a.value, want, s, vars) : check(a.value, want);
                if (is_bad(got)) bad_arg = true;
            }
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
        for (auto& [v, want] : later) {
            // A second `none` that still can't tell its type would only repeat the first's error.
            if (bad_arg && v->kind == ExprKind::none_lit && unresolved(subst(want, s), vars, s)) continue;
            if (is_bad(check_inferred(v, want, s, vars))) bad_arg = true;
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
        Type* result = f->result_type ? f->result_type : T.error;
        if (!generic) return result;
        std::vector<Type*> args;
        for (Type* p : vars) {
            auto it = s.find(p);
            if (it == s.end()) {
                if (ok && !bad_arg) {
                    error(e->callee->span, "can't tell what " + q(p->name) + " is in this call to " + q(text(f->name)) +
                                               (f->generics.empty() ? std::string("")
                                                                    : "; give it, as in " + q(std::string(text(f->name)) + "<...>(...)")));
                }
                return T.error;
            }
            args.push_back(it->second);
        }
        if (!ok) return T.error;
        if (!check_bounds(vars, args, e->span, q(text(f->name)))) return T.error;
        result = subst(result, s);
        bool open = false;
        for (Type* a : args) open = open || a->open;
        // Inside a generic definition the call is only checked; its instance comes with the caller's.
        if (!open) {
            Item* inst = instance_fn(f, args, e->span);
            if (!inst) return T.error;
            e->item = inst;
        }
        return result;
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
        if (e->block || e->scrutinee) return true;  // blocks are rare in arguments; assume they might
        return false;
    }

    Type* construct(Expr* e, Type* t, Type* expected) {
        if (is_identity(t)) {
            if (!expected || expected->generic != t->generic) {
                error(e->span, "can't tell what types this " + q(text(t->generic->name)) + " has; give them, as in " +
                                   q(std::string(text(t->generic->name)) + "<...>()") + ", or give the variable a type");
                for (Arg& a : e->args) check(a.value, nullptr);
                return T.error;
            }
            t = expected;
        }
        Item* s = t->item;
        use_item(s->origin ? s->origin : s);
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
        if (from->kind == TypeKind::enum_) {
            // An enum with a representation converts as that integer type does.
            if (from->has_data || !from->item->repr) {
                error(e->span, name_of(from) + " has no number to convert; give it a representation, such as " +
                                   q("enum " + std::string(from->name) + ": u8"));
                return T.error;
            }
            from = from->repr;
            if (from == to) {
                if (conv != Conv::exact) {
                    error(e->span, name_of(a->type) + " converts to " + a_type(to) + " exactly; write " + q(target + "(...)"));
                    return T.error;
                }
                if (a->has_bits) set_int(e, to, a->const_bits);
                return to;
            }
        } else if (from == to) {
            error(e->span, q(a->kind == ExprKind::name ? std::string(text(a->path.parts[0])) : std::string("this")) +
                               " is already " + a_type(to) + "; remove the conversion");
            return T.error;
        }
        bool convertible = (is_integral(from) || is_float(from)) && (is_integral(to) || is_float(to));
        if (!convertible) {
            if (to->kind == TypeKind::enum_) {
                error(e->span, "convert a number to an enum with " + q(std::string(to->name) + ".try_from(...)"));
            } else if (from->kind == TypeKind::bool_ || to->kind == TypeKind::bool_) {
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
                m->type = string_literal(m, nullptr, true);
            } else {
                Type* t = check(m, nullptr);
                if (!is_bad(t) && t != T.str && t != T.string) error(m->span, "expected a string, found " + name_of(t));
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

    // `List<i32>()`: an empty list, from the generic argument or the type expected.
    Type* list_call(Expr* e, Type* expected) {
        Expr* callee = e->callee;
        callee->ref = Ref::type;
        Type* t = nullptr;
        if (callee->type_args.size() == 1) {
            Type* elem = resolve_type(callee->type_args[0]);
            if (is_bad(elem)) return T.error;
            t = T.list_of(elem);
        } else if (!callee->type_args.empty()) {
            error(callee->span, "`List` takes one generic argument, the type of its elements: `List<i32>()`");
            return T.error;
        } else if (expected && expected->kind == TypeKind::list) {
            t = expected;
        } else {
            error(e->span, "can't tell what this list holds; give it, as in `List<i32>()`, or give the variable a type");
            return T.error;
        }
        for (Arg& a : e->args) check(a.value, nullptr);
        if (!e->args.empty()) {
            error(e->args[0].span, "`List<T>()` takes no arguments; push the elements, or write them as `[a, b]`");
            return T.error;
        }
        callee->named_type = t;
        e->call = CallKind::construct;
        e->is_const = true;
        return t;
    }

    // `String()` is empty, and `String(s)` is the `str` as a `String`.
    Type* string_call(Expr* e) {
        if (e->args.empty()) {
            e->call = CallKind::construct;
            e->is_const = true;
            return T.string;
        }
        Expr* a = single_arg(e, "`String(...)`");
        if (!a) return T.error;
        e->call = CallKind::convert;
        e->conv = Conv::exact;
        e->named_type = T.string;
        Type* from = check(a, T.str);
        return is_bad(from) ? T.error : T.string;
    }

    // `x.copy()`: a copy of a value that owns memory, with memory of its own (D76).
    Type* copy_call(Expr* e, Type* recv) {
        e->call = CallKind::builtin_method;
        e->bmethod = BMethod::copy;
        for (Arg& a : e->args) check(a.value, nullptr);
        if (!e->args.empty()) {
            error(e->span, "`copy` takes no arguments");
            return T.error;
        }
        if (is_bad(recv)) return T.error;
        if (is_copy(recv) && recv->kind != TypeKind::param && !instance_mode_) {
            error(e->callee->span, name_of(recv) + " is `Copy`, so use it as it is; `.copy()` is for values that own memory");
            return T.error;
        }
        return recv;
    }

    // The methods of `String` and `List<T>`.
    Type* builtin_method(Expr* e, Type* recv) {
        std::string_view m = text(e->callee->member);
        e->call = CallKind::builtin_method;
        BMethod b = BMethod::none;
        bool changes = false;
        std::vector<Type*> params;
        Type* result = T.unit;
        if (recv->kind == TypeKind::string) {
            if (m == "len") b = BMethod::len, result = T.usize;
            else if (m == "is_empty") b = BMethod::is_empty, result = T.bool_;
            else if (m == "push") b = BMethod::push, changes = true, params = {T.rune};
            // These only look at the text they're given, so it can be a `str` or a `String`;
            // `nullptr` stands for either.
            else if (m == "push_str") b = BMethod::push_str, changes = true, params = {nullptr};
            else if (m == "clear") b = BMethod::clear, changes = true;
            else if (m == "reserve") b = BMethod::reserve, changes = true, params = {T.usize};
            else if (m == "contains") b = BMethod::contains, params = {nullptr}, result = T.bool_;
            else if (m == "starts_with") b = BMethod::starts_with, params = {nullptr}, result = T.bool_;
            else if (m == "ends_with") b = BMethod::ends_with, params = {nullptr}, result = T.bool_;
        } else {
            Type* el = recv->elem;
            if (m == "len") b = BMethod::len, result = T.usize;
            else if (m == "is_empty") b = BMethod::is_empty, result = T.bool_;
            else if (m == "capacity") b = BMethod::capacity, result = T.usize;
            else if (m == "push") b = BMethod::push, changes = true, params = {el};
            else if (m == "pop") b = BMethod::pop, changes = true, result = T.optional_of(el);
            else if (m == "clear") b = BMethod::clear, changes = true;
            else if (m == "insert") b = BMethod::insert, changes = true, params = {T.usize, el};
            else if (m == "remove") b = BMethod::remove, changes = true, params = {T.usize}, result = el;
            else if (m == "swap_remove") b = BMethod::swap_remove, changes = true, params = {T.usize}, result = el;
            else if (m == "swap") b = BMethod::swap, changes = true, params = {T.usize, T.usize};
            else if (m == "truncate") b = BMethod::truncate, changes = true, params = {T.usize};
            else if (m == "reserve") b = BMethod::reserve, changes = true, params = {T.usize};
            else if (m == "contains") b = BMethod::contains, params = {el}, result = T.bool_;
        }
        if (b == BMethod::none) {
            error(e->callee->span, name_of(recv) + " has no method " + q(m));
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        e->bmethod = b;
        if (e->args.size() != params.size()) {
            error(e->span, q(m) + " takes " +
                               (params.empty() ? std::string("no arguments")
                                               : params.size() == 1 ? std::string("one argument")
                                                                    : std::to_string(params.size()) + " arguments"));
            for (Arg& a : e->args) check(a.value, nullptr);
            return T.error;
        }
        for (std::size_t i = 0; i < params.size(); i++) {
            Arg& a = e->args[i];
            if (a.name || a.is_mut) error(a.span, "this argument can't be named or `mut`");
            Type* at = check(a.value, params[i]);
            if (!params[i] && !is_bad(at) && at->kind != TypeKind::str && at->kind != TypeKind::string) {
                error(a.value->span, q(m) + " takes a `str` or a `String`, not " + a_type(at));
                return T.error;
            }
        }
        if (b == BMethod::contains && recv->kind == TypeKind::list && !equatable(recv->elem) && !is_bad(recv->elem)) {
            error(e->callee->span, "`contains` compares with `==`, which " + name_of(recv->elem) + " doesn't have");
            return T.error;
        }
        if (changes) {
            Expr* lhs = e->callee->lhs;
            if (Local* root = place_root(lhs)) {
                require_writable(lhs, root, false);
                // D48: the string being added can't be the one it's added to.
                if (b == BMethod::push_str && mentions(e->args[0].value, root)) {
                    error(e->args[0].value->span, q(text(root->name)) + " is changed by `push_str`, so it can't be "
                                                  "what's pushed; push a copy");
                }
            } else {
                error(lhs->span, q(m) + " changes " + name_of(recv) + ", so call it on a variable");
            }
        }
        return result;
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
        else if (m == "checked_add" && is_int(recv)) pm = PrimMethod::checked_add, args = 1;
        else if (m == "checked_sub" && is_int(recv)) pm = PrimMethod::checked_sub, args = 1;
        else if (m == "checked_mul" && is_int(recv)) pm = PrimMethod::checked_mul, args = 1;
        else if (m == "checked_div" && is_int(recv)) pm = PrimMethod::checked_div, args = 1;
        else if (m == "checked_rem" && is_int(recv)) pm = PrimMethod::checked_rem, args = 1;
        else if (m == "saturating_add" && is_int(recv)) pm = PrimMethod::saturating_add, args = 1;
        else if (m == "saturating_sub" && is_int(recv)) pm = PrimMethod::saturating_sub, args = 1;
        else if (m == "saturating_mul" && is_int(recv)) pm = PrimMethod::saturating_mul, args = 1;
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
        if (pm >= PrimMethod::checked_add && pm <= PrimMethod::checked_rem) return T.optional_of(recv);
        Expr* lhs = e->callee->lhs;
        if (args == 1 && lhs->has_bits && e->args[0].value->has_bits && is_int(recv)) {
            if (pm == PrimMethod::wrapping_add) set_int(e, recv, lhs->const_bits + e->args[0].value->const_bits);
            else if (pm == PrimMethod::wrapping_sub) set_int(e, recv, lhs->const_bits - e->args[0].value->const_bits);
            else if (pm == PrimMethod::wrapping_mul) set_int(e, recv, lhs->const_bits * e->args[0].value->const_bits);
        }
        return recv;
    }

    // --- after the bodies ----------------------------------------------------------------------

    // Puts the types C needs defined in an order where each comes after the types it holds by
    // value, and rejects a struct or enum that holds itself.
    void order_types() {
        std::unordered_map<Type*, int> state;  // 1 visiting, 2 done
        std::unordered_set<const Item*> reported;
        // Generic declarations are walked too, to find a type that holds itself, but only closed
        // types are emitted.
        auto visit = [&](auto& self, Type* t) -> void {
            switch (t->kind) {
                case TypeKind::struct_: case TypeKind::array: case TypeKind::enum_: case TypeKind::optional:
                case TypeKind::tuple: case TypeKind::list: break;
                default: return;
            }
            int& st = state[t];
            if (st == 2) return;
            if (t->kind == TypeKind::list) {
                // Its elements are behind a pointer, so it needs only their declaration.
                st = 2;
                if (!t->open) prog_.type_order.push_back(t);
                return;
            }
            if (st == 1) {
                if (t->kind == TypeKind::struct_ || t->kind == TypeKind::enum_) {
                    const Item* owner = t->generic ? t->generic : t->item;
                    if (reported.insert(owner).second) {
                        error(owner->name_span, q(text(owner->name)) + " holds itself, so it would have no end");
                    }
                }
                return;
            }
            st = 1;
            if (t->kind == TypeKind::array || t->kind == TypeKind::optional) {
                self(self, t->elem);
            } else if (t->kind == TypeKind::tuple) {
                for (Type* x : t->elems) self(self, x);
            } else if (t->kind == TypeKind::struct_) {
                for (const FieldDecl& f : t->item->fields) {
                    if (f.resolved) self(self, f.resolved);
                }
            } else {
                for (const VariantDecl& v : t->item->variants) {
                    for (Type* x : v.types) self(self, x);
                }
            }
            state[t] = 2;
            if (!t->open) prog_.type_order.push_back(t);
        };
        for (Item* s : structs_) visit(visit, s->declared);
        for (Item* e : enums_) visit(visit, e->declared);
        for (std::size_t i = 0; i < instances_.size(); i++) visit(visit, instances_[i]);
        for (Type* a : T.compounds()) visit(visit, a);
    }

    void check_unused_items() {
        for (const auto& info : impl_infos_) {
            if (!info->iface || info->used || info->iface->is_pub) continue;
            const Item* owner = info->self->generic ? info->self->generic : info->self->item;
            if (owner && owner->is_pub) continue;
            error(info->impl->self_type->span, "this impl of " + q(text(info->iface->name)) + " for " + name_of(info->self) +
                                                   " is never used");
        }
        for (const auto& [m, scope] : scopes_) {
            for (const auto& [n, imp] : scope->imports) {
                if (!imp.used) error(imp.span, q(text(n)) + " is imported but never used");
            }
        }
        for (Module* mod : prog_.modules) for (Item* it : mod->items) {
            if (it->kind == ItemKind::impl) {
                for (Item* m : it->members) {
                    if (m->kind == ItemKind::fn && !m->is_pub && !m->used && m->impl_type && !m->interface_item) {
                        error(m->name_span, "function " + q(text(m->name)) + " is never used");
                    }
                }
                continue;
            }
            if (it->is_pub || it->used) continue;
            if (it->kind == ItemKind::fn && text(it->name) == "main" && mod == prog_.modules.back()) continue;
            if (it->kind == ItemKind::fn || is_type_item(it) || it->kind == ItemKind::const_ || it->kind == ItemKind::interface) {
                if (it->kind == ItemKind::fn && !it->body) continue;
                error(it->name_span, std::string(item_kind(it)) + " " + q(text(it->name)) + " is never used");
            }
        }
    }

    void find_main(Module* m) {
        auto& items = scopes_.at(m)->items;
        auto it = items.find(ctx_.names.intern("main"));
        if (it == items.end() || it->second->kind != ItemKind::fn) {
            error(Span{m->file, 0, 0}, "a program needs a `fn main()`");
            return;
        }
        Item* f = it->second;
        if (!f->type_params.empty()) {
            error(f->name_span, "`main` can't be generic");
            return;
        }
        if (!f->params.empty() || (f->result_type && f->result_type->kind != TypeKind::unit)) {
            error(f->name_span, "`main` takes no parameters and returns nothing");
        }
        prog_.main = f;
    }

    CheckContext ctx_;
    Types& T;
    Program& prog_;
    Name n_self_, n_underscore_, n_Self_, n_List_, n_Copy_, n_copy_;
    std::vector<Name> builtin_names_, reserved_names_;
    std::unordered_map<const Module*, std::unique_ptr<Scope>> scopes_;
    Scope* scope_ = nullptr;  // the module being worked on
    std::vector<Item*> structs_, enums_, consts_, impls_, all_fns_;
    std::unordered_map<Item*, bool> checking_consts_;
    Item* current_item_ = nullptr;
    Type* current_impl_type_ = nullptr;
    Item* fn_ = nullptr;
    std::vector<Local*> visible_;
    std::vector<std::size_t> scope_marks_;
    std::vector<LoopCtx> loops_;
    std::uint32_t next_loop_ = 1;
    bool pattern_var_ = false;  // the names a pattern binds are `var`s
    std::vector<std::unique_ptr<DPat>> pool_;
    // Generics.
    std::vector<Item*> interfaces_;
    std::vector<std::unique_ptr<ImplInfo>> impl_infos_;
    std::unordered_map<const void*, std::vector<ImplInfo*>> impls_by_head_;
    std::vector<std::pair<Type*, GenericParam*>> param_decls_;
    std::unordered_map<Type*, Name> param_names_;
    std::unordered_map<Type*, Span> param_spans_;
    std::unordered_map<Type*, Item*> param_owner_;
    std::unordered_map<Type*, bool> param_used_;
    std::vector<std::pair<Name, Type*>> env_;
    std::map<std::pair<Item*, std::vector<Type*>>, Type*> type_instances_;
    std::map<std::pair<Item*, std::vector<Type*>>, Item*> fn_instances_;
    std::vector<Type*> instances_;
    std::unordered_map<Item*, std::vector<Type*>> unfilled_;
    std::vector<Item*> pending_fns_;
    std::vector<DeferredBounds> deferred_bounds_;
    std::unordered_set<std::string> c_names_;
    std::unordered_set<Item*> failed_generics_;
    std::vector<Type*> pending_defaults_;  // instances made before the generic structs' defaults were checked
    bool defaults_ready_ = false;
    bool defer_bounds_ = false;
    bool instance_mode_ = false;
};

}  // namespace

void check_program(CheckContext ctx, const std::vector<Module*>& modules, Program& program, bool need_main) {
    Checker c(ctx, program);
    c.run(modules, need_main);
}

}  // namespace calx
