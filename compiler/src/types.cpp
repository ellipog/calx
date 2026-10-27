#include "types.h"

#include "ast.h"

namespace calx {

Types::Types(Arena& arena) : arena_(arena) {
    error = make(TypeKind::error, "{error}");
    unit = make(TypeKind::unit, "()");
    never = make(TypeKind::never, "never");
    bool_ = make(TypeKind::bool_, "bool");
    i8 = make_int(8, true, "i8");
    i16 = make_int(16, true, "i16");
    i32 = make_int(32, true, "i32");
    i64 = make_int(64, true, "i64");
    u8 = make_int(8, false, "u8");
    u16 = make_int(16, false, "u16");
    u32 = make_int(32, false, "u32");
    u64 = make_int(64, false, "u64");
    usize = make_int(64, false, "usize");
    usize->is_usize = true;
    char_ = make(TypeKind::char_, "char");
    char_->bits = 16;
    rune = make(TypeKind::rune, "rune");
    rune->bits = 32;
    f32 = make(TypeKind::float_, "f32");
    f32->bits = 32;
    f64 = make(TypeKind::float_, "f64");
    f64->bits = 64;
    str = make(TypeKind::str, "str");
    string = make(TypeKind::string, "String");
    for (Type* t : {bool_, i8, i16, i32, i64, u8, u16, u32, u64, usize, char_, rune, f32, f64, str, string}) {
        primitives_.push_back({t->name, t});
    }
}

Type* Types::make(TypeKind kind, std::string_view name) {
    Type* t = arena_.make<Type>();
    t->kind = kind;
    t->name = name;
    t->id = next_id_++;
    return t;
}

Type* Types::make_int(std::uint8_t bits, bool is_signed, std::string_view name) {
    Type* t = make(TypeKind::int_, name);
    t->bits = bits;
    t->is_signed = is_signed;
    return t;
}

Type* Types::primitive(std::string_view name) const {
    for (const auto& [n, t] : primitives_) {
        if (n == name) return t;
    }
    return nullptr;
}

Type* Types::make_struct(Item* item, std::string_view name) {
    Type* t = make(TypeKind::struct_, name);
    t->item = item;
    return t;
}

Type* Types::array_of(Type* elem, std::uint64_t length) {
    auto key = std::make_pair(elem, length);
    auto it = array_map_.find(key);
    if (it != array_map_.end()) return it->second;
    std::string name = "[" + std::string(elem->name) + "; " + std::to_string(length) + "]";
    Type* t = make(TypeKind::array, arena_.copy(name));
    t->elem = elem;
    t->length = length;
    t->open = elem->open;
    t->has_pointer = elem->has_pointer;
    array_map_.emplace(key, t);
    compounds_.push_back(t);
    return t;
}

Type* Types::make_enum(Item* item, std::string_view name) {
    Type* t = make(TypeKind::enum_, name);
    t->item = item;
    return t;
}

Type* Types::optional_of(Type* elem) {
    auto it = optional_map_.find(elem);
    if (it != optional_map_.end()) return it->second;
    Type* t = make(TypeKind::optional, arena_.copy(std::string(elem->name) + "?"));
    t->elem = elem;
    t->open = elem->open;
    t->has_pointer = elem->has_pointer;
    optional_map_.emplace(elem, t);
    compounds_.push_back(t);
    return t;
}

Type* Types::list_of(Type* elem) {
    auto it = list_map_.find(elem);
    if (it != list_map_.end()) return it->second;
    Type* t = make(TypeKind::list, arena_.copy("List<" + std::string(elem->name) + ">"));
    t->elem = elem;
    t->open = elem->open;
    t->has_pointer = elem->has_pointer;
    list_map_.emplace(elem, t);
    compounds_.push_back(t);
    return t;
}

Type* Types::tuple_of(const std::vector<Type*>& elems) {
    auto it = tuple_map_.find(elems);
    if (it != tuple_map_.end()) return it->second;
    std::string name = "(";
    for (std::size_t i = 0; i < elems.size(); i++) {
        if (i) name += ", ";
        name += elems[i]->name;
    }
    name += ")";
    Type* t = make(TypeKind::tuple, arena_.copy(name));
    t->elems = arena_.copy(elems);
    for (Type* e : elems) {
        t->open = t->open || e->open;
        t->has_pointer = t->has_pointer || e->has_pointer;
    }
    tuple_map_.emplace(elems, t);
    compounds_.push_back(t);
    return t;
}

Type* Types::make_param(std::string_view name) {
    Type* t = make(TypeKind::param, name);
    t->open = true;
    return t;
}

Type* Types::pointer_of(Type* elem, bool is_const) {
    auto key = std::make_pair(elem, is_const);
    auto it = pointer_map_.find(key);
    if (it != pointer_map_.end()) return it->second;
    Type* t = make(TypeKind::pointer, arena_.copy((is_const ? "*const " : "*") + std::string(elem->name)));
    t->elem = elem;
    t->is_const = is_const;
    t->open = elem->open;
    t->has_pointer = true;
    pointer_map_.emplace(key, t);
    return t;
}

namespace {

// A struct that holds itself is an error found later; the depth stops the walk before then.
bool copy_walk(const Type* t, int depth) {
    if (depth > 64) return true;
    switch (t->kind) {
        case TypeKind::string: case TypeKind::list: return false;
        case TypeKind::param: return t->copy_bound;
        case TypeKind::array: case TypeKind::optional: return copy_walk(t->elem, depth + 1);
        case TypeKind::tuple:
            for (const Type* e : t->elems) {
                if (!copy_walk(e, depth + 1)) return false;
            }
            return true;
        case TypeKind::struct_:
            for (const FieldDecl& f : t->item->fields) {
                if (f.resolved && !copy_walk(f.resolved, depth + 1)) return false;
            }
            return true;
        case TypeKind::enum_:
            for (const VariantDecl& v : t->item->variants) {
                for (const Type* x : v.types) {
                    if (!copy_walk(x, depth + 1)) return false;
                }
            }
            return true;
        default: return true;
    }
}

}  // namespace

bool is_copy(const Type* t) { return copy_walk(t, 0); }

}  // namespace calx
