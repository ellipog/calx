// Checked types. Every type is interned, so two types are the same exactly when their pointers are.
#pragma once

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "common.h"

namespace calx {

struct Item;

enum class TypeKind : std::uint8_t {
    error,     // after an error, so one mistake isn't reported again everywhere it flows
    unit,      // `()`, what a function with no `->` returns
    never,     // what `panic(...)` and a `loop` without `break` give: it fits any type
    bool_,
    int_,      // i8 to u64, and usize
    char_,     // a UTF-16 code unit, Java's `char`
    rune,      // a Unicode scalar value
    float_,    // f32, f64
    str,       // a read-only view of UTF-8 bytes
    string,    // `String`: UTF-8 bytes it owns
    list,      // `List<T>`: a growable array that owns its elements
    deque,     // `Deque<T>`: a growable ring of elements, which it owns, taken from and added at both ends
    map,       // `Map<K, V>` or `JavaHashMap<K, V>`: a hash table of keys to values, which it owns
    set,       // `Set<T>` or `JavaHashSet<T>`: a hash table of values, which it owns
    struct_,
    array,     // `[T; N]`
    enum_,
    optional,  // `T?`
    tuple,     // `(A, B)`, with at least two elements
    param,     // a generic function's or type's parameter, such as `T` in `fn f<T: Shape>`
    pointer,   // `*T` and `*const T`, only in `unsafe` code and `extern` signatures
    arena,     // `Arena`: a bump allocator that frees everything at once (D114)
    // A queue between threads (D121): `Sender<T>` adds messages and `Receiver<T>` takes them, and
    // the queue lasts while either has an end of it. `Thread` is a thread `Thread.spawn` started.
    sender,
    receiver,
    thread,
    library,   // `Library`: a library the program opened while it runs (D123)
    // `fn(A, mut B) -> R`: a plain function as a value (D122). `elems` are its parameters' types
    // and `modes` their modes, and `elem` is what it gives back, a result when it can fail.
    fn_,
};

// Which hash table a map or a set is: Calx's own, Java's `HashMap` (D105), fastutil's open hash
// table (D107), or Calx's own with a list through its keys in the order they went in (D109).
// Each goes in its own order.
enum class TableKind : std::uint8_t { native, java, fastutil, linked, sorted };

struct Type {
    TypeKind kind;
    std::uint8_t bits = 0;   // int_ and float_; usize says 64, and is_usize marks it
    bool is_signed = false;
    bool is_usize = false;
    Type* elem = nullptr;    // array, optional, list, set; map: the values
    Type* key = nullptr;     // map: the keys
    Type* entry = nullptr;   // map: `(K, V)`, what the table holds and a `for` loop goes over
    std::uint64_t length = 0;
    Slice<Type*> elems;      // tuple; fn_: the parameters
    Slice<std::uint8_t> modes; // fn_: each parameter's mode, a ParamMode
    bool unsafe_fn = false;  // fn_: `unsafe fn`, which only `unsafe` code can call
    Item* item = nullptr;    // struct_, enum_
    Type* repr = nullptr;    // enum_: the integer type of its tag
    bool has_data = false;   // enum_: some variant holds data, so it's a tagged union in C
    // Generics. A use of a generic struct or enum, such as `Pair<i32, str>`, is its own type, with
    // its own copy of the item whose field types have the arguments in place of the parameters.
    Item* generic = nullptr; // struct_, enum_: the generic item this is an instance of
    Slice<Type*> args;       // struct_, enum_: the instance's arguments
    Slice<Item*> bounds;     // param: the interfaces it must implement
    bool copy_bound = false; // param: it has the bound `Copy`
    bool equal_bound = false;   // param: it has the bound `Equal`, so `==` works on it
    bool display_bound = false; // param: it has the bound `Display`, so it can be shown in a string
    bool hash_bound = false;    // param: it has the bound `Hash`, so it can be a key, and `==` works on it
    bool order_bound = false;   // param: it has the bound `Order`, so `<` and `cmp` work on it, and `==`
    bool open = false;       // it mentions a parameter, so it's only checked, never emitted
    bool is_const = false;   // pointer: `*const T`, which can't be written through
    bool has_pointer = false; // it is or holds a pointer
    bool is_result = false;  // enum_: `T ! E`, what a call to a function that can fail gives back
    TableKind table = TableKind::native;  // map, set: which table it is
    // list: a `PriorityQueue<T>`, a binary heap in Java's order (D113). It's a list in memory, so
    // it's freed, copied, compared, shown and gone over as one, but it's its own type, and only
    // the heap's methods change it.
    bool heap = false;
    // list: a `Box<T>`, which owns one value on the heap, so a type can hold itself through it.
    // It's a list of that one value in memory, so it's freed, copied, compared and hashed as one,
    // but it's its own type: `b.value` is the value, and it has no list methods.
    bool boxed = false;
    bool java_hash_bound = false; // param: it has the bound `JavaHash`, so it has Java's `hashCode`
    bool send_bound = false;      // param: it has the bound `Send`, so it can go in a message (D115)
    // list: an `ArenaList<T>`, whose memory comes from an `Arena` (D114). It's a list in memory,
    // with the arena beside it, so it's gone over, indexed, compared and shown as one, but it
    // grows from its arena, frees nothing itself, and can't move or be stored.
    bool arena_list = false;
    bool scoped = false;     // it is or holds an `ArenaList`, so it's only ever a variable or parameter of its own
    std::uint32_t id = 0;    // a number unique to the type, for naming it in C
    std::string_view name;   // what messages call it
};

class Types {
public:
    explicit Types(Arena& arena);

    Type* error;
    Type* unit;
    Type* never;
    Type* bool_;
    Type* i8, *i16, *i32, *i64, *u8, *u16, *u32, *u64, *usize;
    Type* char_;
    Type* rune;
    Type* f32, *f64;
    Type* str;
    Type* string;
    Type* arena;
    Type* thread;
    Type* library;

    // The primitive named `name`, or null.
    Type* primitive(std::string_view name) const;
    Type* make_struct(Item* item, std::string_view name);
    Type* make_enum(Item* item, std::string_view name);
    Type* array_of(Type* elem, std::uint64_t length);
    Type* optional_of(Type* elem);
    Type* list_of(Type* elem);
    Type* deque_of(Type* elem);
    Type* heap_of(Type* elem);
    Type* box_of(Type* elem);
    Type* arena_list_of(Type* elem);
    Type* sender_of(Type* elem);
    Type* receiver_of(Type* elem);
    // `JavaHashMap<K, V>` and `OpenHashMap<K, V>`, and their sets, with their kinds (D105, D107).
    Type* map_of(Type* key, Type* value, TableKind table = TableKind::native);
    Type* set_of(Type* elem, TableKind table = TableKind::native);
    Type* tuple_of(const std::vector<Type*>& elems);
    Type* make_param(std::string_view name);
    Type* pointer_of(Type* elem, bool is_const);
    Type* fn_of(const std::vector<Type*>& params, const std::vector<std::uint8_t>& modes, Type* result, bool is_unsafe);
    // The function types made so far, in the order they were made.
    const std::vector<Type*>& fn_types() const { return fns_; }
    // The arrays, optionals, tuples, lists, maps and sets made so far, in the order they were made.
    const std::vector<Type*>& compounds() const { return compounds_; }

private:
    Type* make(TypeKind kind, std::string_view name);
    Type* make_int(std::uint8_t bits, bool is_signed, std::string_view name);

    Arena& arena_;
    std::uint32_t next_id_ = 1;
    std::vector<std::pair<std::string_view, Type*>> primitives_;
    std::map<std::pair<Type*, std::uint64_t>, Type*> array_map_;
    std::map<Type*, Type*> optional_map_;
    std::map<Type*, Type*> list_map_;
    std::map<Type*, Type*> deque_map_;
    std::map<Type*, Type*> heap_map_;
    std::map<Type*, Type*> box_map_;
    std::map<Type*, Type*> arena_list_map_;
    std::map<Type*, Type*> sender_map_;
    std::map<Type*, Type*> receiver_map_;
    std::map<std::tuple<Type*, Type*, TableKind>, Type*> map_map_;
    std::map<std::pair<Type*, TableKind>, Type*> set_map_;
    std::map<std::vector<Type*>, Type*> tuple_map_;
    std::map<std::pair<Type*, bool>, Type*> pointer_map_;
    std::map<std::tuple<std::vector<Type*>, std::vector<std::uint8_t>, Type*, bool>, Type*> fn_map_;
    std::vector<Type*> compounds_;
    std::vector<Type*> fns_;
};

inline bool is_int(const Type* t) { return t->kind == TypeKind::int_; }
inline bool is_float(const Type* t) { return t->kind == TypeKind::float_; }
inline bool is_numeric(const Type* t) { return is_int(t) || is_float(t); }
// A type where a value is exactly some range of integers, for conversions: the integers, `char`
// and `rune`.
inline bool is_integral(const Type* t) {
    return t->kind == TypeKind::int_ || t->kind == TypeKind::char_ || t->kind == TypeKind::rune;
}
// Bad types never cause a second error.
inline bool is_bad(const Type* t) { return t->kind == TypeKind::error; }

// Whether the type owns no heap memory, so copying it is just copying its bytes (the derived
// property `Copy`). A value of any other type moves when it's assigned or passed to `own`, and
// is dropped at the end of its owner's scope. A generic parameter is `Copy` only with the bound.
bool is_copy(const Type* t);
// `Send` (D115): a value that can go from one region to another, in a message. Every type is,
// except one that is or holds a pointer or an arena list, and a generic parameter without the
// bound.
bool is_send(const Type* t);
// Whether a read parameter of the type is passed as a pointer, since copying it could be costly.
bool is_view_type(const Type* t);

}  // namespace calx
