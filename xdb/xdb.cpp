
// Sixgill: Static assertion checker for C/C++ programs.
// Copyright (C) 2009-2010  Stanford University
// Author: Brian Hackett
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "xdb.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

NAMESPACE_XGILL_BEGIN

/////////////////////////////////////////////////////////////////////
// XdbFile
/////////////////////////////////////////////////////////////////////

void XdbFile::set_error(int rc, const char *context)
{
  m_error = true;
  fprintf(logfile, "ERROR: XdbFile %s %s: %s\n", m_path, context,
          m_db ? sqlite3_errmsg(m_db) : sqlite3_errstr(rc));
}

void XdbFile::setup_pragmas()
{
  // Values are tuned a bit to reduce overhead compared to previous XDB file format.
  sqlite3_exec(m_db, "PRAGMA page_size=65536",    NULL, NULL, NULL);

  // WAL mode (Write-Ahead Log) for fast writes with fsync only at transaction
  // commit.
  sqlite3_exec(m_db, "PRAGMA journal_mode=WAL",   NULL, NULL, NULL);
  sqlite3_exec(m_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

  sqlite3_exec(m_db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);

  // Negative values mean the units are KiB instead of #pages. Ugh.
  sqlite3_exec(m_db, "PRAGMA cache_size=-65536", NULL, NULL, NULL);

  // It's easy to construct a situation with lots of concurrent accesses, and
  // this will make them wait their turn rather than having a thundering herd
  // where the later ones fail outright.
  sqlite3_exec(m_db, "PRAGMA busy_timeout=60000", NULL, NULL, NULL);
}

void XdbFile::finalize_file()
{
  // Called once on close of a writable connection (i.e. at end of build, when
  // the manager signals shutdown via `xsource -end-manager`). Fold the WAL back
  // into the main database file and switch to a rollback journal so the
  // resulting single .sqlite file is self-contained -- no -wal/-shm side files
  // to lose track of when the artifact is copied around, and ready to be
  // treated as read-only. Safe here because this is the sole connection and all
  // prepared statements/transactions have already been finalized/committed.
  if (!m_db || m_read_only || m_error) return;

  // Truncate rather than the default PASSIVE checkpoint so the -wal file is
  // emptied, not just replayed into the db.
  sqlite3_exec(m_db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);

  // Switching out of WAL performs a final checkpoint and removes -wal/-shm.
  int rc = sqlite3_exec(m_db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    fprintf(logfile, "WARNING: XdbFile %s could not switch to DELETE journal "
            "mode: %s\n", m_path, sqlite3_errmsg(m_db));
    }
}

void XdbFile::open_db(bool create)
{
  int flags = m_read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
  if (create) {
    flags |= SQLITE_OPEN_CREATE;
  }

  int rc = sqlite3_open_v2(m_path, &m_db, flags, NULL);
  if (rc != SQLITE_OK) {
    if (rc == SQLITE_CANTOPEN && !create) {
      // File does not yet exist, which is ok.
    } else {
      set_error(rc, "open");
    }
    if (m_db) {
      sqlite3_close(m_db);
      m_db = NULL;
    }
  }
}

void XdbFile::begin_tx()
{
  if (m_in_tx) return;
  int rc = sqlite3_exec(m_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    set_error(rc, "BEGIN IMMEDIATE");
  } else {
    m_in_tx = true;
  }
}

XdbFile::XdbFile(const char *path, uint32_t flags)
  : m_db(NULL), m_read_only(!!(flags & kReadOnly)),
    m_error(false), m_in_tx(false), m_table_count(0)
{
  m_path = new char[strlen(path) + 1];
  strcpy(m_path, path);
  memset(m_tables, 0, sizeof(m_tables));

  open_db(!!(flags & kCreate));
  if (Exists()) {
    setup_pragmas();
  }
}

XdbFile::~XdbFile()
{
  if (m_db) {
    Flush();
    for (int i = 0; i < m_table_count; i++) {
      delete m_tables[i];
    }
    // Statements are now finalized and any write tx committed, so the
    // connection is idle: fold the WAL in and produce a self-contained file.
    finalize_file();
    sqlite3_close(m_db);
    m_db = NULL;
  }
  delete[] m_path;
}

void XdbFile::Create()
{
  if (m_db) return;
  open_db(true);
  if (Exists()) {
    setup_pragmas();
  }
  // DDL for individual tables runs via Xdb::Create(); stmts are prepared
  // lazily on first use.
}

void XdbFile::Flush()
{
  if (!m_db || m_read_only || !m_in_tx) return;
  int rc = sqlite3_exec(m_db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    set_error(rc, "COMMIT");
  }
  m_in_tx = false;
}

void XdbFile::Recover()
{
  // Tables in use before must be recreated in the new file so that
  // previously handed-out Xdb handles remain usable. A table with prepared
  // statements is known to exist, since preparing fails on a missing table.
  bool existed[kMaxTables] = {};
  if (m_db) {
    if (m_in_tx) {
      sqlite3_exec(m_db, "ROLLBACK", NULL, NULL, NULL);
    }
    for (int i = 0; i < m_table_count; i++) {
      if (m_tables[i]) {
        existed[i] = m_tables[i]->m_stmt_find != NULL;
        m_tables[i]->finalize_stmts();
      }
    }
    sqlite3_close(m_db);
    m_db = NULL;
  }
  m_error  = false;
  m_in_tx  = false;
  unlink(m_path);
  Create();
  for (int i = 0; i < m_table_count; i++) {
    if (existed[i]) {
      m_tables[i]->Create();
    }
  }
}

Xdb *XdbFile::Table(const char *table_name)
{
  for (int i = 0; i < m_table_count; i++) {
    if (strcmp(m_tables[i]->GetTableName(), table_name) == 0) {
      return m_tables[i];
    }
  }

  Assert(m_table_count < kMaxTables);
  Xdb *xdb = new Xdb(this, table_name);
  // Statements are prepared lazily (on first use) or by Xdb::Create().
  m_tables[m_table_count] = xdb;
  m_table_count++;
  return xdb;
}

/////////////////////////////////////////////////////////////////////
// Xdb
/////////////////////////////////////////////////////////////////////

Xdb::Xdb(XdbFile *file, const char *table)
  : m_file(file), m_table(NULL),
    m_sql_schema(NULL), m_sql_find(NULL), m_sql_has(NULL),
    m_sql_add(NULL), m_sql_replace(NULL), m_sql_keys(NULL), m_sql_stats(NULL),
    m_stmt_find(NULL), m_stmt_has(NULL), m_stmt_add(NULL), m_stmt_replace(NULL)
{
  m_table = strdup(table);
  build_sql();
}

Xdb::~Xdb()
{
  finalize_stmts();
  free(m_table);
  free(m_sql_schema);
  free(m_sql_find);
  free(m_sql_has);
  free(m_sql_add);
  free(m_sql_replace);
  free(m_sql_keys);
  free(m_sql_stats);
}

void Xdb::build_sql()
{
  bool ok = true;
  ok = ok && asprintf(&m_sql_schema,
    "CREATE TABLE IF NOT EXISTS %s"
    " (name TEXT PRIMARY KEY, data BLOB NOT NULL) STRICT, WITHOUT ROWID",
    m_table) >= 0;
  ok = ok && asprintf(&m_sql_find,
    "SELECT data FROM %s WHERE name=?", m_table) >= 0;
  ok = ok && asprintf(&m_sql_has,
    "SELECT 1 FROM %s WHERE name=? LIMIT 1", m_table) >= 0;
  ok = ok && asprintf(&m_sql_add,
    "INSERT OR IGNORE INTO %s(name,data) VALUES(?,?)", m_table) >= 0;
  ok = ok && asprintf(&m_sql_replace,
    "INSERT INTO %s(name,data) VALUES(?,?)"
    " ON CONFLICT(name) DO UPDATE SET data=excluded.data",
    m_table) >= 0;
  ok = ok && asprintf(&m_sql_keys,
    "SELECT name FROM %s ORDER BY name", m_table) >= 0;
  ok = ok && asprintf(&m_sql_stats,
    "SELECT count(*), coalesce(sum(length(data)),0),"
    " (SELECT page_count*page_size FROM pragma_page_count(), pragma_page_size())"
    " FROM %s",
    m_table) >= 0;
  if (!ok) {
    m_file->m_error = true;
  }
}

void Xdb::set_error(int rc, const char *context)
{
  m_file->set_error(rc, context);
}

void Xdb::bind_key(sqlite3_stmt *stmt, int idx, const Buffer *key)
{
  sqlite3_bind_text(stmt, idx, (const char*)key->base, -1, SQLITE_STATIC);
}

void Xdb::bind_val(sqlite3_stmt *stmt, int idx, const Buffer *val)
{
  sqlite3_bind_blob(stmt, idx, val->base, (int)val->size, SQLITE_STATIC);
}

void Xdb::prepare_stmts()
{
  sqlite3 *db = m_file->m_db;
  auto prep = [&](const char *sql, sqlite3_stmt **out) {
    int rc = sqlite3_prepare_v2(db, sql, -1, out, NULL);
    if (rc != SQLITE_OK) {
      set_error(rc, "prepare");
    }
  };
  prep(m_sql_find,    &m_stmt_find);
  prep(m_sql_has,     &m_stmt_has);
  prep(m_sql_add,     &m_stmt_add);
  prep(m_sql_replace, &m_stmt_replace);
}

void Xdb::ensure_prepared()
{
  if (!m_stmt_find && m_file->Exists())
    prepare_stmts();
}

void Xdb::finalize_stmts()
{
  sqlite3_finalize(m_stmt_find);
  m_stmt_find    = NULL;
  sqlite3_finalize(m_stmt_has);
  m_stmt_has     = NULL;
  sqlite3_finalize(m_stmt_add);
  m_stmt_add     = NULL;
  sqlite3_finalize(m_stmt_replace);
  m_stmt_replace = NULL;
}

bool Xdb::Exists() const
{
  // An absent file is normal (nothing has been written yet), but a file
  // that failed to open or is corrupt must not masquerade as empty.
  Assert(!m_file->m_error);
  if (!m_file->m_db) return false;
  const char *sql =
    "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(m_file->m_db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, m_table, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
  }
  bool found = (rc == SQLITE_ROW);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    m_file->set_error(rc, "Exists");
  }
  sqlite3_finalize(stmt);
  Assert(!m_file->m_error);
  return found;
}

void Xdb::Create()
{
  if (!m_file->m_db) {
    m_file->Create();
  }
  if (!m_file->Exists()) {
    return;
  }

  char *err = NULL;

  int rc = sqlite3_exec(m_file->m_db, m_sql_schema, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    fprintf(logfile, "ERROR: Xdb %s.%s schema: %s\n",
            m_file->m_path, m_table, err ? err : sqlite3_errmsg(m_file->m_db));
    sqlite3_free(err);
    m_file->m_error = true;
    return;
  }

  if (!m_stmt_find) {
    prepare_stmts();
  }
}

void Xdb::PrintStats()
{
  Assert(m_file->Exists());

  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2(m_file->m_db, m_sql_stats, -1, &stmt, NULL);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    int64_t rows   = sqlite3_column_int64(stmt, 0);
    int64_t vbytes = sqlite3_column_int64(stmt, 1);
    int64_t fsize  = sqlite3_column_int64(stmt, 2);
    fprintf(logfile, "Xdb statistics:\n");
    fprintf(logfile, "File: %s\n",         m_file->m_path);
    fprintf(logfile, "Table: %s\n",        m_table);
    fprintf(logfile, "Rows: %lld\n",       (long long)rows);
    fprintf(logfile, "Value bytes: %lld\n",(long long)vbytes);
    fprintf(logfile, "File size: %lld\n",  (long long)fsize);
  }
  sqlite3_finalize(stmt);
}

bool Xdb::HasKey(const Buffer *key)
{
  Assert(m_file->Exists());
  ensure_prepared();
  Assert(!m_file->m_error);
  bind_key(m_stmt_has, 1, key);
  int rc = sqlite3_step(m_stmt_has);
  bool found = (rc == SQLITE_ROW);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    set_error(rc, "HasKey");
  }
  sqlite3_reset(m_stmt_has);
  sqlite3_clear_bindings(m_stmt_has);
  return found;
}

bool Xdb::Find(const Buffer *key, Buffer *out)
{
  Assert(m_file->Exists());
  ensure_prepared();
  Assert(!m_file->m_error);
  bind_key(m_stmt_find, 1, key);
  int rc = sqlite3_step(m_stmt_find);
  bool found = false;
  if (rc == SQLITE_ROW) {
    int len          = sqlite3_column_bytes(m_stmt_find, 0);
    const void *blob = sqlite3_column_blob(m_stmt_find, 0);
    out->Append(blob, len);
    found = true;
  } else if (rc != SQLITE_DONE) {
    set_error(rc, "Find");
  }
  sqlite3_reset(m_stmt_find);
  sqlite3_clear_bindings(m_stmt_find);
  return found;
}

bool Xdb::Add(const Buffer *key, const Buffer *val)
{
  Assert(m_file->Exists() && !m_file->m_read_only);
  ensure_prepared();
  m_file->begin_tx();
  Assert(!m_file->m_error);
  bind_key(m_stmt_add, 1, key);
  bind_val(m_stmt_add, 2, val);
  int rc = sqlite3_step(m_stmt_add);
  bool inserted = false;
  if (rc == SQLITE_DONE) {
    inserted = sqlite3_changes(m_file->m_db) > 0;
  } else {
    set_error(rc, "Add");
  }
  sqlite3_reset(m_stmt_add);
  sqlite3_clear_bindings(m_stmt_add);
  return inserted;
}

void Xdb::Replace(const Buffer *key, const Buffer *val)
{
  Assert(m_file->Exists() && !m_file->m_read_only);
  ensure_prepared();
  m_file->begin_tx();
  Assert(!m_file->m_error);
  bind_key(m_stmt_replace, 1, key);
  bind_val(m_stmt_replace, 2, val);
  int rc = sqlite3_step(m_stmt_replace);
  if (rc != SQLITE_DONE) {
    set_error(rc, "Replace");
  }
  sqlite3_reset(m_stmt_replace);
  sqlite3_clear_bindings(m_stmt_replace);
}

/////////////////////////////////////////////////////////////////////
// Xdb::KeyIter
/////////////////////////////////////////////////////////////////////

Xdb::KeyIter Xdb::Keys()
{
  Assert(m_file->Exists());
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(m_file->m_db, m_sql_keys, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    set_error(rc, "Keys");
    stmt = NULL;
  }
  Assert(!m_file->m_error);
  return KeyIter(stmt);
}

bool Xdb::KeyIter::Next(Buffer *key)
{
  if (!m_stmt) return false;
  int rc = sqlite3_step(m_stmt);
  if (rc == SQLITE_ROW) {
    int len = sqlite3_column_bytes(m_stmt, 0);
    const char *text = (const char*)sqlite3_column_text(m_stmt, 0);
    key->Append(text, len + 1);
    return true;
  }
  return false;
}

Xdb::KeyIter::~KeyIter()
{
  sqlite3_finalize(m_stmt);
}

NAMESPACE_XGILL_END
