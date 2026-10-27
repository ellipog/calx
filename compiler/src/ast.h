// The syntax tree. Every node lives in the arena; children are pointers or slices into it.
#pragma once

#include "common.h"
#include "lexer.h"

namespace calx {

struct Expr;
struct Stmt;
struct Type;
struct Pattern;
struct TypeExpr;
struct Block;
struct Item;

// `a.b.c`: a dotted name, resolved by the checker.
struct Path {
    Slice<Name> parts;
    Span span;
};

// ---------------------------------------------------------------------------------------------
// Types as written.

enum class TypeExprKind : std::uint8_t {
    named,     // `i32`, `Point`, `List<T>`, `std.c.int`
    optional,  // `T?`
    array,     // `[T; N]`
    tuple,     // `(A, B)`; `()` is the unit type
    fn,        // `fn(A, B) -> R`
    pointer,   // `*T` and `*const T`, for `unsafe` code
    dyn,       // `dyn Shape`
    self,      // `Self`
};

struct TypeExpr {
    TypeExprKind kind;
    Span span;
    Path path;                    // named
    Slice<TypeExpr*> args;        // named: generic arguments; tuple: elements; fn: parameters
    TypeExpr* inner = nullptr;    // optional, array, pointer: the element; fn: the result; dyn: the interface
    TypeExpr* error = nullptr;    // fn: the error type after `!`
    Expr* length = nullptr;       // array
    bool is_const = false;        // pointer
};

// ---------------------------------------------------------------------------------------------
// Expressions.

enum class ExprKind : std::uint8_t {
    int_lit, float_lit, bool_lit, char_lit, string_lit, none_lit,
    name,         // `x`, `Point`, `std.math.sin`: a path the checker resolves
    self,         // `self`
    unary,        // `-x`, `~x`, `not x`
    binary,       // `a + b` and every other infix operator, including `and`, `or` and `??`
    range,        // `a..b`, `a..=b`
    call,         // `f(a, name: b)`, and `f(a) |x| { ... }` with a trailing closure
    field,        // `a.b`, `a.0`
    optional_field,  // `a?.b`
    index,        // `a[i]`
    unwrap,       // `a!`
    try_,         // `try f()`
    struct_lit,   // `Point { x: 1, y: 2 }`, `Point()` is a call
    array_lit,    // `[a, b, c]`, `[x; n]`
    tuple,        // `(a, b)`; `()` is the unit value
    block,        // `{ ... }` used as a value
    if_,          // `if c { } else { }`, `if let x = opt { }`
    match,        // `match v { p => e, ... }`
    closure,      // `|a, b| expr`
    loop,         // `loop { }`, which can `break` with a value
    unsafe_block, // `unsafe { }`
};

enum class Op : std::uint8_t {
    // unary
    neg, bit_not, log_not,
    // binary, by precedence from high to low
    mul, div, rem, add, sub, shl, shr, ushr, bit_and, bit_xor, bit_or, coalesce,
    eq, ne, lt, le, gt, ge, log_and, log_or,
};

const char* op_text(Op op);

struct Arg {
    Name name;  // for `f(name: value)`
    Expr* value = nullptr;
    bool is_mut = false;  // `f(mut x)`: passing a mutable view
    Span span;
};

struct FieldInit {
    Name name;
    Expr* value = nullptr;  // null for the shorthand `Point { x, y }`
    Span span;
};

struct MatchArm {
    Pattern* pattern = nullptr;
    Expr* guard = nullptr;  // `p if cond => e`
    Expr* body = nullptr;
    Span span;
};

struct Param;

enum class ParamMode : std::uint8_t { read, mut, own };

// A local variable or parameter, made by the checker.
struct Local {
    Name name;
    Type* type = nullptr;
    Span span;
    bool is_var = false;     // `var`, which can be assigned again
    bool is_param = false;
    bool is_loop_var = false;  // a `for` loop's variable, which the loop assigns
    ParamMode mode{};        // parameters: read, mut or own
    bool is_view = false;    // a parameter passed as a pointer: structs and arrays in read or mut mode
    // What the body did with it, for the unused and never-changed checks (D63).
    bool used = false;
    bool reassigned = false;
    bool mutated = false;
    std::uint32_t borrowed = 0;  // nonzero while a `for` loop iterates over it
};

// What the checker found a name or call to be.
enum class Ref : std::uint8_t { none, local, fn, const_, type, builtin, variant };

enum class Builtin : std::uint8_t { none, print, println, panic, assert_ };

enum class CallKind : std::uint8_t {
    none,
    fn,          // a function, or a static function in an impl: `T.make()`
    method,      // `value.method()`: `lhs` of the callee is the receiver
    convert,     // `i64(x)`, `i8.wrap(x)`, `i32.saturate(f)`, `f32.nearest(d)`
    builtin,     // `println(...)` and the others in Builtin
    construct,   // `Point()`: a struct with every field at its default
    prim_method, // `x.abs()` and the other methods on primitive types
    variant,     // `Shape.Circle(r)`: an enum variant with data; `field_index` is the variant
    some,        // `some(x)`
    try_from,    // `i8.try_from(x)`, `Flag.try_from(n)`: a checked conversion to an optional
};

enum class Conv : std::uint8_t { none, exact, wrap, saturate, nearest };

enum class PrimMethod : std::uint8_t {
    none, abs, min, max, is_nan, sqrt, floor, ceil, wrapping_add, wrapping_sub, wrapping_mul, floor_div, floor_mod,
    checked_add, checked_sub, checked_mul, checked_div, checked_rem, saturating_add, saturating_sub, saturating_mul,
};

struct Expr {
    ExprKind kind;
    Span span;
    bool parens = false;  // written in parentheses, which the precedence rules (D63) look at
    // Literals.
    Token literal;  // int, float, char and string literals keep their token
    bool bool_value = false;
    Slice<Expr*> interpolations;  // string_lit: the parsed `{expr}` parts, in order
    // Names and fields.
    Path path;          // name
    Slice<TypeExpr*> type_args;  // name, struct_lit: generic arguments, `f<i32>(x)`, `Pair<i32, str> { ... }`
    Name member;        // field, optional_field: the field name (empty for a tuple index)
    std::uint32_t tuple_index = 0;
    bool is_tuple_index = false;
    // Operators.
    Op op{};
    Expr* lhs = nullptr;  // binary, range, index (the base), field (the base), unwrap, try_, unary
    Expr* rhs = nullptr;  // binary, range (null for an open end), index (the index)
    bool inclusive = false;  // range
    // Calls and literals.
    Expr* callee = nullptr;  // call
    Slice<Arg> args;         // call
    Expr* trailing = nullptr;  // call: a trailing closure
    Path type_path;          // struct_lit
    Slice<FieldInit> fields; // struct_lit
    Slice<Expr*> elems;      // array_lit, tuple; struct_lit: the field values in declaration order
    Expr* repeat = nullptr;  // array_lit `[x; n]`: the count
    // Control flow.
    Block* block = nullptr;      // block, loop, unsafe_block; if_: the then-branch
    Expr* cond = nullptr;        // if_: the condition, or the optional for `if let`
    Pattern* let_pattern = nullptr;  // if_: `if let p = e`
    Expr* else_ = nullptr;       // if_: a block expression or another if_
    Expr* scrutinee = nullptr;   // match
    Slice<MatchArm> arms;        // match
    Slice<Param> params;         // closure
    Expr* body = nullptr;        // closure

    // Filled in by the checker.
    Type* type = nullptr;
    Ref ref = Ref::none;
    Local* local = nullptr;      // a name that's a local
    Item* item = nullptr;        // a name that's a function or constant; the function a call calls
    Type* named_type = nullptr;  // a name or path that's a type, such as `i32` or `Point`
    CallKind call = CallKind::none;
    Builtin builtin = Builtin::none;
    Conv conv = Conv::none;
    PrimMethod prim = PrimMethod::none;
    std::uint32_t field_index = 0;  // field: the field, or the tuple element; a variant: its index
    Slice<Expr*> ordered_args;   // call: the arguments in parameter order, after names are matched
    // A compile-time constant. When `has_bits`, its value is in `const_bits`: integers, chars and
    // runes as two's complement (sign-extended for signed types), bools as 0 or 1, and floats as
    // the bits of the f64 holding the value.
    bool is_const = false;
    bool has_bits = false;
    std::uint64_t const_bits = 0;
    bool is_nan_const = false;   // `f64.NAN`, which comparing with is an error
    std::uint32_t loop_id = 0;   // loop, match: its labels in C
    bool let_unwraps = false;    // if_: `if let p = opt` matches `p` against the optional's value
    // call: written `Iface.method(x, ...)` and rewritten as `x.method(...)` that looks only in
    // that interface. A copy for an instance keeps the rewrite.
    Item* only_interface = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Patterns.

enum class PatternKind : std::uint8_t {
    wildcard,  // `_`
    binding,   // `x`, `mut x`
    literal,   // `1`, `"a"`, `'c'`, `true`, `-1`
    range,     // `1..5`, `'a'..='z'`
    variant,   // `Shape.Circle(r)`, `Circle(r)`, `some(x)`, `ok(v)`, `err(e)`, `Flag.OnGround`
    record,    // `Rect { w, h: height }`
    tuple,     // `(a, b)`
    none,      // `none`
    or_,       // `A | B`: any of the alternatives in `elems`
};

struct FieldPattern {
    Name name;
    Pattern* pattern = nullptr;  // null for the shorthand `{ w }`, until the checker makes it a binding
    Span span;
    std::uint32_t index = 0;     // filled in by the checker: the field's position
};

struct Pattern {
    PatternKind kind;
    Span span;
    Name name;           // binding
    bool is_mut = false; // binding
    Path path;           // variant, record
    Slice<Pattern*> elems;      // variant payload, tuple
    Slice<FieldPattern> fields; // record
    bool has_rest = false;      // record `{ x, .. }`
    Expr* value = nullptr;      // literal, range start
    Expr* end = nullptr;        // range end
    bool inclusive = false;     // range

    // Filled in by the checker.
    Type* type = nullptr;       // the type of the value matched
    Local* local = nullptr;     // binding
    std::uint32_t variant = 0;  // variant: the enum variant's index; for an optional, 1 is `some`
};

// ---------------------------------------------------------------------------------------------
// Statements and blocks.

enum class StmtKind : std::uint8_t {
    let,       // `let x = e`, `var x: T = e`, `let (a, b) = t`, `let x = opt else return 0`
    assign,    // `a = b`, `a += b`
    expr,      // an expression on its own line
    return_,
    break_,
    continue_,
    fail,      // `fail e`
    while_,
    for_,      // `for x in e { }`, `parallel for x in mut xs { }`
    checkout,  // `checkout x = c[key] else continue`
    send,      // `send dest msg`
    item,      // a nested `fn` or `const`
};

struct Stmt {
    StmtKind kind;
    Span span;
    Pattern* pattern = nullptr;    // let, for, checkout
    bool is_var = false;           // let: `var`
    TypeExpr* type = nullptr;      // let
    Expr* value = nullptr;         // let, assign (the right side), expr, return_, break_, fail, for_ (the iterable)
    Expr* target = nullptr;        // assign; send: the destination
    Op op{};                       // assign: the compound operator
    bool compound = false;         // assign
    Expr* else_ = nullptr;         // let-else and checkout-else: the diverging expression or block
    Block* else_block = nullptr;
    Expr* cond = nullptr;          // while_
    Block* body = nullptr;         // while_, for_
    bool is_parallel = false;      // for_
    Expr* pool = nullptr;          // for_: `parallel(pool) for`
    bool iter_mut = false;         // for_: `in mut xs`
    Item* item = nullptr;

    // Filled in by the checker.
    Local* local = nullptr;        // let, for: the variable bound by a plain name
    bool let_unwraps = false;      // let-else: the pattern matches the optional's value
    std::uint32_t loop_id = 0;     // while, for: its labels in C
    Type* elem_type = nullptr;     // for: the element type
};

struct Block {
    Span span;
    Slice<Stmt*> stmts;
    // The last expression, if the block ends with one: the block's value.
    Expr* tail = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Items.

struct Param {
    Name name;
    ParamMode mode = ParamMode::read;
    TypeExpr* type = nullptr;  // null for a bare `self`, or a closure parameter without a type
    Expr* default_value = nullptr;
    bool is_self = false;
    Span span;
    Local* local = nullptr;  // filled in by the checker
};

struct Attribute {
    Path name;
    Slice<Arg> args;
    Span span;
};

struct GenericParam {
    Name name;
    Slice<TypeExpr*> bounds;  // `T: Shape + Hash`
    Span span;
};

enum class ItemKind : std::uint8_t {
    fn, struct_, enum_, interface, impl, const_, import, extern_block, attribute_block, type_alias,
};

struct FieldDecl {
    Name name;
    TypeExpr* type = nullptr;
    Expr* default_value = nullptr;
    bool is_pub = false;
    Slice<Attribute> attributes;
    std::string_view doc;
    Span span;
    Type* resolved = nullptr;  // filled in by the checker
};

struct VariantDecl {
    Name name;
    Slice<TypeExpr*> payload;   // `Circle(f64)`
    Slice<FieldDecl> fields;    // `Rect { w: f64, h: f64 }`
    bool has_fields = false;
    Expr* value = nullptr;      // `OnGround = 1`
    std::string_view doc;
    Span span;
    // Filled in by the checker.
    Slice<Type*> types;         // the data's types: the payload's, or the fields' in order
    std::uint64_t tag = 0;      // the value of its tag
};

struct ImportName {
    Name name;
    Name alias;
    Span span;
};

struct Item {
    ItemKind kind;
    Span span;
    Span name_span;
    Name name;
    bool is_pub = false;
    std::string_view doc;  // the `///` lines before it, joined with newlines
    Slice<Attribute> attributes;
    Slice<GenericParam> generics;
    // fn
    Slice<Param> params;
    TypeExpr* result = nullptr;
    TypeExpr* error = nullptr;   // after `!`
    Block* body = nullptr;       // null for a declaration without a body (interfaces, extern)
    bool is_extern = false;      // `extern "C" fn`
    std::string_view abi;
    // struct, enum, actor
    Slice<FieldDecl> fields;
    bool is_tuple_struct = false;  // `struct Name(String)`
    bool is_actor = false;
    Slice<VariantDecl> variants;
    TypeExpr* repr = nullptr;    // `enum Flag: u8`
    // interface and impl
    TypeExpr* interface = nullptr;  // impl: `impl Shape for Square`
    TypeExpr* self_type = nullptr;  // impl: the type; type_alias: the aliased type
    Slice<Item*> members;           // interface, impl, extern_block, attribute_block
    // const
    TypeExpr* type = nullptr;
    Expr* value = nullptr;
    // import
    Path import_path;
    Slice<ImportName> import_names;  // `import a.b.{X, Y}`

    // Filled in by the checker.
    Type* declared = nullptr;     // struct: its type; const: the constant's type
    Type* result_type = nullptr;  // fn
    Type* impl_type = nullptr;    // a function in an impl block: the type
    bool is_method = false;       // its first parameter is the impl's type
    bool used = false;            // referred to from somewhere other than itself
    bool checked_signature = false;
    std::string_view c_name;
    // Generics.
    Slice<Type*> type_params;     // what it's generic over: an impl's parameters, then its own; an interface member's `Self` first
    Item* origin = nullptr;       // an instance: the generic item it was made from
    Slice<Type*> type_args;       // an instance: what each of `type_params` stands for
    Item* interface_item = nullptr;  // impl of an interface, and a member of either: the interface
    Span instance_site;           // an instance: where it was first needed, for notes on its errors
    std::uint32_t instance_depth = 0;
    bool resolved = false;        // struct, enum: its fields have types
    bool resolving = false;
};

struct Module {
    std::uint32_t file = 0;
    Path name;  // from `module a.b.c`
    std::string_view doc;
    Slice<Item*> items;
};

}  // namespace calx
