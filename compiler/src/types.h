// Checked types. Every type is interned, so two types are the same exactly when their pointers are.
#pragma once

#include <map>
#include <string>
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
    struct_,
    array,     // `[T; N]`
};

struct Type {
    TypeKind kind;
    std::uint8_t bits = 0;   // int_ and float_; usize says 64, and is_usize marks it
    bool is_signed = false;
    bool is_usize = false;
    Type* elem = nullptr;    // array
    std::uint64_t length = 0;
    Item* item = nullptr;    // struct_
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

    // The primitive named `name`, or null.
    Type* primitive(std::string_view name) const;
    Type* make_struct(Item* item, std::string_view name);
    Type* array_of(Type* elem, std::uint64_t length);
    const std::vector<Type*>& arrays() const { return arrays_; }

private:
    Type* make(TypeKind kind, std::string_view name);
    Type* make_int(std::uint8_t bits, bool is_signed, std::string_view name);

    Arena& arena_;
    std::uint32_t next_id_ = 1;
    std::vector<std::pair<std::string_view, Type*>> primitives_;
    std::map<std::pair<Type*, std::uint64_t>, Type*> array_map_;
    std::vector<Type*> arrays_;
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

}  // namespace calx
