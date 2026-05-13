
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

#include "backend_xdb.h"
#include "backend_util.h"
#include <xdb/xdb.h>
#include <imlang/storage.h>
#include <string.h>
#include <stdlib.h>

NAMESPACE_XGILL_BEGIN

static bool is_keeping_source() {
  const char *keep_source_env = getenv("XGILL_KEEP_SOURCE");
  return keep_source_env && *keep_source_env && *keep_source_env != '0';
}

// Every table in XDB_FILE.
static const char *const g_main_tables[] = {
  BODY_DATABASE, INIT_DATABASE, COMP_DATABASE, NULL
};

// Every table in XDB_SOURCE_FILE.
static const char *const g_source_tables[] = {
  SOURCE_DATABASE, PREPROC_DATABASE, NULL
};

// Returns true if table_name names a table containing pre- or post-processed
// source code.
static bool is_source_table(const char *table_name)
{
  for (const char *const *t = g_source_tables; *t; t++) {
    if (strcmp(table_name, *t) == 0) {
      return true;
    }
  }
  return false;
}

/////////////////////////////////////////////////////////////////////
// backend data
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

static XdbFile *g_xdb_main   = NULL;
static XdbFile *g_xdb_source = NULL;

// Returns the filesystem path to use for the named xdb base file. If
// XGILL_OUTPUT_DIR is set, returns "<dir>/<base>"; otherwise returns <base>
// unchanged (sqlite resolves it against cwd).
static const char *get_xdb_path(const char *base)
{
  static const char *dir = getenv("XGILL_OUTPUT_DIR");
  if (!dir || !*dir) return base;
  // Two-slot cache: slot 0 for XDB_FILE, slot 1 for XDB_SOURCE_FILE.
  static char *cache[2] = {NULL, NULL};
  int slot = strcmp(base, XDB_SOURCE_FILE) == 0 ? 1 : 0;
  if (!cache[slot]) {
    size_t n = strlen(dir) + 1 + strlen(base) + 1;
    cache[slot] = (char*) malloc(n);
    snprintf(cache[slot], n, "%s/%s", dir, base);
  }
  return cache[slot];
}

static XdbFile *get_xdb_file(const char *table_name, CreateMode mode)
{
  if (is_source_table(table_name)) {
    static bool keepingSource = is_keeping_source();
    if (!g_xdb_source && keepingSource) {
      g_xdb_source = new XdbFile(get_xdb_path(XDB_SOURCE_FILE),
                                 mode == CreateMode::Create ? XdbFile::kCreate : 0,
                                 g_source_tables);
    }
    return g_xdb_source;
  }
  if (!g_xdb_main) {
    g_xdb_main = new XdbFile(get_xdb_path(XDB_FILE),
                             mode == CreateMode::Create ? XdbFile::kCreate : 0,
                             g_main_tables);
  }
  return g_xdb_main;
}

void ClearDatabases()
{
  delete g_xdb_main;
  g_xdb_main   = NULL;
  delete g_xdb_source;
  g_xdb_source = NULL;
}

void FlushDatabases()
{
  if (g_xdb_main) {
    g_xdb_main->Flush();
  }
  if (g_xdb_source) {
    g_xdb_source->Flush();
  }
}

BACKEND_IMPL_END

Xdb* GetDatabase(const char *table_name, CreateMode mode)
{
  XdbFile *file = Backend_IMPL::get_xdb_file(table_name, mode);
  if (!file) return NULL;

  if (mode == CreateMode::Create) {
    file->Create();  // idempotent; creates all of the file's tables
    if (file->HasError()) {
      logout << "ERROR: Corrupt database " << file->GetPath() << endl;
      file->Recover();
    }
  }

  return file->Table(table_name);
}

void FlushDatabases()
{
  Backend_IMPL::FlushDatabases();
}

bool XdbFindUncompressed(Xdb *xdb, String *key, Buffer *data)
{
  Buffer key_buf((const uint8_t*) key->Value(), strlen(key->Value()) + 1);

  static Buffer compress_buf;
  bool res = xdb->Find(&key_buf, &compress_buf);

  if (res) {
    UncompressBufferInUse(&compress_buf, data);
  }

  compress_buf.Reset();
  return res;
}

void XdbReplaceCompress(Xdb *xdb, String *key, Buffer *data)
{
  Buffer key_buf((const uint8_t*) key->Value(), strlen(key->Value()) + 1);

  static Buffer compress_buf;
  CompressBufferInUse(data, &compress_buf);

  Buffer final_buf(compress_buf.base, compress_buf.pos - compress_buf.base);
  xdb->Replace(&key_buf, &final_buf);

  compress_buf.Reset();
}

/////////////////////////////////////////////////////////////////////
// backend implementations
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

bool XdbReplace(Transaction *t, const Vector<TOperand*> &arguments,
                TOperand **result)
{
  BACKEND_ARG_COUNT(3);
  BACKEND_ARG_STRING(0, db_name, db_length);
  BACKEND_ARG_STRING(1, key_data, key_length);
  BACKEND_ARG_DATA(2, value_data, value_length);

  Xdb *xdb = GetDatabase((const char*) db_name, CreateMode::Create);
  if (!xdb) return true;  // source table with keep_source off: silently skip
  Assert(!xdb->HasError());

  Buffer keybuf(key_data, key_length);
  Buffer valuebuf(value_data, value_length);

  xdb->Replace(&keybuf, &valuebuf);
  Assert(!xdb->HasError());

  return true;
}

bool XdbLookup(Transaction *t, const Vector<TOperand*> &arguments,
               TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, db_name, db_length);
  BACKEND_ARG_STRING(1, key_data, key_length);

  Xdb *xdb = GetDatabase((const char*) db_name, CreateMode::DontCreate);

  if (!xdb || !xdb->Exists()) {
    *result = new TOperandString(t, NULL, 0);
    return true;
  }

  Buffer keybuf(key_data, key_length);

  Buffer *valuebuf = new Buffer();
  t->AddBuffer(valuebuf);

  bool success = xdb->Find(&keybuf, valuebuf);
  Assert(!xdb->HasError());

  if (success) {
    size_t retlen = valuebuf->pos - valuebuf->base;
    if (retlen == 0) {
      logout << "ERROR: Database " << db_name
             << " contains an empty value for " << key_data << endl;
      return false;
    }

    *result = new TOperandString(t, valuebuf->base, retlen);
  }
  else {
    *result = new TOperandString(t, NULL, 0);
  }

  return true;
}

bool XdbAllKeys(Transaction *t, const Vector<TOperand*> &arguments,
                TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, db_name, db_length);

  Xdb *xdb = GetDatabase((const char*) db_name, CreateMode::DontCreate);

  if (!xdb || !xdb->Exists()) {
    *result = new TOperandList(t);
    return true;
  }

  TOperandList *list = new TOperandList(t);

  Buffer *buf = NULL;

  Buffer key;
  Xdb::KeyIter it = xdb->Keys();
  while (it.Next(&key)) {
    size_t key_length = key.pos - key.base;

    if (!ValidString(key.base, key_length)) {
      logout << "ERROR: Database contains a key that is not NULL-terminated."
             << endl;
      return false;
    }

    if (buf == NULL || !buf->HasRemaining(key_length)) {
      buf = new Buffer(4096 * 16 + key_length);
      t->AddBuffer(buf);
    }

    list->PushOperand(new TOperandString(t, buf->pos, key_length));
    buf->Append(key.base, key_length);
    key.Reset();
  }

  *result = list;
  return true;
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// backend
/////////////////////////////////////////////////////////////////////

static void start_Xdb()
{
  BACKEND_REGISTER(XdbReplace);
  BACKEND_REGISTER(XdbLookup);
  BACKEND_REGISTER(XdbAllKeys);
}

static void finish_Xdb()
{
  BACKEND_IMPL::ClearDatabases();
}

TransactionBackend backend_Xdb(start_Xdb, finish_Xdb);

/////////////////////////////////////////////////////////////////////
// backend wrappers
/////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(Backend)

TAction* XdbReplace(Transaction *t,
                    const char *db_name,
                    TOperand *key,
                    TOperand *value)
{
  BACKEND_CALL(XdbReplace, 0);
  call->PushArgument(new TOperandString(t, db_name));
  call->PushArgument(key);
  call->PushArgument(value);
  return call;
}

TAction* XdbLookup(Transaction *t,
                   const char *db_name,
                   TOperand *key,
                   size_t var_result)
{
  BACKEND_CALL(XdbLookup, var_result);
  call->PushArgument(new TOperandString(t, db_name));
  call->PushArgument(key);
  return call;
}

TAction* XdbAllKeys(Transaction *t,
                    const char *db_name,
                    size_t var_result)
{
  BACKEND_CALL(XdbAllKeys, var_result);
  call->PushArgument(new TOperandString(t, db_name));
  return call;
}

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
