# SQLite

`sqlite3.c` and `sqlite3.h` are SQLite 3.54.1's amalgamation, as sqlite.org gives it, unchanged. [std.sqlite](../../docs/std/sqlite.md) is built on it, through [calx_sqlite.c](../calx_sqlite.c).

- From: https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
- SHA3-256 of the zip: `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`, as the download page lists it
- Source ID: `2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76`

SQLite is in the public domain, so no notice is needed. Its authors' blessing is at the top of `sqlite3.c`.

The compiler builds `sqlite3.c` once for each build mode, always optimized, and keeps the object in its cache. The options it builds with are in `compiler/src/toolchain.cpp`, beside the reasons for them.

To update SQLite, download the newest amalgamation zip from https://www.sqlite.org/download.html, check its SHA3-256 against the page, and replace the two files. Then change the version, address, hash and source ID above, and run the tests.
