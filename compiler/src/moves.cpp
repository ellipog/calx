#include "moves.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace calx {

namespace {

std::string q(std::string_view s) { return "`" + std::string(s) + "`"; }

// Types the checker couldn't work out behave as `Copy`, so they never cause an error here.
bool copyable(const Type* t) { return !t || is_copy(t); }

// What an expression's value is for: to keep, which moves it; to look at; or to change.
enum class Use : std::uint8_t { consume, view, change };

// Why a local doesn't own its value.
enum class ViewOf : std::uint8_t { none, read_param, mut_param, element, part };

bool is_local_name(const Expr* e) {
    return (e->kind == ExprKind::name || e->kind == ExprKind::self) && e->ref == Ref::local && e->local;
}

// The local a place such as `a.b[i].c` or `opt!` is part of, or null for a new value.
Local* place_root(const Expr* e) {
    while (true) {
        if (e->kind == ExprKind::name || e->kind == ExprKind::self) return e->ref == Ref::local ? e->local : nullptr;
        if (e->kind == ExprKind::field && e->ref != Ref::variant && e->ref != Ref::const_ && !e->has_bits) {
            e = e->lhs;
            continue;
        }
        if (e->kind == ExprKind::index || e->kind == ExprKind::unwrap) {
            e = e->lhs;
            continue;
        }
        return nullptr;
    }
}

class Pass {
public:
    Pass(MoveContext ctx, bool report) : ctx_(ctx), report_(report) {}

    void run(Item* f) {
        // The scan finds every local that's moved anywhere, which decides which patterns take
        // their value apart. Then the walk follows the flow of control.
        scan_ = true;
        body(f);
        scan_ = false;
        body(f);
    }

private:
    enum : std::uint8_t { live = 0, moved = 1, maybe = 2 };

    // A field moved out of a local that owns its value, such as `p.name`: its path of field
    // numbers. The rest of the local can still be used, and is dropped as usual, since the move
    // zeroed the field.
    struct Part {
        std::uint32_t local;
        std::vector<std::uint32_t> path;
        bool maybe;
        Span at;
    };

    // Each local's state at a point in the body. A dead state is after a `return`, `break` or
    // `continue`, which joins with nothing.
    struct State {
        std::vector<std::uint8_t> v;
        std::vector<Part> parts;
        bool dead = false;
    };

    struct Loop {
        std::uint32_t id;
        std::size_t depth;  // the scopes inside it start here
        std::vector<State> breaks, conts;
    };

    struct Borrow {
        Local* root;
        Local* by;     // the name that's a view into it, or null for a `for` loop
        Span at;
        // A `for` loop: what it goes over, and the fields from the root to it, up to an index.
        const Expr* over = nullptr;
        std::vector<std::uint32_t> path;
        // A string's `{...}` part, which holds `over` until the string is made, instead of a loop.
        bool shown = false;
    };

    std::string_view text(Name n) const { return ctx_.names.text(n); }

    void error(Span s, std::string msg) {
        if (scan_ || muted_ || !report_) return;
        ctx_.diags.error(s, std::move(msg));
        noted_ = true;
    }

    void note(Span s, std::string msg) {
        if (noted_) ctx_.diags.note(s, std::move(msg));
    }

    // --- locals and their states -------------------------------------------------------------

    std::uint32_t id(Local* l) {
        auto [it, fresh] = ids_.emplace(l, static_cast<std::uint32_t>(locals_.size()));
        if (fresh) {
            locals_.push_back(l);
            moved_at_.push_back(Span{});
            reported_.push_back(false);
        }
        return it->second;
    }

    std::uint8_t get(Local* l) {
        std::uint32_t i = id(l);
        return i < st_.v.size() ? st_.v[i] : live;
    }

    void set(Local* l, std::uint8_t s) {
        std::uint32_t i = id(l);
        if (st_.v.size() <= i) st_.v.resize(i + 1, live);
        st_.v[i] = s;
    }

    static State join(const State& a, const State& b) {
        if (a.dead) return b;
        if (b.dead) return a;
        State r;
        r.v.resize(std::max(a.v.size(), b.v.size()), live);
        for (std::size_t i = 0; i < r.v.size(); i++) {
            std::uint8_t x = i < a.v.size() ? a.v[i] : live;
            std::uint8_t y = i < b.v.size() ? b.v[i] : live;
            r.v[i] = x == y ? x : maybe;
        }
        for (const Part& p : a.parts) {
            const Part* other = find_part(b, p);
            r.parts.push_back({p.local, p.path, p.maybe || !other || other->maybe, p.at});
        }
        for (const Part& p : b.parts) {
            if (!find_part(a, p)) r.parts.push_back({p.local, p.path, true, p.at});
        }
        return r;
    }

    static const Part* find_part(const State& s, const Part& p) {
        for (const Part& x : s.parts) {
            if (x.local == p.local && x.path == p.path) return &x;
        }
        return nullptr;
    }

    // Whether one path is the start of the other: then they share memory.
    static bool overlap(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
        std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; i++) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    static bool starts_with(const std::vector<std::uint32_t>& path, const std::vector<std::uint32_t>& prefix) {
        return prefix.size() <= path.size() && overlap(path, prefix);
    }

    void clear_parts(Local* l, const std::vector<std::uint32_t>& under) {
        std::uint32_t i = id(l);
        auto& ps = st_.parts;
        ps.erase(std::remove_if(ps.begin(), ps.end(), [&](const Part& p) { return p.local == i && starts_with(p.path, under); }),
                 ps.end());
    }

    static State join(State s, const std::vector<State>& more) {
        for (const State& m : more) s = join(s, m);
        return s;
    }

    static State dead_state() {
        State s;
        s.dead = true;
        return s;
    }

    void declare(Local* l) {
        scopes_.back().push_back(l);
        set(l, live);
        clear_parts(l, {});
    }

    void end_scope() { scopes_.pop_back(); }

    // The locals of the scopes from `from` inward that may still hold a value, innermost first:
    // what's dropped when leaving them. One that may have moved is zeroed when it moves, so
    // dropping it is safe either way.
    Slice<Local*> drops_from(std::size_t from) {
        if (scan_ || st_.dead) return {};
        std::vector<Local*> out;
        for (std::size_t s = scopes_.size(); s-- > from;) {
            for (auto it = scopes_[s].rbegin(); it != scopes_[s].rend(); ++it) {
                Local* l = *it;
                if (!l->owns || copyable(l->type)) continue;
                std::uint8_t x = get(l);
                if (x == moved) continue;
                if (x == maybe) l->clear_on_move = true;
                out.push_back(l);
            }
        }
        return ctx_.arena.copy(out);
    }

    Local* make_hidden(Expr* v) {
        auto* l = ctx_.arena.make<Local>();
        l->type = v->type;
        l->span = v->span;
        l->owns = true;
        return l;
    }

    // --- uses ----------------------------------------------------------------------------------

    std::string name_of(const Local* l) const { return q(text(l->name)); }

    // Reading or changing the place at `path` in `l`, which must hold a value there.
    void use(Expr* e, Local* l, const std::vector<std::uint32_t>& path = {}) {
        std::uint8_t x = get(l);
        std::uint32_t i = id(l);
        if (x != live) {
            if (!reported_[i] && !scan_ && !muted_ && report_) {
                reported_[i] = true;
                Span at = moved_at_[i];
                if (at.file == e->span.file && at.begin == e->span.begin) {
                    // Moved where it's used, in a loop: the next pass finds it gone.
                    error(e->span, name_of(l) + " is moved here on every pass through the loop, so the next pass can't "
                                                "use it; move a copy with " + q(std::string(text(l->name)) + ".copy()") +
                                                ", or give it a new value before the loop comes around");
                } else {
                    error(e->span, name_of(l) + (x == moved ? " was moved, so it can't be used here"
                                                            : " might have been moved, so it can't be used here"));
                    note(at, "it's moved here");
                }
            }
            set(l, live);  // one error for each move
            return;
        }
        for (const Part& p : st_.parts) {
            if (p.local != i || !overlap(p.path, path)) continue;
            std::string part = q(path_text(l, p.path));
            error(e->span, part + (p.maybe ? " might have been moved" : " was moved") + ", so " +
                               (path.size() < p.path.size() ? q(path_text(l, path)) : part) + " can't be used here");
            note(p.at, "it's moved here");
            clear_parts(l, p.path);
            return;
        }
    }

    // `p.a.b`, from a local and a path of field numbers.
    std::string path_text(const Local* l, const std::vector<std::uint32_t>& path) const {
        std::string s(text(l->name));
        const Type* t = l->type;
        for (std::uint32_t k : path) {
            if (t->kind == TypeKind::tuple) {
                s += "." + std::to_string(k);
                t = t->elems[k];
            } else {
                s += "." + std::string(text(t->item->fields[k].name));
                t = t->item->fields[k].resolved;
            }
        }
        return s;
    }

    void move(Expr* e, Local* l) {
        if (!l->owns) {
            cant_move(e, l);
            return;
        }
        if (const Borrow* b = borrowed(l)) {
            if (b->by) {
                error(e->span, "can't move " + name_of(l) + " while " + name_of(b->by) + " is a view into it");
                note(b->at, name_of(b->by) + " is a view into " + name_of(l) + " here");
            } else if (b->shown) {
                error(e->span, "can't move " + name_of(l) + shown_after(b));
            } else {
                error(e->span, "can't move " + name_of(l) + " while a `for` loop goes over " + looped(b));
            }
            return;
        }
        set(l, moved);
        moved_at_[id(l)] = e->span;
        e->moves = true;
    }

    // A pattern takes the whole value of `l` apart.
    void take(Local* l, Span at) {
        if (const Borrow* b = borrowed(l)) {
            error(at, "can't take " + name_of(l) + " apart" +
                          (b->by      ? " while " + name_of(b->by) + " is a view into it"
                           : b->shown ? shown_after(b)
                                      : " while a `for` loop goes over " + looped(b)));
            return;
        }
        set(l, moved);
        moved_at_[id(l)] = at;
    }

    // In generic code, a parameter's values move unless it's `Copy`.
    static std::string copy_hint(const Type* t) {
        if (t && t->kind == TypeKind::param) return ", or give " + q(t->name) + " the bound `Copy`";
        return "";
    }

    void cant_move(Expr* e, Local* l) {
        std::string n(text(l->name));
        std::string hint = copy_hint(l->type);
        auto it = view_of_.find(l);
        ViewOf why = it == view_of_.end() ? ViewOf::none : it->second;
        switch (why) {
            case ViewOf::read_param:
                error(e->span, q(n) + " is a view of the caller's value, so it can't be moved; take it as " +
                                   q(n + ": own " + std::string(l->type->name)) + ", or move a copy with " +
                                   q(n + ".copy()") + hint);
                break;
            case ViewOf::mut_param:
                error(e->span, q(n) + " is a `mut` view of the caller's value, so it can't be moved; move a copy with " +
                                   q(n + ".copy()") + hint);
                break;
            case ViewOf::element:
                error(e->span, q(n) + " is an element the loop looks at, so it can't be moved; move a copy with " +
                                   q(n + ".copy()") + hint);
                break;
            default: {
                auto d = part_of_.find(l);
                std::string whole = d == part_of_.end() ? "the value matched" : q(d->second);
                error(e->span, q(n) + " is part of " + whole + ", which can't be taken apart here, so " + q(n) +
                                   " can't be moved; move a copy with " + q(n + ".copy()") + hint);
                break;
            }
        }
    }

    // What keeps `l` from moving, or the part of it at `path` when that's given. A `for` loop over
    // a field keeps only that field, what holds it and what's in it.
    const Borrow* borrowed(const Local* l, const std::vector<std::uint32_t>* path = nullptr) const {
        for (const Borrow& b : borrows_) {
            if (b.root != l) continue;
            if (path && !b.by) {
                std::size_t n = std::min(path->size(), b.path.size());
                if (!std::equal(path->begin(), path->begin() + static_cast<std::ptrdiff_t>(n), b.path.begin())) continue;
            }
            return &b;
        }
        return nullptr;
    }

    // What a `for` loop goes over, for a message about its whole root: `it`, or the field.
    std::string looped(const Borrow* b) const { return b->path.empty() ? "it" : q(place_text(b->over)); }
    // Why a string's part keeps what's moved, for the end of a message.
    std::string shown_after(const Borrow* b) const {
        return " after " + q("{" + place_text(b->over) + "}") +
               " in the same string, which shows it as it is once every part is worked out; work this part out first, into a variable";
    }

    // The fields from a place's root to its first index, as the checker's loop rule counts them.
    static std::vector<std::uint32_t> field_path(const Expr* e) {
        std::vector<std::uint32_t> path;
        for (; e->kind == ExprKind::field || e->kind == ExprKind::index; e = e->lhs) {
            if (e->kind == ExprKind::index) path.clear();
            else path.push_back(e->field_index);
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    // A change to the place rooted at `l`. A `for` loop's own rule is the checker's.
    void check_change(Local* l, Span at) {
        for (const Borrow& b : borrows_) {
            if (b.root != l || !b.by) continue;
            error(at, name_of(l) + " can't be changed while " + name_of(b.by) + " is a view into it");
            note(b.at, name_of(b.by) + " is a view into " + name_of(l) + " here");
            return;
        }
    }

    // How a place is written, for messages: `p.name`, `xs[...]`.
    std::string place_text(const Expr* e) const {
        switch (e->kind) {
            case ExprKind::name: return std::string(text(e->path.parts[0]));
            case ExprKind::self: return "self";
            case ExprKind::field:
                return place_text(e->lhs) + "." + (e->is_tuple_index ? std::to_string(e->tuple_index) : std::string(text(e->member)));
            case ExprKind::index: return place_text(e->lhs) + "[...]";
            case ExprKind::unwrap: return place_text(e->lhs) + "!";
            default: return "...";
        }
    }

    // --- functions and blocks --------------------------------------------------------------------

    void body(Item* f) {
        st_ = State{};
        scopes_.clear();
        loops_.clear();
        borrows_.clear();
        scopes_.emplace_back();
        for (Param& p : f->params) {
            Local* l = p.local;
            if (!l) continue;
            l->owns = p.mode == ParamMode::own;
            view_of_[l] = p.mode == ParamMode::own ? ViewOf::none
                          : p.mode == ParamMode::mut ? ViewOf::mut_param
                                                     : ViewOf::read_param;
            declare(l);
        }
        Type* r = f->result_type && f->result_type->is_result ? f->result_type->args[0] : f->result_type;
        bool value = r && r->kind != TypeKind::unit && r->kind != TypeKind::never;
        // The parameters are in the body's scope, so its end drops the `own` ones.
        block(f->body, value ? Use::consume : Use::view, false);
        scopes_.clear();
    }

    void block(Block* b, Use u, bool new_scope = true) {
        if (new_scope) scopes_.emplace_back();
        std::vector<Stmt*> lets;
        for (Stmt* s : b->stmts) {
            stmt(s);
            if (scan_ && is_pattern_let(s)) lets.push_back(s);
        }
        if (b->tail) expr(b->tail, u);
        if (scan_) {
            // A later `let` can take apart what an earlier one bound, so the last goes first.
            for (auto it = lets.rbegin(); it != lets.rend(); ++it) decide_let(*it);
        }
        b->drops = drops_from(scopes_.size() - 1);
        unborrow_scope();
        end_scope();
    }

    static bool is_pattern_let(const Stmt* s) {
        return s->kind == StmtKind::let && (s->else_ || s->else_block || s->pattern->kind != PatternKind::binding) &&
               !(s->pattern->kind == PatternKind::wildcard && !s->else_ && !s->else_block);
    }

    // --- statements ----------------------------------------------------------------------------

    void stmt(Stmt* s) {
        switch (s->kind) {
            case StmtKind::let: let(s); break;
            case StmtKind::assign: assign(s); break;
            case StmtKind::expr: expr(s->value, Use::view); break;
            case StmtKind::return_:
            case StmtKind::fail:
                if (s->value) expr(s->value, Use::consume);
                s->drops = drops_from(0);
                st_ = dead_state();
                break;
            case StmtKind::break_:
            case StmtKind::continue_: {
                if (s->value) expr(s->value, Use::consume);
                Loop* l = find_loop(s->loop_id);
                if (l) {
                    s->drops = drops_from(l->depth);
                    (s->kind == StmtKind::break_ ? l->breaks : l->conts).push_back(st_);
                }
                st_ = dead_state();
                break;
            }
            case StmtKind::while_: while_loop(s); break;
            case StmtKind::for_: for_loop(s); break;
            default: break;
        }
    }

    Loop* find_loop(std::uint32_t id) {
        for (auto it = loops_.rbegin(); it != loops_.rend(); ++it) {
            if (it->id == id) return &*it;
        }
        return nullptr;
    }

    void let(Stmt* s) {
        Pattern* p = s->pattern;
        bool has_else = s->else_ || s->else_block;
        if (!has_else && p->kind == PatternKind::binding) {
            expr(s->value, Use::consume);
            if (s->local) {
                s->local->owns = true;
                declare(s->local);
            }
            return;
        }
        if (!has_else && p->kind == PatternKind::wildcard) {
            expr(s->value, Use::consume);
            s->drop_value = !copyable(s->value->type);
            return;
        }
        Expr* v = s->value;
        Local* root = place_root(v);
        Local* hidden = nullptr;
        if (root) {
            expr(v, Use::view);
        } else {
            expr(v, Use::consume);
            if (!copyable(v->type)) {
                if (!s->hidden) s->hidden = make_hidden(v);
                hidden = s->hidden;
                if (!scan_) declare(hidden);
            }
        }
        if (has_else) {
            // The other way runs when the pattern doesn't match, and leaves.
            State before = st_;
            if (s->else_block) block(s->else_block, Use::view);
            else expr(s->else_, Use::view);
            st_ = before;
        }
        if (scan_) {
            bind(p, false, nullptr, "");
            return;
        }
        bool whole = root && is_local_name(v) && root->owns;
        s->consumes = s->consumes && (whole || hidden);
        if (s->consumes) take(whole ? root : hidden, v->span);
        bind(p, s->consumes, s->consumes || !root ? nullptr : root, place_text(v));
    }

    // Decides whether a `let` with a pattern takes its value apart: it does when one of the names
    // that isn't `Copy` is moved somewhere, or is `mut`.
    void decide_let(Stmt* s) {
        s->consumes = binds_moved(s->pattern);
        if (s->consumes && is_local_name(s->value)) moved_any_.insert(s->value->local);
    }

    bool binds_moved(const Pattern* p) const {
        if (!p) return false;
        if (p->kind == PatternKind::binding) {
            return p->local && !copyable(p->local->type) && (moved_any_.count(p->local) || p->is_mut);
        }
        for (const Pattern* x : p->elems) {
            if (binds_moved(x)) return true;
        }
        for (const FieldPattern& f : p->fields) {
            if (binds_moved(f.pattern)) return true;
        }
        return false;
    }

    // Declares a pattern's names. With `owns`, they own their parts of the value. Otherwise those
    // that aren't `Copy` are views into it, and `root`, the local it's part of, can't change or
    // move while they're in scope.
    void bind(Pattern* p, bool owns, Local* root, const std::string& whole, ViewOf why = ViewOf::part) {
        if (!p) return;
        if (p->kind == PatternKind::binding && p->local) {
            Local* l = p->local;
            l->owns = owns || copyable(l->type);
            if (!l->owns) {
                view_of_[l] = why;
                part_of_[l] = whole;
                if (root && !scan_) borrows_.push_back({root, l, p->span, nullptr, {}, false});
            }
            if (!scan_) {
                declare(l);
                if (root && !l->owns) borrow_scopes_.push_back(scopes_.size());
            }
            return;
        }
        for (Pattern* x : p->elems) bind(x, owns, root, whole, why);
        for (FieldPattern& f : p->fields) bind(f.pattern, owns, root, whole, why);
    }

    // Ends the views that the innermost scope's names made.
    void unborrow_scope() {
        while (!borrow_scopes_.empty() && borrow_scopes_.back() >= scopes_.size()) {
            borrow_scopes_.pop_back();
            borrows_.pop_back();
        }
    }

    void assign(Stmt* s) {
        Expr* t = s->target;
        if (t->kind == ExprKind::name && !t->local && text(t->path.parts[0]) == "_") {
            // `_ = e` only looks at a place, and drops a new value.
            if (place_root(s->value)) {
                expr(s->value, Use::view);
            } else {
                expr(s->value, Use::consume);
                if (!scan_) s->drop_value = !copyable(s->value->type);
            }
            return;
        }
        if (s->compound) {
            expr(s->value, Use::view);
            expr(t, Use::change);
            return;
        }
        expr(s->value, Use::consume);
        if (is_local_name(t)) {
            Local* l = t->local;
            if (scan_) return;
            check_change(l, t->span);
            if (!copyable(l->type)) {
                // A `mut` parameter always holds the caller's value.
                std::uint8_t x = l->owns ? get(l) : live;
                s->drop_old = x != moved;
                if (x == maybe) l->clear_on_move = true;
            }
            set(l, live);
            clear_parts(l, {});
            return;
        }
        if (!place_root(t)) {
            expr(t, Use::change);
            return;
        }
        Where w = locate(t);
        if (scan_) return;
        Local* l = w.root;
        // Assigning to a field that moved out puts a value back; inside one, it's still moved.
        std::uint32_t i = id(l);
        bool inside = false;
        for (const Part& p : st_.parts) {
            if (p.local == i && starts_with(w.path, p.path) && p.path != w.path) inside = true;
        }
        if (!inside && w.fields_only) clear_parts(l, w.path);
        use(t, l, w.path);
        check_change(l, t->span);
        s->drop_old = !copyable(t->type);
    }

    // --- loops -------------------------------------------------------------------------------------

    // Walks a loop: once quietly from the state before it, to find the state when it comes back
    // around, and again from the two joined, which is where every pass through it can start. The
    // body's moves and assignments decide a local's state on the way back, so twice is enough.
    // `iteration` walks from the top of the loop to its end, and returns the state there.
    template <class F>
    State walk_loop(std::uint32_t id, std::size_t depth, F iteration, State* head_out) {
        State entry = st_;
        muted_++;
        loops_.push_back({id, depth, {}, {}});
        State back = iteration();
        back = join(back, loops_.back().conts);
        loops_.pop_back();
        muted_--;
        State head = join(entry, back);
        st_ = head;
        if (head_out) *head_out = head;
        loops_.push_back({id, depth, {}, {}});
        iteration();
        State breaks = join(dead_state(), loops_.back().breaks);
        loops_.pop_back();
        return breaks;
    }

    void while_loop(Stmt* s) {
        if (scan_) {
            expr(s->cond, Use::view);
            block(s->body, Use::view);
            return;
        }
        State exit;
        State breaks = walk_loop(s->loop_id, scopes_.size(), [&] {
            expr(s->cond, Use::view);
            exit = st_;
            block(s->body, Use::view);
            return st_;
        }, nullptr);
        st_ = join(exit, breaks);
    }

    void for_loop(Stmt* s) {
        Expr* it = s->value;
        scopes_.emplace_back();
        Local* root = nullptr;
        Local* hidden = nullptr;
        if (it->kind == ExprKind::range) {
            expr(it->lhs, Use::view);
            expr(it->rhs, Use::view);
        } else if ((root = place_root(it))) {
            expr(it, Use::view);
        } else {
            expr(it, Use::consume);
            if (!copyable(it->type)) {
                if (!s->hidden) s->hidden = make_hidden(it);
                hidden = s->hidden;
                if (!scan_) declare(hidden);
            }
        }
        // The ends of `m.range(a..b)` are worked out after `m`, before the loop (D112).
        if (s->range_of) {
            if (s->range_of->lhs) expr(s->range_of->lhs, Use::view);
            if (s->range_of->rhs) expr(s->range_of->rhs, Use::view);
        }
        auto iteration = [&] {
            scopes_.emplace_back();
            if (root) borrows_.push_back({root, nullptr, it->span, it, field_path(it), false});
            if (s->local) {
                s->local->owns = copyable(s->local->type);
                if (!s->local->owns) view_of_[s->local] = ViewOf::element;
                if (!scan_) declare(s->local);
            } else {
                bind(s->pattern, false, nullptr, "", ViewOf::element);
            }
            block(s->body, Use::view);
            unborrow_scope();
            if (root) borrows_.pop_back();
            end_scope();
            return st_;
        };
        if (scan_) {
            iteration();
        } else {
            State head;
            State breaks = walk_loop(s->loop_id, scopes_.size(), iteration, &head);
            st_ = join(head, breaks);
            if (hidden) {
                std::uint8_t x = get(hidden);
                s->drop_hidden = !st_.dead && x != moved;
                if (x == maybe) hidden->clear_on_move = true;
            }
        }
        end_scope();
    }

    void loop_expr(Expr* e) {
        if (scan_) {
            block(e->block, Use::view);
            return;
        }
        st_ = walk_loop(e->loop_id, scopes_.size(), [&] {
            block(e->block, Use::view);
            return st_;
        }, nullptr);
    }

    // --- expressions ---------------------------------------------------------------------------------

    // A block, `if` or `match` whose value is only looked at keeps it in a temporary, so its
    // branches move their values into that.
    Use branch_use(Expr* e, Use u) {
        if (u == Use::view && !copyable(e->type)) {
            if (!scan_) e->temp = true;
            return Use::consume;
        }
        return u;
    }

    void rvalue(Expr* e, Use u) {
        if (!scan_ && u == Use::view && !copyable(e->type)) e->temp = true;
    }

    void expr(Expr* e, Use u) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::string_lit: {
                // A part that's a place and owns memory is held until the string is made, so the
                // parts after it can't move it.
                std::size_t held = borrows_.size();
                for (Expr* x : e->interpolations) {
                    expr(x, Use::view);
                    Local* root = place_root(x);
                    if (root && !copyable(x->type)) borrows_.push_back({root, nullptr, x->span, x, field_path(x), true});
                }
                borrows_.resize(held);
                if (!e->interpolations.empty()) rvalue(e, u);
                return;
            }
            case ExprKind::name:
            case ExprKind::self: name(e, u); return;
            case ExprKind::unary: expr(e->lhs, Use::view); return;
            case ExprKind::binary: binary(e, u); return;
            case ExprKind::range:
                expr(e->lhs, Use::view);
                expr(e->rhs, Use::view);
                return;
            case ExprKind::call: call(e, u); return;
            case ExprKind::field:
                if (e->ref == Ref::variant || e->ref == Ref::const_ || e->has_bits || !e->lhs) return;
                if (e->lhs->ref == Ref::type) return;
                project(e, u);
                return;
            case ExprKind::index:
            case ExprKind::unwrap: project(e, u); return;
            case ExprKind::try_:
                // The error path leaves the function, dropping what a `return` here would.
                expr(e->lhs, Use::consume);
                e->exit_drops = drops_from(0);
                rvalue(e, u);
                return;
            case ExprKind::optional_field:
                expr(e->lhs, Use::view);
                if (e->type && e->type->kind == TypeKind::optional && !copyable(e->type->elem)) {
                    error(e->span, "`?.` can only read a field that's `Copy` for now; unwrap the optional first");
                }
                return;
            case ExprKind::struct_lit:
                for (FieldInit& f : e->fields) expr(f.value, Use::consume);
                rvalue(e, u);
                return;
            case ExprKind::array_lit:
                for (Expr* x : e->elems) expr(x, Use::consume);
                expr(e->repeat, Use::view);
                rvalue(e, u);
                return;
            case ExprKind::tuple:
                for (Expr* x : e->elems) expr(x, Use::consume);
                rvalue(e, u);
                return;
            case ExprKind::block:
            case ExprKind::unsafe_block: block(e->block, branch_use(e, u)); return;
            case ExprKind::if_: if_expr(e, u); return;
            case ExprKind::match: match_expr(e, u); return;
            case ExprKind::loop:
                loop_expr(e);
                rvalue(e, u);
                return;
            default: return;
        }
    }

    void name(Expr* e, Use u) {
        if (e->ref != Ref::local || !e->local) return;
        place(e, u);
    }

    // `a.b`, `a[i]` and `a!`: parts of a place, or of a new value.
    void project(Expr* e, Use u) {
        Expr* base = e->lhs;
        bool keep = u == Use::consume && !copyable(e->type);
        if (e->kind == ExprKind::unwrap) {
            // Keeping `opt!` moves the whole optional.
            expr(base, keep ? Use::consume : u == Use::change ? Use::change : Use::view);
            return;
        }
        if (place_root(e)) {
            place(e, u);
            return;
        }
        if (keep && e->kind == ExprKind::field) {
            // A field of a new value: the rest of it is dropped.
            expr(base, Use::consume);
            if (!scan_) e->part_move = true;
            return;
        }
        expr(base, Use::view);
        if (e->kind == ExprKind::index) expr(e->rhs, Use::view);
        if (keep) error(e->span, "can't move an element out of a new list; keep the list in a variable first");
    }

    // The local a place is in, and the path of fields to it, which stops at the first index or `!`.
    struct Where {
        Local* root = nullptr;
        std::vector<std::uint32_t> path;
        bool fields_only = true;
    };

    // Works out where a place is, and walks the indexes in it, in the order they run.
    Where locate(Expr* e) {
        std::vector<Expr*> chain;
        Expr* x = e;
        while (x->kind == ExprKind::field || x->kind == ExprKind::index || x->kind == ExprKind::unwrap) {
            chain.push_back(x);
            x = x->lhs;
        }
        Where w;
        w.root = x->local;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            Expr* c = *it;
            if (c->kind == ExprKind::field && w.fields_only) {
                w.path.push_back(c->field_index);
            } else {
                w.fields_only = false;
                if (c->kind == ExprKind::index) expr(c->rhs, Use::view);
            }
        }
        return w;
    }

    // A variable, or a part of one.
    void place(Expr* e, Use u) {
        Where w = locate(e);
        Local* l = w.root;
        bool keep = u == Use::consume && !copyable(e->type);
        bool whole = is_local_name(e);
        if (scan_) {
            // Moving a field counts too: a pattern's name whose field moves must own its value.
            if (keep && (whole || w.fields_only)) moved_any_.insert(l);
            return;
        }
        use(e, l, w.path);
        if (u == Use::change) check_change(l, e->span);
        if (!keep) return;
        if (whole) {
            move(e, l);
            return;
        }
        Expr* base = e->lhs;
        std::string r(text(l->name));
        if (e->kind == ExprKind::index) {
            TypeKind k = base->type ? base->type->kind : TypeKind::error;
            const char* how = k == TypeKind::list  ? "; take it out with `remove` or `swap_remove`, or move a copy with `.copy()`"
                            : k == TypeKind::deque ? "; take it out with `pop_first` or `pop_last`, or move a copy with `.copy()`"
                            : k == TypeKind::map  ? "; take it out with `remove`, or move a copy with `.copy()`"
                                                  : "; move a copy with `.copy()`";
            error(e->span, std::string("can't move ") + (k == TypeKind::map ? "a value" : "an element") + " out of " +
                               q(place_text(base)) + how + copy_hint(e->type));
        } else if (!w.fields_only || !l->owns) {
            error(e->span, "can't move " + q(place_text(e)) + " out of " + q(r) +
                               (l->owns ? "" : ", a view of the caller's value") + "; move a copy with " +
                               q(place_text(e) + ".copy()") + copy_hint(e->type));
        } else if (const Borrow* b = borrowed(l, &w.path)) {
            std::string over = b->by || b->path.empty() ? r : place_text(b->over);
            if (b->shown) {
                error(e->span, "can't move " + q(place_text(e)) + shown_after(b));
                return;
            }
            error(e->span, "can't move " + q(place_text(e)) + " while " +
                               (b->by ? name_of(b->by) + " is a view into " + q(r)
                                      : "a `for` loop goes over " + (over == place_text(e) ? std::string("it") : q(over))));
        } else {
            // A field of a local that owns its value moves out, and is zeroed.
            st_.parts.push_back({id(l), w.path, false, e->span});
            e->moves = true;
        }
    }

    void binary(Expr* e, Use u) {
        switch (e->op) {
            case Op::log_and:
            case Op::log_or: {
                expr(e->lhs, Use::view);
                State before = st_;
                expr(e->rhs, Use::view);
                st_ = join(before, st_);
                return;
            }
            case Op::coalesce: {
                Use ou = copyable(e->type) ? Use::view : Use::consume;
                expr(e->lhs, ou);
                State before = st_;
                expr(e->rhs, ou);
                st_ = join(before, st_);
                rvalue(e, u);
                return;
            }
            default:
                expr(e->lhs, Use::view);
                expr(e->rhs, Use::view);
                return;
        }
    }

    void call(Expr* e, Use u) {
        switch (e->call) {
            case CallKind::fn:
            case CallKind::method: {
                Item* f = e->item;
                // In the order they run: the receiver, then the arguments as written.
                std::vector<Expr*> src;
                if (e->call == CallKind::method) src.push_back(e->callee->lhs);
                for (const Arg& a : e->args) src.push_back(a.value);
                for (Expr* x : src) {
                    ParamMode m = ParamMode::read;
                    for (std::size_t i = 0; i < e->ordered_args.size() && f && i < f->params.size(); i++) {
                        if (e->ordered_args[i] == x) m = f->params[i].mode;
                    }
                    expr(x, m == ParamMode::own ? Use::consume : m == ParamMode::mut ? Use::change : Use::view);
                }
                break;
            }
            case CallKind::variant:
            case CallKind::some:
                for (const Arg& a : e->args) expr(a.value, Use::consume);
                break;
            case CallKind::builtin_method: builtin_method(e); break;
            case CallKind::prim_static:
                // `String.from_utf8` takes the list's memory for the string.
                for (const Arg& a : e->args) expr(a.value, e->prim == PrimMethod::from_utf8 ? Use::consume : Use::view);
                break;
            case CallKind::prim_method:
                expr(e->callee->lhs, Use::view);
                for (const Arg& a : e->args) expr(a.value, Use::view);
                break;
            default:
                for (const Arg& a : e->args) expr(a.value, Use::view);
                break;
        }
        rvalue(e, u);
    }

    void builtin_method(Expr* e) {
        Expr* recv = e->callee->lhs;
        auto arg = [&](std::size_t i) { return i < e->args.size() ? e->args[i].value : nullptr; };
        switch (e->bmethod) {
            case BMethod::push_first: case BMethod::push_last:
                expr(recv, Use::change);
                expr(arg(0), Use::consume);
                return;
            case BMethod::push:
                expr(recv, Use::change);
                expr(arg(0), recv->type && recv->type->kind == TypeKind::list ? Use::consume : Use::view);
                return;
            case BMethod::insert: case BMethod::compute_if_absent: case BMethod::insert_first: case BMethod::insert_last: {
                // A list's index is read; a map's key and value, and a set's value, move in.
                expr(recv, Use::change);
                bool list = recv->type && recv->type->kind == TypeKind::list;
                expr(arg(0), list ? Use::view : Use::consume);
                if (arg(1)) expr(arg(1), Use::consume);
                return;
            }
            case BMethod::pop: case BMethod::clear: case BMethod::remove: case BMethod::swap_remove:
            case BMethod::swap: case BMethod::truncate: case BMethod::reserve: case BMethod::push_str:
            case BMethod::set_len: case BMethod::as_mut_ptr: case BMethod::sort: case BMethod::hash_add: case BMethod::trim:
            case BMethod::pop_first: case BMethod::pop_last: case BMethod::move_to_first: case BMethod::move_to_last:
                expr(recv, Use::change);
                for (const Arg& a : e->args) expr(a.value, Use::view);
                return;
            default:
                expr(recv, Use::view);
                for (const Arg& a : e->args) expr(a.value, Use::view);
                return;
        }
    }

    void if_expr(Expr* e, Use u) {
        Use bu = branch_use(e, u);
        if (e->let_pattern) {
            if_let(e, bu);
            return;
        }
        expr(e->cond, Use::view);
        State before = st_;
        block(e->block, bu);
        State then = st_;
        st_ = before;
        if (e->else_) expr(e->else_, bu);
        st_ = join(then, st_);
    }

    // `if let p = v { ... } else { ... }`: a `match` with two arms, the second binding nothing.
    void if_let(Expr* e, Use bu) {
        Expr* v = e->cond;
        Local* root = place_root(v);
        if (scan_) {
            scopes_.emplace_back();
            bind(e->let_pattern, false, nullptr, "");
            block(e->block, bu);
            end_scope();
            if (e->else_) expr(e->else_, bu);
            e->let_consumes = binds_moved(e->let_pattern);
            bool whole = root && is_local_name(v);
            if (e->let_consumes && whole) moved_any_.insert(root);
            expr(v, root ? Use::view : Use::consume);
            return;
        }
        scopes_.emplace_back();
        Local* hidden = nullptr;
        if (root) {
            expr(v, Use::view);
        } else {
            expr(v, Use::consume);
            if (!copyable(v->type)) {
                if (!e->hidden) e->hidden = make_hidden(v);
                hidden = e->hidden;
                declare(hidden);
            }
        }
        State start = st_;
        bool whole = root && is_local_name(v) && root->owns;
        e->let_consumes = e->let_consumes && (whole || hidden);
        scopes_.emplace_back();
        if (e->let_consumes) take(whole ? root : hidden, v->span);
        bind(e->let_pattern, e->let_consumes, e->let_consumes || !root ? nullptr : root, place_text(v));
        block(e->block, bu);
        e->let_drops = drops_from(scopes_.size() - 1);
        unborrow_scope();
        end_scope();
        State then = st_;
        st_ = start;
        if (e->else_) expr(e->else_, bu);
        st_ = join(then, st_);
        finish_hidden(e, hidden);
        end_scope();
    }

    void finish_hidden(Expr* e, Local* hidden) {
        if (!hidden) return;
        std::uint8_t x = get(hidden);
        e->drop_hidden = !st_.dead && x != moved;
        if (x == maybe) hidden->clear_on_move = true;
    }

    void match_expr(Expr* e, Use u) {
        Use bu = branch_use(e, u);
        Expr* v = e->scrutinee;
        Local* root = place_root(v);
        if (scan_) {
            bool any = false;
            for (MatchArm& a : e->arms) {
                scopes_.emplace_back();
                bind(a.pattern, false, nullptr, "");
                if (a.guard) expr(a.guard, Use::view);
                expr(a.body, bu);
                end_scope();
                a.consumes = binds_moved(a.pattern);
                any = any || a.consumes;
            }
            if (any && root && is_local_name(v)) moved_any_.insert(root);
            expr(v, root ? Use::view : Use::consume);
            return;
        }
        scopes_.emplace_back();
        Local* hidden = nullptr;
        if (root) {
            expr(v, Use::view);
        } else {
            expr(v, Use::consume);
            if (!copyable(v->type)) {
                if (!e->hidden) e->hidden = make_hidden(v);
                hidden = e->hidden;
                declare(hidden);
            }
        }
        bool whole = root && is_local_name(v) && root->owns;
        State start = st_;
        State out = dead_state();
        for (MatchArm& a : e->arms) {
            st_ = start;
            scopes_.emplace_back();
            a.consumes = a.consumes && (whole || hidden);
            if (a.consumes) take(whole ? root : hidden, v->span);
            bind(a.pattern, a.consumes, a.consumes || !root ? nullptr : root, place_text(v));
            if (a.guard) expr(a.guard, Use::view);
            expr(a.body, bu);
            a.drops = drops_from(scopes_.size() - 1);
            unborrow_scope();
            end_scope();
            out = join(out, st_);
        }
        st_ = out;
        finish_hidden(e, hidden);
        end_scope();
    }

    MoveContext ctx_;
    bool report_;
    bool scan_ = false;
    int muted_ = 0;
    bool noted_ = false;
    State st_;
    std::unordered_map<const Local*, std::uint32_t> ids_;
    std::vector<Local*> locals_;
    std::vector<Span> moved_at_;
    std::vector<bool> reported_;
    std::vector<std::vector<Local*>> scopes_;
    std::vector<Loop> loops_;
    std::vector<Borrow> borrows_;
    std::vector<std::size_t> borrow_scopes_;  // for each borrow made by a name, its scope's depth
    std::unordered_set<const Local*> moved_any_;
    std::unordered_map<const Local*, ViewOf> view_of_;
    std::unordered_map<const Local*, std::string> part_of_;
};

}  // namespace

void check_moves(MoveContext ctx, Item* fn, bool report) {
    if (!fn->body) return;
    Pass(ctx, report).run(fn);
}

}  // namespace calx
