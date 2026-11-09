---
title: std.sqlite
description: SQLite databases as Python's sqlite3 reads and writes them, with SQLite built in - Database.open, execute, execute_many, query, query_records, transactions, prepared statements, Value.
---

# `std.sqlite`

SQLite databases, the way Python's `sqlite3` reads and writes them: a file or `:memory:`, SQL run with `?` parameters, and rows of SQLite's five kinds of value. SQLite 3.54.1 is built into the program, so there's nothing to install, and every system has the same SQLite.

```calx
import std.sqlite.{Database, SqliteError, Value}

fn main() ! SqliteError {
    let db = try Database.open(String(":memory:"))
    _ = try db.execute(String("CREATE TABLE mobs (name TEXT, hp INTEGER)"))
    _ = try db.execute(String("INSERT INTO mobs VALUES (?, ?)"), [Value.Text(String("zombie")), Value.Int(20)])
    let rows: List<List<Value>> = [[Value.Text(String("witch")), Value.Int(26)], [Value.Text(String("bat")), Value.Int(6)]]
    _ = try db.execute_many(String("INSERT INTO mobs VALUES (?, ?)"), rows)
    for row in try db.query(String("SELECT name, hp FROM mobs WHERE hp > ? ORDER BY hp"), [Value.Int(10)]) {
        println("{row[0]} has {row[1]}")
    }
    // out: zombie has 20
    // out: witch has 26
}
```

The first program on a machine to use `std.sqlite` waits about 12 seconds while SQLite compiles into the [cache](../reference/command.md), once for debug builds and once for release ones. After that it links from there, and SQLite adds about 1.6 MB to a program.

## Values

| `Value` | SQLite | Python |
| --- | --- | --- |
| `Null` | `NULL` | `None` |
| `Int(i64)` | `INTEGER` | `int` |
| `Real(f64)` | `REAL` | `float` |
| `Text(String)` | `TEXT` | `str` |
| `Blob(List<u8>)` | `BLOB` | `bytes` |

A column gives back the kind its value was stored as, which SQLite decides from the value and the column's type, as it does for Python. `as_int()`, `as_real()`, `as_text()` and `as_blob()` give the value if it's that kind, and `as_real` takes an `Int` too. `is_null()` says whether it's `Null`. Printed, a value is `NULL`, the number, the text as it is, or a blob as SQL writes one, `x'CAFE'`.

Text that isn't UTF-8, which SQLite stores as it's given, comes back with U+FFFD for each piece that isn't, as Python's `errors="replace"` reads it. Python fails on it unless its `text_factory` is changed.

## The database

| Method | Does |
| --- | --- |
| `Database.open(path)` | Opens the file, making it if nothing is there. `:memory:` is a new database in memory. Python's `sqlite3.connect` |
| `Database.open_read(path)` | Opens the file only to read. It fails if nothing is there, and so does SQL that would change it |
| `db.execute(sql, params = [])` | Runs one statement, with `params` for its `?`s in order. Gives the rows an `INSERT`, `UPDATE` or `DELETE` changed, or 0 for any other statement, as Python's `rowcount` does |
| `db.execute_many(sql, rows)` | Runs one statement once for each list of values, as `executemany` does, and gives the rows changed in all. Outside a transaction it makes one of its own, so if a row fails, none are written |
| `db.execute_script(sql)` | Runs every statement in `sql`, as `executescript` does, for a schema or a dump |
| `db.query(sql, params = [])` | The rows, each a list of its columns' values. `execute(...).fetchall()` |
| `db.query_one(sql, params = [])` | The first row, or `none`. `execute(...).fetchone()` |
| `db.query_records(sql, params = [])` | The rows, each a `LinkedMap` from the columns' names to their values, as `sqlite3.Row` reads them by name |
| `db.prepare(sql)` | The statement, prepared, to go through by hand ([below](#statements)) |
| `db.begin()`, `db.commit()`, `db.rollback()` | A transaction: what runs between `begin` and `commit` is saved together or not at all |
| `db.in_transaction()` | Whether one is open, Python's `in_transaction` |
| `db.last_insert_id()` | The rowid of the last row an `INSERT` added, Python's `lastrowid` |
| `db.close()` | Closes it, rolling back a transaction left open |
| `version()` | The SQLite that's built in, `3.54.1`, Python's `sqlite3.sqlite_version` |

`Database` is a [handle](../reference/ownership.md#handles): it can't be copied, and it's open until `close` or until the program ends. A failure is a `SqliteError`, with SQLite's extended result `code` and its `message`, which is what Python's exception says. Calls that can't run fail too: SQL with more than one statement in `execute`, as Python refuses it, or the wrong number of values for the parameters.

```calx
import std.sqlite.{Database, SqliteError, Value}

fn main() ! SqliteError {
    let db = try Database.open(String(":memory:"))
    try db.execute_script(String("CREATE TABLE players (name TEXT PRIMARY KEY, level INTEGER); INSERT INTO players VALUES ('Ann', 3);"))
    match db.execute(String("INSERT INTO players VALUES (?, ?)"), [Value.Text(String("Ann")), Value.Int(5)]) {
        ok(n) => println("added {n}"),
        err(e) => println("{e.code}: {e}"),    // out: 1555: UNIQUE constraint failed: players.name
    }
    try db.begin()
    _ = try db.execute(String("UPDATE players SET level = level + 1"))
    try db.rollback()
    for p in try db.query_records(String("SELECT name, level FROM players")) {
        println("{p["name"]} is level {p["level"]}")    // out: Ann is level 3
    }
}
```

It differs from Python's `sqlite3` in these places:

- **Each statement is its own transaction** unless it's between `begin` and `commit`, as with Python 3.12's `autocommit=True`. Python's default opens a transaction before an `INSERT`, `UPDATE` or `DELETE` and waits for a `commit`, so a script that forgets it loses its changes. A program that writes many rows one at a time puts them between `begin` and `commit`, which is much faster, since SQLite then waits for the disk once instead of once a row. `execute_many` does that by itself.
- **Strings are in single quotes.** SQLite is built as its authors advise, so `"name"` is always a column's name. Where there's no column with that name, it's an error, where SQLite's default reads it as the string `'name'`.
- **Parameters are given in order.** A statement's `:name`, `@name` and `$name` parameters are numbered in the order they're written, and [`Statement.parameter`](#statements) gives the number for a name.

## Statements

`db.prepare(sql)` gives a `Statement`, the quickest way to run SQL many times or to read many rows, since nothing is made for a value it doesn't need. Values are bound to its parameters, numbered from 1 as SQL's `?1` numbers them. `step` runs it to each row in turn, and the column methods read that row, with columns numbered from 0. It's a handle too, given back with `finish`.

| Method | Does |
| --- | --- |
| `bind_int(i, v)`, `bind_real(i, v)`, `bind_text(i, v)`, `bind_blob(i, v)`, `bind_null(i)` | Binds a value to parameter `i`. SQLite copies text and bytes |
| `bind(i, value)`, `bind_all(values)` | Binds a `Value`, or values to all the parameters in order |
| `parameters()`, `parameter(name)` | How many parameters there are, and the number of the one written `name`, such as `:id` |
| `step() -> bool` | Runs it to its next row, and gives whether there's one. At the end it's ready to run again, with the same values or new ones |
| `int(col)`, `real(col)`, `text(col)`, `blob(col)` | The column, converted as SQLite converts it: `int` of text is the number it starts with, and `text` of `NULL` is empty |
| `value(col)`, `is_null(col)`, `row()` | The column as a `Value`, whether it's `NULL`, and the whole row |
| `columns()`, `column_name(col)`, `column_names()` | How many columns there are, and their names |
| `run()`, `run_with(values)` | Runs it to its end, after binding `values`, and gives the rows changed |
| `rows()` | Every row from where it is, as `query` gives them |
| `reset()`, `clear()` | Starts it again from the first row, keeping the values, and unbinds them all |
| `finish()` | Gives it back to the database |

A column past the last panics, since that's a mistake in the program, where SQLite would give `NULL`.

```calx
import std.sqlite.{Database, SqliteError}

fn main() ! SqliteError {
    let db = try Database.open(String(":memory:"))
    _ = try db.execute(String("CREATE TABLE blocks (x INTEGER, y INTEGER, kind TEXT)"))
    try db.begin()
    let put = try db.prepare(String("INSERT INTO blocks VALUES (?, ?, ?)"))
    for x in 0..4 {
        try put.bind_int(1, i64(x))
        try put.bind_int(2, 64)
        try put.bind_text(3, String(if x % 2 == 0 { "stone" } else { "dirt" }))
        _ = try put.run()
    }
    put.finish()
    try db.commit()
    let find = try db.prepare(String("SELECT x, kind FROM blocks WHERE kind = :kind ORDER BY x"))
    try find.bind_text(find.parameter(String(":kind")) ?? 1, String("stone"))
    while try find.step() {
        println("{find.int(0)} {find.text(1)}")
    }
    // out: 0 stone
    // out: 2 stone
    find.finish()
}
```

A database keeps the last 32 statements it prepared, keyed by their SQL, as Python keeps its own, so `execute` and `query` with the same SQL again skip SQLite's parser and planner. `prepare` takes from the same cache, and `finish` gives a statement back to it. While a statement is out, the same SQL prepared again gets a statement of its own, so the same query can run inside its own loop. After `close`, a statement still out fails, and only needs its `finish`.

## How it's built

[std/sqlite.calx](../../std/sqlite.calx) calls a thin layer in C, [runtime/calx_sqlite.c](../../runtime/calx_sqlite.c), over SQLite's own C API. SQLite is its amalgamation, [runtime/sqlite/sqlite3.c](../../runtime/sqlite), unchanged. It's in the public domain, and its version, address and hash are in the [README](../../runtime/sqlite/README.md) beside it.

The compiler builds `sqlite3.c` once for each build mode, always optimized with `-O2`, and keeps the object in its cache. It's left out of ThinLTO, which would optimize it again in every release link, and linked only into programs that use `std.sqlite`. SQLite is built with the options its authors recommend for speed, such as no memory statistics, along with the full-text search (FTS5), R-tree and math functions Python's builds have. It runs in multi-thread mode, since a connection is only used by one thread at a time.

A value is read from a row in one call for a number, and in two for text or bytes: one for the length, then one that copies them into a list the right size. `query_records` [interns](../reference/strings.md#interned-strings) the columns' names, so the records share them.

## Speed

On our benchmark, an i9-13900K with other work running, so rough, the best of five runs, on a database in memory. The table has 200,000 rows of an integer key, a name, an integer and a float. The comparisons are Python 3.14's `sqlite3`, which has SQLite 3.50.4, and Rust 1.91 with rusqlite 0.37, which builds its own SQLite 3.50.2, built for release with `x86-64-v3`:

| Work | Calx | Python | Rust `rusqlite` |
| --- | --- | --- | --- |
| `execute_many`, 200,000 rows | 40.7 ms | 229 ms | |
| `execute` 200,000 times, in a transaction | 46.1 ms | 153 ms | |
| A prepared statement, bound and run 200,000 times | 36.4 ms | | 47.5 ms |
| `query`, every row as values | 15.0 ms | 110 ms | 35.8 ms, or 24.7 ms as tuples |
| Stepping through a query's 95,000 rows | 4.0 ms | 31.1 ms | 4.8 ms |
| `GROUP BY` over the table | 33.0 ms | 41.0 ms | 40.1 ms |

- Python's times are `executemany`, `execute` in a loop between `BEGIN` and `COMMIT`, `fetchall`, and a loop over the cursor.
- Rust's are `prepare_cached` and `execute` in a transaction, `query_map` into `Vec<Vec<Value>>` or into tuples of the columns' types, and `Rows::next`.
- The `GROUP BY` is SQLite's own work, and it's quicker here mostly for the newer SQLite and its options.
