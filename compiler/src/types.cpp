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
    this->arena = make(TypeKind::arena, "Arena");
    thread = make(TypeKind::thread, "Thread");
    library = make(TypeKind::library, "Library");
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
    t->scoped = elem->scoped;
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
    // `(fn() -> i32)?`, since `fn() -> i32?` gives back an optional.
    std::string name = elem->kind == TypeKind::fn_ ? "(" + std::string(elem->name) + ")?" : std::string(elem->name) + "?";
    Type* t = make(TypeKind::optional, arena_.copy(name));
    t->elem = elem;
    t->open = elem->open;
    t->has_pointer = elem->has_pointer;
    t->scoped = elem->scoped;
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
    t->scoped = elem->scoped;
    list_map_.emplace(elem, t);
    compounds_.push_back(t);
    return t;
}

Type* Types::heap_of(Type* elem) {
    auto it = heap_map_.find(elem);
    if (it != heap_map_.end()) return it->second;
    Type* t = make(TypeKind::list, arena_.copy("PriorityQueue<" + std::string(elem->name) + ">"));
    t->heap = true;
    t->elem = elem;
    t->open = elem->open;
    t->has_pointer = elem->has_pointer;
    t->scoped = elem->scoped;
    heap_map_.emplace(elem, t);
    compounds_.push_back(t);
    return t;
}

Type* Types::arena_list_of(Type* elem) {
    auto it = arena_list_map_.find(elem);
    if (it != arena_list_map_.end()) return it->second;
    Type* t = make(TypeKind::list, arena_.copy("ArenaList<" + std::string(elem->name) + ">"));
    t->arena_list = true;
    t->scoped = true;
    t->elem = elem;
    t->open = elem->open;
    t->has_pointer = elem->has_pointer;
    arena_list_map_.emplace(elem, t);
    compounds_.push_back(t);
    return t;
}

Type* Types::deque_of(Type* elem) {
    auto it = deque_map_.find(elem);
    if (it != deque_map_.end()) return it->second;
    Type* t = make(TypeKind::deque, arena_.copy("Deque<" + std::string(elem->name) + ">"));
    t->elem = elem;
    t->open = elem->open;
    t->has_pointer = elem->has_pointer;
    t->scoped = elem->scoped;
    deque_map_.emplace(elem, t);
    compounds_.push_back(t);
    return t;
}

// A queue's ends are handles to the runtime's queue, the same C type whatever the messages are,
// so they aren't compounds with glue of their own.
Type* Types::sender_of(Type* elem) {
    auto it = sender_map_.find(elem);
    if (it != sender_map_.end()) return it->second;
    Type* t = make(TypeKind::sender, arena_.copy("Sender<" + std::string(elem->name) + ">"));
    t->elem = elem;
    t->open = elem->open;
    sender_map_.emplace(elem, t);
    return t;
}

Type* Types::receiver_of(Type* elem) {
    auto it = receiver_map_.find(elem);
    if (it != receiver_map_.end()) return it->second;
    Type* t = make(TypeKind::receiver, arena_.copy("Receiver<" + std::string(elem->name) + ">"));
    t->elem = elem;
    t->open = elem->open;
    receiver_map_.emplace(elem, t);
    return t;
}

Type* Types::map_of(Type* key, Type* value, TableKind table) {
    auto k = std::make_tuple(key, value, table);
    auto it = map_map_.find(k);
    if (it != map_map_.end()) return it->second;
    Type* entry = tuple_of({key, value});
    const char* name = table == TableKind::java       ? "JavaHashMap<"
                       : table == TableKind::fastutil ? "OpenHashMap<"
                       : table == TableKind::linked   ? "LinkedMap<"
                       : table == TableKind::sorted   ? "SortedMap<"
                                                      : "Map<";
    Type* t = make(TypeKind::map, arena_.copy(std::string(name) + std::string(key->name) + ", " + std::string(value->name) + ">"));
    t->table = table;
    t->key = key;
    t->elem = value;
    t->entry = entry;
    t->open = key->open || value->open;
    t->has_pointer = key->has_pointer || value->has_pointer;
    t->scoped = key->scoped || value->scoped;
    map_map_.emplace(k, t);
    compounds_.push_back(t);
    return t;
}

Type* Types::set_of(Type* elem, TableKind table) {
    auto k = std::make_pair(elem, table);
    auto it = set_map_.find(k);
    if (it != set_map_.end()) return it->second;
    const char* name = table == TableKind::java       ? "JavaHashSet<"
                       : table == TableKind::fastutil ? "OpenHashSet<"
                       : table == TableKind::linked   ? "LinkedSet<"
                       : table == TableKind::sorted   ? "SortedSet<"
                                                      : "Set<";
    Type* t = make(TypeKind::set, arena_.copy(std::string(name) + std::string(elem->name) + ">"));
    t->table = table;
    t->elem = elem;
    t->open = elem->open;
    t->has_pointer = elem->has_pointer;
    t->scoped = elem->scoped;
    set_map_.emplace(k, t);
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
        t->scoped = t->scoped || e->scoped;
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

// A function value is the function's address, whatever it takes, so only `open` comes from its
// parts: it holds no pointer, nothing scoped and nothing to free.
Type* Types::fn_of(const std::vector<Type*>& params, const std::vector<std::uint8_t>& modes, Type* result, bool is_unsafe) {
    auto key = std::make_tuple(params, modes, result, is_unsafe);
    auto it = fn_map_.find(key);
    if (it != fn_map_.end()) return it->second;
    std::string name = is_unsafe ? "unsafe fn(" : "fn(";
    for (std::size_t i = 0; i < params.size(); i++) {
        if (i) name += ", ";
        if (modes[i] == 1) name += "mut ";
        if (modes[i] == 2) name += "own ";
        name += params[i]->name;
    }
    name += ")";
    if (result->is_result && result->args.size() == 2 && result->args[0]->kind == TypeKind::unit) {
        name += " ! " + std::string(result->args[1]->name);  // `fn() ! E`, as a function is declared
    } else if (result->kind != TypeKind::unit) {
        name += " -> " + std::string(result->name);
    }
    Type* t = make(TypeKind::fn_, arena_.copy(name));
    t->elems = arena_.copy(params);
    t->modes = arena_.copy(modes);
    t->elem = result;
    t->unsafe_fn = is_unsafe;
    t->open = result->open;
    for (Type* p : params) t->open = t->open || p->open;
    fn_map_.emplace(std::move(key), t);
    fns_.push_back(t);
    return t;
}

bool is_view_type(const Type* t) {
    switch (t->kind) {
        case TypeKind::struct_: case TypeKind::array: case TypeKind::tuple: case TypeKind::string:
        case TypeKind::list: case TypeKind::deque: case TypeKind::map: case TypeKind::set: case TypeKind::param:
        case TypeKind::arena: case TypeKind::sender: case TypeKind::receiver: case TypeKind::thread:
        case TypeKind::library:
            return true;
        case TypeKind::enum_: return t->has_data;
        case TypeKind::optional: return is_view_type(t->elem);
        default: return false;
    }
}

namespace {

// The structs and enums a walk over a type has gone into. Each is gone into once: the walks
// stop at the first type that answers the question, so one seen again either answered nothing
// or is being gone into, as a type that holds itself through a list is. Without it a walk over
// types that hold each other takes a path for each way down, which for a JSON tree and a text
// component took minutes.
// The first few are kept in place, since most walks see few and `is_copy` is asked often.
struct Seen {
    const void* first[16];
    int n = 0;
    std::vector<const void*> rest;

    // False if `t` was gone into already.
    bool enter(const Type* t) {
        const void* item = t->item;
        for (int i = 0; i < n; i++) {
            if (first[i] == item) return false;
        }
        for (const void* p : rest) {
            if (p == item) return false;
        }
        if (n < 16) first[n++] = item;
        else rest.push_back(item);
        return true;
    }
};

// A struct that holds itself is an error found later; the depth stops the walk before then.
bool copy_walk(const Type* t, int depth, Seen& seen) {
    if (depth > 64) return true;
    if ((t->kind == TypeKind::struct_ || t->kind == TypeKind::enum_) && !seen.enter(t)) return true;
    switch (t->kind) {
        case TypeKind::string: case TypeKind::list: case TypeKind::deque: case TypeKind::map: case TypeKind::set:
        case TypeKind::arena: case TypeKind::sender: case TypeKind::receiver: case TypeKind::thread:
        case TypeKind::library: return false;
        case TypeKind::param: return t->copy_bound;
        case TypeKind::array: case TypeKind::optional: return copy_walk(t->elem, depth + 1, seen);
        case TypeKind::tuple:
            for (const Type* e : t->elems) {
                if (!copy_walk(e, depth + 1, seen)) return false;
            }
            return true;
        case TypeKind::struct_:
            for (const FieldDecl& f : t->item->fields) {
                if (f.resolved && !copy_walk(f.resolved, depth + 1, seen)) return false;
            }
            return true;
        case TypeKind::enum_:
            for (const VariantDecl& v : t->item->variants) {
                for (const Type* x : v.types) {
                    if (!copy_walk(x, depth + 1, seen)) return false;
                }
            }
            return true;
        default: return true;
    }
}

bool send_walk(const Type* t, int depth, Seen& seen) {
    if (depth > 64) return true;
    if ((t->kind == TypeKind::struct_ || t->kind == TypeKind::enum_) && !seen.enter(t)) return true;
    if (t->has_pointer || t->scoped) return false;
    switch (t->kind) {
        case TypeKind::pointer: return false;
        case TypeKind::param: return t->send_bound;
        case TypeKind::array: case TypeKind::optional: case TypeKind::list: case TypeKind::deque: case TypeKind::set:
            return send_walk(t->elem, depth + 1, seen);
        case TypeKind::map: return send_walk(t->key, depth + 1, seen) && send_walk(t->elem, depth + 1, seen);
        case TypeKind::tuple:
            for (const Type* e : t->elems) {
                if (!send_walk(e, depth + 1, seen)) return false;
            }
            return true;
        case TypeKind::struct_:
            for (const FieldDecl& f : t->item->fields) {
                if (f.resolved && !send_walk(f.resolved, depth + 1, seen)) return false;
            }
            return true;
        case TypeKind::enum_:
            for (const VariantDecl& v : t->item->variants) {
                for (const Type* x : v.types) {
                    if (!send_walk(x, depth + 1, seen)) return false;
                }
            }
            return true;
        default: return true;
    }
}

}  // namespace

bool is_copy(const Type* t) {
    Seen seen;
    return copy_walk(t, 0, seen);
}

bool is_send(const Type* t) {
    Seen seen;
    return send_walk(t, 0, seen);
}

}  // namespace calx
