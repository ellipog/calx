/* `SlotMap<T>`, as a template (CLX-69). Define
 *
 *   CX_SL_T          the map's type: a struct of `ptr`, `len`, `cap`, `free` and `count`, whose
 *                    `ptr` is a `struct CX_SL_NAME_slot*`
 *   CX_SL_NAME       the start of its functions' names, as `cx_slots_i32`
 *   CX_SL_E          the value type
 *   CX_SL_DROP(p)    optional: free what the value `p` points to owns
 *   CX_SL_COPY(p)    optional, with CX_SL_DROP: a copy of the value `p` points to
 *
 * and include this file, once for each map type. It undefines them all.
 *
 * The map is an array of `len` slots, each a generation and a value. A handle is a slot's index
 * and the generation it had when its value went in, and it reaches the value while the slot
 * still has that generation:
 *
 * - a slot's first generation is 1, and each insert and each remove adds one, so a slot with a
 *   value has an odd generation, and every handle has one, since only `insert` makes them;
 * - a removed value's slot goes on the front of a list through the free ones, `free` and each
 *   slot's `next` holding the next's index plus one, or 0 at the end, and the next insert takes
 *   the front one, whose memory was the last touched;
 * - a slot whose generation would go past 2^32 - 1 back round to 0 is retired instead, never
 *   given out again, so no handle of it can ever reach a value again. It takes four billion
 *   inserts and removes of one slot.
 *
 * `count` is how many values it holds. A slot map's index is 32 bits, so it holds at most
 * 2^32 - 1 slots. */

#define CX_SL_CAT2(a, b) a##b
#define CX_SL_CAT(a, b) CX_SL_CAT2(a, b)
#define CX_SL_F(x) CX_SL_CAT(CX_SL_NAME, CX_SL_CAT(_, x))
#define CX_SL_S struct CX_SL_F(slot)
#ifdef CX_SL_DROP
#define CX_SL_HAS_DROP
#else
#define CX_SL_DROP(p) ((void)0)
#define CX_SL_COPY(p) (*(p))
#endif

CX_SL_S {
    cx_u32 gen;
    cx_u32 next;
    CX_SL_E v;
};

/* The value `h` reaches, or null if its value was removed. */
CX_INLINE CX_SL_E* CX_SL_F(find)(const CX_SL_T* m, cx_handle h) {
    if (h.index >= m->len || m->ptr[h.index].gen != h.gen) return 0;
    return &m->ptr[h.index].v;
}

/* `m[h]`: the value `h` reaches, or a panic at `at` if its value was removed. */
CX_INLINE CX_SL_E* CX_SL_F(at)(const CX_SL_T* m, cx_handle h, const char* at) {
    if (__builtin_expect(h.index >= m->len || m->ptr[h.index].gen != h.gen, 0)) cx_panic_handle(h, at);
    return &m->ptr[h.index].v;
}

/* Room for `extra` more slots past `len`. */
CX_INLINE void CX_SL_F(grow)(CX_SL_T* m, cx_usize extra) {
    if (__builtin_expect(extra > 0xFFFFFFFFu - m->len, 0)) cx_panic_text("<memory>", "a slot map holds at most 4294967295 values");
    m->ptr = (CX_SL_S*)cx_grow(m->ptr, &m->cap, m->len, extra, sizeof(CX_SL_S));
}

/* Puts `v` in the front free slot, or in a new one: its handle. */
CX_INLINE cx_handle CX_SL_F(insert)(CX_SL_T* m, CX_SL_E v) {
    cx_u32 i;
    if (m->free) {
        i = m->free - 1;
        m->free = m->ptr[i].next;
        m->ptr[i].gen++;
    } else {
        if (__builtin_expect(m->len >= m->cap, 0)) CX_SL_F(grow)(m, 1);
        i = (cx_u32)m->len++;
        m->ptr[i].gen = 1;
    }
    m->ptr[i].v = v;
    m->count++;
    return (cx_handle){i, m->ptr[i].gen};
}

/* Makes slot `i`, which has a value, free: its generation goes on, and it goes on the front of
 * the free list, unless that retires it. */
CX_INLINE void CX_SL_F(vacate)(CX_SL_T* m, cx_u32 i) {
    CX_SL_S* s = &m->ptr[i];
    if (++s->gen) {
        s->next = m->free;
        m->free = i + 1;
    }
    m->count--;
}

/* Takes out the value `h` reaches into `*out`, or false if it was removed already. */
CX_INLINE cx_bool CX_SL_F(take)(CX_SL_T* m, cx_handle h, CX_SL_E* out) {
    CX_SL_E* v = CX_SL_F(find)(m, h);
    if (!v) return 0;
    *out = *v;
    CX_SL_F(vacate)(m, h.index);
    return 1;
}

/* Frees every value, and every handle goes stale. The slots stay, with their generations, so an
 * old handle can't reach a value put in its slot later; the free list goes up from the first. */
static inline void CX_SL_F(clear)(CX_SL_T* m) {
    m->free = 0;
    for (cx_usize i = m->len; i-- > 0;) {
        CX_SL_S* s = &m->ptr[i];
        if (s->gen & 1) {
            CX_SL_DROP(&s->v);
            s->gen++;
        }
        if (s->gen) {
            s->next = m->free;
            m->free = (cx_u32)i + 1;
        }
    }
    m->count = 0;
}

/* Room for `n` more values without growing: free slots count, and a retired one, which is rare,
 * is counted as one too, so the room can be a little less. */
CX_INLINE void CX_SL_F(reserve)(CX_SL_T* m, cx_usize n) {
    cx_usize spare = m->len - m->count;
    if (n > spare && n - spare > m->cap - m->len) CX_SL_F(grow)(m, n - spare);
}

/* Frees the values and the slots. */
static inline void CX_SL_F(drop)(CX_SL_T* m) {
#ifdef CX_SL_HAS_DROP
    for (cx_usize i = 0; i < m->len; i++) {
        if (m->ptr[i].gen & 1) CX_SL_DROP(&m->ptr[i].v);
    }
#endif
    if (m->cap) cx_free(m->ptr, m->cap * sizeof(CX_SL_S));
}

/* A copy, with the same slots, generations and free list, so every handle reaches the same value
 * in it as in `m`. */
static inline CX_SL_T CX_SL_F(copy)(const CX_SL_T* m) {
    CX_SL_T c = *m;
    c.cap = m->len;
    c.ptr = 0;
    if (m->len) {
        c.ptr = (CX_SL_S*)cx_alloc(m->len * sizeof(CX_SL_S));
#ifdef CX_SL_HAS_DROP
        for (cx_usize i = 0; i < m->len; i++) {
            c.ptr[i].gen = m->ptr[i].gen;
            c.ptr[i].next = m->ptr[i].next;
            if (m->ptr[i].gen & 1) c.ptr[i].v = CX_SL_COPY(&m->ptr[i].v);
        }
#else
        __builtin_memcpy(c.ptr, m->ptr, m->len * sizeof(CX_SL_S));
#endif
    }
    return c;
}

/* The handles of the values, in slot order, into `out`, which has room for `count`. */
CX_INLINE void CX_SL_F(handles)(const CX_SL_T* m, cx_handle* out) {
    cx_usize k = 0;
    for (cx_usize i = 0; i < m->len; i++) {
        if (m->ptr[i].gen & 1) out[k++] = (cx_handle){(cx_u32)i, m->ptr[i].gen};
    }
}

#undef CX_SL_T
#undef CX_SL_NAME
#undef CX_SL_E
#undef CX_SL_DROP
#undef CX_SL_COPY
#undef CX_SL_HAS_DROP
#undef CX_SL_S
#undef CX_SL_F
#undef CX_SL_CAT
#undef CX_SL_CAT2
