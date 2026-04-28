
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

#pragma once

// SQLite-backed key/value store for sixgill analysis databases.
//
// XdbFile owns a sqlite3* connection to one .sqlite file. Xdb is a handle to
// one SQL table within that file.
//
// Note that this file once defined a custom whole file database format, but now
// only handles an embedding of analysis data stored with SQLite.

#include <util/buffer.h>
#include <sqlite3.h>

NAMESPACE_XGILL_BEGIN

class Xdb;

class XdbFile
{
 public:
  enum {
    kCreate   = 1,  // create the file if absent
    kReadOnly = 2   // open read-only
  };

  // Open (or defer opening) the .sqlite file at path.
  // If kCreate is set, the file is created and pragmas are applied now.
  // Otherwise the open is attempted; if the file is absent the XdbFile
  // sits dormant (Exists() returns false) until Create() is called.
  XdbFile(const char *path, uint32_t flags);
  ~XdbFile();

  const char *GetPath() const { return m_path; }
  bool HasError() const { return m_error; }

  // Whether the file is open and accessible.
  bool Exists() const { return m_db != NULL && !m_error; }

  // Open with create + apply pragmas. Idempotent if already open.
  void Create();

  // COMMIT any pending write transaction.
  void Flush();

  // Close, delete the file, reopen with Create, and recreate (empty) every
  // table previously handed out via Table() that was in use.
  void Recover();

  // Return the Xdb handle for the named SQL table within this file.
  // Creates the handle if this is the first call for that name.
  // The returned Xdb* is owned by this XdbFile; do not delete it.
  // Calling Table() twice with the same name returns the same pointer.
  Xdb *Table(const char *table_name);

 private:
  char    *m_path;
  sqlite3 *m_db;
  bool     m_read_only;
  bool     m_error;
  bool     m_in_tx;

  static const int kMaxTables = 16;
  int   m_table_count;
  Xdb  *m_tables[kMaxTables];

  void open_db(bool create);
  void setup_pragmas();
  void finalize_file();
  void begin_tx();
  void set_error(int rc, const char *context);

  friend class Xdb;
};

class Xdb
{
 public:
  const char *GetTableName() const { return m_table; }
  bool HasError() const { return m_file->m_error; }

  // Whether the SQL table exists in the file. A file that does not exist
  // yet counts as not existing; a file with an open or read error is fatal.
  // Callers must check this before any of the operations below.
  bool Exists() const;

  // Run CREATE TABLE IF NOT EXISTS for this table, opening the file
  // first if necessary.
  void Create();

  // Print row count and total value bytes to logfile.
  void PrintStats();

  // Key/value operations.  Keys are NUL-terminated byte strings;
  // values are opaque BLOBs.
  bool HasKey (const Buffer *key);
  bool Find   (const Buffer *key, Buffer *out);
  bool Add    (const Buffer *key, const Buffer *val);
  void Replace(const Buffer *key, const Buffer *val);

  // Forward-only iterator over all keys in lexicographic order.
  class KeyIter
  {
   public:
    bool Next(Buffer *key);
    ~KeyIter();
   private:
    sqlite3_stmt *m_stmt;
    explicit KeyIter(sqlite3_stmt *stmt) : m_stmt(stmt) {}
    KeyIter(const KeyIter &) = delete;
    KeyIter &operator=(const KeyIter &) = delete;
    friend class Xdb;
  };
  KeyIter Keys();

 private:
  XdbFile *m_file;
  char    *m_table;

  char *m_sql_schema;
  char *m_sql_find;
  char *m_sql_has;
  char *m_sql_add;
  char *m_sql_replace;
  char *m_sql_keys;
  char *m_sql_stats;

  sqlite3_stmt *m_stmt_find;
  sqlite3_stmt *m_stmt_has;
  sqlite3_stmt *m_stmt_add;
  sqlite3_stmt *m_stmt_replace;

  explicit Xdb(XdbFile *file, const char *table);
  ~Xdb();
  Xdb(const Xdb &) = delete;
  Xdb &operator=(const Xdb &) = delete;

  void build_sql();
  void prepare_stmts();
  void ensure_prepared();
  void finalize_stmts();
  void set_error(int rc, const char *context);

  static void bind_key(sqlite3_stmt *stmt, int idx, const Buffer *key);
  static void bind_val(sqlite3_stmt *stmt, int idx, const Buffer *val);

  friend class XdbFile;
};

NAMESPACE_XGILL_END
