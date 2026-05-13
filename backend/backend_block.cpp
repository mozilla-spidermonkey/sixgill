
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

#include "backend_block.h"
#include "backend_xdb.h"
#include <imlang/storage.h>
#include <util/monitor.h>
#include <string.h>

NAMESPACE_XGILL_BEGIN

BACKEND_IMPL_BEGIN

/////////////////////////////////////////////////////////////////////
// Backend construction data
/////////////////////////////////////////////////////////////////////

// databases for storing program values.
static Xdb *g_body_xdb = NULL;
static Xdb *g_init_xdb = NULL;
static Xdb *g_comp_xdb = NULL;

// source archive (only when XGILL_KEEP_SOURCE=1).
static Xdb *g_source_xdb = NULL;
static Xdb *g_preproc_xdb = NULL;

typedef HashSet<String*,String> StringSet;

// names of all program values we've written out.
static StringSet g_write_body;
static StringSet g_write_init;
static StringSet g_write_comp;

// all files whose contents we've written out.
static StringSet g_write_files;

// open the block databases if they are not already open.
void LoadDatabases()
{
  // only need to load these once.
  static bool have_load = false;
  if (have_load) return;
  have_load = true;

  g_body_xdb = GetDatabase(BODY_DATABASE, CreateMode::Create);
  g_init_xdb = GetDatabase(INIT_DATABASE, CreateMode::Create);
  g_comp_xdb = GetDatabase(COMP_DATABASE, CreateMode::Create);

  g_source_xdb  = GetDatabase(SOURCE_DATABASE,  CreateMode::Create);
  g_preproc_xdb = GetDatabase(PREPROC_DATABASE, CreateMode::Create);
}

void MarkRoots()
{
  HashIterate(g_write_body)
    g_write_body.ItKey()->Mark();
  HashIterate(g_write_init)
    g_write_init.ItKey()->Mark();
  HashIterate(g_write_comp)
    g_write_comp.ItKey()->Mark();
  HashIterate(g_write_files)
    g_write_files.ItKey()->Mark();
}


/////////////////////////////////////////////////////////////////////
// Backend implementations
/////////////////////////////////////////////////////////////////////

bool BlockQueryList(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  Assert(arguments[0]->Kind() == TO_String);
  TOperandString *list = (TOperandString*) arguments[0];

  LoadDatabases();

  static Buffer result_buf;

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  while (read_buf.pos != read_buf.base + read_buf.size) {
    switch (PeekOpenTag(&read_buf)) {

    case TAG_Name: {
      String *name = String::ReadWithTag(&read_buf, TAG_Name);

      if (!g_write_comp.Insert(name))
        String::WriteWithTag(&result_buf, name, TAG_Name);

      break;
    }

    case TAG_BlockId: {
      BlockId *id = BlockId::Read(&read_buf);
      String *name = id->Function();

      if (id->Kind() == B_FunctionWhole) {
        if (!g_write_body.Insert(name))
          BlockId::Write(&result_buf, id);
      }
      else if (id->Kind() == B_Initializer) {
        if (!g_write_init.Insert(name))
          BlockId::Write(&result_buf, id);
      }
      else {
        Assert(false);
      }

      break;
    }

    default:
      Assert(false);
    }
  }

  data_buf.Reset();

  if (result_buf.pos == result_buf.base) {
    // none of the elements in the list need to be processed.
    *result = new TOperandString(t, NULL, 0);
    return true;
  }

  Buffer *compress_buf = new Buffer();
  t->AddBuffer(compress_buf);

  CompressBufferInUse(&result_buf, compress_buf);
  result_buf.Reset();

  *result = new TOperandString(t, compress_buf->base,
                               compress_buf->pos - compress_buf->base);
  return true;
}

bool BlockWriteList(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  Assert(arguments[0]->Kind() == TO_String);
  TOperandString *list = (TOperandString*) arguments[0];

  LoadDatabases();

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  static Buffer write_buf;

  while (read_buf.pos != read_buf.base + read_buf.size) {
    switch (PeekOpenTag(&read_buf)) {

    case TAG_CompositeCSU: {
      CompositeCSU *csu = CompositeCSU::Read(&read_buf);
      CompositeCSU::Write(&write_buf, csu);

      String *name = csu->GetName();
      Assert(g_write_comp.Lookup(name));

      Assert(g_comp_xdb);
      XdbReplaceCompress(g_comp_xdb, name, &write_buf);

      write_buf.Reset();
      break;
    }

    case TAG_UInt32: {
      uint32_t count = 0;
      Try(ReadUInt32(&read_buf, &count));
      Assert(count);

      // the count indicates the number of CFGs for the next function/global.
      Vector<BlockCFG*> function_cfgs;

      for (size_t ind = 0; ind < count; ind++) {
        BlockCFG *cfg = BlockCFG::Read(&read_buf);
        function_cfgs.PushBack(cfg);
      }

      BlockId *id = function_cfgs.Back()->GetId();
      String *name = id->Function();

      // now write the CFGs to disk.

      for (size_t ind = 0; ind < count; ind++)
        BlockCFG::Write(&write_buf, function_cfgs[ind]);

      if (id->Kind() == B_Function) {
        Assert(g_write_body.Lookup(name));

        Assert(g_body_xdb);
        XdbReplaceCompress(g_body_xdb, name, &write_buf);
      }
      else if (id->Kind() == B_Initializer) {
        Assert(g_write_init.Lookup(name));

        Assert(g_init_xdb);
        XdbReplaceCompress(g_init_xdb, name, &write_buf);
      }
      else {
        Assert(false);
      }

      write_buf.Reset();
      break;
    }

    default:
      Assert(false);
    }
  }

  data_buf.Reset();
  return true;
}

bool BlockQueryFile(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, file_name, file_length);

  LoadDatabases();

  String *file = String::Make((const char*) file_name);
  bool found = g_write_files.Insert(file);

  *result = new TOperandBoolean(t, found);
  return true;
}

bool BlockWriteFile(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(3);
  BACKEND_ARG_STRING(0, file_name, file_length);
  BACKEND_ARG_DATA(1, source_data, source_length);
  BACKEND_ARG_DATA(2, preproc_data, preproc_length);

  String *file = String::Make((const char*) file_name);

  // Write full source and preprocessed text to the source archive.
  {
    static Buffer scratch_buf;

    if (g_source_xdb) {
      scratch_buf.Append(source_data, source_length);
      XdbReplaceCompress(g_source_xdb, file, &scratch_buf);
      scratch_buf.Reset();
    }

    if (g_preproc_xdb) {
      scratch_buf.Append(preproc_data, preproc_length);
      XdbReplaceCompress(g_preproc_xdb, file, &scratch_buf);
      scratch_buf.Reset();
    }
  }

  return true;
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// Backend
/////////////////////////////////////////////////////////////////////

static void start_Block()
{
  BACKEND_REGISTER(BlockQueryList);
  BACKEND_REGISTER(BlockWriteList);
  BACKEND_REGISTER(BlockQueryFile);
  BACKEND_REGISTER(BlockWriteFile);
}

TransactionBackend backend_Block(start_Block, NULL);

/////////////////////////////////////////////////////////////////////
// Backend wrappers
/////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(Backend)

TAction* BlockQueryList(Transaction *t, TOperand *query_data,
                        size_t var_result)
{
  BACKEND_CALL(BlockQueryList, var_result);
  call->PushArgument(query_data);
  return call;
}

TAction* BlockWriteList(Transaction *t, TOperand *write_data)
{
  BACKEND_CALL(BlockWriteList, 0);
  call->PushArgument(write_data);
  return call;
}

TAction* BlockQueryFile(Transaction *t, const char *file, size_t var_result)
{
  BACKEND_CALL(BlockQueryFile, var_result);
  call->PushArgument(new TOperandString(t, file));
  return call;
}

TAction* BlockWriteFile(Transaction *t, const char *file,
                        TOperand *source_data, TOperand *preproc_data)
{
  BACKEND_CALL(BlockWriteFile, 0);
  call->PushArgument(new TOperandString(t, file));
  call->PushArgument(source_data);
  call->PushArgument(preproc_data);
  return call;
}

void MarkRoots()
{
    Backend_IMPL::MarkRoots();
}

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
