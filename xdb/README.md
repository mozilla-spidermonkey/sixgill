# xdb — SQLite-backed key/value store

Sixgill analysis results are stored in one or two SQLite files written during
compilation.

All tables use this schema, with NUL-terminated string keys and compressed
binary blob values (zlib deflate):

```sql
CREATE TABLE <name> (name TEXT PRIMARY KEY, data BLOB NOT NULL) STRICT, WITHOUT ROWID;
```

All of a file's tables are created together, in one transaction, when the file
is created, so a table exists exactly when its file does.

Keys are NUL-terminated strings. Values are compressed binary blobs (zlib
deflate) except where noted. The binary data is in a custom "XDB" format that
requires custom code (in this directory) to interpret. There are facilities for
converting it to JSON.

## Files and tables

### `xdb.sqlite` — always written

| Table | Key type | Key | Value |
|-------|----------|-----|-------|
| `body` | text | function name | serialized `BlockCFG` |
| `init` | text | variable type and name | serialized `BlockCFG` for initializers |
| `comp` | text | type name | serialized `CompositeCSU` |

### `xdb-source.sqlite` — written by `xsource` (via `wrap_gcc`)

| Table | Key | Value |
|-------|-----|-------|
| `source` | source file path | raw source text (compressed) |
| `preproc` | source file path | full preprocessed text (compressed) |

This file is written whenever `xsource` runs. Set `XGILL_NO_SOURCE=1` in the
environment to make eg `run_complete` skip `xsource` and omit the source archive
entirely. The archive is independent of the main file and can be deleted without
affecting analysis state.

## Inspecting the databases

Two command-line tools are provided:

```
xdbkeys [--db=<path>] [--print-stats] <table> [<substring>]
xdbfind [--db=<path>] [--plain-text | --raw-tags | --json] <table> <key>
```

Both default to `./xdb.sqlite` when `--db=` is not given.

**List all keys in the `body` table:**
```
xdbkeys body
```

**List keys containing a substring:**
```
xdbkeys body MyClass
```

**Print row count and total value bytes:**
```
xdbkeys --print-stats body
```

**Dump a single entry as JSON:**
```
xdbfind --json body 'void MyClass::method()'
```

**Read from a non-default file or table:**
```
xdbkeys --db=/path/to/xdb.sqlite comp
xdbkeys --db=xdb-source.sqlite source
```

## WAL and concurrent writes

All files use WAL journaling (`PRAGMA journal_mode=WAL`) with
`synchronous=NORMAL`. Multiple gcc-plugin processes can write
concurrently; each process holds one `BEGIN IMMEDIATE` transaction across
all tables in a file and commits it at the end of each backend transaction
via `FlushDatabases()`. The busy timeout is 60 seconds.

Unfortunately, this is kind of useless in large codebases compiled in parallel.
Sqlite really isn't intended for lots of concurrent writers, and it works far
better to synchronize updates through a single writing process rather than
relying on sqlite's concurrent writer support. So although I got this to more or
less working in hopes that I could remove the `run_complete` complexity, I ended
up scrapping that idea and I recommend wasting no further time on it, at least
until we decide to migrate to a real concurrent DB like Postgres instead.

## File finalization

When a writable connection is closed (`XdbFile::~XdbFile`, reached at end of
build via the `xsource -end-manager` shutdown path), the WAL is checkpointed
with `PRAGMA wal_checkpoint(TRUNCATE)` and the file is switched back to a
rollback journal with `PRAGMA journal_mode=DELETE`. This removes the `-wal` and
`-shm` side files so the finished `.sqlite` is a single self-contained artifact
that is safe to copy and treat as read-only. Read-only connections
(`kReadOnly`, used by `xdbkeys`/`xdbfind`) skip this step.
