//! The Rust reference for Calx's tick-loop benchmark (`docs/benchmarks.md`, `P3.1`).
//!
//! Idiomatic, safe Rust: no `unsafe`, bounds checks left in, wrapping arithmetic where Java wraps.
//! It implements exactly the algorithms in `docs/benchmarks.md`, and prints the checksum the Calx
//! version must match.
//!
//!     tickbench [--threads N] [--warmup 200] [--ticks 1000] [--stats]

use std::collections::HashMap;
use std::hash::{BuildHasherDefault, Hasher};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Mutex, RwLock, RwLockReadGuard};
use std::time::Instant;

const SEED: i64 = 20261027;
const STATES: i32 = 30000;
const WORLD_CHUNKS: i32 = 16;
const SECTIONS: usize = 24;
const MIN_Y: i32 = -64;
const MAX_Y: i32 = 320;
const REGION_CHUNKS: i32 = 4;
const REGIONS: usize = 16;
const ENTITIES: usize = 4096;
const MESSAGES: usize = 64;
const RANDOM_TICKS: usize = 3;
const SECTION_SIZE: usize = 4096;
const EPS: f64 = 1.0e-7;

// ---------------------------------------------------------------------------------------------
// java.util.Random, as its Javadoc specifies it.

struct JavaRandom {
    seed: i64,
}

impl JavaRandom {
    const MULTIPLIER: i64 = 0x5DEECE66D;
    const ADDEND: i64 = 0xB;
    const MASK: i64 = (1 << 48) - 1;

    fn new(seed: i64) -> Self {
        JavaRandom { seed: (seed ^ Self::MULTIPLIER) & Self::MASK }
    }

    fn next(&mut self, bits: u32) -> i32 {
        self.seed = self.seed.wrapping_mul(Self::MULTIPLIER).wrapping_add(Self::ADDEND) & Self::MASK;
        (self.seed >> (48 - bits)) as i32
    }

    fn next_int(&mut self, bound: i32) -> i32 {
        if bound & bound.wrapping_neg() == bound {
            return ((bound as i64 * self.next(31) as i64) >> 31) as i32;
        }
        loop {
            let bits = self.next(31);
            let val = bits % bound;
            if bits.wrapping_sub(val).wrapping_add(bound - 1) >= 0 {
                return val;
            }
        }
    }

    fn next_long(&mut self) -> i64 {
        let high = (self.next(32) as i64) << 32;
        high.wrapping_add(self.next(32) as i64)
    }

    fn next_double(&mut self) -> f64 {
        let high = (self.next(26) as i64) << 27;
        (high + self.next(27) as i64) as f64 * (1.0 / (1u64 << 53) as f64)
    }
}

// ---------------------------------------------------------------------------------------------
// The checksum: 64-bit FNV-1a over u64 values, each fed as 8 little-endian bytes.

#[derive(Clone, Copy)]
struct Fnv(u64);

impl Fnv {
    fn new() -> Self {
        Fnv(0xcbf29ce484222325)
    }

    fn u64(&mut self, v: u64) {
        for b in v.to_le_bytes() {
            self.0 ^= b as u64;
            self.0 = self.0.wrapping_mul(0x100000001b3);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The fast native map's hasher: one multiply per word, as rustc's FxHasher does.

#[derive(Default)]
struct FxHasher(u64);

impl Hasher for FxHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.write_u64(b as u64);
        }
    }

    fn write_u64(&mut self, word: u64) {
        self.0 = (self.0.rotate_left(5) ^ word).wrapping_mul(0x517cc1b727220a95);
    }

    fn write_i64(&mut self, word: i64) {
        self.write_u64(word as u64);
    }
}

type FxMap<K, V> = HashMap<K, V, BuildHasherDefault<FxHasher>>;

// ---------------------------------------------------------------------------------------------
// Positions, packed as vanilla packs them.

fn block_key(x: i32, y: i32, z: i32) -> i64 {
    ((x as i64 & 0x3FF_FFFF) << 38) | ((z as i64 & 0x3FF_FFFF) << 12) | (y as i64 & 0xFFF)
}

fn block_x(key: i64) -> i32 {
    (key >> 38) as i32
}

fn block_y(key: i64) -> i32 {
    ((key << 52) >> 52) as i32
}

fn block_z(key: i64) -> i32 {
    ((key << 26) >> 38) as i32
}

fn chunk_key(cx: i32, cz: i32) -> i64 {
    (cx as i64 & 0xFFFF_FFFF) | ((cz as i64) << 32)
}

fn region_of(cx: i32, cz: i32) -> usize {
    ((cz / REGION_CHUNKS) * (WORLD_CHUNKS / REGION_CHUNKS) + cx / REGION_CHUNKS) as usize
}

// ---------------------------------------------------------------------------------------------
// Bit storage: values packed into u64 words, never spanning two.

struct BitStorage {
    bits: u32,
    per_word: usize,
    divide_mul: u64,
    mask: u64,
    data: Vec<u64>,
}

impl BitStorage {
    fn new(bits: u32) -> Self {
        if bits == 0 {
            return BitStorage { bits, per_word: 0, divide_mul: 0, mask: 0, data: Vec::new() };
        }
        let per_word = 64 / bits as usize;
        BitStorage {
            bits,
            per_word,
            // floor(2^32 / per_word) + 1, so (i * divide_mul) >> 32 == i / per_word for i < 4096.
            divide_mul: (1u64 << 32) / per_word as u64 + 1,
            mask: (1u64 << bits) - 1,
            data: vec![0; SECTION_SIZE.div_ceil(per_word)],
        }
    }

    #[inline]
    fn locate(&self, i: usize) -> (usize, u32) {
        let word = ((i as u64 * self.divide_mul) >> 32) as usize;
        (word, ((i - word * self.per_word) as u32) * self.bits)
    }

    #[inline]
    fn get(&self, i: usize) -> u32 {
        let (word, shift) = self.locate(i);
        ((self.data[word] >> shift) & self.mask) as u32
    }

    #[inline]
    fn set(&mut self, i: usize, v: u32) {
        let (word, shift) = self.locate(i);
        let w = &mut self.data[word];
        *w = (*w & !(self.mask << shift)) | ((v as u64) << shift);
    }
}

// ---------------------------------------------------------------------------------------------
// The hash palette's ID-to-index map: open addressing with linear probing.

struct IdIndex {
    slots: Vec<u16>, // palette index + 1, or 0 for empty
    shift: u32,
}

impl IdIndex {
    fn new(bits: u32, states: &[u32]) -> Self {
        let capacity_log = bits + 1; // twice the palette's capacity
        let mut index = IdIndex { slots: vec![0; 1 << capacity_log], shift: 32 - capacity_log };
        for (i, &s) in states.iter().enumerate() {
            index.insert(s, i);
        }
        index
    }

    #[inline]
    fn home(&self, id: u32) -> usize {
        (id.wrapping_mul(0x9E3779B9) >> self.shift) as usize
    }

    fn find(&self, id: u32, states: &[u32]) -> Option<u32> {
        let mask = self.slots.len() - 1;
        let mut h = self.home(id);
        loop {
            let e = self.slots[h];
            if e == 0 {
                return None;
            }
            if states[e as usize - 1] == id {
                return Some(e as u32 - 1);
            }
            h = (h + 1) & mask;
        }
    }

    fn insert(&mut self, id: u32, index: usize) {
        let mask = self.slots.len() - 1;
        let mut h = self.home(id);
        while self.slots[h] != 0 {
            h = (h + 1) & mask;
        }
        self.slots[h] = index as u16 + 1;
    }
}

// ---------------------------------------------------------------------------------------------
// A section: 4,096 block states in one of vanilla's four layouts.

enum Palette {
    Single(u32),
    Linear(Vec<u32>),
    Hash(Vec<u32>, IdIndex),
    Global,
}

struct Section {
    palette: Palette,
    storage: BitStorage,
}

const LINEAR_BITS: u32 = 4;
const HASH_MIN_BITS: u32 = 5;
const HASH_MAX_BITS: u32 = 8;
const GLOBAL_BITS: u32 = 15;

fn ceil_log2(n: usize) -> u32 {
    usize::BITS - (n - 1).leading_zeros()
}

impl Section {
    /// A section whose palette is `states`, in that order, with every index 0.
    fn with_states(states: Vec<u32>) -> Self {
        let n = states.len();
        if n == 1 {
            Section { palette: Palette::Single(states[0]), storage: BitStorage::new(0) }
        } else if n <= 1 << LINEAR_BITS {
            Section { palette: Palette::Linear(states), storage: BitStorage::new(LINEAR_BITS) }
        } else if n <= 1 << HASH_MAX_BITS {
            let bits = ceil_log2(n).max(HASH_MIN_BITS);
            let index = IdIndex::new(bits, &states);
            Section { palette: Palette::Hash(states, index), storage: BitStorage::new(bits) }
        } else {
            Section { palette: Palette::Global, storage: BitStorage::new(GLOBAL_BITS) }
        }
    }

    #[inline]
    fn get(&self, i: usize) -> u32 {
        match &self.palette {
            Palette::Single(v) => *v,
            Palette::Linear(states) | Palette::Hash(states, _) => states[self.storage.get(i) as usize],
            Palette::Global => self.storage.get(i),
        }
    }

    fn set(&mut self, i: usize, id: u32) {
        let index = match &mut self.palette {
            Palette::Single(v) => {
                if *v == id {
                    return;
                }
                let states = vec![*v, id];
                self.resize(Palette::Linear(states), LINEAR_BITS);
                1
            }
            Palette::Linear(states) => match states.iter().position(|&s| s == id) {
                Some(j) => j as u32,
                None if states.len() < 1 << LINEAR_BITS => {
                    states.push(id);
                    states.len() as u32 - 1
                }
                None => {
                    let mut grown = states.clone();
                    grown.push(id);
                    let index = IdIndex::new(HASH_MIN_BITS, &grown);
                    self.resize(Palette::Hash(grown, index), HASH_MIN_BITS);
                    1 << LINEAR_BITS
                }
            },
            Palette::Hash(states, index) => match index.find(id, states) {
                Some(j) => j,
                None if states.len() < 1 << self.storage.bits => {
                    states.push(id);
                    index.insert(id, states.len() - 1);
                    states.len() as u32 - 1
                }
                None if self.storage.bits < HASH_MAX_BITS => {
                    let bits = self.storage.bits + 1;
                    let mut grown = states.clone();
                    grown.push(id);
                    let index = IdIndex::new(bits, &grown);
                    let j = grown.len() as u32 - 1;
                    self.resize(Palette::Hash(grown, index), bits);
                    j
                }
                None => {
                    self.resize(Palette::Global, GLOBAL_BITS);
                    id
                }
            },
            Palette::Global => id,
        };
        self.storage.set(i, index);
    }

    /// Re-encodes all 4,096 values into a larger layout. The new palette keeps the old one's
    /// entries at the same indices, so a palette index carries over; the global layout stores IDs.
    fn resize(&mut self, palette: Palette, bits: u32) {
        let mut storage = BitStorage::new(bits);
        let global = matches!(palette, Palette::Global);
        for i in 0..SECTION_SIZE {
            let v = match &self.palette {
                Palette::Single(_) => 0,
                Palette::Linear(states) | Palette::Hash(states, _) => {
                    let j = self.storage.get(i);
                    if global { states[j as usize] } else { j }
                }
                Palette::Global => unreachable!("the global layout never resizes"),
            };
            storage.set(i, v);
        }
        self.palette = palette;
        self.storage = storage;
    }

    fn sum(&self) -> u64 {
        (0..SECTION_SIZE).map(|i| self.get(i) as u64).sum()
    }

    fn kind(&self) -> usize {
        match self.palette {
            Palette::Single(_) => 0,
            Palette::Linear(_) => 1,
            Palette::Hash(..) => 2,
            Palette::Global => 3,
        }
    }
}

fn section_index(lx: i32, ly: i32, lz: i32) -> usize {
    ((ly << 8) | (lz << 4) | lx) as usize
}

// ---------------------------------------------------------------------------------------------
// A Java-compatible HashMap<Long, Integer>: Java's hash spreading, power-of-two table, tail
// insertion, load factor 0.75 and the order-preserving resize split. A bin that would reach
// Java's treeify threshold on a table of 64 or more aborts the run: the benchmark's workload
// never makes one, and supporting tree bins here would be dead code.

const NIL: u32 = u32::MAX;

struct Node {
    hash: i32,
    key: i64,
    value: i32,
    next: u32,
}

struct JavaHashMap {
    table: Vec<u32>,
    nodes: Vec<Node>,
    free: Vec<u32>,
    size: usize,
    threshold: usize,
}

impl JavaHashMap {
    fn new() -> Self {
        JavaHashMap { table: Vec::new(), nodes: Vec::new(), free: Vec::new(), size: 0, threshold: 0 }
    }

    fn hash(key: i64) -> i32 {
        let h = (key ^ ((key as u64) >> 32) as i64) as i32;
        h ^ ((h as u32) >> 16) as i32
    }

    fn alloc(&mut self, node: Node) -> u32 {
        match self.free.pop() {
            Some(i) => {
                self.nodes[i as usize] = node;
                i
            }
            None => {
                self.nodes.push(node);
                self.nodes.len() as u32 - 1
            }
        }
    }

    fn put(&mut self, key: i64, value: i32) {
        if self.table.is_empty() {
            self.resize();
        }
        let hash = Self::hash(key);
        let bucket = (hash as u32 as usize) & (self.table.len() - 1);
        let mut p = self.table[bucket];
        if p == NIL {
            let n = self.alloc(Node { hash, key, value, next: NIL });
            self.table[bucket] = n;
        } else {
            let mut bin_count = 0;
            loop {
                let node = &mut self.nodes[p as usize];
                if node.hash == hash && node.key == key {
                    node.value = value;
                    return;
                }
                if node.next == NIL {
                    let n = self.alloc(Node { hash, key, value, next: NIL });
                    self.nodes[p as usize].next = n;
                    if bin_count >= 7 {
                        // Java's treeifyBin: on a table under 64 it resizes instead.
                        if self.table.len() < 64 {
                            self.resize();
                        } else {
                            eprintln!("tickbench: a scheduled-tick bin reached Java's treeify threshold");
                            std::process::exit(2);
                        }
                    }
                    break;
                }
                p = node.next;
                bin_count += 1;
            }
        }
        self.size += 1;
        if self.size > self.threshold {
            self.resize();
        }
    }

    fn remove(&mut self, key: i64) {
        let hash = Self::hash(key);
        let bucket = (hash as u32 as usize) & (self.table.len() - 1);
        let mut prev = NIL;
        let mut p = self.table[bucket];
        while p != NIL {
            let node = &self.nodes[p as usize];
            if node.hash == hash && node.key == key {
                let next = node.next;
                if prev == NIL {
                    self.table[bucket] = next;
                } else {
                    self.nodes[prev as usize].next = next;
                }
                self.free.push(p);
                self.size -= 1;
                return;
            }
            prev = p;
            p = node.next;
        }
    }

    fn resize(&mut self) {
        let old = self.table.len();
        let new = if old == 0 { 16 } else { old * 2 };
        self.threshold = new * 3 / 4;
        let mut table = vec![NIL; new];
        for j in 0..old {
            let (mut lo_head, mut lo_tail, mut hi_head, mut hi_tail) = (NIL, NIL, NIL, NIL);
            let mut p = self.table[j];
            while p != NIL {
                let next = self.nodes[p as usize].next;
                self.nodes[p as usize].next = NIL;
                let (head, tail) = if self.nodes[p as usize].hash as u32 as usize & old == 0 {
                    (&mut lo_head, &mut lo_tail)
                } else {
                    (&mut hi_head, &mut hi_tail)
                };
                if *tail == NIL {
                    *head = p;
                } else {
                    self.nodes[*tail as usize].next = p;
                }
                *tail = p;
                p = next;
            }
            table[j] = lo_head;
            table[j + old] = hi_head;
        }
        self.table = table;
    }

    /// The keys whose value is at most `tick`, in Java's iteration order.
    fn due(&self, tick: i32, out: &mut Vec<i64>) {
        for &head in &self.table {
            let mut p = head;
            while p != NIL {
                let node = &self.nodes[p as usize];
                if node.value <= tick {
                    out.push(node.key);
                }
                p = node.next;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The world: regions own their chunks, scheduled ticks, entities and random streams.

struct Chunk {
    cx: i32,
    cz: i32,
    sections: Vec<Section>,
}

#[derive(Clone, Copy)]
struct Entity {
    id: u32,
    x: f64,
    y: f64,
    z: f64,
    vx: f64,
    vy: f64,
    vz: f64,
}

#[derive(Clone, Copy)]
struct Message {
    pos: i64,
    value: i64,
}

struct Blocks {
    chunks: Vec<Chunk>,
}

struct State {
    rng: JavaRandom,
    scheduled: JavaHashMap,
    tick_hash: Fnv,
    message_hash: Fnv,
    inbox: Vec<Message>,
    outbox: Vec<(usize, Message)>,
    entities: Vec<Entity>,
    handoffs: Vec<(usize, Entity)>,
    per_chunk: FxMap<i64, u32>,
    due: Vec<i64>,
}

struct Region {
    blocks: RwLock<Blocks>,
    state: Mutex<State>,
}

struct World {
    regions: Vec<Region>,
    /// Chunk position to (region, chunk index within it): the fast native map.
    chunk_map: FxMap<i64, (usize, usize)>,
}

fn generate() -> World {
    let mut rng = JavaRandom::new(SEED);
    let mut regions: Vec<Blocks> = (0..REGIONS).map(|_| Blocks { chunks: Vec::new() }).collect();
    let mut chunk_map = FxMap::default();
    // Each region lists its chunks in x, then z order, the same as generation.
    for cx in 0..WORLD_CHUNKS {
        for cz in 0..WORLD_CHUNKS {
            let r = region_of(cx, cz);
            chunk_map.insert(chunk_key(cx, cz), (r, regions[r].chunks.len()));
            regions[r].chunks.push(Chunk { cx, cz, sections: Vec::with_capacity(SECTIONS) });
        }
    }
    for cx in 0..WORLD_CHUNKS {
        for cz in 0..WORLD_CHUNKS {
            let (r, c) = chunk_map[&chunk_key(cx, cz)];
            for _ in 0..SECTIONS {
                let roll = rng.next_int(100);
                let n = if roll < 40 {
                    1
                } else if roll < 80 {
                    2 + rng.next_int(15)
                } else if roll < 95 {
                    17 + rng.next_int(240)
                } else {
                    257 + rng.next_int(768)
                } as usize;
                let mut states: Vec<u32> = Vec::with_capacity(n);
                while states.len() < n {
                    let s = rng.next_int(STATES) as u32;
                    if !states.contains(&s) {
                        states.push(s);
                    }
                }
                let mut section = Section::with_states(states.clone());
                for i in 0..SECTION_SIZE {
                    let j = rng.next_int(n as i32) as usize;
                    match section.palette {
                        Palette::Single(_) => {}
                        Palette::Global => section.storage.set(i, states[j]),
                        _ => section.storage.set(i, j as u32),
                    }
                }
                regions[r].chunks[c].sections.push(section);
            }
        }
    }
    let mut entities: Vec<Vec<Entity>> = (0..REGIONS).map(|_| Vec::new()).collect();
    for id in 0..ENTITIES as u32 {
        let x = 0.3 + rng.next_double() * 255.4;
        let y = MAX_Y as f64 + rng.next_double() * 64.0;
        let z = 0.3 + rng.next_double() * 255.4;
        let vx = rng.next_double() - 0.5;
        let vy = rng.next_double() - 0.5;
        let vz = rng.next_double() - 0.5;
        let r = region_of((x.floor() as i32) >> 4, (z.floor() as i32) >> 4);
        entities[r].push(Entity { id, x, y, z, vx, vy, vz });
    }
    let regions = regions
        .into_iter()
        .zip(entities)
        .enumerate()
        .map(|(k, (blocks, entities))| Region {
            blocks: RwLock::new(blocks),
            state: Mutex::new(State {
                rng: JavaRandom::new(SEED + k as i64),
                scheduled: JavaHashMap::new(),
                tick_hash: Fnv::new(),
                message_hash: Fnv::new(),
                inbox: Vec::new(),
                outbox: Vec::new(),
                entities,
                handoffs: Vec::new(),
                per_chunk: FxMap::default(),
                due: Vec::new(),
            }),
        })
        .collect();
    World { regions, chunk_map }
}

// ---------------------------------------------------------------------------------------------
// Pass 1: each region's block work.

fn block_pass(blocks: &mut Blocks, st: &mut State, tick: i32) {
    // Messages delivered at the last barrier, in delivery order.
    for m in st.inbox.drain(..) {
        st.message_hash.u64(m.pos as u64);
        st.message_hash.u64(m.value as u64);
    }

    // Scheduled ticks that are due, in the map's iteration order.
    let mut due = std::mem::take(&mut st.due);
    due.clear();
    st.scheduled.due(tick, &mut due);
    for &key in &due {
        st.scheduled.remove(key);
        let (x, y, z) = (block_x(key), block_y(key), block_z(key));
        let local = (((x >> 4) % REGION_CHUNKS) * REGION_CHUNKS + (z >> 4) % REGION_CHUNKS) as usize;
        let section = &mut blocks.chunks[local].sections[((y - MIN_Y) >> 4) as usize];
        let i = section_index(x & 15, y & 15, z & 15);
        let id = section.get(i);
        section.set(i, (id + 1) % STATES as u32);
        st.tick_hash.u64(key as u64);
    }
    st.due = due;

    // Random ticks, then one full read, for every section.
    for chunk in &mut blocks.chunks {
        for (sy, section) in chunk.sections.iter_mut().enumerate() {
            for _ in 0..RANDOM_TICKS {
                let i = st.rng.next_int(SECTION_SIZE as i32);
                let (lx, lz, ly) = (i & 15, (i >> 4) & 15, i >> 8);
                let id = section.get(i as usize);
                let sum = id
                    + section.get(section_index((lx - 1).max(0), ly, lz))
                    + section.get(section_index((lx + 1).min(15), ly, lz))
                    + section.get(section_index(lx, (ly - 1).max(0), lz))
                    + section.get(section_index(lx, (ly + 1).min(15), lz))
                    + section.get(section_index(lx, ly, (lz - 1).max(0)))
                    + section.get(section_index(lx, ly, (lz + 1).min(15)));
                if sum % 7 == 0 {
                    section.set(i as usize, (id * 31 + 7) % STATES as u32);
                    let key = block_key(chunk.cx * 16 + lx, MIN_Y + sy as i32 * 16 + ly, chunk.cz * 16 + lz);
                    st.scheduled.put(key, tick + 2);
                }
            }
            st.tick_hash.u64(section.sum());
        }
    }

    // Messages to random regions.
    for _ in 0..MESSAGES {
        let dest = st.rng.next_int(REGIONS as i32) as usize;
        let pos = st.rng.next_long();
        let value = st.rng.next_long();
        st.outbox.push((dest, Message { pos, value }));
    }
}

// ---------------------------------------------------------------------------------------------
// Pass 2: each region's entity physics, reading blocks anywhere in the world.

struct Reader<'a> {
    blocks: &'a [RwLockReadGuard<'a, Blocks>],
    chunk_map: &'a FxMap<i64, (usize, usize)>,
    // The chunk the entity started the tick in, looked up once.
    cx: i32,
    cz: i32,
    home: (usize, usize),
}

impl Reader<'_> {
    fn solid(&self, bx: i32, by: i32, bz: i32) -> bool {
        // The world border is a wall at every height, so nothing leaves the world.
        let edge = WORLD_CHUNKS * 16;
        if bx < 0 || bz < 0 || bx >= edge || bz >= edge || by < MIN_Y {
            return true;
        }
        if by >= MAX_Y {
            return false;
        }
        let (cx, cz) = (bx >> 4, bz >> 4);
        let (r, c) = if cx == self.cx && cz == self.cz { self.home } else { self.chunk_map[&chunk_key(cx, cz)] };
        let section = &self.blocks[r].chunks[c].sections[((by - MIN_Y) >> 4) as usize];
        section.get(section_index(bx & 15, by & 15, bz & 15)) != 0
    }
}

#[derive(Clone, Copy)]
enum Axis {
    X,
    Y,
    Z,
}

struct Aabb {
    min: [f64; 3],
    max: [f64; 3],
}

impl Aabb {
    fn of(e: &Entity) -> Self {
        Aabb { min: [e.x - 0.3, e.y, e.z - 0.3], max: [e.x + 0.3, e.y + 1.8, e.z + 0.3] }
    }
}

/// How far the box can move by `d` along `axis` before it meets a solid block. Blocks it already
/// overlaps by more than EPS are ignored, as in vanilla.
fn clip(reader: &Reader, b: &Aabb, axis: Axis, d: f64) -> f64 {
    let a = axis as usize;
    let (u, v) = match axis {
        Axis::X => (1, 2),
        Axis::Y => (0, 2),
        Axis::Z => (0, 1),
    };
    let (u0, u1) = ((b.min[u] + EPS).floor() as i32, (b.max[u] - EPS).floor() as i32);
    let (v0, v1) = ((b.min[v] + EPS).floor() as i32, (b.max[v] - EPS).floor() as i32);
    let hit = |layer: i32| {
        for p in u0..=u1 {
            for q in v0..=v1 {
                let mut c = [0; 3];
                c[a] = layer;
                c[u] = p;
                c[v] = q;
                if reader.solid(c[0], c[1], c[2]) {
                    return true;
                }
            }
        }
        false
    };
    if d < 0.0 {
        let first = (b.min[a] + EPS).floor() as i32 - 1;
        let last = (b.min[a] + d + EPS).floor() as i32;
        let mut layer = first;
        while layer >= last {
            if hit(layer) {
                return d.max((layer + 1) as f64 - b.min[a]);
            }
            layer -= 1;
        }
    } else if d > 0.0 {
        let first = (b.max[a] - EPS).floor() as i32 + 1;
        let last = (b.max[a] + d - EPS).floor() as i32;
        for layer in first..=last {
            if hit(layer) {
                return d.min(layer as f64 - b.max[a]);
            }
        }
    }
    d
}

fn step(reader: &Reader, e: &mut Entity, axis: Axis) {
    let d = match axis {
        Axis::X => e.vx,
        Axis::Y => e.vy,
        Axis::Z => e.vz,
    };
    let moved = clip(reader, &Aabb::of(e), axis, d);
    match axis {
        Axis::X => {
            e.x += moved;
            if moved != d {
                e.vx = 0.0;
            }
        }
        Axis::Y => {
            e.y += moved;
            if moved != d {
                e.vy = 0.0;
            }
        }
        Axis::Z => {
            e.z += moved;
            if moved != d {
                e.vz = 0.0;
            }
        }
    }
}

fn entity_pass(world: &World, guards: &[RwLockReadGuard<Blocks>], k: usize, st: &mut State) {
    let mut kept = Vec::with_capacity(st.entities.len());
    for mut e in st.entities.drain(..) {
        let (cx, cz) = ((e.x.floor() as i32) >> 4, (e.z.floor() as i32) >> 4);
        let reader = Reader { blocks: guards, chunk_map: &world.chunk_map, cx, cz, home: world.chunk_map[&chunk_key(cx, cz)] };
        step(&reader, &mut e, Axis::Y);
        if e.vx.abs() < e.vz.abs() {
            step(&reader, &mut e, Axis::Z);
            step(&reader, &mut e, Axis::X);
        } else {
            step(&reader, &mut e, Axis::X);
            step(&reader, &mut e, Axis::Z);
        }
        e.vy = (e.vy - 0.08) * 0.98;
        e.vx *= 0.91;
        e.vz *= 0.91;
        let dest = region_of((e.x.floor() as i32) >> 4, (e.z.floor() as i32) >> 4);
        if dest == k {
            kept.push(e);
        } else {
            st.handoffs.push((dest, e));
        }
    }
    st.entities = kept;

    st.per_chunk.clear();
    for e in &st.entities {
        *st.per_chunk.entry(chunk_key((e.x.floor() as i32) >> 4, (e.z.floor() as i32) >> 4)).or_insert(0) += 1;
    }
    let crowding: u64 = st.per_chunk.values().map(|&c| c as u64 * c as u64).sum();
    st.tick_hash.u64(crowding);
}

// ---------------------------------------------------------------------------------------------
// The barrier: deliver in sender order, apply handoffs, absorb each region's hashes.

fn barrier(world: &World, checksum: &mut Fnv) {
    let mut states: Vec<_> = world.regions.iter().map(|r| r.state.lock().unwrap()).collect();
    for s in 0..REGIONS {
        let outbox = std::mem::take(&mut states[s].outbox);
        for (dest, m) in &outbox {
            states[*dest].inbox.push(*m);
        }
        let mut outbox = outbox;
        outbox.clear();
        states[s].outbox = outbox;
        let handoffs = std::mem::take(&mut states[s].handoffs);
        for (dest, e) in &handoffs {
            states[*dest].entities.push(*e);
        }
        let mut handoffs = handoffs;
        handoffs.clear();
        states[s].handoffs = handoffs;
    }
    for st in states.iter_mut() {
        checksum.u64(st.tick_hash.0);
        checksum.u64(st.message_hash.0);
        st.tick_hash = Fnv::new();
    }
}

// ---------------------------------------------------------------------------------------------
// A small pool: persistent workers that take regions from a shared counter, with a spinning
// barrier between passes. With one thread, everything runs inline.

struct SpinBarrier {
    count: AtomicUsize,
    generation: AtomicUsize,
    parties: usize,
}

impl SpinBarrier {
    fn new(parties: usize) -> Self {
        SpinBarrier { count: AtomicUsize::new(0), generation: AtomicUsize::new(0), parties }
    }

    fn wait(&self) {
        let generation = self.generation.load(Ordering::Acquire);
        if self.count.fetch_add(1, Ordering::AcqRel) + 1 == self.parties {
            self.count.store(0, Ordering::Relaxed);
            self.generation.fetch_add(1, Ordering::Release);
            return;
        }
        let mut spins = 0u32;
        while self.generation.load(Ordering::Acquire) == generation {
            if spins < 1 << 14 {
                std::hint::spin_loop();
                spins += 1;
            } else {
                std::thread::yield_now();
            }
        }
    }
}

const PASS_BLOCKS: usize = 0;
const PASS_ENTITIES: usize = 1;

struct Pool<'a> {
    world: &'a World,
    pass: AtomicUsize,
    tick: AtomicUsize,
    next: AtomicUsize,
    stop: AtomicBool,
    start: SpinBarrier,
    end: SpinBarrier,
}

impl Pool<'_> {
    fn work(&self) {
        let pass = self.pass.load(Ordering::Acquire);
        let tick = self.tick.load(Ordering::Acquire) as i32;
        if pass == PASS_BLOCKS {
            loop {
                let k = self.next.fetch_add(1, Ordering::AcqRel);
                if k >= REGIONS {
                    break;
                }
                let region = &self.world.regions[k];
                block_pass(&mut region.blocks.write().unwrap(), &mut region.state.lock().unwrap(), tick);
            }
        } else {
            let guards: Vec<_> = self.world.regions.iter().map(|r| r.blocks.read().unwrap()).collect();
            loop {
                let k = self.next.fetch_add(1, Ordering::AcqRel);
                if k >= REGIONS {
                    break;
                }
                entity_pass(self.world, &guards, k, &mut self.world.regions[k].state.lock().unwrap());
            }
        }
    }

    fn run_pass(&self, pass: usize, tick: i32) {
        self.pass.store(pass, Ordering::Release);
        self.tick.store(tick as usize, Ordering::Release);
        self.next.store(0, Ordering::Release);
        self.start.wait();
        self.work();
        self.end.wait();
    }
}

// ---------------------------------------------------------------------------------------------

struct Options {
    threads: usize,
    warmup: usize,
    ticks: usize,
    stats: bool,
}

fn parse_args() -> Options {
    let mut o = Options { threads: 1, warmup: 200, ticks: 1000, stats: false };
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < args.len() {
        let value = |i: usize| -> usize {
            args.get(i + 1).and_then(|v| v.parse().ok()).unwrap_or_else(|| {
                eprintln!("tickbench: {} needs a number", args[i]);
                std::process::exit(2);
            })
        };
        match args[i].as_str() {
            "--threads" => o.threads = value(i).max(1),
            "--warmup" => o.warmup = value(i),
            "--ticks" => o.ticks = value(i),
            "--stats" => {
                o.stats = true;
                i += 1;
                continue;
            }
            other => {
                eprintln!("tickbench: unknown option {other}");
                std::process::exit(2);
            }
        }
        i += 2;
    }
    o
}

fn print_stats(world: &World, when: &str) {
    let mut kinds = [0usize; 4];
    for r in &world.regions {
        for c in &r.blocks.read().unwrap().chunks {
            for s in &c.sections {
                kinds[s.kind()] += 1;
            }
        }
    }
    let scheduled: usize = world.regions.iter().map(|r| r.state.lock().unwrap().scheduled.size).sum();
    eprintln!(
        "{when}: sections single {} linear {} hash {} global {}; scheduled ticks {scheduled}",
        kinds[0], kinds[1], kinds[2], kinds[3]
    );
}

fn final_checksum(world: &World, checksum: &mut Fnv) {
    let blocks: Vec<_> = world.regions.iter().map(|r| r.blocks.read().unwrap()).collect();
    for cx in 0..WORLD_CHUNKS {
        for cz in 0..WORLD_CHUNKS {
            let (r, c) = world.chunk_map[&chunk_key(cx, cz)];
            for section in &blocks[r].chunks[c].sections {
                for i in 0..SECTION_SIZE {
                    checksum.u64(section.get(i) as u64);
                }
            }
        }
    }
    let mut entities: Vec<Entity> =
        world.regions.iter().flat_map(|r| r.state.lock().unwrap().entities.clone()).collect();
    assert_eq!(entities.len(), ENTITIES, "an entity was lost or duplicated");
    entities.sort_by_key(|e| e.id);
    for e in &entities {
        checksum.u64(e.x.to_bits());
        checksum.u64(e.y.to_bits());
        checksum.u64(e.z.to_bits());
    }
}

fn main() {
    let o = parse_args();
    let started = Instant::now();
    let world = generate();
    let generated = started.elapsed();
    if o.stats {
        print_stats(&world, "generated");
    }

    let total = o.warmup + o.ticks;
    let mut times = Vec::with_capacity(o.ticks);
    let mut checksum = Fnv::new();
    let pool = Pool {
        world: &world,
        pass: AtomicUsize::new(0),
        tick: AtomicUsize::new(0),
        next: AtomicUsize::new(0),
        stop: AtomicBool::new(false),
        start: SpinBarrier::new(o.threads),
        end: SpinBarrier::new(o.threads),
    };
    std::thread::scope(|s| {
        for _ in 1..o.threads {
            s.spawn(|| loop {
                pool.start.wait();
                if pool.stop.load(Ordering::Acquire) {
                    break;
                }
                pool.work();
                pool.end.wait();
            });
        }
        for tick in 0..total {
            if o.stats && tick == o.warmup {
                print_stats(&world, "after warmup");
            }
            let t0 = Instant::now();
            pool.run_pass(PASS_BLOCKS, tick as i32);
            pool.run_pass(PASS_ENTITIES, tick as i32);
            barrier(&world, &mut checksum);
            if tick >= o.warmup {
                times.push(t0.elapsed().as_nanos() as u64);
            }
        }
        pool.stop.store(true, Ordering::Release);
        pool.start.wait();
    });
    if o.stats {
        print_stats(&world, "end");
    }
    final_checksum(&world, &mut checksum);

    let mean = times.iter().sum::<u64>() as f64 / times.len().max(1) as f64;
    let mut sorted = times.clone();
    sorted.sort_unstable();
    let p99 = sorted.get(((sorted.len() * 99).div_ceil(100)).saturating_sub(1)).copied().unwrap_or(0);
    println!(
        "{{\"impl\": \"rust\", \"threads\": {}, \"warmup\": {}, \"ticks\": {}, \"checksum\": \"{:016x}\", \
         \"mean_tick_us\": {:.3}, \"p99_tick_us\": {:.3}, \"generate_s\": {:.3}, \"total_s\": {:.3}}}",
        o.threads,
        o.warmup,
        o.ticks,
        checksum.0,
        mean / 1000.0,
        p99 as f64 / 1000.0,
        generated.as_secs_f64(),
        started.elapsed().as_secs_f64()
    );
}
