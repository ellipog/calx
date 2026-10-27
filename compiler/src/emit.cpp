#include "emit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace calx {

namespace {

const std::unordered_set<std::string_view>& c_reserved() {
    static const std::unordered_set<std::string_view> words = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern",
        "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed",
        "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
        "_Bool", "_Complex", "_Imaginary", "_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn",
        "_Static_assert", "_Thread_local", "asm", "typeof", "main",
    };
    return words;
}

std::string c_string_literal(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '?') {
            out += "\\?";  // never a trigraph
        } else if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", c);
            out += buf;
        }
    }
    out += '"';
    return out;
}

class Emitter {
public:
    Emitter(const Program& prog, const Sources& sources, const Names& names, const EmitOptions& opt)
        : prog_(prog), sources_(sources), names_(names), opt_(opt) {}

    // Each module becomes one C file with its own functions, a `static` copy of each generic
    // instance they use, and exactly the declarations those need, so the file's text says
    // everything it depends on and its hash is a complete cache key (D64, D75).
    std::vector<CFile> run() {
        for (Item* f : prog_.fns) fns_.emplace(f, emit_fn(f));
        for (Item* c : prog_.consts) consts_.emplace(c, emit_const(c));

        struct Plan {
            Module* module;
            std::vector<Item*> defs;     // defined here: its own functions, then the instances they reach
            std::vector<Item*> externs;  // written in another module, and called from here
            std::vector<Item*> c_fns;    // C functions called from here
        };
        std::vector<Plan> plans;
        std::unordered_set<const Item*> external;  // written functions another file calls
        for (Module* m : prog_.modules) {
            Plan p{m, {}, {}, {}};
            std::unordered_set<const Item*> seen;
            for (Item* f : prog_.fns) {
                if (f->module == m && !f->origin && seen.insert(f).second) p.defs.push_back(f);
            }
            for (std::size_t i = 0; i < p.defs.size(); i++) {
                for (Item* g : fns_.at(p.defs[i]).uses.fns) {
                    if (!seen.insert(g).second) continue;
                    if (g->is_extern) {
                        p.c_fns.push_back(g);
                    } else if (g->origin || g->module == m) {
                        p.defs.push_back(g);
                    } else {
                        p.externs.push_back(g);
                        external.insert(g);
                    }
                }
            }
            plans.push_back(std::move(p));
        }

        std::vector<CFile> files;
        Module* main_module = prog_.modules.empty() ? nullptr : prog_.modules.back();
        for (const Plan& p : plans) {
            bool has_main = p.module == main_module && prog_.main;
            if (p.defs.empty() && !has_main) continue;  // types and constants go where they're used
            Uses all;
            for (Item* f : p.defs) {
                all.merge(fns_.at(f).sig_uses);
                all.merge(fns_.at(f).uses);
            }
            for (Item* f : p.externs) all.merge(fns_.at(f).sig_uses);
            // A constant's value can name other constants.
            for (std::size_t i = 0; i < all.consts.size(); i++) all.merge(consts_.at(all.consts[i]).uses);
            // A `main` that fails shows its error.
            if (has_main && prog_.main->result_type->is_result) {
                const Type* rt = prog_.main->result_type;
                all.add_type(rt);
                if (has_glue(rt->args[1])) all.add_show(rt->args[1]);
                else if (rt->args[1]->kind == TypeKind::enum_) all.add_name(rt->args[1]);
            }
            // Comparing or showing a type compares or shows what it holds.
            for (std::size_t i = 0; i < all.eqs.size(); i++) {
                for (const Type* x : held(all.eqs[i])) {
                    if (has_glue(x)) all.add_eq(x);
                }
            }
            for (std::size_t i = 0; i < all.shows.size(); i++) {
                for (const Type* x : held(all.shows[i])) {
                    if (has_glue(x)) all.add_show(x);
                    else if (x->kind == TypeKind::enum_) all.add_name(x);
                }
            }
            for (const Type* t : all.names) all.add_type(t);
            for (const Type* t : all.eqs) all.add_type(t);
            for (const Type* t : all.shows) all.add_type(t);
            printed_ = std::unordered_set<const Type*>(all.names.begin(), all.names.end());
            equated_ = std::unordered_set<const Type*>(all.eqs.begin(), all.eqs.end());
            shown_ = std::unordered_set<const Type*>(all.shows.begin(), all.shows.end());

            out_ = "// Generated by calx. Do not edit.\n#include \"calx_rt.h\"\n\n";
            emit_types(all.types);
            for (const Item* c : prog_.consts) {
                if (all.has(c)) out_ += consts_.at(c).text;
            }
            if (!all.consts.empty()) out_ += "\n";
            for (Item* f : p.c_fns) out_ += c_declaration(f);
            for (Item* f : p.externs) out_ += fns_.at(f).sig + ";\n";
            for (Item* f : p.defs) out_ += linkage(f, external) + fns_.at(f).sig + ";\n";
            out_ += "\n";
            for (Item* f : p.defs) {
                const FnOut& o = fns_.at(f);
                out_ += o.helpers + o.directive + linkage(f, external) + o.sig + o.body;
            }
            if (has_main) {
                out_ += "int main(int argc, char** argv) {\n    cx_start(argc, argv);\n    ";
                const Type* rt = prog_.main->result_type;
                if (rt->is_result) {
                    // A `main` that fails shows its error on standard error, and the program
                    // exits with status 1.
                    std::string e = "r.u.v1." + variant_field(rt, 1, 0);
                    const Type* et = rt->args[1];
                    out_ += ctype(rt) + " r = " + std::string(prog_.main->c_name) + "();\n";
                    out_ += "    if (r.tag != 0) {\n        cx_err_begin();\n        cx_print_str(" + str_const("error: ") +
                            ");\n        " + show_value(et, e) + "\n        cx_print_newline();\n        cx_err_end();\n";
                    if (!is_copy(et)) out_ += "        " + drop_fn(et) + "(&" + e + ");\n";
                    out_ += "        int status = cx_finish();\n        return status ? status : 1;\n    }\n    ";
                } else {
                    out_ += std::string(prog_.main->c_name) + "();\n    ";
                }
                out_ += "return cx_finish();\n}\n";
            }
            files.push_back({std::string(p.module->prefix) + ".c", std::move(out_)});
            out_.clear();
        }
        return files;
    }

private:
    // What a function's or constant's C names, so each file can declare exactly that.
    struct Uses {
        std::vector<const Type*> types;    // structs, enums, arrays, optionals and tuples
        std::vector<Item*> fns;            // the functions it calls
        std::vector<const Item*> consts;   // the constants it reads that C defines
        std::vector<const Type*> names;    // the enums whose variant names it prints
        std::vector<const Type*> eqs;      // the types it compares with a function of their own
        std::vector<const Type*> shows;    // the types it prints with a function of their own
        std::unordered_set<const void*> seen;

        bool has(const void* p) const { return seen.count(p) != 0; }
        void add_type(const Type* t) {
            if (seen.insert(t).second) types.push_back(t);
        }
        void add_fn(Item* f) {
            if (seen.insert(f).second) fns.push_back(f);
        }
        void add_const(const Item* c) {
            if (seen.insert(c).second) consts.push_back(c);
        }
        void add_name(const Type* t) {
            // Keyed apart from the type itself, which is in `types` too.
            if (seen.insert(reinterpret_cast<const char*>(t) + 1).second) names.push_back(t);
        }
        // Keyed apart from the type itself, like `add_name`.
        void add_eq(const Type* t) {
            if (seen.insert(reinterpret_cast<const char*>(t) + 2).second) eqs.push_back(t);
        }
        void add_show(const Type* t) {
            if (seen.insert(reinterpret_cast<const char*>(t) + 3).second) shows.push_back(t);
        }
        void merge(const Uses& o) {
            for (const Type* t : o.types) add_type(t);
            for (Item* f : o.fns) add_fn(f);
            for (const Item* c : o.consts) add_const(c);
            for (const Type* t : o.names) add_name(t);
            for (const Type* t : o.eqs) add_eq(t);
            for (const Type* t : o.shows) add_show(t);
        }
    };

    struct FnOut {
        std::string directive;  // the `#line` before it
        std::string sig;        // its signature, without `static`
        Uses sig_uses;
        std::string body;       // from its `{` to its `}`
        Uses uses;
        std::string helpers;    // what goes before it: its `parallel for` bodies, as functions
    };

    struct ConstOut {
        std::string text;
        Uses uses;
    };

    // A C function's prototype. Its C name goes to the linker through an asm label, so the Calx
    // name it's called by in C can't meet anything a header declares.
    std::string c_declaration(const Item* f) const {
        std::string s = "extern " + ctype(f->result_type) + " " + std::string(f->c_name) + "(";
        for (std::size_t i = 0; i < f->params.size(); i++) {
            if (i) s += ", ";
            s += ctype(f->params[i].local->type);
        }
        if (f->params.empty()) s += "void";
        return s + ") __asm__(CX_SYMBOL(\"" + std::string(text(f->name)) + "\"));\n";
    }

    // Written functions are `static` unless another file calls them, which lets clang inline and
    // drop them freely. Instances are always `static`: each file has its own copy.
    static std::string linkage(const Item* f, const std::unordered_set<const Item*>& external) {
        return f->origin || !external.count(f) ? "static " : "";
    }

    // --- names and types ---------------------------------------------------------------------

    std::string_view text(Name n) const { return names_.text(n); }

    std::string ident(Name n) const {
        std::string_view s = text(n);
        if (c_reserved().count(s) || s.find("__") != std::string_view::npos || s.rfind("cx_", 0) == 0) {
            return "cx_v_" + std::string(s);
        }
        return std::string(s);
    }

    std::string local_name(const Local* l) const {
        if (!l->name) {
            // A value the ownership pass holds for a `match`, `if let`, `let` or `for`.
            auto it = hidden_names_.find(l);
            if (it != hidden_names_.end()) return it->second;
            return hidden_names_.emplace(l, "cx_h" + std::to_string(hidden_names_.size() + 1)).first->second;
        }
        return ident(l->name);
    }

    std::string ctype(const Type* t) const {
        if (uses_) {
            switch (t->kind) {
                case TypeKind::struct_: case TypeKind::enum_: case TypeKind::array: case TypeKind::optional:
                case TypeKind::tuple: case TypeKind::list: case TypeKind::map: case TypeKind::set: uses_->add_type(t); break;
                default: break;
            }
        }
        switch (t->kind) {
            case TypeKind::unit:
            case TypeKind::never:
            case TypeKind::param:  // only in generic definitions, which aren't emitted
            case TypeKind::error: return "void";
            case TypeKind::bool_: return "cx_bool";
            case TypeKind::int_: return t->is_usize ? "cx_usize" : "cx_" + std::string(t->name);
            case TypeKind::char_: return "cx_char";
            case TypeKind::rune: return "cx_rune";
            case TypeKind::float_: return t->bits == 32 ? "cx_f32" : "cx_f64";
            case TypeKind::str: return "cx_str";
            case TypeKind::string: return "cx_String";
            case TypeKind::pointer: return ctype(t->elem) + (t->is_const ? " const*" : "*");
            case TypeKind::struct_: return std::string(t->item->c_name);
            case TypeKind::enum_: return std::string(t->item->c_name);
            case TypeKind::array:
            case TypeKind::optional:
            case TypeKind::tuple:
            case TypeKind::list:
            case TypeKind::map:
            case TypeKind::set: return compound_name(t);
        }
        return "void";
    }

    // Arrays, optionals and tuples are named after what they hold, `cx_opt_i32`, `cx_arr3_f64`,
    // so a file's text doesn't change when another module makes new types. Two that would spell
    // the same name are told apart by a number.
    const std::string& compound_name(const Type* t) const {
        if (auto it = compound_names_.find(t); it != compound_names_.end()) return it->second;
        std::string s;
        switch (t->kind) {
            case TypeKind::array: s = "cx_arr" + std::to_string(t->length) + "_" + part(t->elem); break;
            case TypeKind::optional: s = "cx_opt_" + part(t->elem); break;
            case TypeKind::list: s = "cx_list_" + part(t->elem); break;
            case TypeKind::map: s = "cx_map_" + part(t->key) + "_" + part(t->elem); break;
            case TypeKind::set: s = "cx_set_" + part(t->elem); break;
            default:
                s = "cx_tup" + std::to_string(t->elems.size());
                for (const Type* e : t->elems) s += "_" + part(e);
                break;
        }
        if (s.size() > 96) {
            std::uint64_t h = 1469598103934665603ull;
            for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
            char buf[17];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
            s = s.substr(0, 72) + "_" + buf;
        }
        std::string name = s;
        for (int i = 2; !compound_used_.insert(name).second; i++) name = s + "_" + std::to_string(i);
        return compound_names_.emplace(t, std::move(name)).first->second;
    }

    std::string part(const Type* t) const {
        switch (t->kind) {
            case TypeKind::struct_: case TypeKind::enum_: return std::string(t->item->c_name);
            case TypeKind::array: case TypeKind::optional: case TypeKind::tuple: case TypeKind::list:
            case TypeKind::map: case TypeKind::set:
                return compound_name(t).substr(3);
            case TypeKind::string: return "String";
            case TypeKind::int_: return t->is_usize ? "usize" : std::string(t->name);
            case TypeKind::float_: return t->bits == 32 ? "f32" : "f64";
            case TypeKind::bool_: return "bool";
            case TypeKind::char_: return "char";
            case TypeKind::rune: return "rune";
            case TypeKind::str: return "str";
            default: return "x";
        }
    }

    // The C name of a variant's `k`th value: its field's name, or `f0`, `f1` for unnamed values.
    std::string variant_field(const Type* t, std::uint32_t i, std::size_t k) const {
        const VariantDecl& v = t->item->variants[i];
        return v.has_fields ? ident(v.fields[k].name) : "f" + std::to_string(k);
    }

    // `.x` or `.f0`: a field of a struct, or an element of a tuple.
    std::string member(const Type* t, std::uint32_t index) const {
        if (t->kind == TypeKind::tuple) return ".f" + std::to_string(index);
        return "." + ident(t->item->fields[index].name);
    }

    static Type* member_type(const Type* t, std::uint32_t index) {
        if (t->kind == TypeKind::tuple) return t->elems[index];
        return t->item->fields[index].resolved;
    }

    // The function that gives a data-less enum's variant name, for printing.
    std::string enum_name_fn(const Type* t) const {
        if (uses_) uses_->add_name(t);
        return "cx_name__" + std::string(t->item->c_name);
    }

    // The unsigned type arithmetic on `t` happens in, so overflow wraps (D63's integer rules).
    static const char* wide_unsigned(const Type* t) { return t->bits == 64 ? "cx_u64" : "cx_u32"; }
    static const char* unsigned_of(const Type* t) {
        switch (t->bits) {
            case 8: return "cx_u8";
            case 16: return "cx_u16";
            case 32: return "cx_u32";
            default: return "cx_u64";
        }
    }
    // The runtime's suffix for a type's helpers: `cx_div_i32` and the like.
    static std::string helper(const Type* t) {
        if (t->is_usize) return "usize";
        if (t->kind == TypeKind::float_) return t->bits == 32 ? "f32" : "f64";
        return std::string(t->name);
    }

    // Every type the program uses, each after the types it holds. An optional is its value and a
    // flag, a tuple a struct of `f0`, `f1`..., and an enum with data a tag and a union of its
    // variants' values. An enum without data is its tag.
    void emit_types(const std::vector<const Type*>& used) {
        std::unordered_set<const Type*> need;
        std::function<void(const Type*)> close = [&](const Type* t) {
            switch (t->kind) {
                case TypeKind::struct_: case TypeKind::enum_: case TypeKind::array: case TypeKind::optional:
                case TypeKind::tuple: case TypeKind::list: case TypeKind::map: case TypeKind::set: break;
                default: return;
            }
            if (!need.insert(t).second) return;
            switch (t->kind) {
                case TypeKind::struct_: for (const FieldDecl& f : t->item->fields) close(f.resolved); break;
                case TypeKind::enum_:
                    for (const VariantDecl& v : t->item->variants) for (const Type* x : v.types) close(x);
                    break;
                case TypeKind::array: case TypeKind::optional: case TypeKind::list: case TypeKind::set: close(t->elem); break;
                case TypeKind::map:
                    close(t->key);
                    close(t->elem);
                    close(t->entry);
                    break;
                default: for (const Type* e : t->elems) close(e); break;
            }
        };
        for (const Type* t : used) close(t);
        std::vector<const Type*> order;
        for (const Type* t : prog_.type_order) {
            if (need.count(t)) order.push_back(t);
        }
        // Each is declared first, so a list can point to any of them.
        for (const Type* t : order) {
            if (t->kind == TypeKind::enum_ && !t->has_data) {
                out_ += "typedef " + ctype(t->repr) + " " + ctype(t) + ";\n";
            } else {
                out_ += "typedef struct " + ctype(t) + " " + ctype(t) + ";\n";
            }
        }
        for (const Type* t : order) {
            switch (t->kind) {
                case TypeKind::array:
                    out_ += "struct " + ctype(t) + " { " + ctype(t->elem) + " v[" + std::to_string(t->length) + "]; };\n";
                    break;
                case TypeKind::optional:
                    out_ += "struct " + ctype(t) + " { " + ctype(t->elem) + " v; cx_bool has; };\n";
                    break;
                case TypeKind::list:
                    out_ += "struct " + ctype(t) + " { " + ctype(t->elem) + "* ptr; cx_usize len; cx_usize cap; };\n";
                    break;
                case TypeKind::map:
                case TypeKind::set:
                    // The control bytes, the slots, the slot count less one, the values in it, and
                    // how many more fit before it must grow (D94).
                    out_ += "struct " + ctype(t) + " { cx_u8* ctrl; " + ctype(slot_type(t)) +
                            "* slots; cx_usize mask; cx_usize len; cx_usize growth; };\n";
                    break;
                case TypeKind::tuple:
                    out_ += "struct " + ctype(t) + " {";
                    for (std::size_t i = 0; i < t->elems.size(); i++) {
                        out_ += " " + ctype(t->elems[i]) + " f" + std::to_string(i) + ";";
                    }
                    out_ += " };\n";
                    break;
                case TypeKind::struct_:
                    out_ += "struct " + ctype(t) + " {";
                    for (const FieldDecl& f : t->item->fields) out_ += " " + ctype(f.resolved) + " " + ident(f.name) + ";";
                    out_ += " };\n";
                    break;
                case TypeKind::enum_: {
                    if (!t->has_data) break;
                    out_ += "struct " + ctype(t) + " { " + ctype(t->repr) + " tag; union {";
                    const Slice<VariantDecl>& vs = t->item->variants;
                    for (std::uint32_t i = 0; i < vs.size(); i++) {
                        if (vs[i].types.empty()) continue;
                        out_ += " struct {";
                        for (std::size_t k = 0; k < vs[i].types.size(); k++) {
                            out_ += " " + ctype(vs[i].types[k]) + " " + variant_field(t, i, k) + ";";
                        }
                        out_ += " } v" + std::to_string(i) + ";";
                    }
                    out_ += " } u; };\n";
                    break;
                }
                default: break;
            }
        }
        for (const Type* t : order) {
            if (t->kind != TypeKind::enum_ || t->has_data || !printed_.count(t)) continue;
            out_ += "static cx_str " + enum_name_fn(t) + "(" + ctype(t) + " v) {\n    switch (v) {\n";
            for (const VariantDecl& v : t->item->variants) {
                out_ += "        case " + int_const(t->repr, v.tag) + ": return " + str_const(text(v.name)) + ";\n";
            }
            out_ += "    }\n    return " + str_const("") + ";\n}\n";
        }
        // A map's or a set's functions hash and compare its keys, and what those hold.
        hashed_.clear();
        std::function<void(const Type*, std::unordered_set<const Type*>&)> reach = [&](const Type* x,
                                                                                    std::unordered_set<const Type*>& set) {
            if (!has_glue(x) || !set.insert(x).second) return;
            for (const Type* h : held(x)) reach(h, set);
        };
        for (const Type* x : order) {
            if (!is_table(x)) continue;
            reach(table_key(x), hashed_);
            reach(table_key(x), equated_);
        }
        emit_glue(order);
        if (!order.empty()) out_ += "\n";
    }

    // --- drop, copy and show ---------------------------------------------------------------------

    // Every type in the file that owns memory gets a function that frees it and one that copies
    // it, all `static inline`, so the unused ones cost nothing. The types the file compares or
    // prints, and those they hold, get a function that does that: the derived `Equal` and
    // `Display`, which go through each value in turn.
    void emit_glue(const std::vector<const Type*>& order) {
        std::vector<const Type*> owning, eqs, shows, hashes, tables;
        for (const Type* t : order) {
            if (!is_copy(t)) owning.push_back(t);
            if (equated_.count(t)) eqs.push_back(t);
            if (shown_.count(t)) shows.push_back(t);
            if (hashed_.count(t)) hashes.push_back(t);
            if (is_table(t)) tables.push_back(t);
        }
        if (owning.empty() && eqs.empty() && shows.empty() && tables.empty()) return;
        for (const Type* t : owning) {
            std::string c = ctype(t);
            out_ += "static inline void " + drop_fn(t) + "(" + c + "* p);\n";
            out_ += "static inline " + c + " " + copy_fn(t) + "(const " + c + "* p);\n";
        }
        for (const Type* t : eqs) out_ += "static inline cx_bool " + eq_fn(t) + "(" + ctype(t) + " a, " + ctype(t) + " b);\n";
        for (const Type* t : shows) out_ += "static inline void " + show_fn(t) + "(" + ctype(t) + " v);\n";
        for (const Type* t : hashes) out_ += "static inline cx_u64 " + hash_fn(t) + "(" + ctype(t) + " v);\n";
        for (const Type* t : tables) out_ += table_decls(t);
        for (const Type* t : owning) {
            out_ += "static inline void " + drop_fn(t) + "(" + ctype(t) + "* p) {\n" + drop_body(t) + "}\n";
            out_ += "static inline " + ctype(t) + " " + copy_fn(t) + "(const " + ctype(t) + "* p) {\n" + copy_body(t) + "}\n";
        }
        for (const Type* t : eqs) {
            out_ += "static inline cx_bool " + eq_fn(t) + "(" + ctype(t) + " a, " + ctype(t) + " b) {\n" + eq_body(t) + "}\n";
        }
        for (const Type* t : shows) out_ += "static inline void " + show_fn(t) + "(" + ctype(t) + " v) {\n" + show_body(t) + "}\n";
        for (const Type* t : hashes) {
            out_ += "static inline cx_u64 " + hash_fn(t) + "(" + ctype(t) + " v) {\n" + hash_body(t) + "}\n";
        }
        for (const Type* t : tables) out_ += table_bodies(t);
    }

    // --- maps and sets (D94) -------------------------------------------------------------------

    static bool is_table(const Type* t) { return t->kind == TypeKind::map || t->kind == TypeKind::set; }

    // `body`, whose lines are already indented one step past `indent`, run for each slot `i` of
    // the table `tab` (`m->` or `v.`) that holds a value: a group of 16 control bytes at a time,
    // so finding the full slots takes no branch per slot.
    static std::string each_full(const std::string& tab, const std::string& indent, const std::string& body) {
        std::string s = indent + "for (cx_usize g = 0; g <= " + tab + "mask; g += CX_GROUP) {\n" + indent +
                        "    for (cx_u32 bits = cx_group_full(" + tab + "ctrl + g); bits; bits &= bits - 1) {\n" + indent +
                        "        cx_usize i = g + (cx_usize)__builtin_ctz(bits);\n";
        for (std::size_t at = 0; at < body.size();) {
            std::size_t end = body.find('\n', at);
            s += "    " + body.substr(at, end - at + 1);
            at = end + 1;
        }
        return s + indent + "    }\n" + indent + "}\n";
    }
    // What a table's keys are: a map's keys, or a set's values.
    static Type* table_key(const Type* t) { return t->kind == TypeKind::map ? t->key : t->elem; }
    // What a slot holds: a map's `(K, V)`, or a set's value.
    static Type* slot_type(const Type* t) { return t->kind == TypeKind::map ? t->entry : t->elem; }
    // The key in the slot `s`.
    static std::string slot_key(const Type* t, const std::string& s) { return t->kind == TypeKind::map ? s + ".f0" : s; }
    // A `String` key is looked up as a `cx_str`, so a lookup never makes a `String`.
    static bool str_key(const Type* t) { return table_key(t)->kind == TypeKind::string; }
    std::string lookup_ctype(const Type* t) { return str_key(t) ? "cx_str" : ctype(table_key(t)); }
    static std::string as_lookup(const Type* t, const std::string& k) { return str_key(t) ? "cx_sv(" + k + ")" : k; }
    std::string lookup_hash(const Type* t, const std::string& k) {
        return str_key(t) ? "cx_hash_str(" + k + ")" : hash_value(table_key(t), k);
    }
    // A key argument, as `lookup_ctype` has it: a `String` key's can be a `str` or a `String`.
    std::string lookup_value(Expr* a, const Type* t) {
        if (str_key(t)) return a->type->kind == TypeKind::string ? "cx_sv(" + expr(a) + ")" : expr(a);
        return value(a, table_key(t));
    }
    std::string table_fn(const char* what, const Type* t) const {
        (void)ctype(t);
        return std::string("cx_") + what + "_" + part(t);
    }
    std::string hash_fn(const Type* t) const {
        (void)ctype(t);
        return "cx_hash_" + part(t);
    }

    // The hash of the value `v` of type `t`: equal values hash the same.
    std::string hash_value(const Type* t, const std::string& v) {
        if (t->kind == TypeKind::str) return "cx_hash_str(" + v + ")";
        if (t->kind == TypeKind::string) return "cx_hash_str(cx_sv(" + v + "))";
        if (has_glue(t)) return hash_fn(t) + "(" + v + ")";
        return "cx_hash_word(CX_HASH_SEED, " + hash_word(t, v) + ")";
    }
    // What a value adds to the hash of what holds it: a number, or what's stored as one, as
    // itself, and anything else as its own hash.
    std::string hash_word(const Type* t, const std::string& v) {
        if (t->kind == TypeKind::str || t->kind == TypeKind::string || has_glue(t)) return hash_value(t, v);
        if (is_int(t) && t->is_signed) return "(cx_u64)(cx_i64)(" + v + ")";
        return "(cx_u64)(" + v + ")";
    }

    // Everything a value holds, in order, folded into the hash. An enum's variant, an optional's
    // flag and a list's length go in first, so `[1], [2]` and `[1, 2]` differ.
    std::string hash_body(const Type* t) {
        std::string s = "    cx_u64 h = CX_HASH_SEED;\n";
        auto add = [&](const Type* x, const std::string& v, const std::string& indent) {
            return indent + "h = cx_hash_word(h, " + hash_word(x, v) + ");\n";
        };
        switch (t->kind) {
            case TypeKind::struct_:
                for (std::uint32_t i = 0; i < t->item->fields.size(); i++) s += add(t->item->fields[i].resolved, "v" + member(t, i), "    ");
                break;
            case TypeKind::tuple:
                for (std::uint32_t i = 0; i < t->elems.size(); i++) s += add(t->elems[i], "v" + member(t, i), "    ");
                break;
            case TypeKind::optional:
                s += "    h = cx_hash_word(h, v.has);\n    if (v.has) " + add(t->elem, "v.v", "").substr(0) ;
                break;
            case TypeKind::array:
                s += "    for (cx_usize i = 0; i < " + std::to_string(t->length) + "; i++) " + add(t->elem, "v.v[i]", "");
                break;
            case TypeKind::list:
                s += "    h = cx_hash_word(h, v.len);\n    for (cx_usize i = 0; i < v.len; i++) " + add(t->elem, "v.ptr[i]", "");
                break;
            default: {
                s += "    h = cx_hash_word(h, (cx_u64)v.tag);\n    switch (v.tag) {\n";
                const Slice<VariantDecl>& vs = t->item->variants;
                for (std::uint32_t i = 0; i < vs.size(); i++) {
                    if (vs[i].types.empty()) continue;
                    s += "    case " + int_const(t->repr, vs[i].tag) + ":\n";
                    for (std::size_t k = 0; k < vs[i].types.size(); k++) {
                        s += add(vs[i].types[k], "v.u.v" + std::to_string(i) + "." + variant_field(t, i, k), "        ");
                    }
                    s += "        break;\n";
                }
                s += "    default: break;\n    }\n";
                break;
            }
        }
        return s + "    return h;\n";
    }

    // `$name` in `text` replaced by its value.
    static std::string fill(std::string text, const std::vector<std::pair<std::string, std::string>>& with) {
        for (const auto& [k, v] : with) {
            for (std::size_t at = text.find(k); at != std::string::npos; at = text.find(k, at + v.size())) {
                text.replace(at, k.size(), v);
            }
        }
        return text;
    }

    std::vector<std::pair<std::string, std::string>> table_names(const Type* t) {
        bool map = t->kind == TypeKind::map;
        return {
            {"$find", table_fn("find", t)}, {"$grow", table_fn("grow", t)}, {"$put", table_fn("put", t)},
            {"$clear", table_fn("clear", t)}, {"$reserve", table_fn("reserve", t)}, {"$T", ctype(t)},
            {"$S", ctype(slot_type(t))}, {"$KL", lookup_ctype(t)}, {"$K", ctype(table_key(t))},
            {"$V", map ? ctype(t->elem) : std::string()},
        };
    }

    std::string table_decls(const Type* t) {
        bool map = t->kind == TypeKind::map;
        return fill(std::string("CX_INLINE cx_usize $find(const $T* m, $KL k, cx_u64 h);\n"
                                "static void $grow($T* m, cx_usize buckets);\n") +
                        (map ? "static inline void $put($T* m, $K k, $V v);\n" : "static inline void $put($T* m, $K k);\n") +
                        "static void $clear($T* m);\nstatic void $reserve($T* m, cx_usize n);\n",
                    table_names(t));
    }

    // Finding a key looks at the 16 control bytes from where its hash points, and at each slot
    // whose byte is the hash's top 7 bits; an EMPTY byte among the 16 means it isn't there.
    // Putting a key that's there replaces its value and drops the key given. Growing moves every
    // value into a new table, so it's out of line: it's rare, and `put` stays small.
    std::string table_bodies(const Type* t) {
        bool map = t->kind == TypeKind::map;
        const Type* k = table_key(t);
        const Type* slot = slot_type(t);
        std::string eq = str_key(t) ? "cx_str_eq(cx_sv(" + slot_key(t, "m->slots[i]") + "), k)" : equal(k, slot_key(t, "m->slots[i]"), "k");
        std::string eqj = str_key(t) ? "cx_str_eq(cx_sv(" + slot_key(t, "m->slots[j]") + "), cx_sv(k))" : equal(k, slot_key(t, "m->slots[j]"), "k");
        std::string drop_slots = is_copy(slot) ? std::string() : each_full("m->", "    ", "        " + drop_fn(slot) + "(&m->slots[i]);\n");
        std::string move_slots = each_full("m->", "        ", "            cx_u64 h = " + hash_value(k, slot_key(t, "m->slots[i]")) +
                                                                ";\n            cx_usize j = cx_table_free_slot(ctrl, mask, h);\n"
                                                                "            cx_table_set(ctrl, mask, j, cx_table_tag(h));\n"
                                                                "            slots[j] = m->slots[i];\n");
        std::string replace;
        if (map) {
            if (!is_copy(t->elem)) replace += "                " + drop_fn(t->elem) + "(&m->slots[j].f1);\n";
            replace += "                m->slots[j].f1 = v;\n";
        }
        if (!is_copy(k)) replace += "                " + drop_fn(k) + "(&k);\n";
        auto names = table_names(t);
        names.insert(names.begin(), {
            {"$eqj", eqj}, {"$eq", eq}, {"$slothash", hash_value(k, slot_key(t, "m->slots[i]"))}, {"$hash", hash_value(k, "k")},
            {"$lookup", as_lookup(t, "k")}, {"$replace", replace}, {"$dropall", drop_slots},
            {"$params", map ? "$K k, $V v" : "$K k"}, {"$new", map ? "($S){ .f0 = k, .f1 = v }" : "k"}, {"$moveall", move_slots},
        });
        return fill(R"(CX_INLINE cx_usize $find(const $T* m, $KL k, cx_u64 h) {
    cx_u8 tag = cx_table_tag(h);
    cx_usize pos = (cx_usize)h & m->mask, stride = 0;
    for (;;) {
        const cx_u8* g = m->ctrl + pos;
        for (cx_u32 bits = cx_group_match(g, tag); bits; bits &= bits - 1) {
            cx_usize i = (pos + (cx_usize)__builtin_ctz(bits)) & m->mask;
            if ($eq) return i;
        }
        if (cx_group_empty(g)) return CX_NOT_FOUND;
        stride += CX_GROUP;
        pos = (pos + stride) & m->mask;
    }
}
static __attribute__((noinline)) void $grow($T* m, cx_usize buckets) {
    $S* slots;
    cx_u8* ctrl = cx_table_alloc(buckets, sizeof($S), (void**)&slots);
    cx_usize mask = buckets - 1;
    if (m->slots) {
$moveall        cx_table_free(m->slots, m->mask + 1, sizeof($S));
    }
    m->ctrl = ctrl;
    m->slots = slots;
    m->mask = mask;
    m->growth = cx_table_capacity(buckets) - m->len;
}
static inline void $put($T* m, $params) {
    cx_u64 h = $hash;
    cx_u8 tag = cx_table_tag(h);
    cx_usize pos = (cx_usize)h & m->mask, stride = 0, i = CX_NOT_FOUND;
    // One probe finds the key, or the first free slot on the way to where it would be.
    for (;;) {
        const cx_u8* g = m->ctrl + pos;
        for (cx_u32 bits = cx_group_match(g, tag); bits; bits &= bits - 1) {
            cx_usize j = (pos + (cx_usize)__builtin_ctz(bits)) & m->mask;
            if ($eqj) {
$replace                return;
            }
        }
        cx_u32 free = cx_group_free(g);
        if (free && i == CX_NOT_FOUND) i = (pos + (cx_usize)__builtin_ctz(free)) & m->mask;
        if (cx_group_empty(g)) break;
        stride += CX_GROUP;
        pos = (pos + stride) & m->mask;
    }
    if (__builtin_expect(m->ctrl[i] < 0x80, 0)) i = (cx_usize)__builtin_ctz(cx_group_free(m->ctrl));
    if (__builtin_expect(m->growth == 0 && m->ctrl[i] == CX_CTRL_EMPTY, 0)) {
        $grow(m, cx_table_grown(m->slots ? m->mask + 1 : 0, m->len));
        i = cx_table_free_slot(m->ctrl, m->mask, h);
    }
    m->growth -= m->ctrl[i] == CX_CTRL_EMPTY;
    cx_table_set(m->ctrl, m->mask, i, cx_table_tag(h));
    m->slots[i] = $new;
    m->len++;
}
static void $clear($T* m) {
    if (!m->slots) return;
$dropall    __builtin_memset(m->ctrl, CX_CTRL_EMPTY, m->mask + 1 + CX_GROUP);
    m->len = 0;
    m->growth = cx_table_capacity(m->mask + 1);
}
static void $reserve($T* m, cx_usize n) {
    if (n <= m->growth) return;
    cx_usize b = cx_table_buckets_for(m->len + n);
    if (m->slots && b < m->mask + 1) b = m->mask + 1;
    $grow(m, b);
}
)", names);
    }

    // `m.insert(k, v)` and the rest. The receiver is found first, then the arguments, in order.
    std::string table_method(Expr* e) {
        Expr* recv = e->callee->lhs;
        Type* t = recv->type;
        bool map = t->kind == TypeKind::map;
        Type* k = table_key(t);
        std::string tc = ctype(t), p = tmp(), key = tmp(), i = tmp();
        std::string head = "({ " + tc + "* " + p + " = " + address(recv) + "; ";
        std::string view = "({ const " + tc + "* " + p + " = " + address(recv) + "; ";
        auto found = [&](Expr* a) {
            return lookup_ctype(t) + " " + key + " = " + lookup_value(a, t) + "; cx_usize " + i + " = " + table_fn("find", t) + "(" + p +
                   ", " + key + ", " + lookup_hash(t, key) + "); ";
        };
        std::string slot = p + "->slots[" + i + "]";
        std::string erase = p + "->growth += cx_table_erase(" + p + "->ctrl, " + p + "->mask, " + i + "); " + p + "->len--; ";
        switch (e->bmethod) {
            case BMethod::len: return "(" + expr(recv) + ").len";
            case BMethod::is_empty: return "((" + expr(recv) + ").len == 0)";
            case BMethod::capacity: return view + p + "->len + " + p + "->growth; })";
            case BMethod::insert: {
                std::string s = head + ctype(k) + " " + key + " = " + value(e->args[0].value, k) + "; ";
                if (!map) return s + table_fn("put", t) + "(" + p + ", " + key + "); })";
                std::string v = tmp();
                return s + ctype(t->elem) + " " + v + " = " + value(e->args[1].value, t->elem) + "; " + table_fn("put", t) + "(" + p + ", " +
                       key + ", " + v + "); })";
            }
            case BMethod::contains: return view + found(e->args[0].value) + i + " != CX_NOT_FOUND; })";
            case BMethod::get: {
                std::string o = ctype(e->type);
                return view + found(e->args[0].value) + i + " == CX_NOT_FOUND ? (" + o + "){0} : (" + o + "){ .v = " + slot +
                       ".f1, .has = 1 }; })";
            }
            case BMethod::remove: {
                std::string s = head + found(e->args[0].value);
                std::string drop_key = is_copy(k) ? std::string() : drop_fn(k) + "(&" + slot_key(t, slot) + "); ";
                if (!map) return s + "if (" + i + " != CX_NOT_FOUND) { " + drop_key + erase + "} })";
                std::string o = ctype(e->type), r = tmp();
                return s + o + " " + r + " = {0}; if (" + i + " != CX_NOT_FOUND) { " + r + ".v = " + slot + ".f1; " + r +
                       ".has = 1; " + drop_key + erase + "} " + r + "; })";
            }
            case BMethod::clear: return table_fn("clear", t) + "(" + address(recv) + ")";
            case BMethod::reserve:
                return head + "cx_usize " + key + " = " + value(e->args[0].value, e->args[0].value->type) + "; " + table_fn("reserve", t) +
                       "(" + p + ", " + key + "); })";
            default: return "0";
        }
    }

    using Parts = std::vector<std::pair<std::string, const Type*>>;  // members, as `.x` or `.f0`, and their types

    // Two values are equal when everything they hold is, in order. An enum's also have the same
    // variant, and a list's the same length.
    std::string eq_body(const Type* t) {
        auto all = [&](const Parts& parts) {
            std::string s;
            for (const auto& [m, x] : parts) {
                if (!s.empty()) s += "\n        && ";
                s += equal(x, "a" + m, "b" + m);
            }
            return s.empty() ? std::string("1") : s;
        };
        switch (t->kind) {
            case TypeKind::struct_: {
                Parts parts;
                for (std::uint32_t i = 0; i < t->item->fields.size(); i++) parts.push_back({member(t, i), t->item->fields[i].resolved});
                return "    return " + all(parts) + ";\n";
            }
            case TypeKind::tuple: {
                Parts parts;
                for (std::uint32_t i = 0; i < t->elems.size(); i++) parts.push_back({member(t, i), t->elems[i]});
                return "    return " + all(parts) + ";\n";
            }
            case TypeKind::optional:
                return "    return a.has == b.has && (!a.has || " + equal(t->elem, "a.v", "b.v") + ");\n";
            case TypeKind::array:
                return "    for (cx_usize i = 0; i < " + std::to_string(t->length) + "; i++) {\n        if (!" +
                       equal(t->elem, "a.v[i]", "b.v[i]") + ") return 0;\n    }\n    return 1;\n";
            case TypeKind::list:
                return "    if (a.len != b.len) return 0;\n    for (cx_usize i = 0; i < a.len; i++) {\n        if (!" +
                       equal(t->elem, "a.ptr[i]", "b.ptr[i]") + ") return 0;\n    }\n    return 1;\n";
            case TypeKind::map:
            case TypeKind::set: {
                // The same keys, each with an equal value, whatever order they're in.
                std::string key = slot_key(t, "a.slots[i]");
                std::string body = "        cx_usize j = " + table_fn("find", t) + "(&b, " + as_lookup(t, key) + ", " +
                                   hash_value(table_key(t), key) + ");\n        if (j == CX_NOT_FOUND) return 0;\n";
                if (t->kind == TypeKind::map) body += "        if (!" + equal(t->elem, "a.slots[i].f1", "b.slots[j].f1") + ") return 0;\n";
                return "    if (a.len != b.len) return 0;\n" + each_full("a.", "    ", body) + "    return 1;\n";
            }
            default: {
                std::string s = "    if (a.tag != b.tag) return 0;\n    switch (a.tag) {\n";
                const Slice<VariantDecl>& vs = t->item->variants;
                for (std::uint32_t i = 0; i < vs.size(); i++) {
                    if (vs[i].types.empty()) continue;
                    Parts parts;
                    for (std::size_t k = 0; k < vs[i].types.size(); k++) {
                        parts.push_back({".u.v" + std::to_string(i) + "." + variant_field(t, i, k), vs[i].types[k]});
                    }
                    s += "        case " + int_const(t->repr, vs[i].tag) + ": return " + all(parts) + ";\n";
                }
                return s + "    }\n    return 1;\n";
            }
        }
    }

    // How the derived `Display` writes a value: a struct as its literal, `Point { x: 1, y: 2 }`, a
    // variant as it's built, `Circle(1.0)`, a tuple as `(1, 2)`, an array or a list as `[1, 2]`,
    // and an optional as `some(1)` or `none`. Text inside appears without quotes, as in a list.
    std::string show_body(const Type* t) {
        auto print = [&](const std::string& s) { return "    cx_print_str(" + str_const(s) + ");\n"; };
        // `open`, then each part, labelled with its name if it has one, then `close`. The text
        // between two parts is written in one go.
        auto list = [&](const Parts& parts, const std::vector<std::string>& names, const std::string& open,
                        const std::string& close) {
            std::string s, text = open;
            for (std::size_t i = 0; i < parts.size(); i++) {
                text += (i ? ", " : "") + (names.empty() ? std::string() : names[i] + ": ");
                s += print(text) + "    " + show_value(parts[i].second, "v" + parts[i].first) + "\n";
                text.clear();
            }
            return s + print(text + close);
        };
        switch (t->kind) {
            case TypeKind::struct_: {
                Parts parts;
                std::vector<std::string> names;
                for (std::uint32_t i = 0; i < t->item->fields.size(); i++) {
                    parts.push_back({member(t, i), t->item->fields[i].resolved});
                    names.push_back(std::string(text(t->item->fields[i].name)));
                }
                if (parts.empty()) return print(item_name(t) + " {}");
                return list(parts, names, item_name(t) + " { ", " }");
            }
            case TypeKind::tuple: {
                Parts parts;
                for (std::uint32_t i = 0; i < t->elems.size(); i++) parts.push_back({member(t, i), t->elems[i]});
                return list(parts, {}, "(", ")");
            }
            case TypeKind::optional:
                return "    if (!v.has) {\n    " + print("none") + "        return;\n    }\n" + print("some(") + "    " +
                       show_value(t->elem, "v.v") + "\n" + print(")");
            case TypeKind::map:
            case TypeKind::set: {
                // `{a: 1, b: 2}` and `{1, 2}`, in the order a loop goes.
                std::string body = "        if (!first) cx_print_str(" + str_const(", ") + ");\n        first = 0;\n        " +
                                   show_value(table_key(t), slot_key(t, "v.slots[i]")) + "\n";
                if (t->kind == TypeKind::map) {
                    body += "        cx_print_str(" + str_const(": ") + ");\n        " + show_value(t->elem, "v.slots[i].f1") + "\n";
                }
                return print("{") + "    cx_bool first = 1;\n" + each_full("v.", "    ", body) + print("}");
            }
            case TypeKind::array:
            case TypeKind::list: {
                std::string n = t->kind == TypeKind::array ? std::to_string(t->length) : "v.len";
                std::string at = t->kind == TypeKind::array ? "v.v[i]" : "v.ptr[i]";
                return print("[") + "    for (cx_usize i = 0; i < " + n + "; i++) {\n        if (i) cx_print_str(" +
                       str_const(", ") + ");\n        " + show_value(t->elem, at) + "\n    }\n" + print("]");
            }
            default: {
                std::string s = "    switch (v.tag) {\n";
                const Slice<VariantDecl>& vs = t->item->variants;
                for (std::uint32_t i = 0; i < vs.size(); i++) {
                    const VariantDecl& vd = vs[i];
                    std::string name(text(vd.name));
                    s += "    case " + int_const(t->repr, vd.tag) + ":\n";
                    std::string body;
                    if (vd.types.empty()) {
                        body = print(name);
                    } else {
                        Parts parts;
                        std::vector<std::string> names;
                        for (std::size_t k = 0; k < vd.types.size(); k++) {
                            parts.push_back({".u.v" + std::to_string(i) + "." + variant_field(t, i, k), vd.types[k]});
                            if (vd.has_fields) names.push_back(std::string(text(vd.fields[k].name)));
                        }
                        body = vd.has_fields ? list(parts, names, name + " { ", " }") : list(parts, {}, name + "(", ")");
                    }
                    // One step further in, inside the case.
                    for (std::size_t at = 0; at < body.size();) {
                        std::size_t end = body.find('\n', at);
                        s += "    " + body.substr(at, end - at + 1);
                        at = end + 1;
                    }
                    s += "        break;\n";
                }
                return s + "    }\n";
            }
        }
    }

    std::string drop_fn(const Type* t) const {
        (void)ctype(t);
        return "cx_drop_" + part(t);
    }
    std::string copy_fn(const Type* t) const {
        (void)ctype(t);
        return "cx_copy_" + part(t);
    }
    std::string show_fn(const Type* t) const {
        (void)ctype(t);
        if (uses_) uses_->add_show(t);
        return "cx_show_" + part(t);
    }

    std::string eq_fn(const Type* t) const {
        (void)ctype(t);
        if (uses_) uses_->add_eq(t);
        return "cx_eq_" + part(t);
    }

    // Whether comparing or showing the type takes a function of its own: it holds other values.
    static bool has_glue(const Type* t) {
        switch (t->kind) {
            case TypeKind::struct_: case TypeKind::tuple: case TypeKind::array: case TypeKind::list:
            case TypeKind::optional: case TypeKind::map: case TypeKind::set: return true;
            case TypeKind::enum_: return t->has_data;
            default: return false;
        }
    }

    // The types a value of type `t` holds directly.
    static std::vector<const Type*> held(const Type* t) {
        std::vector<const Type*> out;
        switch (t->kind) {
            case TypeKind::struct_: for (const FieldDecl& f : t->item->fields) out.push_back(f.resolved); break;
            case TypeKind::enum_:
                for (const VariantDecl& v : t->item->variants) out.insert(out.end(), v.types.begin(), v.types.end());
                break;
            case TypeKind::tuple: out.assign(t->elems.begin(), t->elems.end()); break;
            case TypeKind::array: case TypeKind::list: case TypeKind::optional: case TypeKind::set: out.push_back(t->elem); break;
            case TypeKind::map:
                out.push_back(t->key);
                out.push_back(t->elem);
                break;
            default: break;
        }
        return out;
    }

    // A struct's or enum's name as Calx code writes it, without a generic type's arguments.
    std::string item_name(const Type* t) const { return std::string(text((t->generic ? t->generic : t->item)->name)); }

    // Frees what the value at `p` owns.
    std::string drop_body(const Type* t) {
        std::string s;
        auto part_drop = [&](const Type* x, const std::string& at) {
            if (!is_copy(x)) s += "    " + drop_fn(x) + "(&" + at + ");\n";
        };
        switch (t->kind) {
            case TypeKind::list:
                if (!is_copy(t->elem)) s += "    for (cx_usize i = 0; i < p->len; i++) " + drop_fn(t->elem) + "(&p->ptr[i]);\n";
                s += "    cx_free(p->ptr, p->cap * sizeof(*p->ptr));\n";
                break;
            case TypeKind::map:
            case TypeKind::set:
                // A table that was never filled, or was moved out of, has no slots.
                s += "    if (!p->slots) return;\n";
                if (!is_copy(slot_type(t))) s += each_full("p->", "    ", "        " + drop_fn(slot_type(t)) + "(&p->slots[i]);\n");
                s += "    cx_table_free(p->slots, p->mask + 1, sizeof(*p->slots));\n";
                break;
            case TypeKind::struct_:
                for (const FieldDecl& f : t->item->fields) part_drop(f.resolved, "p->" + ident(f.name));
                break;
            case TypeKind::tuple:
                for (std::size_t i = 0; i < t->elems.size(); i++) part_drop(t->elems[i], "p->f" + std::to_string(i));
                break;
            case TypeKind::array:
                s += "    for (cx_usize i = 0; i < " + std::to_string(t->length) + "; i++) " + drop_fn(t->elem) + "(&p->v[i]);\n";
                break;
            case TypeKind::optional: s += "    if (p->has) " + drop_fn(t->elem) + "(&p->v);\n"; break;
            case TypeKind::enum_: {
                s += "    switch (p->tag) {\n";
                const Slice<VariantDecl>& vs = t->item->variants;
                for (std::uint32_t i = 0; i < vs.size(); i++) {
                    bool any = false;
                    for (const Type* x : vs[i].types) any = any || !is_copy(x);
                    if (!any) continue;
                    s += "    case " + int_const(t->repr, vs[i].tag) + ":\n";
                    for (std::size_t k = 0; k < vs[i].types.size(); k++) {
                        if (is_copy(vs[i].types[k])) continue;
                        s += "        " + drop_fn(vs[i].types[k]) + "(&p->u.v" + std::to_string(i) + "." +
                             variant_field(t, i, k) + ");\n";
                    }
                    s += "        break;\n";
                }
                s += "    default: break;\n    }\n";
                break;
            }
            default: break;
        }
        return s;
    }

    // A copy of the value at `p` that owns memory of its own.
    std::string copy_body(const Type* t) {
        std::string c = ctype(t);
        std::string s = "    " + c + " r = *p;\n";
        auto part_copy = [&](const Type* x, const std::string& to, const std::string& from) {
            if (!is_copy(x)) s += "    " + to + " = " + copy_fn(x) + "(&" + from + ");\n";
        };
        switch (t->kind) {
            case TypeKind::map:
            case TypeKind::set: {
                const Type* st = slot_type(t);
                std::string e = ctype(st);
                s += "    if (!p->slots) return r;\n    r.ctrl = cx_table_alloc(p->mask + 1, sizeof(" + e +
                     "), (void**)&r.slots);\n    __builtin_memcpy(r.ctrl, p->ctrl, p->mask + 1 + CX_GROUP);\n";
                if (is_copy(st)) {
                    s += "    __builtin_memcpy(r.slots, p->slots, (p->mask + 1) * sizeof(" + e + "));\n";
                } else {
                    s += each_full("p->", "    ", "        r.slots[i] = " + copy_fn(st) + "(&p->slots[i]);\n");
                }
                break;
            }
            case TypeKind::list: {
                std::string e = ctype(t->elem);
                s = "    " + c + " r = {0};\n    if (!p->len) return r;\n    r.ptr = (" + e + "*)cx_alloc(p->len * sizeof(" + e +
                    "));\n    r.len = r.cap = p->len;\n";
                if (is_copy(t->elem)) s += "    __builtin_memcpy(r.ptr, p->ptr, p->len * sizeof(" + e + "));\n";
                else s += "    for (cx_usize i = 0; i < p->len; i++) r.ptr[i] = " + copy_fn(t->elem) + "(&p->ptr[i]);\n";
                break;
            }
            case TypeKind::struct_:
                for (const FieldDecl& f : t->item->fields) part_copy(f.resolved, "r." + ident(f.name), "p->" + ident(f.name));
                break;
            case TypeKind::tuple:
                for (std::size_t i = 0; i < t->elems.size(); i++) {
                    part_copy(t->elems[i], "r.f" + std::to_string(i), "p->f" + std::to_string(i));
                }
                break;
            case TypeKind::array:
                s += "    for (cx_usize i = 0; i < " + std::to_string(t->length) + "; i++) r.v[i] = " + copy_fn(t->elem) +
                     "(&p->v[i]);\n";
                break;
            case TypeKind::optional: s += "    if (p->has) r.v = " + copy_fn(t->elem) + "(&p->v);\n"; break;
            case TypeKind::enum_: {
                s += "    switch (p->tag) {\n";
                const Slice<VariantDecl>& vs = t->item->variants;
                for (std::uint32_t i = 0; i < vs.size(); i++) {
                    bool any = false;
                    for (const Type* x : vs[i].types) any = any || !is_copy(x);
                    if (!any) continue;
                    s += "    case " + int_const(t->repr, vs[i].tag) + ":\n";
                    for (std::size_t k = 0; k < vs[i].types.size(); k++) {
                        if (is_copy(vs[i].types[k])) continue;
                        std::string f = ".u.v" + std::to_string(i) + "." + variant_field(t, i, k);
                        s += "        r" + f + " = " + copy_fn(vs[i].types[k]) + "(&p->" + f.substr(1) + ");\n";
                    }
                    s += "        break;\n";
                }
                s += "    default: break;\n    }\n";
                break;
            }
            default: break;
        }
        return s + "    return r;\n";
    }

    // The call that prints the value `v` of type `t`.
    std::string show_value(const Type* t, const std::string& v) {
        if (is_int(t)) return t->is_signed ? "cx_print_i64((cx_i64)" + v + ");" : "cx_print_u64((cx_u64)" + v + ");";
        if (is_float(t)) return std::string(t->bits == 32 ? "cx_print_f32(" : "cx_print_f64(") + v + ");";
        switch (t->kind) {
            case TypeKind::bool_: return "cx_print_bool(" + v + ");";
            case TypeKind::char_: return "cx_print_char(" + v + ");";
            case TypeKind::rune: return "cx_print_rune(" + v + ");";
            case TypeKind::str: return "cx_print_str(" + v + ");";
            case TypeKind::string: return "cx_print_str(cx_sv(" + v + "));";
            case TypeKind::enum_:
                if (t->has_data) return show_fn(t) + "(" + v + ");";
                return "cx_print_str(" + enum_name_fn(t) + "(" + v + "));";
            case TypeKind::struct_: case TypeKind::tuple: case TypeKind::array: case TypeKind::list:
            case TypeKind::optional: case TypeKind::map: case TypeKind::set: return show_fn(t) + "(" + v + ");";
            default: return "";
        }
    }

    ConstOut emit_const(Item* c) {
        ConstOut o;
        uses_ = &o.uses;
        o.text = "static const " + ctype(c->declared) + " " + std::string(c->c_name) + " = " + expr(c->value) + ";\n";
        uses_ = nullptr;
        return o;
    }

    std::string signature(const Item* f) {
        std::string s = ctype(f->result_type) + " " + std::string(f->c_name) + "(";
        if (f->params.empty()) s += "void";
        for (std::size_t i = 0; i < f->params.size(); i++) {
            const Local* l = f->params[i].local;
            if (i) s += ", ";
            if (l->is_view) {
                // D48: views never overlap, so they can be `restrict`.
                s += (l->mode == ParamMode::mut ? "" : "const ") + ctype(l->type) + "* restrict " + local_name(l);
            } else {
                s += ctype(l->type) + " " + local_name(l);
            }
        }
        return s + ")";
    }

    // --- output ------------------------------------------------------------------------------

    void line(const std::string& s) {
        out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
        out_ += s;
        out_ += '\n';
    }

    // `#line`. A function's first one names its file, and the rest in that file give just the
    // line, which keeps the C small. Text can be emitted out of order, so once any directive
    // names another file, every later one names its file again.
    void directive(Span s) {
        if (!opt_.line_directives) return;
        auto [ln, col] = sources_.file(s.file).position(s.begin);
        (void)col;
        if (s.file == line_file_ && !line_files_mixed_) {
            out_ += "#line " + std::to_string(ln) + "\n";
            return;
        }
        if (line_file_ != kNoFile && s.file != line_file_) line_files_mixed_ = true;
        line_file_ = s.file;
        out_ += "#line " + std::to_string(ln) + " " + c_string_literal(abs_path(s.file)) + "\n";
    }

    const std::string& abs_path(std::uint32_t file) {
        auto it = abs_paths_.find(file);
        if (it != abs_paths_.end()) return it->second;
        std::error_code ec;
        std::filesystem::path p = std::filesystem::absolute(sources_.file(file).path, ec);
        std::string s = (ec ? std::filesystem::path(sources_.file(file).path) : p).generic_string();
        return abs_paths_.emplace(file, s).first->second;
    }

    // `path:line:col`, for panic messages.
    std::string location(Span s) {
        auto [ln, col] = sources_.file(s.file).position(s.begin);
        return c_string_literal(sources_.file(s.file).path + ":" + std::to_string(ln) + ":" + std::to_string(col));
    }

    std::string tmp() { return "cx_t" + std::to_string(++tmp_); }

    // Runs `body` with the output redirected, and returns what it wrote.
    std::string capture(const std::function<void()>& body) {
        std::string saved;
        saved.swap(out_);
        body();
        saved.swap(out_);
        return saved;
    }

    // --- functions and statements --------------------------------------------------------------

    FnOut emit_fn(Item* f) {
        FnOut o;
        fn_ = f;
        tmp_ = 0;
        frames_.clear();
        loop_frames_.clear();
        hidden_names_.clear();
        helpers_.clear();
        out_.clear();
        line_file_ = kNoFile;
        line_files_mixed_ = false;
        directive(f->name_span);
        o.directive.swap(out_);
        uses_ = &o.sig_uses;
        o.sig = signature(f);
        uses_ = &o.uses;
        out_ = " {\n";
        indent_ = 1;
        for (const Param& p : f->params) {
            if (p.local->is_view) line("CX_VIEW(" + local_name(p.local) + ");");
        }
        stmts(f->body, /*value=*/false);
        if (f->result_type->is_result && f->result_type->item->variants[0].types.empty()) {
            line("return " + variant_value(f->result_type, 0, {}) + ";");
        }
        indent_ = 0;
        out_ += "}\n\n";
        o.body.swap(out_);
        o.helpers.swap(helpers_);
        uses_ = nullptr;
        return o;
    }

    // A block's statements, and its tail as a statement. With `value`, the tail is left out for
    // the caller.
    void stmts(Block* b, bool value) {
        for (Stmt* s : b->stmts) stmt(s);
        if (b->tail && !value) {
            directive(b->tail->span);
            full([&] { expr_stmt(b->tail); });
        }
        if (!value) drops(b->drops);
    }

    // --- ownership (D76) ---------------------------------------------------------------------------

    // What matching a pattern takes: the tests, all of which must hold, and the names it binds,
    // each with the C for its value.
    struct Match {
        std::vector<std::string> tests;
        std::vector<std::pair<const Local*, std::string>> binds;
    };

    // The new values a statement only looks at, which live until it ends. Each is declared zeroed
    // before the statement, so dropping one that never got its value is safe.
    struct Frame {
        std::vector<std::pair<std::string, const Type*>> temps;
    };

    // Emits a statement in its own frame: its temporaries are declared before it and dropped
    // after it.
    void full(const std::function<void()>& body) {
        frames_.emplace_back();
        std::string text = capture(body);
        Frame f = std::move(frames_.back());
        frames_.pop_back();
        for (const auto& [name, t] : f.temps) line(ctype(t) + " " + name + " = " + zero(t) + ";");
        out_ += text;
        for (auto it = f.temps.rbegin(); it != f.temps.rend(); ++it) line(drop_fn(it->second) + "(&" + it->first + ");");
    }

    // A condition, with its own temporaries dropped once it's worked out.
    std::string condition_expr(Expr* e) {
        frames_.emplace_back();
        std::string c = expr(e);
        Frame f = std::move(frames_.back());
        frames_.pop_back();
        if (f.temps.empty()) return c;
        std::string s = "({ ", r = tmp();
        for (const auto& [name, t] : f.temps) s += ctype(t) + " " + name + " = " + zero(t) + "; ";
        s += "cx_bool " + r + " = " + c + "; ";
        for (auto it = f.temps.rbegin(); it != f.temps.rend(); ++it) s += drop_fn(it->second) + "(&" + it->first + "); ";
        return s + r + "; })";
    }

    // Drops the locals in `ls`, in that order.
    void drops(const Slice<Local*>& ls) {
        for (Local* l : ls) line(drop_fn(l->type) + "(&" + local_name(l) + ");");
    }

    // Before leaving early: the temporaries of the statements being left, from frame `from` in.
    void exit_drops(std::size_t from, const Slice<Local*>& ls) {
        for (std::size_t i = frames_.size(); i-- > from;) {
            const Frame& f = frames_[i];
            for (auto it = f.temps.rbegin(); it != f.temps.rend(); ++it) line(drop_fn(it->second) + "(&" + it->first + ");");
        }
        drops(ls);
    }

    bool exit_needs_drops(std::size_t from, const Slice<Local*>& ls) const {
        if (!ls.empty()) return true;
        for (std::size_t i = from; i < frames_.size(); i++) {
            if (!frames_[i].temps.empty()) return true;
        }
        return false;
    }

    // A new value kept for the statement: `(*(t = v, &t))`, a place that lasts until it ends.
    std::string temp_of(Expr* e) {
        std::string t = tmp();
        frames_.back().temps.emplace_back(t, e->type);
        return "(*(" + t + " = " + expr_value(e) + ", &" + t + "))";
    }

    // Zeroes the parts of the value at `at` that a pattern binds, so dropping the value frees only
    // the rest.
    void zero_bound(const Match& m) {
        for (const auto& [l, v] : m.binds) {
            if (!is_copy(l->type)) line(v + " = " + zero(l->type) + ";");
        }
    }

    // A pattern that takes its value apart: its names now own their parts, and the rest of the
    // value is dropped. The value is zeroed after, since a field it's in is still dropped with
    // the rest of its local.
    void take_apart(const Match& m, const std::string& at, const Type* t) {
        zero_bound(m);
        line(drop_fn(t) + "(&" + at + ");");
        line(at + " = " + zero(t) + ";");
    }

    void stmt(Stmt* s) {
        directive(s->span);
        switch (s->kind) {
            case StmtKind::let: {
                if (s->pattern->kind == PatternKind::wildcard && !s->else_ && !s->else_block) {
                    full([&] { discard(s->value, s->drop_value); });
                    break;
                }
                if (s->else_ || s->else_block || s->pattern->kind != PatternKind::binding) {
                    full([&] { let_pattern(s); });
                    break;
                }
                Local* l = s->local;
                if (!l || l->type->kind == TypeKind::error || s->value->type->kind == TypeKind::never) {
                    full([&] { expr_stmt(s->value); });
                    break;
                }
                full([&] { line(ctype(l->type) + " " + local_name(l) + " = " + value(s->value, l->type) + ";"); });
                break;
            }
            case StmtKind::assign: full([&] { assign(s); }); break;
            case StmtKind::expr: full([&] { expr_stmt(s->value); }); break;
            case StmtKind::return_:
            case StmtKind::fail: ret(s); break;
            case StmtKind::break_: {
                std::string id = std::to_string(s->loop_id);
                std::size_t from = loop_frames_.count(s->loop_id) ? loop_frames_[s->loop_id] : frames_.size();
                if (s->value && s->value->type->kind != TypeKind::unit && s->value->type->kind != TypeKind::never) {
                    full([&] { line("cx_lv" + id + " = " + value(s->value, s->value->type) + ";"); });
                } else if (s->value) {
                    full([&] { expr_stmt(s->value); });
                }
                exit_drops(from, s->drops);
                line("goto cx_brk" + id + ";");
                break;
            }
            case StmtKind::continue_: {
                std::size_t from = loop_frames_.count(s->loop_id) ? loop_frames_[s->loop_id] : frames_.size();
                exit_drops(from, s->drops);
                line("goto cx_cont" + std::to_string(s->loop_id) + ";");
                break;
            }
            case StmtKind::while_: {
                std::string id = std::to_string(s->loop_id);
                line("{");
                indent_++;
                line("while (" + condition_expr(s->cond) + ") {");
                indent_++;
                loop_frames_[s->loop_id] = frames_.size();
                stmts(s->body, false);
                line("cx_cont" + id + ":;");
                indent_--;
                line("}");
                line("cx_brk" + id + ":;");
                indent_--;
                line("}");
                break;
            }
            case StmtKind::for_: full([&] { s->is_parallel ? parallel_loop(s) : for_loop(s); }); break;
            default: line("/* unsupported statement */"); break;
        }
    }

    // `return`: its value, then what's dropped, then the jump.
    void ret(Stmt* s) {
        if (s->value && s->value->type->kind == TypeKind::never) {
            full([&] { expr_stmt(s->value); });
            return;
        }
        Type* rt = fn_->result_type;
        frames_.emplace_back();
        std::string body = capture([&] {
            if (!s->value && !rt->is_result) {
                exit_drops(0, s->drops);
                line("return;");
            } else {
                // Worked out first, since it can need temporaries of its own. In a function that
                // can fail, `return v` gives back `ok(v)` and `fail e` gives back `err(e)`.
                std::string v;
                if (rt->is_result) {
                    std::uint32_t i = s->kind == StmtKind::fail ? 1 : 0;
                    std::vector<std::string> parts;
                    if (s->value) parts.push_back(value(s->value, rt->item->variants[i].types[0]));
                    v = variant_value(rt, i, parts);
                } else {
                    v = value(s->value, rt);
                }
                if (exit_needs_drops(0, s->drops)) {
                    std::string r = tmp();
                    line(ctype(fn_->result_type) + " " + r + " = " + v + ";");
                    exit_drops(0, s->drops);
                    line("return " + r + ";");
                } else {
                    line("return " + v + ";");
                }
            }
        });
        Frame f = std::move(frames_.back());
        frames_.pop_back();
        bool braces = !f.temps.empty();
        if (braces) {
            line("{");
            indent_++;
            for (const auto& [name, t] : f.temps) line(ctype(t) + " " + name + " = " + zero(t) + ";");
            std::string pad(4, ' ');
            std::string shifted;
            std::size_t start = 0;
            while (start < body.size()) {
                std::size_t end = body.find('\n', start);
                if (end == std::string::npos) end = body.size();
                std::string ln = body.substr(start, end - start);
                shifted += (ln.empty() || ln[0] == '#' ? "" : pad) + ln + "\n";
                start = end + 1;
            }
            out_ += shifted;
            indent_--;
            line("}");
        } else {
            out_ += body;
        }
    }

    // `let _ = e` and `_ = e`: worked out, and dropped if it's a new value that owns memory.
    void discard(Expr* v, bool drop) {
        if (!drop) {
            std::string x = expr(v);
            if (v->type->kind == TypeKind::never || v->type->kind == TypeKind::unit) line(x + ";");
            else line("(void)(" + x + ");");
            return;
        }
        std::string t = tmp();
        line("{ " + ctype(v->type) + " " + t + " = " + value(v, v->type) + "; " + drop_fn(v->type) + "(&" + t + "); }");
    }

    void for_loop(Stmt* s) {
        std::string id = std::to_string(s->loop_id);
        Type* elem = s->elem_type;
        std::string var = s->local ? local_name(s->local) : "";
        Expr* it = s->value;
        line("{");
        indent_++;
        loop_frames_[s->loop_id] = frames_.size();
        if (it->kind == ExprKind::range) {
            std::string i = tmp(), end = tmp();
            line(ctype(elem) + " " + i + " = " + value(it->lhs, elem) + ";");
            line(ctype(elem) + " " + end + " = " + value(it->rhs, elem) + ";");
            if (it->inclusive) {
                // `a..=MAX` must stop without stepping past the end.
                line("if (" + i + " <= " + end + ") for (;;) {");
            } else {
                line("for (; " + i + " < " + end + "; " + i + "++) {");
            }
            indent_++;
            if (!var.empty()) line(ctype(elem) + " " + var + " = " + i + ";");
            stmts(s->body, false);
            line("cx_cont" + id + ":;");
            if (it->inclusive) {
                line("if (" + i + " == " + end + ") break;");
                line(i + "++;");
            }
            indent_--;
            line("}");
        } else {
            Type* at = it->type;
            bool list = at->kind == TypeKind::list;
            std::string arr = tmp(), i = tmp();
            // A variable is read in place, since the loop can't change it. Anything else is
            // evaluated once, into the ownership pass's local when it owns memory.
            if (s->hidden) {
                line(ctype(at) + " " + local_name(s->hidden) + " = " + value(it, at) + ";");
                line("const " + ctype(at) + "* " + arr + " = &" + local_name(s->hidden) + ";");
            } else if (is_place(it) && !effects(it)) {
                line("const " + ctype(at) + "* " + arr + " = &" + place(it) + ";");
            } else {
                std::string val = tmp();
                line(ctype(at) + " " + val + " = " + value(it, at) + ";");
                line("const " + ctype(at) + "* " + arr + " = &" + val + ";");
            }
            std::string elem_at;
            if (is_table(at)) {
                // Slot by slot, skipping the empty ones: the order depends only on the hashes and
                // what was done to the table, so it's the same in every run (D56).
                elem_at = arr + "->slots[" + i + "]";
                line("for (cx_usize " + i + " = 0; " + i + " <= " + arr + "->mask; " + i + "++) {");
                indent_++;
                line("if (" + arr + "->ctrl[" + i + "] >= 0x80) continue;");
            } else {
                std::string n = list ? arr + "->len" : std::to_string(at->length);
                elem_at = arr + (list ? "->ptr[" : "->v[") + i + "]";
                line("for (cx_usize " + i + " = 0; " + i + " < " + n + "; " + i + "++) {");
                indent_++;
            }
            if (!var.empty()) {
                line(ctype(elem) + " " + var + " = " + elem_at + ";");
            } else if (s->pattern->kind != PatternKind::wildcard) {
                // `for (a, b) in pairs`: the checker made sure the pattern matches every element.
                Match m;
                walk(s->pattern, elem_at, m);
                bind(m);
            }
            stmts(s->body, false);
            line("cx_cont" + id + ":;");
            indent_--;
            line("}");
        }
        line("cx_brk" + id + ":;");
        if (s->hidden && s->drop_hidden) line(drop_fn(s->hidden->type) + "(&" + local_name(s->hidden) + ");");
        indent_--;
        line("}");
    }

    // `parallel for k in a..b { body }`: the body becomes a function of its own, which the runtime
    // calls with chunks of the range, on its threads (D21). The body reaches the locals it uses
    // from outside through a context struct: numbers by value, since the body can't change them,
    // and anything else by pointer. The checker made sure iterations never touch the same memory,
    // so the pointers can be `restrict`, and views are whole and live, as for parameters.
    void parallel_loop(Stmt* s) {
        Type* elem = s->elem_type;
        Expr* it = s->value;
        std::string id = std::to_string(s->loop_id);
        std::string name = "cx_par_" + std::string(fn_->c_name) + "_" + id;
        std::string ctx_t = "struct " + name + "_c";
        struct Cap {
            Local* l;
            bool by_value;
            std::string type;  // the field's C type
        };
        std::vector<Cap> caps;
        for (const Capture& c : s->captures) {
            Local* l = c.local;
            bool pointer = l->is_view || par_pointers_.count(l);
            bool scalar = l->type->kind == TypeKind::int_ || l->type->kind == TypeKind::float_ ||
                          l->type->kind == TypeKind::bool_ || l->type->kind == TypeKind::char_ || l->type->kind == TypeKind::rune;
            if (scalar && !pointer && !c.written) {
                caps.push_back({l, true, ctype(l->type)});
            } else {
                // A view parameter that can't change stays `const`.
                bool read_only = l->is_view && !par_pointers_.count(l) && l->mode != ParamMode::mut;
                caps.push_back({l, false, (read_only ? "const " : "") + ctype(l->type) + "*"});
            }
        }

        // The body's function, emitted out of line with frames of its own.
        std::vector<Frame> outer_frames = std::move(frames_);
        frames_.clear();
        int outer_indent = indent_;
        std::uint32_t outer_file = line_file_;
        bool outer_mixed = line_files_mixed_;
        line_file_ = kNoFile;
        line_files_mixed_ = false;
        std::string fn_text = capture([&] {
            indent_ = 0;
            std::string fields;
            for (const Cap& c : caps) fields += " " + c.type + " " + local_name(c.l) + ";";
            line(ctx_t + " {" + fields + " cx_u64 cx__lo; };");
            directive(s->span);
            line("static void " + name + "(void* cx__v, cx_u64 cx__begin, cx_u64 cx__end) {");
            indent_ = 1;
            line(ctx_t + "* cx__c = (" + ctx_t + "*)cx__v;");
            std::vector<const Local*> pointed;
            for (const Cap& c : caps) {
                std::string v = local_name(c.l);
                if (c.by_value) {
                    line("const " + c.type + " " + v + " = cx__c->" + v + ";");
                } else {
                    line(c.type + " restrict " + v + " = cx__c->" + v + ";");
                    line("CX_VIEW(" + v + ");");
                    if (!c.l->is_view && par_pointers_.insert(c.l).second) pointed.push_back(c.l);
                }
            }
            line("for (cx_u64 cx__i = cx__begin; cx__i < cx__end; cx__i++) {");
            indent_ = 2;
            loop_frames_[s->loop_id] = frames_.size();
            if (s->local) {
                line(ctype(elem) + " " + local_name(s->local) + " = (" + ctype(elem) + ")(cx__c->cx__lo + cx__i);");
            }
            stmts(s->body, false);
            line("cx_cont" + id + ":;");
            indent_ = 1;
            line("}");
            indent_ = 0;
            line("}");
            for (const Local* l : pointed) par_pointers_.erase(l);
        });
        helpers_ += fn_text + "\n";
        frames_ = std::move(outer_frames);
        indent_ = outer_indent;
        line_file_ = outer_file;
        line_files_mixed_ = outer_mixed;

        // The loop itself: the range, worked out once, and the call.
        std::string lo = tmp(), hi = tmp(), c = tmp();
        std::string t = ctype(elem);
        line("{");
        indent_++;
        line(t + " " + lo + " = " + value(it->lhs, elem) + ";");
        line(t + " " + hi + " = " + value(it->rhs, elem) + ";");
        line("if (" + lo + (it->inclusive ? " <= " : " < ") + hi + ") {");
        indent_++;
        std::string n = "(cx_u64)" + hi + " - (cx_u64)" + lo;
        if (it->inclusive) {
            std::string count = tmp();
            line("cx_u64 " + count + " = " + n + " + 1;");
            // Only a range over every 64-bit value has 2^64 iterations.
            line("if (" + count + " == 0) { cx_panic_begin(" + location(it->span) +
                 "); cx_print_str((cx_str){(const cx_u8*)\"a `parallel for` can't run 2^64 iterations\", 42}); cx_panic_end(); }");
            n = count;
        }
        std::string inits;
        for (const Cap& cap : caps) {
            std::string v = local_name(cap.l);
            bool pointer = cap.l->is_view || par_pointers_.count(cap.l);
            inits += (cap.by_value || pointer ? v : "&" + v) + ", ";
        }
        line(ctx_t + " " + c + " = {" + inits + "(cx_u64)" + lo + "};");
        line("cx_parallel(" + n + ", " + name + ", &" + c + ");");
        indent_--;
        line("}");
        indent_--;
        line("}");
    }

    // An expression whose value isn't used.
    void expr_stmt(Expr* e) {
        switch (e->kind) {
            case ExprKind::if_: if_stmt(e); return;
            case ExprKind::match: match_arms(e, nullptr, nullptr); return;
            case ExprKind::block:
            case ExprKind::unsafe_block:
                line("{");
                indent_++;
                stmts(e->block, false);
                indent_--;
                line("}");
                return;
            case ExprKind::loop: {
                std::string id = std::to_string(e->loop_id);
                line("for (;;) {");
                indent_++;
                loop_frames_[e->loop_id] = frames_.size();
                stmts(e->block, false);
                line("cx_cont" + id + ":;");
                indent_--;
                line("}");
                line("cx_brk" + id + ":;");
                return;
            }
            default: break;
        }
        std::string s = expr(e);
        if (!s.empty()) line(s + ";");
    }

    void if_stmt(Expr* e) {
        if (e->let_pattern) {
            // `if let p = v`: its names exist only in the first block.
            line("{");
            indent_++;
            std::string at;
            Match m = let_match(e, at);
            line("if (" + condition(m) + ") {");
            indent_++;
            bind(m);
            if (e->let_consumes) take_apart(m, at, e->cond->type);
            stmts(e->block, false);
            drops(e->let_drops);
            indent_--;
            if (e->else_) {
                line("} else {");
                indent_++;
                if (e->else_->kind == ExprKind::if_) if_stmt(e->else_);
                else stmts(e->else_->block, false);
                indent_--;
            }
            line("}");
            if (e->hidden && e->drop_hidden) line(drop_fn(e->hidden->type) + "(&" + local_name(e->hidden) + ");");
            indent_--;
            line("}");
            return;
        }
        line("if (" + condition_expr(e->cond) + ") {");
        indent_++;
        stmts(e->block, false);
        indent_--;
        Expr* el = e->else_;
        while (el) {
            if (el->kind == ExprKind::if_ && el->let_pattern) {
                line("} else {");
                indent_++;
                if_stmt(el);
                indent_--;
                el = nullptr;
            } else if (el->kind == ExprKind::if_) {
                line("} else if (" + condition_expr(el->cond) + ") {");
                indent_++;
                stmts(el->block, false);
                indent_--;
                el = el->else_;
            } else {
                line("} else {");
                indent_++;
                stmts(el->block, false);
                indent_--;
                el = nullptr;
            }
        }
        line("}");
    }

    // --- patterns --------------------------------------------------------------------------------

    // Collects the tests and bindings for matching `p` against the value at `at`, a C place.
    void walk(const Pattern* p, const std::string& at, Match& m) {
        const Type* t = p->type;
        switch (p->kind) {
            case PatternKind::wildcard: return;
            case PatternKind::binding:
                if (p->local) m.binds.emplace_back(p->local, at);
                return;
            case PatternKind::literal:
                if (t->kind == TypeKind::str || t->kind == TypeKind::string) {
                    std::string v = t->kind == TypeKind::string ? "cx_sv(" + at + ")" : at;
                    m.tests.push_back("cx_str_eq(" + v + ", " + str_const(literal_text(p->value)) + ")");
                } else {
                    m.tests.push_back(at + " == " + expr(p->value));
                }
                return;
            case PatternKind::range: {
                std::string lo = at + " >= " + expr(p->value);
                std::string hi = at + (p->inclusive ? " <= " : " < ") + expr(p->end);
                // An unsigned range from 0 needs no lower test.
                bool from_zero = !t->is_signed && p->value->const_bits == 0;
                m.tests.push_back(from_zero ? hi : "(" + lo + " && " + hi + ")");
                return;
            }
            case PatternKind::none: m.tests.push_back("!" + at + ".has"); return;
            case PatternKind::variant:
                if (t->kind == TypeKind::optional) {
                    m.tests.push_back(at + ".has");
                    walk(p->elems[0], at + ".v", m);
                    return;
                }
                if (!t->has_data) {
                    m.tests.push_back(at + " == " + int_const(t->repr, t->item->variants[p->variant].tag));
                    return;
                }
                m.tests.push_back(at + ".tag == " + int_const(t->repr, t->item->variants[p->variant].tag));
                for (std::size_t k = 0; k < p->elems.size(); k++) {
                    walk(p->elems[k], at + ".u.v" + std::to_string(p->variant) + "." + variant_field(t, p->variant, k), m);
                }
                return;
            case PatternKind::record:
                if (t->kind == TypeKind::enum_) {
                    m.tests.push_back(at + ".tag == " + int_const(t->repr, t->item->variants[p->variant].tag));
                    for (const FieldPattern& f : p->fields) {
                        walk(f.pattern, at + ".u.v" + std::to_string(p->variant) + "." + variant_field(t, p->variant, f.index), m);
                    }
                    return;
                }
                for (const FieldPattern& f : p->fields) walk(f.pattern, at + member(t, f.index), m);
                return;
            case PatternKind::tuple:
                for (std::size_t k = 0; k < p->elems.size(); k++) walk(p->elems[k], at + ".f" + std::to_string(k), m);
                return;
            case PatternKind::or_: {
                // The checker made sure no alternative binds a name.
                std::string any;
                for (const Pattern* a : p->elems) {
                    Match sub;
                    walk(a, at, sub);
                    if (sub.tests.empty()) return;  // this alternative always matches
                    any += (any.empty() ? "(" : " || (") + condition(sub) + ")";
                }
                m.tests.push_back("(" + any + ")");
                return;
            }
        }
    }

    static std::string condition(const Match& m) {
        if (m.tests.empty()) return "1";
        std::string c;
        for (std::size_t i = 0; i < m.tests.size(); i++) c += (i ? " && " : "") + m.tests[i];
        return c;
    }

    void bind(const Match& m) {
        for (const auto& [l, v] : m.binds) line(ctype(l->type) + " " + local_name(l) + " = " + v + ";");
    }

    // The value being matched, in a place: the variable itself when reading it can't change
    // anything, or a temporary. Declares the temporary at the current indent.
    std::string scrutinee(Expr* v, bool may_change, Local* hidden) {
        if (hidden) {
            line(ctype(v->type) + " " + local_name(hidden) + " = " + value(v, v->type) + ";");
            return local_name(hidden);
        }
        // A value that owns memory isn't copied: the names bound are views into it, or take it apart.
        bool owning = !is_copy(v->type);
        if ((!may_change || owning) && is_place(v) && !place_has_effects(v)) return place(v);
        std::string t = tmp();
        line(ctype(v->type) + " " + t + " = " + value(v, v->type) + ";");
        return t;
    }

    // `if let p = v`. When it unwraps, `v` is an optional and `p` matches its value. `at` is
    // where the value is.
    Match let_match(Expr* e, std::string& at) {
        at = scrutinee(e->cond, false, e->hidden);
        Match m;
        std::string inner = at;
        if (e->let_unwraps) {
            m.tests.push_back(at + ".has");
            inner += ".v";
        }
        walk(e->let_pattern, inner, m);
        return m;
    }

    // `let (a, b) = t`, `let Shape.Circle(r) = s else { ... }` and `let x = opt else return`.
    void let_pattern(Stmt* s) {
        if (s->value->type->kind == TypeKind::never || s->value->type->kind == TypeKind::error) {
            expr_stmt(s->value);
            return;
        }
        // A temporary, since the names it binds outlive any statement the value could be read in,
        // unless it's a place that owns memory, whose names are views into it.
        std::string t;
        Type* vt = s->value->type;
        if (s->hidden) {
            t = local_name(s->hidden);
            line(ctype(vt) + " " + t + " = " + value(s->value, vt) + ";");
        } else if (!is_copy(vt) && is_place(s->value) && !place_has_effects(s->value)) {
            t = place(s->value);
        } else {
            t = tmp();
            line(ctype(vt) + " " + t + " = " + value(s->value, vt) + ";");
        }
        Match m;
        std::string at = t;
        if (s->let_unwraps) {
            m.tests.push_back(t + ".has");
            at += ".v";
        }
        walk(s->pattern, at, m);
        if ((s->else_ || s->else_block) && !m.tests.empty()) {
            line("if (!(" + condition(m) + ")) {");
            indent_++;
            if (s->else_block) {
                stmts(s->else_block, false);
            } else {
                directive(s->else_->span);
                expr_stmt(s->else_);
            }
            indent_--;
            line("}");
        }
        bind(m);
        if (s->consumes) take_apart(m, t, vt);
    }

    // A `match`'s arms, as an if-chain that jumps to the end after the arm that ran. With `r`, each
    // arm's value goes in it. The last arm needs no test: the checker made sure the arms are
    // exhaustive, and with no guard on the last arm, whatever reaches it matches it.
    void match_arms(Expr* e, const std::string* r, Type* t) {
        bool guards = false;
        for (const MatchArm& a : e->arms) guards = guards || a.guard;
        std::string end = "cx_brk" + std::to_string(e->loop_id);
        line("{");
        indent_++;
        std::string at = scrutinee(e->scrutinee, guards, e->hidden);
        for (std::size_t i = 0; i < e->arms.size(); i++) {
            const MatchArm& arm = e->arms[i];
            bool last = i + 1 == e->arms.size();
            Match m;
            walk(arm.pattern, at, m);
            directive(arm.pattern->span);
            line(last && !arm.guard ? "{" : "if (" + condition(m) + ") {");
            indent_++;
            bind(m);
            if (arm.guard) {
                line("if (" + condition_expr(arm.guard) + ") {");
                indent_++;
            }
            if (arm.consumes) take_apart(m, at, e->scrutinee->type);
            directive(arm.body->span);
            if (r && arm.body->type->kind != TypeKind::never) {
                full([&] { line(*r + " = " + value(arm.body, t) + ";"); });
            } else {
                full([&] { expr_stmt(arm.body); });
            }
            drops(arm.drops);
            if (!last || arm.guard) line("goto " + end + ";");
            if (arm.guard) {
                indent_--;
                line("}");
            }
            indent_--;
            line("}");
            if (last && arm.guard) line("__builtin_unreachable();");
        }
        line(end + ":;");
        if (e->hidden && e->drop_hidden) line(drop_fn(e->hidden->type) + "(&" + local_name(e->hidden) + ");");
        indent_--;
        line("}");
    }

    std::string match_value(Expr* e) {
        Type* t = e->type;
        bool has_value = t->kind != TypeKind::never && t->kind != TypeKind::unit;
        std::string r = tmp();
        std::string body = capture([&] {
            indent_++;
            if (has_value) line(ctype(t) + " " + r + ";");
            match_arms(e, has_value ? &r : nullptr, t);
            if (has_value) line(r + ";");
            indent_--;
        });
        return "({\n" + body + std::string(static_cast<std::size_t>(indent_) * 4, ' ') + "})";
    }

    void assign(Stmt* s) {
        Expr* target = s->target;
        if (target->kind == ExprKind::name && target->ref != Ref::local) {
            discard(s->value, s->drop_value);  // `_ = e`
            return;
        }
        Type* t = target->type;
        if (s->drop_old) {
            // The new value is worked out before the old one is dropped, since it can read it.
            std::string c = ctype(t), v = tmp();
            if (place_has_effects(target)) {
                std::string p = tmp();
                line("{ " + c + "* " + p + " = &" + place(target) + "; " + c + " " + v + " = " + value(s->value, t) + "; " +
                     drop_fn(t) + "(" + p + "); *" + p + " = " + v + "; }");
            } else {
                std::string p = place(target);
                line("{ " + c + " " + v + " = " + value(s->value, t) + "; " + drop_fn(t) + "(&" + p + "); " + p + " = " + v +
                     "; }");
            }
            return;
        }
        if (!s->compound) {
            // Java evaluates the place's parts, then the value, then stores.
            if (effects(s->value) && place_has_effects(target)) {
                std::string p = tmp(), v = tmp();
                line("{ " + ctype(t) + "* " + p + " = &" + place(target) + "; " + ctype(t) + " " + v + " = " +
                     value(s->value, t) + "; *" + p + " = " + v + "; }");
            } else {
                line(place(target) + " = " + value(s->value, t) + ";");
            }
            return;
        }
        // `a op= b`: the place's parts, then its old value, then `b`.
        if (effects(s->value) || place_has_effects(target)) {
            std::string p = tmp(), old = tmp(), v = tmp();
            line("{ " + ctype(t) + "* " + p + " = &" + place(target) + "; " + ctype(t) + " " + old + " = *" + p + "; " +
                 ctype(s->value->type) + " " + v + " = " + value(s->value, s->value->type) + "; *" + p + " = " +
                 operate(s->op, t, s->value->type, old, v, s->value, s->span) + "; }");
        } else {
            std::string p = place(target);
            line(p + " = " + operate(s->op, t, s->value->type, p, expr(s->value), s->value, s->span) + ";");
        }
    }

    // --- evaluation order ------------------------------------------------------------------------

    // Whether evaluating `e` can have an effect or panic, so that Java's left-to-right order
    // decides what happens (C leaves the order of operands unspecified).
    static bool effects(const Expr* e) {
        if (!e) return false;
        if (e->has_bits) return false;
        switch (e->kind) {
            case ExprKind::int_lit: case ExprKind::float_lit: case ExprKind::char_lit: case ExprKind::bool_lit:
            case ExprKind::name: case ExprKind::self:
                return false;
            case ExprKind::string_lit:
                for (const Expr* x : e->interpolations) if (effects(x)) return true;
                return false;
            case ExprKind::unary: return effects(e->lhs);
            case ExprKind::binary:
                if ((e->op == Op::div || e->op == Op::rem) && is_int(e->type) && !e->rhs->has_bits) return true;
                return effects(e->lhs) || effects(e->rhs);
            case ExprKind::none_lit: return false;
            case ExprKind::field:
                if (e->ref == Ref::variant) return false;
                return effects(e->lhs);
            case ExprKind::optional_field: return effects(e->lhs);
            case ExprKind::index: return true;
            case ExprKind::tuple:
                for (const Expr* x : e->elems) if (effects(x)) return true;
                return false;
            case ExprKind::call:
                if (e->call == CallKind::convert || e->call == CallKind::try_from) return effects(e->args[0].value);
                if (e->call == CallKind::construct) return false;
                if (e->call == CallKind::some || e->call == CallKind::variant) {
                    for (const Arg& a : e->args) if (effects(a.value)) return true;
                    return false;
                }
                if (e->call == CallKind::prim_method) {
                    if (e->prim == PrimMethod::floor_div || e->prim == PrimMethod::floor_mod) return true;
                    for (const Arg& a : e->args) if (effects(a.value)) return true;
                    return effects(e->callee->lhs);
                }
                if (e->call == CallKind::builtin_method) {
                    BMethod b = e->bmethod;
                    if (b == BMethod::len || b == BMethod::is_empty || b == BMethod::capacity) return effects(e->callee->lhs);
                }
                return true;
            case ExprKind::struct_lit:
                for (const FieldInit& f : e->fields) if (effects(f.value)) return true;
                return false;
            case ExprKind::array_lit:
                for (const Expr* x : e->elems) if (effects(x)) return true;
                return false;
            default: return true;
        }
    }

    static bool is_constant(const Expr* e) {
        return e->has_bits || (e->kind == ExprKind::string_lit && e->interpolations.empty()) || e->kind == ExprKind::float_lit;
    }

    static bool is_place(const Expr* e) {
        while (e->kind == ExprKind::field || e->kind == ExprKind::index || e->kind == ExprKind::unwrap) e = e->lhs;
        return (e->kind == ExprKind::name || e->kind == ExprKind::self) && e->ref == Ref::local;
    }

    // Whether finding the place can panic or run code: a list's index is always checked when it
    // runs, an array's unless it's a constant.
    static bool place_has_effects(const Expr* e) {
        while (e->kind == ExprKind::field || e->kind == ExprKind::index || e->kind == ExprKind::unwrap) {
            if (e->kind == ExprKind::unwrap) return true;
            if (e->kind == ExprKind::index && (!e->rhs->has_bits || e->lhs->type->kind == TypeKind::list ||
                                               e->lhs->type->kind == TypeKind::map)) {
                return true;
            }
            e = e->lhs;
        }
        return false;
    }

    enum class Pass { value, address };

    struct Operand {
        Expr* e;
        Type* want;
        Pass pass = Pass::value;
    };

    // Emits operands in Java's order: when a later operand has an effect, every earlier one that
    // isn't a constant is evaluated into a temporary first. Returns the C for each, and the
    // declarations that must come first in `decls`.
    std::vector<std::string> sequence(const std::vector<Operand>& ops, std::string& decls) {
        int last = -1;
        for (int i = 0; i < static_cast<int>(ops.size()); i++) {
            const Operand& o = ops[i];
            bool temp_view = o.pass == Pass::address && !is_place(o.e) && !o.e->temp;
            if (effects(o.e) || temp_view) last = i;
        }
        std::vector<std::string> out;
        // An owned operand waits in a statement temporary when a later one can leave early (see
        // below). Then the last operand with an effect is worked out in order too, so the C
        // arguments have no effects left but the moves out of those temporaries, and C's
        // unspecified argument order can't run a move before the operand that leaves.
        bool held = false;
        for (int i = 0; i < static_cast<int>(ops.size()); i++) {
            const Operand& o = ops[i];
            bool in_order = i < last || (i == last && held);
            if (o.pass == Pass::address) {
                if (is_place(o.e) || o.e->temp) {
                    // A temporary is a place that lasts until the statement ends.
                    std::string a = "&" + place(o.e);
                    if (in_order && (o.e->temp || place_has_effects(o.e))) {
                        std::string t = tmp();
                        decls += ctype(o.want) + "* " + t + " = " + a + "; ";
                        a = t;
                    }
                    out.push_back(a);
                } else {
                    std::string t = tmp();
                    decls += ctype(o.want) + " " + t + " = " + value(o.e, o.want) + "; ";
                    out.push_back("&" + t);
                }
                continue;
            }
            std::string v = value(o.e, o.want);
            if (in_order && !is_constant(o.e)) {
                std::string t = tmp();
                if (i < last && !is_copy(o.want) && exits_after(ops, i)) {
                    // A later operand can leave the statement, and this value must be dropped
                    // then: it waits in one of the statement's temporaries, zeroed as it moves on.
                    held = true;
                    frames_.back().temps.emplace_back(t, o.want);
                    decls += t + " = " + v + "; ";
                    v = "({ " + ctype(o.want) + " " + t + "_v = " + t + "; " + t + " = " + zero(o.want) + "; " + t + "_v; })";
                } else {
                    decls += ctype(o.want) + " " + t + " = " + v + "; ";
                    v = t;
                }
            }
            out.push_back(v);
        }
        return out;
    }

    static bool exits_after(const std::vector<Operand>& ops, int i) {
        for (std::size_t j = static_cast<std::size_t>(i) + 1; j < ops.size(); j++) {
            if (may_exit(ops[j].e)) return true;
        }
        return false;
    }

    // Whether working out `e` can leave the statement early: a `try`, or a `return`, `fail`,
    // `break` or `continue` inside it.
    static bool may_exit(Expr* e) {
        if (e->kind == ExprKind::try_) return true;
        bool v = false;
        each_child(e, [&](Expr* x) { v = v || may_exit(x); }, [&](Block* b) { v = v || may_exit(b); });
        return v;
    }

    static bool may_exit(Block* b) {
        for (const Stmt* s : b->stmts) {
            if (s->kind == StmtKind::return_ || s->kind == StmtKind::fail || s->kind == StmtKind::break_ ||
                s->kind == StmtKind::continue_) {
                return true;
            }
        }
        bool v = false;
        each_child(b, [&](Expr* x) { v = v || may_exit(x); }, [&](Block* c) { v = v || may_exit(c); });
        return v;
    }

    static std::string wrap(const std::string& decls, const std::string& e) {
        if (decls.empty()) return e;
        return "({ " + decls + e + "; })";
    }

    // --- expressions -------------------------------------------------------------------------------

    // `e` as a value of type `want`. A diverging expression still needs a value in C.
    std::string value(Expr* e, Type* want) {
        if (e->type->kind == TypeKind::never && want && want->kind != TypeKind::never && want->kind != TypeKind::unit) {
            return "({ " + expr(e) + "; __builtin_unreachable(); " + zero(want) + "; })";
        }
        return expr(e);
    }

    std::string zero(const Type* t) {
        switch (t->kind) {
            case TypeKind::struct_: case TypeKind::array: case TypeKind::str: case TypeKind::optional:
            case TypeKind::tuple: case TypeKind::string: case TypeKind::list: case TypeKind::map: case TypeKind::set:
                return "(" + ctype(t) + "){0}";
            case TypeKind::enum_: return t->has_data ? "(" + ctype(t) + "){0}" : "(" + ctype(t) + ")0";
            default: return "(" + ctype(t) + ")0";
        }
    }

    std::string int_const(const Type* t, std::uint64_t bits) {
        if (t->kind == TypeKind::bool_) return bits ? "1" : "0";
        if (t->kind == TypeKind::enum_) return int_const(t->repr, bits);
        std::string c = ctype(t);
        if (t->is_signed) {
            auto v = static_cast<std::int64_t>(bits);
            if (v == INT64_MIN) return "((" + c + ")(-9223372036854775807LL - 1))";
            if (v == INT32_MIN) return "((" + c + ")(-2147483647 - 1))";
            std::string digits = std::to_string(v);
            if (t->bits == 64) digits += "LL";
            return v < 0 ? "((" + c + ")" + digits + ")" : "((" + c + ")" + digits + ")";
        }
        std::string digits = std::to_string(bits);
        if (t->bits == 64) digits += "ULL";
        else digits += "u";
        return "((" + c + ")" + digits + ")";
    }

    std::string float_const(const Type* t, std::uint64_t bits) {
        double d;
        std::memcpy(&d, &bits, sizeof d);
        bool f32 = t->bits == 32;
        if (d != d) return f32 ? "__builtin_nanf(\"\")" : "__builtin_nan(\"\")";
        if (d == HUGE_VAL) return f32 ? "__builtin_inff()" : "__builtin_inf()";
        if (d == -HUGE_VAL) return f32 ? "(-__builtin_inff())" : "(-__builtin_inf())";
        char buf[64];
        std::snprintf(buf, sizeof buf, "%a", d);
        std::string s = buf;
        if (f32) s += "f";
        return "(" + s + ")";
    }

    std::string str_const(std::string_view s) {
        return "((cx_str){(const cx_u8*)" + c_string_literal(s) + ", " + std::to_string(s.size()) + "})";
    }

    // The text of a string literal without interpolations.
    static std::string literal_text(const Expr* e) {
        std::string s;
        for (const StringPart& p : e->literal.parts) {
            if (!p.is_expr) s += p.text;
        }
        return s;
    }

    // An expression, with what the ownership pass decided about it: a new value kept for the
    // statement, a move that zeroes what it leaves, or a field moved out of a new value.
    std::string expr(Expr* e) {
        if (e->temp && !frames_.empty()) return temp_of(e);
        return expr_value(e);
    }

    std::string expr_value(Expr* e) {
        if (e->moves && e->kind == ExprKind::field) {
            // A field moved out of a local: it's zeroed, so dropping the local frees the rest.
            std::string p = place_or_value(e), v = tmp();
            return "({ " + ctype(e->type) + " " + v + " = " + p + "; " + p + " = " + zero(e->type) + "; " + v + "; })";
        }
        if (e->moves && e->local && e->local->clear_on_move) {
            std::string p = local_name(e->local), v = tmp();
            return "({ " + ctype(e->type) + " " + v + " = " + p + "; " + p + " = " + zero(e->type) + "; " + v + "; })";
        }
        if (e->part_move) {
            Type* bt = e->lhs->type;
            std::string b = tmp(), v = tmp();
            std::string f = b + member(bt, e->field_index);
            return "({ " + ctype(bt) + " " + b + " = " + expr(e->lhs) + "; " + ctype(e->type) + " " + v + " = " + f + "; " + f +
                   " = " + zero(e->type) + "; " + drop_fn(bt) + "(&" + b + "); " + v + "; })";
        }
        return expr_inner(e);
    }

    std::string expr_inner(Expr* e) {
        if (e->has_bits) {
            if (is_float(e->type)) return float_const(e->type, e->const_bits);
            return int_const(e->type, e->const_bits);
        }
        switch (e->kind) {
            case ExprKind::string_lit:
                if (e->type->kind != TypeKind::string) return str_const(literal_text(e));
                if (!e->interpolations.empty()) return string_value(e);
                return "cx_string_lit(" + c_string_literal(literal_text(e)) + ", " + std::to_string(literal_text(e).size()) + ")";
            case ExprKind::name:
            case ExprKind::self:
                if (e->ref == Ref::const_) {
                    if (uses_) uses_->add_const(e->item);
                    return std::string(e->item->c_name);
                }
                return place(e);
            case ExprKind::unary: return unary(e);
            case ExprKind::binary: return binary(e);
            case ExprKind::call: return call(e);
            case ExprKind::field:
                if (e->ref == Ref::variant) return variant_value(e->type, e->field_index, {});  // `Shape.Empty`
                return place_or_value(e);
            case ExprKind::index: return place_or_value(e);
            case ExprKind::struct_lit: return e->ref == Ref::variant ? record_variant(e) : struct_literal(e);
            case ExprKind::array_lit: return e->type->kind == TypeKind::list ? list_literal(e) : array_literal(e);
            case ExprKind::tuple: return e->elems.empty() ? "" : tuple_literal(e);  // `()` is nothing
            case ExprKind::none_lit: return "((" + ctype(e->type) + "){0})";
            case ExprKind::unwrap: return is_place(e) && !e->lhs->moves ? place(e) : unwrap(e);
            case ExprKind::try_: return try_value(e);
            case ExprKind::optional_field: return optional_field(e);
            case ExprKind::block:
            case ExprKind::unsafe_block: return block_value(e->block, e->type);
            case ExprKind::if_: return if_value(e);
            case ExprKind::match: return match_value(e);
            case ExprKind::loop: return loop_value(e);
            default: return "/* unsupported */0";
        }
    }

    // A place: a variable, or a field or element of one. Views are pointers in C.
    std::string place(Expr* e) {
        switch (e->kind) {
            case ExprKind::name:
            case ExprKind::self: {
                const Local* l = e->local;
                return l->is_view || par_pointers_.count(l) ? "(*" + local_name(l) + ")" : local_name(l);
            }
            case ExprKind::field: return place_or_value(e);
            case ExprKind::index: return place_or_value(e);
            case ExprKind::unwrap: {
                std::string p = tmp(), at = place(e->lhs);
                return "(*({ __typeof__(&" + at + ") " + p + " = &" + at + "; if (__builtin_expect(!" + p +
                       "->has, 0)) cx_panic_unwrap(" + location(e->span) + "); &" + p + "->v; }))";
            }
            default: return "(" + expr(e) + ")";
        }
    }

    // The address of a place, or of a temporary.
    std::string address(Expr* e) { return "&" + place(e); }

    std::string place_or_value(Expr* e) {
        if (e->kind == ExprKind::field) {
            std::string base = is_place(e->lhs) ? place(e->lhs) : "(" + expr(e->lhs) + ")";
            return base + member(e->lhs->type, e->field_index);
        }
        // index
        Type* at = e->lhs->type;
        if (at->kind == TypeKind::map) {
            // The map is found first, then the key. A key that isn't there panics.
            std::string b = tmp(), k = tmp(), i = tmp();
            return "(*({ const " + ctype(at) + "* " + b + " = " + address(e->lhs) + "; " + lookup_ctype(at) + " " + k + " = " +
                   lookup_value(e->rhs, at) + "; cx_usize " + i + " = " + table_fn("find", at) + "(" + b + ", " + k + ", " +
                   lookup_hash(at, k) + "); if (__builtin_expect(" + i + " == CX_NOT_FOUND, 0)) cx_panic_key(" +
                   location(e->rhs->span) + "); &" + b + "->slots[" + i + "].f1; }))";
        }
        if (at->kind == TypeKind::list) {
            std::string loc = location(e->rhs->span);
            if (is_place(e->lhs) && !place_has_effects(e->lhs) && !effects(e->rhs)) {
                std::string b = place(e->lhs);
                return b + ".ptr[cx_index(" + expr(e->rhs) + ", " + b + ".len, " + loc + ")]";
            }
            // The list is found first, then the index, and the list's length read after.
            std::string b = tmp(), i = tmp();
            return "(*({ const " + ctype(at) + "* " + b + " = " + address(e->lhs) + "; cx_usize " + i + " = " +
                   value(e->rhs, e->rhs->type) + "; &" + b + "->ptr[cx_index(" + i + ", " + b + "->len, " + loc + ")]; }))";
        }
        std::string base;
        std::string decls;
        if (is_place(e->lhs)) {
            base = place(e->lhs);
        } else if (effects(e->lhs) && effects(e->rhs)) {
            std::string t = tmp();
            decls = ctype(at) + " " + t + " = " + expr(e->lhs) + "; ";
            base = t;
        } else {
            base = "(" + expr(e->lhs) + ")";
        }
        std::string index;
        if (e->rhs->has_bits) {
            index = std::to_string(e->rhs->const_bits);  // checked when compiling
        } else {
            index = "cx_index(" + expr(e->rhs) + ", " + std::to_string(at->length) + "u, " + location(e->rhs->span) + ")";
        }
        if (!decls.empty()) return "({ " + decls + base + ".v[" + index + "]; })";
        return base + ".v[" + index + "]";
    }

    std::string unary(Expr* e) {
        Type* t = e->type;
        std::string x = expr(e->lhs);
        switch (e->op) {
            case Op::neg:
                if (is_float(t)) return "(-" + x + ")";
                return "((" + ctype(t) + ")(0u - (" + wide_unsigned(t) + ")" + x + "))";
            case Op::bit_not: return "((" + ctype(t) + ")~(" + wide_unsigned(t) + ")" + x + ")";
            case Op::log_not: return "(!" + x + ")";
            default: return x;
        }
    }

    std::string binary(Expr* e) {
        Op op = e->op;
        if (op == Op::log_and) return "(" + expr(e->lhs) + " && " + expr(e->rhs) + ")";
        if (op == Op::log_or) return "(" + expr(e->lhs) + " || " + expr(e->rhs) + ")";
        if (op == Op::coalesce) return coalesce(e);
        Type* lt = e->lhs->type;
        Type* rt = e->rhs->type;
        if ((op == Op::eq || op == Op::ne) && lt->kind == TypeKind::optional) return optional_equal(e);
        std::string decls;
        std::vector<std::string> v = sequence({{e->lhs, lt}, {e->rhs, rt}}, decls);
        if ((op == Op::eq || op == Op::ne) && (lt->kind == TypeKind::str || lt->kind == TypeKind::string || has_glue(lt))) {
            return wrap(decls, (op == Op::ne ? "(!" : "(") + equal(lt, v[0], v[1]) + ")");
        }
        return wrap(decls, operate(op, lt, rt, v[0], v[1], e->rhs, e->span));
    }

    // Whether `a` and `b`, of type `t`, are equal. For an optional they must be places.
    std::string equal(const Type* t, const std::string& a, const std::string& b) {
        switch (t->kind) {
            case TypeKind::str: return "cx_str_eq(" + a + ", " + b + ")";
            case TypeKind::string: return "cx_str_eq(cx_sv(" + a + "), cx_sv(" + b + "))";
            case TypeKind::optional:
                return "(" + a + ".has == " + b + ".has && (!" + a + ".has || " + equal(t->elem, a + ".v", b + ".v") + "))";
            case TypeKind::enum_:
                if (t->has_data) return eq_fn(t) + "(" + a + ", " + b + ")";
                return "(" + a + " == " + b + ")";
            case TypeKind::struct_: case TypeKind::tuple: case TypeKind::array: case TypeKind::list:
            case TypeKind::map: case TypeKind::set:
                return eq_fn(t) + "(" + a + ", " + b + ")";
            default: return "(" + a + " == " + b + ")";
        }
    }

    // `a op b` on already-evaluated operands.
    std::string operate(Op op, Type* lt, Type* rt, const std::string& a, const std::string& b, const Expr* rhs,
                        Span span) {
        std::string c = ctype(lt);
        switch (op) {
            case Op::eq: return "(" + a + " == " + b + ")";
            case Op::ne: return "(" + a + " != " + b + ")";
            case Op::lt: return "(" + a + " < " + b + ")";
            case Op::le: return "(" + a + " <= " + b + ")";
            case Op::gt: return "(" + a + " > " + b + ")";
            case Op::ge: return "(" + a + " >= " + b + ")";
            default: break;
        }
        if (is_float(lt)) {
            switch (op) {
                case Op::add: return "(" + a + " + " + b + ")";
                case Op::sub: return "(" + a + " - " + b + ")";
                case Op::mul: return "(" + a + " * " + b + ")";
                case Op::div: return "(" + a + " / " + b + ")";
                case Op::rem: return std::string(lt->bits == 32 ? "__builtin_fmodf(" : "__builtin_fmod(") + a + ", " + b + ")";
                default: return "0";
            }
        }
        std::string u = wide_unsigned(lt);
        switch (op) {
            case Op::add: return "((" + c + ")((" + u + ")" + a + " + (" + u + ")" + b + "))";
            case Op::sub: return "((" + c + ")((" + u + ")" + a + " - (" + u + ")" + b + "))";
            case Op::mul: return "((" + c + ")((" + u + ")" + a + " * (" + u + ")" + b + "))";
            case Op::div:
            case Op::rem: {
                std::string name = std::string(op == Op::div ? "cx_div_" : "cx_rem_") + helper(lt);
                if (rhs->has_bits) {
                    // A constant divisor isn't zero (the checker made sure). It can still be -1.
                    auto d = static_cast<std::int64_t>(rhs->const_bits);
                    if (!lt->is_signed || d != -1) return "((" + c + ")(" + a + (op == Op::div ? " / " : " % ") + b + "))";
                }
                return name + "(" + a + ", " + b + ", " + location(span) + ")";
            }
            case Op::bit_and: return "((" + c + ")(" + a + " & " + b + "))";
            case Op::bit_or: return "((" + c + ")(" + a + " | " + b + "))";
            case Op::bit_xor: return "((" + c + ")(" + a + " ^ " + b + "))";
            case Op::shl:
            case Op::shr:
            case Op::ushr: {
                std::string count = rhs->has_bits ? std::to_string(rhs->const_bits)
                                                  : "((cx_u32)" + b + " & " + (lt->bits == 64 ? "63u" : "31u") + ")";
                (void)rt;
                if (op == Op::shl) return "((" + c + ")((" + u + ")" + a + " << " + count + "))";
                if (op == Op::shr) return "((" + c + ")(" + a + " >> " + count + "))";
                return "((" + c + ")((" + u + ")" + a + " >> " + count + "))";
            }
            default: return "0";
        }
    }

    std::string call(Expr* e) {
        switch (e->call) {
            case CallKind::builtin: return builtin(e);
            case CallKind::convert: return conversion(e);
            case CallKind::construct: return construct(e->type);
            case CallKind::prim_method: return prim_method(e);
            case CallKind::prim_static: return prim_static(e);
            case CallKind::variant: return variant_call(e);
            case CallKind::some:
                return "((" + ctype(e->type) + "){ .v = " + value(e->args[0].value, e->type->elem) + ", .has = 1 })";
            case CallKind::try_from: return try_from(e);
            case CallKind::builtin_method: return builtin_method(e);
            case CallKind::fn:
            case CallKind::method: return fn_call(e);
            default: return "0";
        }
    }

    std::string fn_call(Expr* e) {
        Item* f = e->item;
        // Arguments run in the order they're written, which named arguments can make different
        // from the parameters' order.
        std::vector<Expr*> source;
        if (e->call == CallKind::method) source.push_back(e->callee->lhs);
        for (const Arg& a : e->args) source.push_back(a.value);
        std::vector<Operand> ops;
        std::vector<std::size_t> param_of;
        for (Expr* x : source) {
            std::size_t p = 0;
            for (std::size_t i = 0; i < e->ordered_args.size(); i++) {
                if (e->ordered_args[i] == x) p = i;
            }
            const Local* l = f->params[p].local;
            ops.push_back({x, l->type, l->is_view ? Pass::address : Pass::value});
            param_of.push_back(p);
        }
        std::string decls;
        std::vector<std::string> v = sequence(ops, decls);
        std::vector<std::string> by_param(f->params.size());
        for (std::size_t i = 0; i < v.size(); i++) by_param[param_of[i]] = v[i];
        if (uses_) uses_->add_fn(f);
        std::string s = std::string(f->c_name) + "(";
        for (std::size_t i = 0; i < by_param.size(); i++) {
            if (i) s += ", ";
            s += by_param[i];
        }
        return wrap(decls, s + ")");
    }

    std::string construct(Type* t) {
        if (t->kind == TypeKind::string || t->kind == TypeKind::list) return "((" + ctype(t) + "){0})";
        // An empty table points at the shared control bytes, so a lookup needs no check for it.
        if (is_table(t)) return "((" + ctype(t) + "){ .ctrl = (cx_u8*)cx_table_empty })";
        std::string s = "((" + ctype(t) + "){";
        bool first = true;
        for (const FieldDecl& f : t->item->fields) {
            s += first ? " ." : ", .";
            first = false;
            s += ident(f.name) + " = " + expr(f.default_value);
        }
        return s + " })";
    }

    std::string struct_literal(Expr* e) {
        Type* t = e->type;
        Item* s = t->item;
        // Field values run in the order they're written.
        std::vector<Operand> ops;
        std::vector<std::uint32_t> index;
        for (const FieldInit& f : e->fields) {
            for (std::uint32_t i = 0; i < s->fields.size(); i++) {
                if (s->fields[i].name == f.name && e->elems[i] == f.value) {
                    ops.push_back({f.value, s->fields[i].resolved});
                    index.push_back(i);
                }
            }
        }
        std::string decls;
        std::vector<std::string> v = sequence(ops, decls);
        std::vector<std::string> by_field(s->fields.size());
        for (std::size_t i = 0; i < v.size(); i++) by_field[index[i]] = v[i];
        std::string out = "((" + ctype(t) + "){";
        for (std::uint32_t i = 0; i < s->fields.size(); i++) {
            out += i ? ", ." : " .";
            out += ident(s->fields[i].name) + " = ";
            out += by_field[i].empty() ? expr(s->fields[i].default_value) : by_field[i];
        }
        return wrap(decls, out + " })");
    }

    // A variant of an enum with data, from its values in order.
    std::string variant_value(const Type* t, std::uint32_t i, const std::vector<std::string>& values) {
        std::string s = "((" + ctype(t) + "){ .tag = " + int_const(t->repr, t->item->variants[i].tag);
        if (!values.empty()) {
            s += ", .u.v" + std::to_string(i) + " = {";
            for (std::size_t k = 0; k < values.size(); k++) s += (k ? ", ." : " .") + variant_field(t, i, k) + " = " + values[k];
            s += " }";
        }
        return s + " })";
    }

    // `Shape.Circle(r)`
    std::string variant_call(Expr* e) {
        const VariantDecl& v = e->type->item->variants[e->field_index];
        std::vector<Operand> ops;
        for (std::size_t k = 0; k < e->ordered_args.size(); k++) ops.push_back({e->ordered_args[k], v.types[k]});
        std::string decls;
        std::vector<std::string> values = sequence(ops, decls);
        return wrap(decls, variant_value(e->type, e->field_index, values));
    }

    // `Shape.Rect { w: 1.0, h: 2.0 }`: the values run in the order they're written.
    std::string record_variant(Expr* e) {
        const Type* t = e->type;
        const VariantDecl& v = t->item->variants[e->field_index];
        std::vector<Operand> ops;
        std::vector<std::uint32_t> index;
        for (const FieldInit& f : e->fields) {
            for (std::uint32_t i = 0; i < v.fields.size(); i++) {
                if (v.fields[i].name == f.name && e->elems[i] == f.value) {
                    ops.push_back({f.value, v.fields[i].resolved});
                    index.push_back(i);
                }
            }
        }
        std::string decls;
        std::vector<std::string> values = sequence(ops, decls);
        std::vector<std::string> by_field(v.fields.size());
        for (std::size_t i = 0; i < values.size(); i++) by_field[index[i]] = values[i];
        return wrap(decls, variant_value(t, e->field_index, by_field));
    }

    std::string tuple_literal(Expr* e) {
        Type* t = e->type;
        std::vector<Operand> ops;
        for (std::size_t i = 0; i < e->elems.size(); i++) ops.push_back({e->elems[i], t->elems[i]});
        std::string decls;
        std::vector<std::string> v = sequence(ops, decls);
        std::string out = "((" + ctype(t) + "){";
        for (std::size_t i = 0; i < v.size(); i++) out += (i ? ", " : " ") + v[i];
        return wrap(decls, out + " })");
    }

    // `a!`: the value, or a panic for `none`.
    // The value in a result that's `ok`, as C after the statements in `s`.
    std::string ok_value(const Type* rt, const std::string& t) {
        if (rt->item->variants[0].types.empty()) return "";
        return t + ".u.v0." + variant_field(rt, 0, 0) + "; ";
    }

    // `try f()`: the call's value, or else its error returned from this function, after the
    // drops a `return` here would do.
    std::string try_value(Expr* e) {
        const Type* rt = e->lhs->type;
        Type* fr = fn_->result_type;
        std::string t = tmp();
        std::string call = expr(e->lhs);
        indent_++;
        std::string leave = capture([&] {
            std::string r = tmp();
            line(ctype(fr) + " " + r + " = " + variant_value(fr, 1, {t + ".u.v1." + variant_field(rt, 1, 0)}) + ";");
            exit_drops(0, e->exit_drops);
            line("return " + r + ";");
        });
        indent_--;
        return "({ " + ctype(rt) + " " + t + " = " + call + "; if (__builtin_expect(" + t + ".tag != " +
               int_const(rt->repr, 0) + ", 0)) {\n" + leave + std::string(static_cast<std::size_t>(indent_) * 4, ' ') +
               "} " + ok_value(rt, t) + "})";
    }

    std::string unwrap(Expr* e) {
        std::string t = tmp();
        if (e->lhs->type->is_result) {
            const Type* rt = e->lhs->type;
            return "({ " + ctype(rt) + " " + t + " = " + expr(e->lhs) + "; if (__builtin_expect(" + t + ".tag != " +
                   int_const(rt->repr, 0) + ", 0)) cx_panic_unwrap_err(" + location(e->span) + "); " + ok_value(rt, t) + "})";
        }
        return "({ " + ctype(e->lhs->type) + " " + t + " = " + expr(e->lhs) + "; if (__builtin_expect(!" + t +
               ".has, 0)) cx_panic_unwrap(" + location(e->span) + "); " + t + ".v; })";
    }

    // `a ?? b`: `b` runs only when `a` is `none`. When `b` is an optional too, so is the result.
    std::string coalesce(Expr* e) {
        Type* lt = e->lhs->type;
        bool flat = e->type == lt;
        std::string t = tmp();
        return "({ " + ctype(lt) + " " + t + " = " + expr(e->lhs) + "; " + t + ".has ? " + t + (flat ? "" : ".v") +
               " : " + value(e->rhs, e->type) + "; })";
    }

    // `a?.b`: `none`, or the field as an optional. A field that's an optional already isn't wrapped.
    std::string optional_field(Expr* e) {
        Type* lt = e->lhs->type;
        Type* ft = member_type(lt->elem, e->field_index);
        std::string o = ctype(e->type);
        std::string t = tmp();
        std::string field = t + ".v" + member(lt->elem, e->field_index);
        std::string some = ft == e->type ? field : "(" + o + "){ .v = " + field + ", .has = 1 }";
        return "({ " + ctype(lt) + " " + t + " = " + expr(e->lhs) + "; " + t + ".has ? " + some + " : (" + o + "){0}; })";
    }

    // `==` and `!=` on optionals: equal when both are `none`, or both hold equal values.
    std::string optional_equal(Expr* e) {
        bool ne = e->op == Op::ne;
        Expr* other = e->rhs->kind == ExprKind::none_lit ? e->lhs : e->lhs->kind == ExprKind::none_lit ? e->rhs : nullptr;
        if (other) return std::string(ne ? "(" : "(!") + "(" + expr(other) + ").has)";
        Type* t = e->lhs->type;
        std::string decls;
        std::vector<std::string> v = sequence({{e->lhs, t}, {e->rhs, t}}, decls);
        std::string a = tmp(), b = tmp();
        std::string eq = equal(t, a, b);
        return "({ " + decls + ctype(t) + " " + a + " = " + v[0] + "; " + ctype(t) + " " + b + " = " + v[1] + "; " +
               (ne ? "!(" + eq + ")" : eq) + "; })";
    }

    // An integral type's values: from -neg to pos. `usize` is 64 bits in C on every target so far.
    struct Bounds {
        std::uint64_t neg, pos;
    };
    static Bounds bounds(const Type* t) {
        if (t->kind == TypeKind::char_) return {0, 0xFFFF};
        if (t->kind == TypeKind::rune) return {0, 0x10FFFF};
        if (t->is_usize || (!t->is_signed && t->bits == 64)) return {0, UINT64_MAX};
        if (t->is_signed) return {std::uint64_t{1} << (t->bits - 1), (std::uint64_t{1} << (t->bits - 1)) - 1};
        return {0, (std::uint64_t{1} << t->bits) - 1};
    }

    // `T.try_from(x)`: `x` as a `T` when it's one of T's values, or `none`.
    std::string try_from(Expr* e) {
        Type* ot = e->type;
        Type* to = ot->elem;
        Expr* a = e->args[0].value;
        Type* from = a->type;
        std::string x = tmp();
        std::vector<std::string> tests;
        if (to->kind == TypeKind::enum_) {
            // The tags, as a range when they're consecutive.
            std::vector<std::int64_t> tags;
            bool is_signed = to->repr->is_signed;
            for (const VariantDecl& v : to->item->variants) tags.push_back(static_cast<std::int64_t>(v.tag));
            auto less = [&](std::int64_t p, std::int64_t q) {
                return is_signed ? p < q : static_cast<std::uint64_t>(p) < static_cast<std::uint64_t>(q);
            };
            std::sort(tags.begin(), tags.end(), less);
            bool consecutive = true;
            for (std::size_t i = 1; i < tags.size(); i++) {
                if (static_cast<std::uint64_t>(tags[i]) != static_cast<std::uint64_t>(tags[i - 1]) + 1) consecutive = false;
            }
            auto lo = static_cast<std::uint64_t>(tags.front()), hi = static_cast<std::uint64_t>(tags.back());
            if (consecutive) {
                if (is_signed || lo != 0) tests.push_back(x + " >= " + int_const(from, lo));
                tests.push_back(x + " <= " + int_const(from, hi));
            } else {
                std::string any;
                for (std::size_t i = 0; i < tags.size(); i++) {
                    any += (i ? " || " : "") + x + " == " + int_const(from, static_cast<std::uint64_t>(tags[i]));
                }
                tests.push_back("(" + any + ")");
            }
        } else {
            Bounds f = bounds(from), b = bounds(to);
            if (f.neg > b.neg) tests.push_back(x + " >= " + int_const(from, 0 - b.neg));
            if (f.pos > b.pos) tests.push_back(x + " <= " + int_const(from, b.pos));
            if (to->kind == TypeKind::rune && f.pos >= 0xD800) {
                tests.push_back("(" + x + " < " + int_const(from, 0xD800) + " || " + x + " > " + int_const(from, 0xDFFF) + ")");
            }
        }
        Match m;
        m.tests = tests;
        return "({ " + ctype(from) + " " + x + " = " + expr(a) + "; (" + condition(m) + ") ? (" + ctype(ot) + "){ .v = (" +
               ctype(to) + ")" + x + ", .has = 1 } : (" + ctype(ot) + "){0}; })";
    }

    std::string min_const(const Type* t) { return int_const(t, 0 - bounds(t).neg); }
    std::string max_const(const Type* t) { return int_const(t, bounds(t).pos); }

    // `a.checked_add(b)` and the like: `none` when the result doesn't fit.
    std::string checked(const char* op, const Type* t, const std::string& ot, const std::vector<std::string>& v) {
        std::string r = tmp();
        return "({ " + ctype(t) + " " + r + "; cx_bool " + r + "o = __builtin_" + op + "_overflow(" + v[0] + ", " + v[1] +
               ", &" + r + "); (" + ot + "){ .v = " + r + ", .has = !" + r + "o }; })";
    }

    // `checked_div` is `none` for a zero divisor and for MIN / -1. `checked_rem` is `none` only for
    // a zero divisor, since MIN % -1 is 0, as in Java.
    std::string checked_div(bool rem, const Type* t, const std::string& ot, const std::vector<std::string>& v) {
        std::string a = tmp(), b = tmp(), c = ctype(t);
        std::string head = "({ " + c + " " + a + " = " + v[0] + "; " + c + " " + b + " = " + v[1] + "; ";
        std::string none = "(" + ot + "){0}";
        std::string some = "(" + ot + "){ .v = (" + c + ")(" + a + (rem ? " % " : " / ") + b + "), .has = 1 }";
        if (!t->is_signed) return head + b + " == 0 ? " + none + " : " + some + "; })";
        std::string minus_one = int_const(t, UINT64_MAX);
        if (rem) {
            return head + b + " == 0 ? " + none + " : " + b + " == " + minus_one + " ? (" + ot + "){ .v = 0, .has = 1 } : " +
                   some + "; })";
        }
        return head + "(" + b + " == 0 || (" + a + " == " + min_const(t) + " && " + b + " == " + minus_one + ")) ? " +
               none + " : " + some + "; })";
    }

    // `a.saturating_add(b)` and the like: the nearest value of the type when the result doesn't fit.
    std::string saturating(const char* op, const Type* t, const std::vector<std::string>& v) {
        std::string a = tmp(), b = tmp(), r = tmp(), c = ctype(t);
        std::string head = "({ " + c + " " + a + " = " + v[0] + "; " + c + " " + b + " = " + v[1] + "; " + c + " " + r +
                           "; __builtin_" + op + "_overflow(" + a + ", " + b + ", &" + r + ") ? ";
        std::string sat;
        std::string_view o = op;
        if (!t->is_signed) {
            sat = o == "sub" ? int_const(t, 0) : max_const(t);
        } else if (o == "add") {
            sat = "(" + b + " < 0 ? " + min_const(t) + " : " + max_const(t) + ")";
        } else if (o == "sub") {
            sat = "(" + b + " < 0 ? " + max_const(t) + " : " + min_const(t) + ")";
        } else {
            sat = "((" + a + " < 0) != (" + b + " < 0) ? " + min_const(t) + " : " + max_const(t) + ")";
        }
        return head + sat + " : " + r + "; })";
    }

    std::string array_literal(Expr* e) {
        Type* t = e->type;
        if (e->repeat) {
            Expr* x = e->elems[0];
            if (x->has_bits && x->const_bits == 0) return "((" + ctype(t) + "){0})";
            std::string a = tmp(), v = tmp(), i = tmp();
            return "({ " + ctype(t) + " " + a + "; " + ctype(t->elem) + " " + v + " = " + value(x, t->elem) +
                   "; for (cx_usize " + i + " = 0; " + i + " < " + std::to_string(t->length) + "; " + i + "++) " + a +
                   ".v[" + i + "] = " + v + "; " + a + "; })";
        }
        if (e->elems.empty()) return "((" + ctype(t) + "){0})";
        std::vector<Operand> ops;
        for (Expr* x : e->elems) ops.push_back({x, t->elem});
        std::string decls;
        std::vector<std::string> v = sequence(ops, decls);
        std::string out = "((" + ctype(t) + "){{";
        for (std::size_t i = 0; i < v.size(); i++) out += (i ? ", " : " ") + v[i];
        return wrap(decls, out + " }})");
    }

    std::string block_value(Block* b, Type* t) {
        std::string body = capture([&] {
            indent_++;
            stmts(b, true);
            if (b->tail && b->drops.empty()) {
                line(value(b->tail, t) + ";");
            } else if (b->tail) {
                std::string r = tmp();
                line(ctype(t) + " " + r + " = " + value(b->tail, t) + ";");
                drops(b->drops);
                line(r + ";");
            } else {
                drops(b->drops);
            }
            indent_--;
        });
        return "({\n" + body + std::string(static_cast<std::size_t>(indent_) * 4, ' ') + "})";
    }

    std::string if_value(Expr* e) {
        Type* t = e->type;
        // A simple `if c { a } else { b }` is C's `?:`.
        Expr* el = e->else_;
        if (!e->let_pattern && e->block->stmts.empty() && e->block->tail && el->kind == ExprKind::block && el->block->stmts.empty() &&
            el->block->tail && e->block->tail->type->kind != TypeKind::never &&
            el->block->tail->type->kind != TypeKind::never) {
            return "(" + condition_expr(e->cond) + " ? " + value(e->block->tail, t) + " : " + value(el->block->tail, t) + ")";
        }
        std::string r = tmp();
        std::string body = capture([&] {
            indent_++;
            line(ctype(t) + " " + r + ";");
            branch_chain(e, t, r);
            line(r + ";");
            indent_--;
        });
        return "({\n" + body + std::string(static_cast<std::size_t>(indent_) * 4, ' ') + "})";
    }

    void branch_chain(Expr* e, Type* t, const std::string& r) {
        if (e->let_pattern) {
            line("{");
            indent_++;
            std::string at;
            Match m = let_match(e, at);
            line("if (" + condition(m) + ") {");
            indent_++;
            bind(m);
            if (e->let_consumes) take_apart(m, at, e->cond->type);
            indent_--;
            branch_body(e->block, t, r);
            indent_++;
            drops(e->let_drops);
            indent_--;
            line("} else {");
            if (e->else_->kind == ExprKind::if_) {
                indent_++;
                branch_chain(e->else_, t, r);
                indent_--;
            } else {
                branch_body(e->else_->block, t, r);
            }
            line("}");
            if (e->hidden && e->drop_hidden) line(drop_fn(e->hidden->type) + "(&" + local_name(e->hidden) + ");");
            indent_--;
            line("}");
            return;
        }
        line("if (" + condition_expr(e->cond) + ") {");
        branch_body(e->block, t, r);
        Expr* el = e->else_;
        if (el->kind == ExprKind::if_) {
            line("} else {");
            indent_++;
            branch_chain(el, t, r);
            indent_--;
            line("}");
            return;
        }
        line("} else {");
        branch_body(el->block, t, r);
        line("}");
    }

    void branch_body(Block* b, Type* t, const std::string& r) {
        indent_++;
        stmts(b, true);
        if (b->tail) {
            if (b->tail->type->kind == TypeKind::never) expr_stmt(b->tail);
            else line(r + " = " + value(b->tail, t) + ";");
        }
        drops(b->drops);
        indent_--;
    }

    std::string loop_value(Expr* e) {
        std::string id = std::to_string(e->loop_id);
        Type* t = e->type;
        std::string body = capture([&] {
            indent_++;
            if (t->kind != TypeKind::never && t->kind != TypeKind::unit) line(ctype(t) + " cx_lv" + id + ";");
            line("for (;;) {");
            indent_++;
            loop_frames_[e->loop_id] = frames_.size();
            stmts(e->block, false);
            line("cx_cont" + id + ":;");
            indent_--;
            line("}");
            line("cx_brk" + id + ":;");
            if (t->kind != TypeKind::never && t->kind != TypeKind::unit) line("cx_lv" + id + ";");
            indent_--;
        });
        return "({\n" + body + std::string(static_cast<std::size_t>(indent_) * 4, ' ') + "})";
    }

    std::string conversion(Expr* e) {
        Type* to = e->type;
        Expr* a = e->args[0].value;
        Type* from = a->type;
        if (to->kind == TypeKind::string) {
            // A literal costs nothing; any other `str` is copied, since it could be a view of
            // memory something else owns.
            if (a->kind == ExprKind::string_lit && a->interpolations.empty()) {
                return "cx_string_lit(" + c_string_literal(literal_text(a)) + ", " + std::to_string(literal_text(a).size()) + ")";
            }
            return "cx_string_from(" + expr(a) + ")";
        }
        std::string x = expr(a);
        switch (e->conv) {
            case Conv::exact:
            case Conv::nearest: return "((" + ctype(to) + ")" + x + ")";
            case Conv::wrap:
                // Through the unsigned type of the target's width, so the low bits are kept.
                if (to->is_signed) return "((" + ctype(to) + ")(" + unsigned_of(to) + ")" + x + ")";
                return "((" + ctype(to) + ")" + x + ")";
            case Conv::saturate: return "cx_" + helper(from) + "_to_" + helper(to) + "(" + x + ")";
            case Conv::none: break;
        }
        return x;
    }

    std::string prim_method(Expr* e) {
        Expr* recv = e->callee->lhs;
        Type* t = recv->type;
        std::vector<Operand> ops{{recv, t}};
        bool rotate = e->prim == PrimMethod::rotate_left || e->prim == PrimMethod::rotate_right;
        for (const Arg& a : e->args) ops.push_back({a.value, rotate ? a.value->type : t});
        std::string decls;
        std::vector<std::string> v = sequence(ops, decls);
        std::string h = helper(t);
        std::string r;
        switch (e->prim) {
            case PrimMethod::abs: r = "cx_abs_" + h + "(" + v[0] + ")"; break;
            case PrimMethod::min: r = "cx_min_" + h + "(" + v[0] + ", " + v[1] + ")"; break;
            case PrimMethod::max: r = "cx_max_" + h + "(" + v[0] + ", " + v[1] + ")"; break;
            case PrimMethod::is_nan: r = "(" + v[0] + " != " + v[0] + ")"; break;
            case PrimMethod::sqrt: r = std::string(t->bits == 32 ? "__builtin_sqrtf(" : "__builtin_sqrt(") + v[0] + ")"; break;
            case PrimMethod::floor: r = std::string(t->bits == 32 ? "__builtin_floorf(" : "__builtin_floor(") + v[0] + ")"; break;
            case PrimMethod::ceil: r = std::string(t->bits == 32 ? "__builtin_ceilf(" : "__builtin_ceil(") + v[0] + ")"; break;
            case PrimMethod::wrapping_add: r = operate(Op::add, t, t, v[0], v[1], e->args[0].value, e->span); break;
            case PrimMethod::wrapping_sub: r = operate(Op::sub, t, t, v[0], v[1], e->args[0].value, e->span); break;
            case PrimMethod::wrapping_mul: r = operate(Op::mul, t, t, v[0], v[1], e->args[0].value, e->span); break;
            case PrimMethod::floor_div: r = "cx_floor_div_" + h + "(" + v[0] + ", " + v[1] + ", " + location(e->span) + ")"; break;
            case PrimMethod::floor_mod: r = "cx_floor_mod_" + h + "(" + v[0] + ", " + v[1] + ", " + location(e->span) + ")"; break;
            case PrimMethod::checked_add: r = checked("add", t, ctype(e->type), v); break;
            case PrimMethod::checked_sub: r = checked("sub", t, ctype(e->type), v); break;
            case PrimMethod::checked_mul: r = checked("mul", t, ctype(e->type), v); break;
            case PrimMethod::checked_div: r = checked_div(false, t, ctype(e->type), v); break;
            case PrimMethod::checked_rem: r = checked_div(true, t, ctype(e->type), v); break;
            case PrimMethod::saturating_add: r = saturating("add", t, v); break;
            case PrimMethod::saturating_sub: r = saturating("sub", t, v); break;
            case PrimMethod::saturating_mul: r = saturating("mul", t, v); break;
            case PrimMethod::to_bits:
                r = t->bits == 32 ? "((union { cx_f32 f; cx_u32 u; }){ .f = " + v[0] + " }).u"
                                  : "((union { cx_f64 f; cx_u64 u; }){ .f = " + v[0] + " }).u";
                break;
            case PrimMethod::rotate_left:
            case PrimMethod::rotate_right: {
                // Java's rotations, which take the count modulo the width.
                std::string bits = std::to_string(t->bits);
                std::string fn = std::string(e->prim == PrimMethod::rotate_left ? "__builtin_rotateleft" : "__builtin_rotateright") + bits;
                r = "((" + ctype(t) + ")" + fn + "((" + unsigned_of(t) + ")" + v[0] + ", (cx_u32)" + v[1] + "))";
                break;
            }
            default: r = "0"; break;
        }
        return wrap(decls, r);
    }

    // `T.parse(text)`, `f64.from_bits(b)` and `String.from_utf8(bytes)`.
    std::string prim_static(Expr* e) {
        Type* t = e->named_type;
        Expr* a = e->args[0].value;
        std::string o = ctype(e->type), v = tmp(), r = tmp();
        switch (e->prim) {
            case PrimMethod::parse: {
                std::string text = a->type->kind == TypeKind::string ? "cx_sv(" + expr(a) + ")" : expr(a);
                std::string call;
                if (is_float(t)) {
                    call = std::string(t->bits == 32 ? "cx_parse_f32(" : "cx_parse_f64(") + text + ", &" + v + ")";
                    return "({ " + ctype(t) + " " + v + "; cx_bool " + r + " = " + call + "; (" + o + "){ .v = " + v +
                           ", .has = " + r + " }; })";
                }
                if (t->is_signed) {
                    std::string lo = t->bits == 64 ? "(-9223372036854775807LL - 1)"
                                                   : "(-" + std::to_string(1ull << (t->bits - 1)) + "LL)";
                    std::string hi = std::to_string((1ull << (t->bits - 1)) - 1) + "LL";
                    call = "cx_parse_i64(" + text + ", " + lo + ", " + hi + ", &" + v + ")";
                    return "({ cx_i64 " + v + "; cx_bool " + r + " = " + call + "; (" + o + "){ .v = (" + ctype(t) + ")" + v +
                           ", .has = " + r + " }; })";
                }
                std::string hi = t->is_usize ? "(cx_u64)(cx_usize)-1"
                                 : t->bits == 64 ? "18446744073709551615ULL"
                                                 : std::to_string((1ull << t->bits) - 1) + "ULL";
                call = "cx_parse_u64(" + text + ", " + hi + ", &" + v + ")";
                return "({ cx_u64 " + v + "; cx_bool " + r + " = " + call + "; (" + o + "){ .v = (" + ctype(t) + ")" + v +
                       ", .has = " + r + " }; })";
            }
            case PrimMethod::from_bits:
                return t->bits == 32 ? "((union { cx_u32 u; cx_f32 f; }){ .u = " + value(a, a->type) + " }).f"
                                     : "((union { cx_u64 u; cx_f64 f; }){ .u = " + value(a, a->type) + " }).f";
            case PrimMethod::from_utf8:
                // The string takes the list's memory, which has the same layout.
                return "({ " + ctype(a->type) + " " + v + " = " + value(a, a->type) + "; " + o + " " + r + " = {0}; if (cx_utf8_valid(" +
                       v + ".ptr, " + v + ".len)) { " + r + ".v.ptr = " + v + ".ptr; " + r + ".v.len = " + v + ".len; " + r +
                       ".v.cap = " + v + ".cap; " + r + ".has = 1; } else { cx_free(" + v + ".ptr, " + v + ".cap); } " + r + "; })";
            default: return "0";
        }
    }

    // The runtime calls that print a string literal's parts, or a `str` value.
    // As in Java, where the string is built before it's printed, the values in braces are worked
    // out first, left to right, into `decls`, and only then is anything printed.
    void print_parts(Expr* m, std::string& decls, std::string& out) {
        if (m->kind != ExprKind::string_lit) {
            out += show_value(m->type, expr(m)) + " ";
            return;
        }
        std::vector<Operand> ops;
        for (Expr* x : m->interpolations) ops.push_back({x, x->type});
        std::vector<std::string> values = sequence(ops, decls);
        // `sequence` leaves only the last operand with an effect in place; it runs first too.
        for (std::size_t i = ops.size(); i-- > 0;) {
            if (!effects(ops[i].e)) continue;
            std::string t = tmp();
            decls += ctype(ops[i].want) + " " + t + " = " + values[i] + "; ";
            values[i] = t;
            break;
        }
        std::size_t next = 0;
        for (const StringPart& p : m->literal.parts) {
            if (!p.is_expr) {
                if (!p.text.empty()) out += "cx_print_str(" + str_const(p.text) + "); ";
                continue;
            }
            Expr* x = m->interpolations[next];
            out += show_value(x->type, values[next++]) + " ";
        }
    }

    // A string literal with `{...}` parts, as a `String`: the values are worked out, then printed
    // into the string.
    std::string string_value(Expr* e) {
        std::string decls, out;
        print_parts(e, decls, out);
        std::string s = tmp(), prev = tmp();
        return "({ " + decls + "cx_String " + s + " = {0}; cx_String* " + prev + " = cx_fmt_begin(&" + s + "); " + out +
               "cx_fmt_end(" + prev + "); " + s + "; })";
    }

    // `[a, b]` and `[x; n]` as a list. The elements are worked out in order, straight into the
    // memory the list owns.
    std::string list_literal(Expr* e) {
        Type* t = e->type;
        std::string l = ctype(t), el = ctype(t->elem);
        if (e->repeat) {
            std::string v = tmp(), n = tmp(), r = tmp(), i = tmp();
            return "({ " + el + " " + v + " = " + value(e->elems[0], t->elem) + "; cx_usize " + n + " = " +
                   value(e->repeat, e->repeat->type) + "; " + l + " " + r + " = {0}; if (" + n + ") { " + r + ".ptr = (" + el +
                   "*)cx_grow(0, &" + r + ".cap, 0, " + n + ", sizeof(" + el + ")); for (cx_usize " + i + " = 0; " + i + " < " +
                   n + "; " + i + "++) " + r + ".ptr[" + i + "] = " + v + "; " + r + ".len = " + n + "; } " + r + "; })";
        }
        if (e->elems.empty()) return "((" + l + "){0})";
        std::string p = tmp(), n = std::to_string(e->elems.size());
        std::string s = "({ " + el + "* " + p + " = (" + el + "*)cx_alloc(" + n + " * sizeof(" + el + ")); ";
        for (std::size_t k = 0; k < e->elems.size(); k++) {
            s += p + "[" + std::to_string(k) + "] = " + value(e->elems[k], t->elem) + "; ";
        }
        return s + "(" + l + "){ " + p + ", " + n + ", " + n + " }; })";
    }

    // The methods of `String` and `List`, and `x.copy()`. Each finds the value it's called on,
    // then works out its arguments in order.
    std::string builtin_method(Expr* e) {
        Expr* recv = e->callee->lhs;
        Type* t = recv->type;
        auto arg = [&](std::size_t i, Type* want) { return value(e->args[i].value, want); };
        // A `str` or a `String` argument, as a `str`.
        auto text_arg = [&](std::size_t i) {
            Expr* a = e->args[i].value;
            return a->type->kind == TypeKind::string ? "cx_sv(" + expr(a) + ")" : expr(a);
        };
        if (e->bmethod == BMethod::copy) {
            if (is_copy(t)) return expr(recv);  // a generic instance's `T` that's `Copy`
            return copy_fn(t) + "(" + address(recv) + ")";
        }
        std::string p = tmp();
        if (t->kind == TypeKind::pointer) {
            std::string pt = ctype(t);
            switch (e->bmethod) {
                case BMethod::ptr_read: return "(*" + expr(recv) + ")";
                case BMethod::ptr_write: {
                    std::string v = tmp();
                    return "({ " + pt + " " + p + " = " + expr(recv) + "; " + ctype(t->elem) + " " + v + " = " + arg(0, t->elem) +
                           "; *" + p + " = " + v + "; })";
                }
                case BMethod::ptr_add: {
                    std::string n = tmp();
                    return "({ " + pt + " " + p + " = " + expr(recv) + "; cx_usize " + n + " = " + arg(0, e->args[0].value->type) +
                           "; " + p + " + " + n + "; })";
                }
                case BMethod::ptr_is_null: return "(" + expr(recv) + " == 0)";
                default: return "0";
            }
        }
        if (t->kind == TypeKind::string || t->kind == TypeKind::str) {
            bool owned = t->kind == TypeKind::string;
            std::string head = "({ cx_String* " + p + " = " + address(recv) + "; ";
            std::string view = "({ cx_str " + p + " = " + (owned ? "cx_sv(" + expr(recv) + ")" : expr(recv)) + "; ";
            switch (e->bmethod) {
                case BMethod::len: return "(" + expr(recv) + ").len";
                case BMethod::is_empty: return "((" + expr(recv) + ").len == 0)";
                case BMethod::as_ptr: return "((cx_u8 const*)(" + expr(recv) + ").ptr)";
                case BMethod::byte_at: {
                    std::string i = tmp();
                    return view + "cx_usize " + i + " = " + arg(0, e->args[0].value->type) + "; " + p + ".ptr[cx_index(" + i + ", " +
                           p + ".len, " + location(e->args[0].value->span) + ")]; })";
                }
                case BMethod::push: return head + "cx_string_push_rune(" + p + ", " + arg(0, e->args[0].value->type) + "); })";
                case BMethod::push_str: return head + "cx_string_push_str(" + p + ", " + text_arg(0) + "); })";
                case BMethod::clear: return "cx_string_clear(" + address(recv) + ")";
                case BMethod::reserve: return head + "cx_string_reserve(" + p + ", " + arg(0, e->args[0].value->type) + "); })";
                case BMethod::contains: return view + "cx_str_contains(" + p + ", " + text_arg(0) + "); })";
                case BMethod::starts_with: return view + "cx_str_starts_with(" + p + ", " + text_arg(0) + "); })";
                case BMethod::ends_with: return view + "cx_str_ends_with(" + p + ", " + text_arg(0) + "); })";
                default: return "0";
            }
        }
        if (is_table(t)) return table_method(e);
        Type* et = t->elem;
        std::string l = ctype(t), el = ctype(et);
        std::string head = "({ " + l + "* " + p + " = " + address(recv) + "; ";
        std::string grow = p + "->ptr = (" + el + "*)cx_grow(" + p + "->ptr, &" + p + "->cap, " + p + "->len, ";
        auto drop_from = [&](const std::string& from) {
            if (is_copy(et)) return std::string();
            std::string i = tmp();
            return "for (cx_usize " + i + " = " + from + "; " + i + " < " + p + "->len; " + i + "++) " + drop_fn(et) + "(&" + p +
                   "->ptr[" + i + "]); ";
        };
        std::string i = tmp(), j = tmp(), v = tmp();
        auto index_at = [&](std::size_t k) { return location(e->args[k].value->span); };
        switch (e->bmethod) {
            case BMethod::len: return "(" + expr(recv) + ").len";
            case BMethod::is_empty: return "((" + expr(recv) + ").len == 0)";
            case BMethod::capacity: return "(" + expr(recv) + ").cap";
            case BMethod::as_ptr: return "((" + el + " const*)(" + expr(recv) + ").ptr)";
            case BMethod::as_mut_ptr: return "(" + address(recv) + ")->ptr";
            case BMethod::set_len:
                return head + "cx_usize " + i + " = " + arg(0, e->args[0].value->type) + "; if (__builtin_expect(" + i + " > " + p +
                       "->cap, 0)) cx_panic_set_len(" + i + ", " + p + "->cap, " + index_at(0) + "); " + p + "->len = " + i + "; })";
            case BMethod::push:
                return head + el + " " + v + " = " + arg(0, et) + "; if (" + p + "->len == " + p + "->cap) " + grow + "1, sizeof(" +
                       el + ")); " + p + "->ptr[" + p + "->len++] = " + v + "; })";
            case BMethod::pop: {
                std::string o = ctype(e->type);
                return head + p + "->len ? (" + o + "){ .v = " + p + "->ptr[--" + p + "->len], .has = 1 } : (" + o + "){0}; })";
            }
            case BMethod::clear: return head + drop_from("0") + p + "->len = 0; })";
            case BMethod::truncate:
                return head + "cx_usize " + i + " = " + arg(0, e->args[0].value->type) + "; if (" + i + " < " + p + "->len) { " +
                       drop_from(i) + p + "->len = " + i + "; } })";
            case BMethod::reserve:
                return head + "cx_usize " + i + " = " + arg(0, e->args[0].value->type) + "; if (" + i + " > " + p + "->cap - " + p +
                       "->len) " + grow + i + ", sizeof(" + el + ")); })";
            case BMethod::insert:
                return head + "cx_usize " + i + " = " + arg(0, e->args[0].value->type) + "; " + el + " " + v + " = " + arg(1, et) +
                       "; if (__builtin_expect(" + i + " > " + p + "->len, 0)) cx_panic_insert(" + i + ", " + p + "->len, " +
                       index_at(0) + "); if (" + p + "->len == " + p + "->cap) " + grow + "1, sizeof(" + el +
                       ")); __builtin_memmove(" + p + "->ptr + " + i + " + 1, " + p + "->ptr + " + i + ", (" + p + "->len - " + i +
                       ") * sizeof(" + el + ")); " + p + "->ptr[" + i + "] = " + v + "; " + p + "->len++; })";
            case BMethod::remove:
                return head + "cx_usize " + i + " = cx_index(" + arg(0, e->args[0].value->type) + ", " + p + "->len, " +
                       index_at(0) + "); " + el + " " + v + " = " + p + "->ptr[" + i + "]; __builtin_memmove(" + p + "->ptr + " +
                       i + ", " + p + "->ptr + " + i + " + 1, (" + p + "->len - " + i + " - 1) * sizeof(" + el + ")); " + p +
                       "->len--; " + v + "; })";
            case BMethod::swap_remove:
                return head + "cx_usize " + i + " = cx_index(" + arg(0, e->args[0].value->type) + ", " + p + "->len, " +
                       index_at(0) + "); " + el + " " + v + " = " + p + "->ptr[" + i + "]; " + p + "->ptr[" + i + "] = " + p +
                       "->ptr[--" + p + "->len]; " + v + "; })";
            case BMethod::swap:
                // Both indexes are worked out before either is checked, as Java's `Collections.swap` does.
                return head + "cx_usize " + i + " = " + arg(0, e->args[0].value->type) + "; cx_usize " + j + " = " +
                       arg(1, e->args[1].value->type) + "; (void)cx_index(" + i + ", " + p + "->len, " + index_at(0) +
                       "); (void)cx_index(" + j + ", " + p + "->len, " + index_at(1) + "); " + el + " " + v + " = " + p +
                       "->ptr[" + i + "]; " + p + "->ptr[" + i + "] = " + p + "->ptr[" + j + "]; " + p + "->ptr[" + j + "] = " +
                       v + "; })";
            case BMethod::contains: {
                std::string r = tmp();
                return "({ const " + l + "* " + p + " = " + address(recv) + "; " + el + " " + v + " = " + arg(0, et) +
                       "; cx_bool " + r + " = 0; for (cx_usize " + i + " = 0; " + i + " < " + p + "->len; " + i + "++) if (" +
                       equal(et, p + "->ptr[" + i + "]", v) + ") { " + r + " = 1; break; } " + r + "; })";
            }
            default: return "0";
        }
    }

    std::string builtin(Expr* e) {
        std::string s, decls;
        switch (e->builtin) {
            case Builtin::print:
                print_parts(e->args[0].value, decls, s);
                return "({ " + decls + s + "})";
            case Builtin::println:
                if (!e->args.empty()) print_parts(e->args[0].value, decls, s);
                return "({ " + decls + s + "cx_print_newline(); })";
            case Builtin::eprint:
            case Builtin::eprintln:
                // The values are worked out before anything goes to standard error.
                if (!e->args.empty()) print_parts(e->args[0].value, decls, s);
                if (e->builtin == Builtin::eprintln) s += "cx_print_newline(); ";
                return "({ " + decls + "cx_err_begin(); " + s + "cx_err_end(); })";
            case Builtin::panic:
                print_parts(e->args[0].value, decls, s);
                return "({ " + decls + "cx_panic_begin(" + location(e->span) + "); " + s + "cx_panic_end(); })";
            case Builtin::assert_:
                // The message is worked out only when the assertion fails.
                s = "cx_print_str(" + str_const("assertion failed") + "); ";
                if (e->args.size() == 2) {
                    s += "cx_print_str(" + str_const(": ") + "); ";
                    print_parts(e->args[1].value, decls, s);
                }
                return "({ if (!" + expr(e->args[0].value) + ") { " + decls + "cx_panic_begin(" + location(e->span) + "); " +
                       s + "cx_panic_end(); } })";
            case Builtin::none: break;
        }
        return "0";
    }

    const Program& prog_;
    const Sources& sources_;
    const Names& names_;
    EmitOptions opt_;
    std::string out_;
    int indent_ = 0;
    std::uint32_t tmp_ = 0;
    Item* fn_ = nullptr;
    std::unordered_map<std::uint32_t, std::string> abs_paths_;
    std::unordered_map<const Item*, FnOut> fns_;
    std::unordered_map<const Item*, ConstOut> consts_;
    Uses* uses_ = nullptr;  // where ctype and the rest note what's named, while emitting one function
    std::unordered_set<const Type*> printed_;  // the enums whose names the file being written prints
    std::unordered_set<const Type*> equated_;  // the types it compares with a function of their own
    std::unordered_set<const Type*> shown_;    // the types it prints with a function of their own
    std::unordered_set<const Type*> hashed_;   // the types its maps and sets hash with a function of their own
    mutable std::unordered_map<const Type*, std::string> compound_names_;
    mutable std::unordered_map<const Local*, std::string> hidden_names_;
    static constexpr std::uint32_t kNoFile = 0xFFFFFFFFu;
    std::string helpers_;  // the function's `parallel for` bodies so far
    std::unordered_set<const Local*> par_pointers_;  // locals a `parallel for` body reaches through a pointer
    std::uint32_t line_file_ = kNoFile;  // the file the function's `#line` directives are in
    bool line_files_mixed_ = false;
    std::vector<Frame> frames_;                                 // the statements being emitted, innermost last
    std::unordered_map<std::uint32_t, std::size_t> loop_frames_;  // a loop's id: how many frames are outside it
    mutable std::unordered_set<std::string> compound_used_;
};

}  // namespace

std::vector<CFile> emit_c(const Program& program, const Sources& sources, const Names& names, const EmitOptions& options) {
    Emitter e(program, sources, names, options);
    return e.run();
}

}  // namespace calx
