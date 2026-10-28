#include "expand.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "check.h"
#include "emit.h"
#include "toolchain.h"

namespace calx {

namespace fs = std::filesystem;

namespace {

std::string q(std::string_view s) { return "`" + std::string(s) + "`"; }

// The attributes the compiler knows itself.
bool builtin_attribute(std::string_view n) { return n == "track_caller" || n == "export" || n == "attribute"; }

// How many programs' expansions the cache keeps (D64). Each is a few small text files.
constexpr std::size_t kKeepExpansions = 256;

// Keeps the `keep` expansions used most recently, by their answers' times, and deletes the rest,
// except those another build may be using.
void trim_expansions(const fs::path& dir, std::size_t keep) {
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> entries;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory(ec)) continue;
        fs::file_time_type t = fs::last_write_time(e.path() / "answer", ec);
        entries.push_back({ec ? fs::file_time_type::min() : t, e.path()});
    }
    if (entries.size() <= keep) return;
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    fs::file_time_type recent = fs::file_time_type::clock::now() - kCacheGrace;
    for (std::size_t i = keep; i < entries.size(); i++) {
        if (entries[i].first < recent) fs::remove_all(entries[i].second, ec);
    }
}

bool has_attribute(const Item* it, const Names& names, std::string_view name) {
    for (const Attribute& a : it->attributes) {
        if (a.name.parts.size() == 1 && names.text(a.name.parts[0]) == name) return true;
    }
    return false;
}

// What an attribute's parameter after the declaration takes, and how the request writes it.
enum class ArgKind { text, integer, flag, variant };

struct ParamInfo {
    ArgKind kind = ArgKind::text;
    Name name;
    std::string type;          // as written: `String`, `i32`, `State`
    Item* enum_item = nullptr; // variant: the enum
    Module* enum_module = nullptr;
};

// An attribute function the program uses.
struct Attr {
    Item* fn = nullptr;
    Module* module = nullptr;
    std::size_t index = 0;   // its place in the request and the driver
    ItemKind applies_to = ItemKind::struct_;
    std::vector<ParamInfo> params;
    bool can_fail = false;
    bool valid = true;
};

struct Use {
    Module* module;
    Item* item;
    const Attribute* attr;
    Attr* fn;
};

// Integer types an attribute's parameter can be, with their ranges.
struct IntType {
    const char* name;
    bool is_signed;
    int bits;
};
constexpr IntType kInts[] = {
    {"i8", true, 8},    {"i16", true, 16},  {"i32", true, 32},  {"i64", true, 64},  {"isize", true, 64},
    {"u8", false, 8},   {"u16", false, 16}, {"u32", false, 32}, {"u64", false, 64}, {"usize", false, 64},
};

const IntType* int_type(std::string_view name) {
    for (const IntType& t : kInts) if (name == t.name) return &t;
    return nullptr;
}

// A piece of a request or an answer: its length in bytes, `:`, and the bytes.
void put(std::string& out, std::string_view s) {
    out += std::to_string(s.size());
    out += ':';
    out += s;
}
void put_count(std::string& out, std::size_t n) { put(out, std::to_string(n)); }
void put_flag(std::string& out, bool b) { put(out, b ? "1" : "0"); }

// Reads the pieces of an answer.
struct PieceReader {
    const std::string& s;
    std::size_t at = 0;
    bool ok = true;

    bool piece(std::string& out) {
        std::size_t n = 0;
        std::size_t i = at;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') n = n * 10 + static_cast<std::size_t>(s[i++] - '0');
        if (i == at || i >= s.size() || s[i] != ':' || s.size() - (i + 1) < n) return ok = false;
        out = s.substr(i + 1, n);
        at = i + 1 + n;
        return true;
    }
    bool done() const { return at >= s.size(); }
};

// A count in an answer, as digits. Anything else is 0, and the answer's pieces run out.
std::size_t to_count(std::string_view s) {
    std::size_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    return n;
}

class Expander {
public:
    Expander(ParseContext ctx, Sources& sources, ModuleLoader& loader, const ExpandOptions& opt)
        : ctx_(ctx), sources_(sources), loader_(loader), opt_(opt) {}

    bool run() {
        find_uses();
        if (uses_.empty()) return !ctx_.diags.has_errors();
        check_cycles();
        if (ctx_.diags.has_errors()) return false;
        std::string request;
        put_count(request, uses_.size());
        for (const Use& u : uses_) write_use(request, u);
        if (ctx_.diags.has_errors()) return false;

        std::string error;
        if (!find_toolchain(tc_, error)) {
            ctx_.diags.error(uses_[0].attr->span, "can't run " + attr_name(uses_[0]) + ": " + error);
            return false;
        }
        std::string self = compiler_hash(tc_);
        std::vector<std::string_view> parts{"expand-1", self, request};
        for (Module* m : loader_.modules()) {
            if (!closure_.count(m) || m->builtin) continue;
            const SourceFile& f = sources_.file(m->file);
            parts.push_back(f.path);
            parts.push_back(f.text);
        }
        key_ = hash_hex(parts);
        dir_ = fs::path(tc_.cache) / "expand" / key_;
        std::string answer;
        std::string saved = (dir_ / "answer").string();
        if (read_file(saved, answer)) {
            std::error_code ec;
            fs::last_write_time(saved, fs::file_time_type::clock::now(), ec);  // just used, for the trim
        } else if (!run_attributes(request, answer)) {
            return false;
        }
        return apply(answer);
    }

private:
    std::string_view text(Name n) const { return ctx_.names.text(n); }

    std::string span_text(Span s) const {
        const SourceFile& f = sources_.file(s.file);
        return f.text.substr(s.begin, s.end - s.begin);
    }

    std::string attr_name(const Use& u) const { return q("@" + std::string(text(u.attr->name.parts[0]))); }

    std::string dotted(const Slice<Name>& parts) const {
        std::string s;
        for (std::size_t i = 0; i < parts.size(); i++) {
            if (i) s += '.';
            s += text(parts[i]);
        }
        return s;
    }

    static bool named_kind(ItemKind k) {
        return k == ItemKind::fn || k == ItemKind::struct_ || k == ItemKind::enum_ || k == ItemKind::const_ ||
               k == ItemKind::interface || k == ItemKind::type_alias;
    }

    // What `name` is in module `m`, as the checker will find it: one of its own items, or one it
    // imports. `from` is the module the item is declared in.
    Item* lookup(Module* m, Name name, Module*& from) const {
        for (Item* it : m->items) {
            if (named_kind(it->kind) && it->name == name) {
                from = m;
                return it;
            }
        }
        for (Item* it : m->items) {
            if (it->kind != ItemKind::import || !it->imported) continue;
            for (const ImportName& n : it->import_names) {
                if ((n.alias ? n.alias : n.name) != name) continue;
                for (Item* t : it->imported->items) {
                    if (named_kind(t->kind) && t->name == n.name) {
                        from = it->imported;
                        return t;
                    }
                }
            }
        }
        return nullptr;
    }

    void find_uses() {
        for (Module* m : loader_.modules()) {
            if (m->builtin) continue;
            for (Item* it : m->items) {
                bool can_have = it->kind == ItemKind::struct_ || it->kind == ItemKind::enum_ ||
                                (it->kind == ItemKind::fn && !it->is_extern);
                if (!can_have) continue;
                for (const Attribute& a : it->attributes) {
                    if (a.name.parts.size() != 1 || builtin_attribute(text(a.name.parts[0]))) continue;
                    Module* from = nullptr;
                    Item* f = lookup(m, a.name.parts[0], from);
                    // Anything else is the checker's to explain.
                    if (!f || f->kind != ItemKind::fn || !has_attribute(f, ctx_.names, "attribute")) continue;
                    if (from == m) {
                        ctx_.diags.error(a.span, q("@" + std::string(text(f->name))) +
                                                     " is defined in this module, so it can't be used here: the compiler "
                                                     "builds and runs an attribute before the modules that use it");
                        continue;
                    }
                    Attr* af = attr_for(f, from);
                    uses_.push_back(Use{m, it, &a, af});
                }
            }
        }
    }

    // The modules the attributes need, which are built before the rest, so none of them can use
    // an attribute itself.
    void check_cycles() {
        std::vector<Module*> work;
        for (const auto& [fn, a] : attrs_) work.push_back(a->module);
        while (!work.empty()) {
            Module* m = work.back();
            work.pop_back();
            if (!closure_.insert(m).second) continue;
            for (Item* it : m->items) {
                if (it->kind == ItemKind::import && it->imported) work.push_back(it->imported);
            }
        }
        for (const Use& u : uses_) {
            if (!closure_.count(u.module)) continue;
            ctx_.diags.error(u.attr->span, attr_name(u) + " can't be used in " + q(dotted(u.module->name.parts)) +
                                               ", since the attributes need that module themselves, and they're built "
                                               "before the modules that use them");
        }
    }

    void error(Span s, std::string msg) { ctx_.diags.error(s, std::move(msg)); }

    Attr* attr_for(Item* f, Module* from) {
        auto found = attrs_.find(f);
        if (found != attrs_.end()) return found->second.get();
        auto a = std::make_unique<Attr>();
        a->fn = f;
        a->module = from;
        a->index = attrs_.size();
        a->can_fail = f->error != nullptr;
        std::string name = q("@" + std::string(text(f->name)));
        auto bad = [&](Span s, std::string msg) {
            error(s, std::move(msg));
            a->valid = false;
        };
        if (!f->generics.empty()) bad(f->generics[0].span, "an attribute can't be generic");
        if (f->params.empty() || f->params[0].is_self) {
            bad(f->name_span, "an attribute's first parameter is the declaration it's on: a `StructDecl`, an `EnumDecl` or an `FnDecl`");
        } else {
            const Param& p = f->params[0];
            std::string t = p.type && p.type->kind == TypeExprKind::named && p.type->path.parts.size() == 1
                                ? std::string(text(p.type->path.parts[0]))
                                : std::string();
            if (t == "StructDecl") {
                a->applies_to = ItemKind::struct_;
            } else if (t == "EnumDecl") {
                a->applies_to = ItemKind::enum_;
            } else if (t == "FnDecl") {
                a->applies_to = ItemKind::fn;
            } else {
                bad(p.span, "an attribute's first parameter is the declaration it's on: a `StructDecl`, an `EnumDecl` or an `FnDecl`");
            }
        }
        for (std::size_t i = 1; i < f->params.size(); i++) {
            const Param& p = f->params[i];
            ParamInfo info;
            info.name = p.name;
            std::string why = "an attribute's parameters after the declaration are `String`, integers, `bool` and enums "
                              "without data, which a use gives as literals";
            if (p.mode != ParamMode::read) {
                bad(p.span, "an attribute's parameters are looked at, not `mut` or `own`");
                continue;
            }
            if (!p.type || p.type->kind != TypeExprKind::named || p.type->path.parts.size() != 1 || !p.type->args.empty()) {
                bad(p.type ? p.type->span : p.span, why);
                continue;
            }
            info.type = std::string(text(p.type->path.parts[0]));
            if (info.type == "String") {
                info.kind = ArgKind::text;
            } else if (info.type == "bool") {
                info.kind = ArgKind::flag;
            } else if (int_type(info.type)) {
                info.kind = ArgKind::integer;
            } else {
                Module* em = nullptr;
                Item* e = lookup(from, p.type->path.parts[0], em);
                bool plain = e && e->kind == ItemKind::enum_ && e->generics.empty() && !e->variants.empty();
                if (plain) {
                    for (const VariantDecl& v : e->variants) {
                        if (!v.payload.empty() || v.has_fields) plain = false;
                    }
                }
                if (!plain) {
                    bad(p.type->span, why);
                    continue;
                }
                info.kind = ArgKind::variant;
                info.enum_item = e;
                info.enum_module = em;
            }
            a->params.push_back(info);
        }
        bool result_ok = f->result && f->result->kind == TypeExprKind::named && f->result->path.parts.size() == 1 &&
                         text(f->result->path.parts[0]) == "Generated";
        if (!result_ok) bad(f->result ? f->result->span : f->name_span, "an attribute returns a `Generated`: the code it adds");
        if (f->error && !(f->error->kind == TypeExprKind::named && f->error->path.parts.size() == 1 &&
                          text(f->error->path.parts[0]) == "String")) {
            bad(f->error->span, "an attribute that can refuse a declaration fails with a `String`: the reason");
        }
        Attr* out = a.get();
        attrs_.emplace(f, std::move(a));
        return out;
    }

    // --- the request -------------------------------------------------------------------------

    void write_use(std::string& out, const Use& u) {
        Attr* a = u.fn;
        if (!a->valid) {
            error(u.attr->span, attr_name(u) + " can't run until the errors in its definition are fixed");
            return;
        }
        if (u.item->kind != a->applies_to) {
            const char* what = a->applies_to == ItemKind::struct_ ? "a struct" : a->applies_to == ItemKind::enum_ ? "an enum" : "a function";
            error(u.attr->span, attr_name(u) + " is for " + what + ", since its first parameter is " +
                                    (a->applies_to == ItemKind::struct_ ? "a `StructDecl`" : a->applies_to == ItemKind::enum_ ? "an `EnumDecl`" : "an `FnDecl`"));
            return;
        }
        // Arguments in order, then by name.
        std::vector<const Arg*> given(a->params.size(), nullptr);
        std::size_t next = 0;
        bool ok = true;
        for (const Arg& arg : u.attr->args) {
            std::size_t at = next;
            if (arg.name) {
                at = a->params.size();
                for (std::size_t i = 0; i < a->params.size(); i++) {
                    if (a->params[i].name == arg.name) at = i;
                }
                if (at == a->params.size()) {
                    error(arg.span, attr_name(u) + " has no parameter " + q(text(arg.name)));
                    ok = false;
                    continue;
                }
            } else {
                next++;
            }
            if (at >= a->params.size()) {
                error(arg.span, attr_name(u) + " takes " + std::to_string(a->params.size()) + " argument" +
                                    (a->params.size() == 1 ? "" : "s"));
                ok = false;
                continue;
            }
            if (given[at]) {
                error(arg.span, q(text(a->params[at].name)) + " is given twice");
                ok = false;
                continue;
            }
            given[at] = &arg;
        }
        std::vector<std::string> values;
        for (std::size_t i = 0; i < a->params.size(); i++) {
            if (!given[i]) {
                if (ok) error(u.attr->span, attr_name(u) + " needs its " + q(text(a->params[i].name)) + " argument");
                ok = false;
                continue;
            }
            std::string v;
            if (arg_value(u, a->params[i], given[i]->value, v)) {
                values.push_back(v);
            } else {
                ok = false;
            }
        }
        if (!ok) return;
        put_count(out, a->index);
        put_count(out, values.size());
        for (const std::string& v : values) put(out, v);
        switch (u.item->kind) {
            case ItemKind::struct_: write_struct(out, u.item); break;
            case ItemKind::enum_: write_enum(out, u.item); break;
            default: write_fn(out, u.item); break;
        }
    }

    // An argument as the request gives it, checked against what the parameter takes.
    bool arg_value(const Use& u, const ParamInfo& p, const Expr* e, std::string& out) {
        std::string where = attr_name(u) + "'s " + q(text(p.name));
        switch (p.kind) {
            case ArgKind::text: {
                bool plain = e->kind == ExprKind::string_lit && e->interpolations.empty();
                if (!plain) {
                    error(e->span, where + " is a `String`, so it takes a string literal");
                    return false;
                }
                for (const StringPart& part : e->literal.parts) out += part.text;
                return true;
            }
            case ArgKind::flag:
                if (e->kind != ExprKind::bool_lit) {
                    error(e->span, where + " is a `bool`, so it takes `true` or `false`");
                    return false;
                }
                out = e->bool_value ? "1" : "0";
                return true;
            case ArgKind::integer: {
                bool negative = e->kind == ExprKind::unary && e->op == Op::neg && e->lhs && e->lhs->kind == ExprKind::int_lit;
                const Expr* lit = negative ? e->lhs : e;
                const IntType* t = int_type(p.type);
                if (lit->kind != ExprKind::int_lit) {
                    error(e->span, where + " is " + (p.type[0] == 'i' ? "an " : "a ") + q(p.type) + ", so it takes an integer literal");
                    return false;
                }
                if (!lit->literal.suffix.empty() && lit->literal.suffix != p.type) {
                    error(e->span, where + " is " + (p.type[0] == 'i' ? "an " : "a ") + q(p.type) + ", not " + q(lit->literal.suffix));
                    return false;
                }
                std::uint64_t v = lit->literal.int_value;
                std::uint64_t max = t->is_signed ? (std::uint64_t{1} << (t->bits - 1)) - 1
                                                 : (t->bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << t->bits) - 1);
                bool fits = !lit->literal.overflow && (negative ? t->is_signed && v <= max + 1 : v <= max);
                if (!fits) {
                    error(e->span, span_text(e->span) + " doesn't fit in " + (p.type[0] == 'i' ? "an " : "a ") + q(p.type));
                    return false;
                }
                // As an i64: the driver turns it back with `wrap`.
                std::int64_t s = negative ? static_cast<std::int64_t>(~v + 1) : static_cast<std::int64_t>(v);
                out = std::to_string(s);
                return true;
            }
            case ArgKind::variant: {
                std::string such = q(std::string(text(p.enum_item->name)) + "." +
                                     std::string(text(p.enum_item->variants[0].name)));
                bool shape = e->kind == ExprKind::field && !e->is_tuple_index && e->lhs && e->lhs->kind == ExprKind::name &&
                             e->lhs->path.parts.size() == 1;
                Module* from = nullptr;
                Item* named = shape ? lookup(u.module, e->lhs->path.parts[0], from) : nullptr;
                if (named != p.enum_item) {
                    error(e->span, where + " is " + (std::string("aeiouAEIOU").find(p.type[0]) != std::string::npos ? "an " : "a ") +
                                       q(p.type) + ", so it takes one of its variants, such as " + such);
                    return false;
                }
                for (std::size_t i = 0; i < p.enum_item->variants.size(); i++) {
                    if (p.enum_item->variants[i].name == e->member) {
                        out = std::to_string(i);
                        return true;
                    }
                }
                error(e->span, q(p.type) + " has no variant " + q(text(e->member)));
                return false;
            }
        }
        return false;
    }

    void write_type(std::string& out, const TypeExpr* t) {
        if (!t) {
            put_count(out, 7);
            put(out, "Self");
            put(out, "Self");
            put(out, "");
            put_count(out, 0);
            return;
        }
        int kind = 0;
        switch (t->kind) {
            case TypeExprKind::named: kind = 0; break;
            case TypeExprKind::optional: kind = 1; break;
            case TypeExprKind::array: kind = 2; break;
            case TypeExprKind::tuple: kind = 3; break;
            case TypeExprKind::fn: kind = 4; break;
            case TypeExprKind::pointer: kind = 5; break;
            case TypeExprKind::dyn: kind = 6; break;
            case TypeExprKind::self: kind = 7; break;
        }
        put_count(out, static_cast<std::size_t>(kind));
        put(out, span_text(t->span));
        if (t->kind == TypeExprKind::named) {
            put(out, dotted(t->path.parts));
        } else if (t->kind == TypeExprKind::dyn && t->inner) {
            put(out, span_text(t->inner->span));
        } else if (t->kind == TypeExprKind::self) {
            put(out, "Self");
        } else {
            put(out, "");
        }
        put(out, t->kind == TypeExprKind::array && t->length ? span_text(t->length->span) : std::string());
        std::vector<const TypeExpr*> args;
        if (t->kind == TypeExprKind::optional || t->kind == TypeExprKind::array || t->kind == TypeExprKind::pointer) {
            if (t->inner) args.push_back(t->inner);
        } else if (t->kind != TypeExprKind::dyn) {
            for (const TypeExpr* x : t->args) args.push_back(x);
        }
        put_count(out, args.size());
        for (const TypeExpr* x : args) write_type(out, x);
    }

    void write_unit(std::string& out) {
        put_count(out, 3);
        put(out, "()");
        put(out, "");
        put(out, "");
        put_count(out, 0);
    }

    void write_generics(std::string& out, const Item* it) {
        put_count(out, it->generics.size());
        for (const GenericParam& g : it->generics) put(out, text(g.name));
    }

    void write_fields(std::string& out, const Slice<FieldDecl>& fields) {
        put_count(out, fields.size());
        for (const FieldDecl& f : fields) {
            put(out, text(f.name));
            put_flag(out, f.is_pub);
            put(out, f.doc);
            write_type(out, f.type);
            put(out, f.default_value ? span_text(f.default_value->span) : std::string());
            put_count(out, f.attributes.size());
            for (const Attribute& a : f.attributes) {
                put(out, dotted(a.name.parts));
                put_count(out, a.args.size());
                for (const Arg& arg : a.args) put(out, span_text(arg.span));
            }
        }
    }

    void write_struct(std::string& out, const Item* it) {
        put(out, text(it->name));
        put_flag(out, it->is_pub);
        put(out, it->doc);
        write_generics(out, it);
        write_fields(out, it->fields);
    }

    void write_enum(std::string& out, const Item* it) {
        put(out, text(it->name));
        put_flag(out, it->is_pub);
        put(out, it->doc);
        write_generics(out, it);
        put(out, it->repr ? span_text(it->repr->span) : std::string());
        put_count(out, it->variants.size());
        for (const VariantDecl& v : it->variants) {
            put(out, text(v.name));
            put(out, v.doc);
            put(out, v.value ? span_text(v.value->span) : std::string());
            put_count(out, v.payload.size());
            for (const TypeExpr* t : v.payload) write_type(out, t);
            write_fields(out, v.fields);
        }
    }

    void write_fn(std::string& out, const Item* it) {
        put(out, text(it->name));
        put_flag(out, it->is_pub);
        put(out, it->doc);
        write_generics(out, it);
        put_flag(out, it->is_unsafe);
        put_count(out, it->params.size());
        for (const Param& p : it->params) {
            put(out, p.is_self ? "self" : text(p.name));
            put_count(out, p.mode == ParamMode::mut ? 1 : p.mode == ParamMode::own ? 2 : 0);
            put_flag(out, p.is_self);
            write_type(out, p.type);
        }
        if (it->result) {
            write_type(out, it->result);
        } else {
            write_unit(out);
        }
        put_flag(out, it->error != nullptr);
        if (it->error) {
            write_type(out, it->error);
        } else {
            write_unit(out);
        }
    }

    // --- running them ------------------------------------------------------------------------

    // The program that runs the attributes: it reads each use, calls its attribute, and answers.
    std::string driver() const {
        std::string s = "// The program the compiler builds to run this program's attributes (D124).\n";
        s += "import std.meta.{Request as MetaRequest, Generated as MetaGenerated, answer as meta_answer, refuse as meta_refuse}\n";
        std::vector<const Attr*> order(attrs_.size());
        for (const auto& [fn, a] : attrs_) order[a->index] = a.get();
        std::unordered_map<const Item*, std::size_t> enums;
        std::vector<const ParamInfo*> enum_list;
        for (const Attr* a : order) {
            s += "import " + dotted(a->module->name.parts) + ".{" + std::string(text(a->fn->name)) + " as attribute_" +
                 std::to_string(a->index) + "}\n";
            for (const ParamInfo& p : a->params) {
                if (p.kind != ArgKind::variant || enums.count(p.enum_item)) continue;
                enums[p.enum_item] = enum_list.size();
                enum_list.push_back(&p);
                s += "import " + dotted(p.enum_module->name.parts) + ".{" + std::string(text(p.enum_item->name)) +
                     " as Enum_" + std::to_string(enums[p.enum_item]) + "}\n";
            }
        }
        for (std::size_t i = 0; i < enum_list.size(); i++) {
            const Item* e = enum_list[i]->enum_item;
            if (e->variants.size() < 2) continue;
            std::string t = "Enum_" + std::to_string(i);
            s += "\nfn variant_" + std::to_string(i) + "(n: i64) -> " + t + " {\n    return match n {\n";
            for (std::size_t v = 0; v < e->variants.size(); v++) {
                s += "        " + (v + 1 == e->variants.size() ? std::string("_") : std::to_string(v)) + " => " + t + "." +
                     std::string(text(e->variants[v].name)) + ",\n";
            }
            s += "    }\n}\n";
        }
        s += "\nfn run(r: mut MetaRequest) -> MetaGenerated ! String {\n    let which = r.attribute()\n";
        for (const Attr* a : order) {
            s += "    if which == " + std::to_string(a->index) + " {\n";
            const char* decl = a->applies_to == ItemKind::struct_ ? "struct_decl" : a->applies_to == ItemKind::enum_ ? "enum_decl" : "fn_decl";
            s += "        let decl = r." + std::string(decl) + "()\n";
            std::string call = "attribute_" + std::to_string(a->index) + "(decl";
            for (std::size_t i = 0; i < a->params.size(); i++) {
                const ParamInfo& p = a->params[i];
                std::string arg = "a" + std::to_string(i), n = std::to_string(i), v;
                switch (p.kind) {
                    case ArgKind::text: v = "r.text(" + n + ")"; break;
                    case ArgKind::flag: v = "r.flag(" + n + ")"; break;
                    case ArgKind::integer: v = p.type == "i64" ? "r.int(" + n + ")" : p.type + ".wrap(r.int(" + n + "))"; break;
                    case ArgKind::variant: {
                        std::size_t k = enums.at(p.enum_item);
                        v = p.enum_item->variants.size() < 2
                                ? "Enum_" + std::to_string(k) + "." + std::string(text(p.enum_item->variants[0].name))
                                : "variant_" + std::to_string(k) + "(r.int(" + n + "))";
                        break;
                    }
                }
                s += "        let " + arg + " = " + v + "\n";
                call += ", " + arg;
            }
            call += ")";
            s += "        return " + std::string(a->can_fail ? "try " : "") + call + "\n    }\n";
        }
        s += "    fail \"the compiler asked for attribute {which}, which isn't one\"\n}\n";
        s += "\nfn main() {\n    var r = MetaRequest.load()\n    while r.next() {\n        match run(mut r) {\n"
             "            ok(g) => meta_answer(g),\n            err(e) => meta_refuse(e),\n        }\n    }\n}\n";
        return s;
    }

    bool run_attributes(const std::string& request, std::string& answer) {
        // Built as a program of its own, from the same files, beside the main file so its imports
        // find the same modules.
        Sources sources;
        Names names;
        Arena arena;
        Types types(arena);
        Diagnostics diags(sources);
        std::string path = (fs::path(loader_.root()) / "calx_attributes.calx").generic_string();
        std::uint32_t file = sources.add(path, driver());
        ParseContext ctx{sources, names, arena, diags};
        std::vector<Module*> modules = load_program(ctx, sources, file, opt_.home, opt_.home_error);
        Program program;
        if (!diags.has_errors()) check_program(CheckContext{sources, names, arena, diags, types}, modules, program, true);
        if (diags.has_errors()) {
            if (opt_.json) {
                diags.print_json(stdout);
                std::fflush(stdout);
            } else {
                diags.print(stderr, opt_.color);
            }
            error(uses_[0].attr->span, attr_name(uses_[0]) + " can't run until the errors in the attributes' code are fixed");
            return false;
        }
        std::vector<CFile> files = emit_c(program, sources, names, EmitOptions{});
        std::vector<std::string> c_sources;
        for (CFile& f : files) c_sources.push_back(std::move(f.text));
        BuildResult b = build_program(tc_, c_sources, BuildMode::debug, BuildKind::program, opt_.verbose);
        if (!b.ok) {
            error(uses_[0].attr->span, "can't build the attributes");
            return false;
        }
        std::error_code ec;
        fs::create_directories(dir_.parent_path(), ec);
        std::string base = (dir_.parent_path() / key_).string();
        std::string in = base + ".request", out = base + ".answer", err = base + ".errors";
        if (!write_file_atomic(in, request)) {
            error(uses_[0].attr->span, "can't write " + in);
            return false;
        }
        int status = run_redirected({b.exe}, in, out, err);
        std::string output;
        read_file(out, output);
        answer = take_printed(output);
        std::string errors;
        read_file(err, errors);
        fs::remove(in, ec);
        fs::remove(out, ec);
        fs::remove(err, ec);
        if (status != 0) {
            // It answers each use as it goes, so the first without an answer is the one that stopped it.
            std::size_t done = count_answers(answer);
            const Use& u = uses_[done < uses_.size() ? done : uses_.size() - 1];
            // A panic writes `panic at PATH:LINE:COL: MESSAGE`, then a frame a line. The message is
            // the error, and each frame a note at its line, as far as the trace is the
            // attributes' own: it ends in the program that ran them, which isn't anyone's code.
            std::vector<std::string> lines;
            for (std::size_t at = 0; at < errors.size();) {
                std::size_t end = errors.find('\n', at);
                if (end == std::string::npos) end = errors.size();
                std::string line = errors.substr(at, end - at);
                while (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) lines.push_back(line);
                at = end + 1;
            }
            std::string message = lines.empty() ? "it ended with status " + std::to_string(status) : lines[0];
            if (message.rfind("panic at ", 0) == 0) {
                std::size_t colon = message.find(": ");
                if (colon != std::string::npos) message = "it panicked: " + message.substr(colon + 2);
            }
            error(u.attr->span, attr_name(u) + " stopped while it generated code for " + q(text(u.item->name)) + "; " + message);
            bool first = true;
            for (std::size_t i = 1; i < lines.size(); i++) {
                const std::string& line = lines[i];
                if (line.rfind("\tat ", 0) != 0) {
                    ctx_.diags.note(Span{}, line);
                    continue;
                }
                std::size_t open = line.find('('), close = line.rfind(')');
                if (open == std::string::npos || close == std::string::npos || close < open) continue;
                std::string fn = line.substr(4, open - 4), where = line.substr(open + 1, close - open - 1);
                if (fn.rfind("calx_attributes.", 0) == 0) break;
                std::size_t colon = where.rfind(':');
                std::size_t number = 0;
                bool digits = colon != std::string::npos && colon + 1 < where.size();
                for (std::size_t k = colon + 1; digits && k < where.size(); k++) {
                    if (where[k] < '0' || where[k] > '9') digits = false;
                    else number = number * 10 + static_cast<std::size_t>(where[k] - '0');
                }
                Span at = digits ? line_span(where.substr(0, colon), number) : Span{};
                ctx_.diags.note(at, (first ? "it panicked in " : "called from ") + q(fn) + (at.file ? "" : ", at " + where));
                first = false;
            }
            return false;
        }
        // Kept for the next build: the answer, written last, so it's whole when it's there.
        fs::create_directories(dir_, ec);
        write_file_atomic((dir_ / "answer").string(), answer);
        trim_expansions(dir_.parent_path(), kKeepExpansions);
        return true;
    }

    // Line `line` of the module at `path`, as a trace names it, among the program's files: from
    // its first character to its end.
    Span line_span(const std::string& path, std::size_t line) const {
        std::error_code ec;
        fs::path want = fs::weakly_canonical(path, ec);
        for (Module* m : loader_.modules()) {
            if (m->builtin) continue;
            const SourceFile& f = sources_.file(m->file);
            if (fs::weakly_canonical(f.path, ec) != want || line == 0 || line > f.line_starts.size()) continue;
            std::uint32_t begin = f.line_starts[line - 1];
            std::string_view s = f.line_text(static_cast<std::uint32_t>(line));
            std::uint32_t skip = 0;
            while (skip < s.size() && (s[skip] == ' ' || s[skip] == '\t')) skip++;
            return Span{m->file, begin + skip, begin + static_cast<std::uint32_t>(s.size())};
        }
        return Span{};
    }

    // The answers in what the attributes' program wrote, each after the mark std.meta puts before
    // it. What an attribute printed comes before its use's answer, and goes to standard error, to
    // help whoever's writing it. It's printed only when the attributes run, not from the cache.
    std::string take_printed(const std::string& output) const {
        static const std::string mark("\0calx answer\0", 13);
        std::string answers;
        std::size_t at = 0;
        for (std::size_t i = 0; at <= output.size(); i++) {
            std::size_t next = output.find(mark, at);
            std::string printed = output.substr(at, next == std::string::npos ? std::string::npos : next - at);
            if (!printed.empty()) {
                const Use& u = uses_[i < uses_.size() ? i : uses_.size() - 1];
                const SourceFile& f = sources_.file(u.attr->span.file);
                std::fprintf(stderr, "%s, for %s at %s:%u, printed:\n%s%s", attr_name(u).c_str(),
                             q(text(u.item->name)).c_str(), f.path.c_str(), f.position(u.attr->span.begin).first,
                             printed.c_str(), printed.back() == '\n' ? "" : "\n");
            }
            if (next == std::string::npos) break;
            // One answer: its tag, then a reason, or a count and that many items.
            PieceReader r{output, next + mark.size()};
            std::string p;
            if (r.piece(p)) {
                if (p == "ok" && r.piece(p)) {
                    std::size_t k = to_count(p);
                    for (std::size_t j = 0; j < k && r.piece(p); j++) {}
                } else if (p == "err") {
                    r.piece(p);
                }
            }
            answers.append(output, next + mark.size(), r.at - (next + mark.size()));
            at = r.at;
        }
        std::fflush(stderr);
        return answers;
    }

    std::size_t count_answers(const std::string& answer) const {
        PieceReader r{answer};
        std::size_t n = 0;
        std::string p;
        while (!r.done() && r.piece(p)) {
            if (p == "err") {
                if (!r.piece(p)) break;
            } else if (p == "ok") {
                std::string count;
                if (!r.piece(count)) break;
                std::size_t k = to_count(count);
                bool whole = true;
                for (std::size_t i = 0; i < k && whole; i++) whole = r.piece(p);
                if (!whole) break;
            } else {
                break;
            }
            n++;
        }
        return n;
    }

    // --- adding what they generated ------------------------------------------------------------

    bool apply(const std::string& answer) {
        PieceReader r{answer};
        std::unordered_map<Module*, std::vector<Item*>> added;
        std::vector<Module*> order;
        std::unordered_set<std::string> names_used;
        std::error_code ec;
        for (const Use& u : uses_) {
            std::string tag;
            if (!r.piece(tag)) {
                error(u.attr->span, attr_name(u) + " gave no answer for " + q(text(u.item->name)));
                return false;
            }
            if (tag == "err") {
                std::string why;
                r.piece(why);
                error(u.attr->span, why);
                continue;
            }
            std::string count;
            r.piece(count);
            std::size_t k = to_count(count);
            std::string code;
            for (std::size_t i = 0; i < k; i++) {
                std::string item;
                r.piece(item);
                if (!code.empty()) code += '\n';  // one blank line between items, which each end their line
                code += item;
                if (!code.empty() && code.back() != '\n') code += '\n';
            }
            const SourceFile& at = sources_.file(u.attr->span.file);
            std::uint32_t line = at.position(u.attr->span.begin).first;
            std::string attr = std::string(text(u.attr->name.parts[0]));
            std::string what = q("@" + attr) + " on " + q(text(u.item->name)) + ", at " + at.path + ":" + std::to_string(line);
            // Named after what it came from, so errors and stack traces in it say so.
            std::string stem = dotted(u.module->name.parts) + "." + std::string(text(u.item->name)) + "." + attr;
            std::string name = stem + ".calx";
            for (int i = 2; !names_used.insert(name).second; i++) name = stem + "." + std::to_string(i) + ".calx";
            std::string path = (dir_ / name).generic_string();
            if (!file_exists(path)) {
                fs::create_directories(dir_, ec);
                write_file_atomic(path, code);
            }
            if (opt_.shown) opt_.shown->push_back(Expansion{what, path, code});
            std::uint32_t id = sources_.add_generated(path, code, u.attr->span,
                                                      "in the code " + q("@" + attr) + " generated for " + q(text(u.item->name)));
            Module* g = parse_file(ctx_, id);
            if (!g->name.parts.empty()) error(g->name.span, "generated code can't have a `module` line");
            if (!added.count(u.module)) order.push_back(u.module);
            std::vector<Item*>& into = added[u.module];
            for (Item* it : g->items) {
                it->generated = true;
                for (Item* m : it->members) m->generated = true;
                if (it->kind == ItemKind::import && !keep_import(u.module, into, it)) continue;
                into.push_back(it);
            }
        }
        for (Module* m : order) loader_.add_items(m, added[m]);
        return !ctx_.diags.has_errors();
    }

    // Whether a generated import adds anything: the module's code, or code generated for it
    // already, may import the same names from the same module, and Calx doesn't allow a name
    // twice. The names already there are dropped from it.
    bool keep_import(Module* m, const std::vector<Item*>& added, Item* imp) {
        std::string path = dotted(imp->import_path.parts);
        auto there = [&](const ImportName& n) {
            auto check = [&](const Item* it) {
                if (it->kind != ItemKind::import || dotted(it->import_path.parts) != path) return false;
                for (const ImportName& o : it->import_names) {
                    if (o.name == n.name && o.alias == n.alias) return true;
                }
                return false;
            };
            for (const Item* it : m->items) if (check(it)) return true;
            for (const Item* it : added) if (check(it)) return true;
            return false;
        };
        std::vector<ImportName> kept;
        for (const ImportName& n : imp->import_names) {
            if (!there(n)) kept.push_back(n);
        }
        if (kept.empty() && !imp->import_names.empty()) return false;
        imp->import_names = ctx_.arena.copy(kept);
        return true;
    }

    ParseContext ctx_;
    Sources& sources_;
    ModuleLoader& loader_;
    const ExpandOptions& opt_;
    Toolchain tc_;
    std::unordered_map<const Item*, std::unique_ptr<Attr>> attrs_;
    std::vector<Use> uses_;
    std::unordered_set<Module*> closure_;
    std::string key_;
    fs::path dir_;
};

}  // namespace

bool expand_attributes(ParseContext ctx, Sources& sources, ModuleLoader& loader, const ExpandOptions& options) {
    return Expander(ctx, sources, loader, options).run();
}

}  // namespace calx
