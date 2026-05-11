
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

#include "storage.h"

NAMESPACE_XGILL_BEGIN

static Buffer scratch_buf("Buffer_imlang_storage");

// maximum size we will tolerate for the scratch buffer before
// shrinking it back down.
#define SCRATCH_BUF_LIMIT  (10 * 1048576)

void ClearBlockCaches()
{
  BlockCFGCache.Clear();
  InitializerCache.Clear();
  CompositeCSUCache.Clear();
}

// maximum capacities for the storage caches.

#define CAP_BLOCK_CFG    50000
#define CAP_INITIALIZER  25000
#define CAP_CSU          50000

/////////////////////////////////////////////////////////////////////
// BlockCFG lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_BlockCFG : public Cache_BlockCFG::ExternalLookup
{
  void LookupInsert(Cache_BlockCFG *cache, BlockId *id)
  {
    Assert(id->Kind() == B_Function || id->Kind() == B_Loop);
    String *function = id->Function();
    const char *function_name = function->Value();

    if (!DoLookupTransaction(BODY_DATABASE, function_name, &scratch_buf)) {
      cache->Insert(id, NULL);
      return;
    }

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    Vector<BlockCFG*> cfg_list;
    BlockCFG::ReadList(&read_buf, &cfg_list);

    scratch_buf.Reset();

    for (size_t ind = 0; ind < cfg_list.Size(); ind++) {
      BlockCFG *cfg = cfg_list[ind];
      BlockId *id = cfg->GetId();
      cache->Insert(id, cfg);
    }
  }
};

ExternalLookup_BlockCFG lookup_BlockCFG;
Cache_BlockCFG BlockCFGCache(&lookup_BlockCFG, CAP_BLOCK_CFG);

BlockCFG* GetBlockCFG(BlockId *id)
{
  BlockCFG *cfg = NULL;

  switch (id->Kind()) {

  case B_Initializer:
    cfg = InitializerCache.Lookup(id->Function());
    InitializerCache.Release(id->Function());
    break;

  case B_Function:
  case B_Loop:
    cfg = BlockCFGCache.Lookup(id);
    BlockCFGCache.Release(id);
    break;

  default:
    Assert(false);
  }

  return cfg;
}

void BlockCFGCacheAddListWithRefs(const Vector<BlockCFG*> &cfgs)
{
  for (size_t ind = 0; ind < cfgs.Size(); ind++) {
    BlockCFG *cfg = cfgs[ind];
    BlockId *id = cfg->GetId();
    BlockCFGCache.Insert(id, cfg);
  }
}

void BlockCFGUncompress(Transaction *t, size_t var_result,
                        Vector<BlockCFG*> *cfgs)
{
  // if the compression buffer is using an inordinate amount of space
  // then reallocate it. some CFGs (particularly static initializers)
  // are gigantic, even if their compressed size is small; there's just
  // lots of redundancy.
  if (scratch_buf.size > SCRATCH_BUF_LIMIT)
    scratch_buf.Reset(SCRATCH_BUF_LIMIT);

  TOperandString *op_data = t->LookupString(var_result);

  // check for unknown functions.
  if (op_data->GetDataLength() == 0)
    return;

  TOperandString::Uncompress(op_data, &scratch_buf);
  Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);

  BlockCFG::ReadList(&read_buf, cfgs);
  scratch_buf.Reset();
}

/////////////////////////////////////////////////////////////////////
// Initializer lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_Initializer : public Cache_Initializer::ExternalLookup
{
  void LookupInsert(Cache_Initializer *cache, String *var)
  {
    if (!DoLookupTransaction(INIT_DATABASE, var->Value(), &scratch_buf)) {
      cache->Insert(var, NULL);
      return;
    }

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    BlockCFG *cfg = BlockCFG::Read(&read_buf);
    cache->Insert(var, cfg);

    scratch_buf.Reset();
  }
};

ExternalLookup_Initializer lookup_Initializer;
Cache_Initializer InitializerCache(&lookup_Initializer, CAP_INITIALIZER);

/////////////////////////////////////////////////////////////////////
// CompositeCSU lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_CompositeCSU : public Cache_CompositeCSU::ExternalLookup
{
  void LookupInsert(Cache_CompositeCSU *cache, String *name)
  {
    if (!DoLookupTransaction(COMP_DATABASE, name->Value(), &scratch_buf)) {
      cache->Insert(name, NULL);
      return;
    }

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    CompositeCSU *csu = CompositeCSU::Read(&read_buf);
    cache->Insert(name, csu);

    scratch_buf.Reset();
  }
};

ExternalLookup_CompositeCSU lookup_CompositeCSU;
Cache_CompositeCSU CompositeCSUCache(&lookup_CompositeCSU, CAP_CSU);

NAMESPACE_XGILL_END
