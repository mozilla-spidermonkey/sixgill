
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
#include <unistd.h>

NAMESPACE_XGILL_BEGIN

// file to read/write worklist information.
#define WORKLIST_FILE "worklist.sort"

BACKEND_IMPL_BEGIN

/////////////////////////////////////////////////////////////////////
// Backend construction data
/////////////////////////////////////////////////////////////////////

// databases for storing program values.
static Xdb *g_body_xdb = NULL;
static Xdb *g_init_xdb = NULL;
static Xdb *g_comp_xdb = NULL;

// databases for storing file contents.
static Xdb *g_source_xdb = NULL;
static Xdb *g_preproc_xdb = NULL;

// databases for storing annotations. these databases do not necessarily exist.
static Xdb *g_annot_body_xdb = NULL;
static Xdb *g_annot_init_xdb = NULL;
static Xdb *g_annot_comp_xdb = NULL;

// whether we've written out any function bodies, initializers or types.
static bool g_have_body = false;

// whether we are doing an incremental build. this will be set whenever there
// is existing worklist information, including when generating new indirect
// call edges during the memory analysis (in this case, no function bodies
// will be written out).
static bool g_incremental = false;

typedef HashSet<String*,String> StringSet;
typedef HashTable<String*,String*,String> StringMap;

// names of all program values we've written out.
static StringSet g_write_body;
static StringSet g_write_init;
static StringSet g_write_comp;

// all files whose contents we've written out.
static StringSet g_write_files;

// for incremental builds, function names which are new or changed from
// a previous run. subset of g_write_body.
static StringSet g_body_new;

// for incremental builds, function names which are not necessarily new
// or changed, but may be affected by a new or changed annotation.
// hold references.
static StringSet g_body_reanalyze;

// map from function names to the files containing them.
// subset of g_write_body.
static StringMap g_body_file;

// map from function names to their version. subset of g_write_body.
static HashTable<String*,VersionId,String> g_body_version;

// list of filenames whose source has changed since a previous run, only used
// for incremental builds.
static Vector<String*> g_file_changed;

// information about all the annotations associated with some block,
// including those stored or removed from databases.
struct KeyAnnotationInfo
{
  // name of the function/global/type this specifies annotations for.
  String *key;

  // CFGs which are currently in the database for the key. these are clones,
  // unless there was an intermediate annotation flush in which case these
  // may be a mix of clones and non-clones.
  Vector<BlockCFG*> database_cfgs;

  // CFGs which were removed from the database. these are all clones.
  // if we are doing an incremental build, old annotations are removed
  // and will end up erased unless they are readded.
  Vector<BlockCFG*> removed_cfgs;

  // CFGs to write out. these are not clones, and may overlap with
  // database_cfgs or removed_cfgs.
  Vector<BlockCFG*> write_cfgs;

  KeyAnnotationInfo(String *_key) : key(_key) {}

  void Mark() const {
    key->Mark();

    for (size_t ind = 0; ind < database_cfgs.Size(); ind++)
      database_cfgs[ind]->Mark();

    for (size_t ind = 0; ind < removed_cfgs.Size(); ind++)
      removed_cfgs[ind]->Mark();

    for (size_t ind = 0; ind < write_cfgs.Size(); ind++)
      write_cfgs[ind]->Mark();
  }
};

// information about annotations for all blocks queried or written at some
// point. blocks not in these hashes have not had their annotations read
// or modified.
typedef HashTable<String*,KeyAnnotationInfo*,String> AnnotationHash;
static AnnotationHash g_annot_body;
static AnnotationHash g_annot_init;
static AnnotationHash g_annot_comp;

// open the block databases if they are not already open.
void LoadDatabases()
{
  // only need to load these once.
  static bool have_load = false;
  if (have_load) return;
  have_load = true;

  g_body_xdb = GetDatabase(BODY_DATABASE, true);
  g_init_xdb = GetDatabase(INIT_DATABASE, true);
  g_comp_xdb = GetDatabase(COMP_DATABASE, true);

  g_source_xdb = GetDatabase(SOURCE_DATABASE, true);
  g_preproc_xdb = GetDatabase(PREPROC_DATABASE, true);

  g_annot_body_xdb = GetDatabase(BODY_ANNOT_DATABASE, false);
  g_annot_init_xdb = GetDatabase(INIT_ANNOT_DATABASE, false);
  g_annot_comp_xdb = GetDatabase(COMP_ANNOT_DATABASE, false);

  // we are doing an incremental analysis if there is an existing worklist.
  if (access(WORKLIST_FILE, F_OK) == 0)
    g_incremental = true;
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
  HashIterate(g_body_reanalyze)
    g_body_reanalyze.ItKey()->Mark();
  HashIterate(g_body_file) {
    g_body_file.ItKey()->Mark();
    g_body_file.ItValueSingle()->Mark();
  }
  HashIterate(g_annot_body) {
      g_annot_body.ItKey()->Mark();
      g_annot_body.ItValueSingle()->Mark();
  }
  HashIterate(g_annot_init) {
    g_annot_init.ItKey()->Mark();
    g_annot_init.ItValueSingle()->Mark();
  }
  HashIterate(g_annot_comp) {
    g_annot_comp.ItKey()->Mark();
    g_annot_comp.ItValueSingle()->Mark();
  }
}


// get the annotation info for key within hash, filling it in from xdb
// if necessary.
KeyAnnotationInfo* GetAnnotations(Xdb *xdb, AnnotationHash &hash, String *key)
{
  Vector<KeyAnnotationInfo*> *entries = hash.Lookup(key, true);
  if (!entries->Empty())
    return entries->At(0);

  KeyAnnotationInfo *info = new KeyAnnotationInfo(key);

  static Buffer scratch_buf;

  if (xdb->Exists() && XdbFindUncompressed(xdb, key, &scratch_buf)) {
    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    BlockCFG::ReadList(&read_buf, &info->database_cfgs, true);
    scratch_buf.Reset();
  }

  entries->PushBack(info);
  return info;
}

// remove any annotations associate with key for hash/xdb.
void RemoveAnnotations(Xdb *xdb, AnnotationHash &hash, String *key)
{
  Assert(g_have_body);

  if (!g_incremental) {
    // only remove annotations when doing an incremental build.
    // otherwise we can spuriously remove annotations if there is no manager.
    return;
  }

  KeyAnnotationInfo *info = GetAnnotations(xdb, hash, key);

  if (!info->database_cfgs.Empty()) {
    // clear the annotation CFGs from the database itself.
    static Buffer empty_buf;
    XdbReplaceCompress(xdb, key, &empty_buf);

    // move the CFGs into the removed list.
    for (size_t ind = 0; ind < info->database_cfgs.Size(); ind++)
      info->removed_cfgs.PushBack(info->database_cfgs[ind]);
    info->database_cfgs.Clear();
  }
}

// write out any annotations for info to one of the annotation databases.
// if there are new annotations, mark any affected functions for reanalysis.
// updates info to reflect the new state of the annotations.
void WriteAnnotations(Xdb *xdb, KeyAnnotationInfo *info)
{
  if (info->write_cfgs.Empty())
    return;

  // make sure the database exists.
  if (!xdb->Exists())
    xdb->Create();

  // make the list we will actually write out. this includes the write CFGs
  // from the info and any existing CFGs in the database which the write
  // CFGs do not duplicate.
  Vector<BlockCFG*> write_list;

  for (size_t ind = 0; ind < info->write_cfgs.Size(); ind++)
    write_list.PushBack(info->write_cfgs[ind]);

  for (size_t ind = 0; ind < info->database_cfgs.Size(); ) {
    BlockCFG *cfg = info->database_cfgs[ind];

    // skip old annotations which have a new version to write out.
    // update the database list to reflect the change.
    bool duplicate = false;
    for (size_t wind = 0; wind < info->write_cfgs.Size(); wind++) {
      if (cfg->GetId()->Loop() == info->write_cfgs[wind]->GetId()->Loop())
        duplicate = true;
    }

    if (duplicate) {
      info->database_cfgs[ind] = info->database_cfgs.Back();
      info->database_cfgs.PopBack();
    }
    else {
      write_list.PushBack(cfg);
      ind++;
    }
  }

  // write out the annotation CFGs.
  static Buffer scratch_buf;
  BlockCFG::WriteList(&scratch_buf, write_list);
  XdbReplaceCompress(xdb, info->key, &scratch_buf);
  scratch_buf.Reset();

  // update the lists to reflect the new state.

  info->removed_cfgs.Clear();

  for (size_t ind = 0; ind < info->write_cfgs.Size(); ind++)
    info->database_cfgs.PushBack(info->write_cfgs[ind]);
  info->write_cfgs.Clear();
}

// write all pending annotations to databases.
void FlushAnnotations()
{
  HashIterate(g_annot_body)
    WriteAnnotations(g_annot_body_xdb, g_annot_body.ItValueSingle());

  HashIterate(g_annot_init)
    WriteAnnotations(g_annot_init_xdb, g_annot_init.ItValueSingle());

  HashIterate(g_annot_comp)
    WriteAnnotations(g_annot_comp_xdb, g_annot_comp.ItValueSingle());
}

/////////////////////////////////////////////////////////////////////
//
// worklist file overview
//
// whether we are doing an initial or incremental build, we will write the
// worklist file indicating all the functions we have bodies for and how
// to process those functions. functions are written out as 'file$name',
// so that we can identify deleted functions when doing incremental builds.
//
////////////////////// initial layout ///////////////////////////////
//
// #stage0
// fnlist
//
////////////////////// incremental layout ///////////////////////////
//
// #new
// fnlist (all new/changed functions)
//
// #old
// fnlist (all remaining functions)
//
/////////////////////////////////////////////////////////////////////

// structure for a file/function pair and lexicographic comparison for sorting.
struct FunctionFilePair
{
  String *function;
  String *file;

  FunctionFilePair() : function(NULL), file(NULL) {}
  FunctionFilePair(String *_function, String *_file)
    : function(_function), file(_file) {}

  static int Compare(FunctionFilePair v0, FunctionFilePair v1)
  {
    int cmp = strcmp(v0.file->Value(), v1.file->Value());
    if (cmp) return cmp;
    return strcmp(v0.function->Value(), v1.function->Value());
  }
};

// sort and write out a list of functions for the worklist file.
void WriteWorklistFunctions(OutStream &out, const Vector<String*> &functions)
{
  // make a list containing the files as well, so we can get a decent sort.
  Vector<FunctionFilePair> write_list;

  for (size_t ind = 0; ind < functions.Size(); ind++) {
    String *function = functions[ind];
    String *file = g_body_file.LookupSingle(function);
    write_list.PushBack(FunctionFilePair(function, file));
  }

  SortVector<FunctionFilePair,FunctionFilePair>(&write_list);

  for (size_t ind = 0; ind < write_list.Size(); ind++)
    out << write_list[ind].file->Value() << "$"
        << write_list[ind].function->Value() << endl;
}

// write out the worklist file for an initial build.
void WriteWorklistInitial()
{
  Assert(g_have_body && !g_incremental);

  Vector<String*> functions;
  HashIterate(g_write_body)
    functions.PushBack(g_write_body.ItKey());

  FileOutStream worklist_out(WORKLIST_FILE);

  worklist_out << "#stage0" << endl;
  WriteWorklistFunctions(worklist_out, functions);
}

// write out the worklist file for an incremental build.
void WriteWorklistIncremental()
{
  Assert(g_have_body && g_incremental);

  // read and store the contents of the old worklist file.
  Buffer worklist_buf;
  Vector<char*> worklist_strings;
  {
    FileInStream worklist_in(WORKLIST_FILE);
    ReadInStream(worklist_in, &worklist_buf);
    SplitBufferStrings(&worklist_buf, '\n', &worklist_strings);
  }

  FileOutStream worklist_out(WORKLIST_FILE);

  // get the list of new/changed functions.
  Vector<String*> new_functions;
  HashIterate(g_body_new)
    new_functions.PushBack(g_body_new.ItKey());

  // get the list of old functions. these are all functions in the old worklist
  // file, except for functions in the new/changed list and functions which
  // have been deleted --- the file they were in has changed, but we did
  // not see them and add them to g_write_body.

  Vector<String*> old_functions;

  for (size_t ind = 0; ind < worklist_strings.Size(); ind++) {
    char *str = worklist_strings[ind];

    // skip blank lines and stage header lines.
    if (*str == 0) continue;
    if (*str == '#') continue;

    // the string should have the format 'file$function', extract the two.
    char *separator = strchr(str, '$');
    Assert(separator);
    *separator = 0;

    String *function = String::Make(separator + 1);

    if (g_body_new.Lookup(function)) {
      // this function is new/changed and has already been written out.
      continue;
    }

    if (g_write_body.Lookup(function)) {
      // we saw the (unchanged) body so this function definitely exists.

      // if the function needs to be reanalyzed then mark it as new.
      if (g_body_reanalyze.Lookup(function))
        new_functions.PushBack(function);
      else
        old_functions.PushBack(function);

      continue;
    }

    String *file = String::Make(str);

    if (g_file_changed.Contains(file)) {
      // the file changed (so we should have recompiled it and any other
      // files including it), but we didn't see the function. treat this
      // function as deleted. we need to overwrite it with a stub,
      // assigning a new version and letting us reanalyze and remove its
      // memory/modset/summary data later.

      static Buffer stub_buf;
      if (!XdbFindUncompressed(g_body_xdb, function, &stub_buf))
          Fail("worklist contains a function in a changed file, but the old function is not in the xdb");
      Buffer read_buf(stub_buf.base, stub_buf.pos - stub_buf.base);

      // clone the old CFGs when reading them in to distinguish them
      // from the stub CFG we're writing out.
      Vector<BlockCFG*> old_cfgs;
      BlockCFG::ReadList(&read_buf, &old_cfgs, true);

      String *stub_file = String::Make("<deleted>");
      Location *stub_loc = Location::Make(stub_file, 0);

      g_body_file.Insert(function, stub_file);

      Variable *func_var = old_cfgs.Back()->GetId()->BaseVar();
      BlockId *id = BlockId::Make(B_Function, func_var);
      BlockCFG *stub_cfg = BlockCFG::Make(id);

      // increment the version number for the stub CFG.
      stub_cfg->SetVersion(old_cfgs.Back()->GetVersion() + 1);

      stub_cfg->SetBeginLocation(stub_loc);
      stub_cfg->SetEndLocation(stub_loc);

      PPoint point = stub_cfg->AddPoint(stub_loc);
      stub_cfg->SetEntryPoint(point);

      stub_buf.Reset();

      BlockCFG::Write(&stub_buf, stub_cfg);
      XdbReplaceCompress(g_body_xdb, function, &stub_buf);
      stub_buf.Reset();

      new_functions.PushBack(function);
      continue;
    }

    // we never saw the body for this function, but its source file was not
    // modified. assume it still exists. this is the common case, where the
    // file containing this function did not need to be rebuilt.
    // this can leave ghost functions in two cases, however. first, if the
    // file was outright deleted (or otherwise not part of the project anymore)
    // we will not be able to recognize this. second, and more esoteric:
    //
    // file.h:
    //   typedef int TYPE;
    //
    // file.cc:
    //   void foo(TYPE t) {}
    //
    // changing file.h to typedef another type will change the signature
    // of foo without changing the contents of file.cc, so doing an
    // incremental build will leave both versions of foo around.

    // keep the file for the function around, for WriteWorklistFunctions.
    g_body_file.Insert(function, file);

    // if the function needs to be reanalyzed then mark it as new.
    if (g_body_reanalyze.Lookup(function))
      new_functions.PushBack(function);
    else
      old_functions.PushBack(function);
  }

  // write out the list of new/changed functions.
  worklist_out << "#new" << endl;
  WriteWorklistFunctions(worklist_out, new_functions);
  worklist_out << endl;

  // write out the list of old functions.
  worklist_out << "#old" << endl;
  WriteWorklistFunctions(worklist_out, old_functions);
}

/////////////////////////////////////////////////////////////////////
// Backend worklist data
/////////////////////////////////////////////////////////////////////

// current stage of the block worklist.
static size_t g_stage = 0;

// remaining elements for stages 0 through the stage count.
static Vector<Vector<String*>*> g_stage_worklist;

// active worklist for stages over the stage count. the next stage's worklist
// is in the WORKLIST_FUNC_NEXT hash.
static Vector<String*> g_overflow_worklist;

/////////////////////////////////////////////////////////////////////
// Backend data cleanup
/////////////////////////////////////////////////////////////////////

// write out all block backend data to disk.
void FinishBlockBackend()
{
  // write out any annotations we found.
  // so need to be done before the worklist is generated.

  FlushAnnotations();

  // write any worklist information.

  if (g_have_body) {
    if (g_incremental)
      WriteWorklistIncremental();
    else
      WriteWorklistInitial();
  }

  // drop references on annotations.

  HashIterate(g_annot_body)
    delete g_annot_body.ItValueSingle();

  HashIterate(g_annot_init)
    delete g_annot_init.ItValueSingle();

  HashIterate(g_annot_comp)
    delete g_annot_comp.ItValueSingle();

  // drop references on worklist data.

  for (size_t ind = 0; ind < g_stage_worklist.Size(); ind++) {
    Vector<String*> *data = g_stage_worklist[ind];
    delete data;
  }
}

/////////////////////////////////////////////////////////////////////
// Backend implementations
/////////////////////////////////////////////////////////////////////

bool BlockQueryAnnot(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(4);
  BACKEND_ARG_STRING(0, db_name, db_length);
  BACKEND_ARG_STRING(1, var_name, var_length);
  BACKEND_ARG_STRING(2, annot_name, annot_length);
  BACKEND_ARG_BOOLEAN(3, check_db);

  LoadDatabases();

  String *new_var_name = String::Make((const char*) var_name);

  KeyAnnotationInfo *info = NULL;

  if (!strcmp((const char*) db_name, BODY_ANNOT_DATABASE))
    info = GetAnnotations(g_annot_body_xdb, g_annot_body, new_var_name);
  else if (!strcmp((const char*) db_name, INIT_ANNOT_DATABASE))
    info = GetAnnotations(g_annot_init_xdb, g_annot_init, new_var_name);
  else if (!strcmp((const char*) db_name, COMP_ANNOT_DATABASE))
    info = GetAnnotations(g_annot_comp_xdb, g_annot_comp, new_var_name);
  else
    Assert(false);

  bool found = false;

  // check the CFGs that we've gotten a BlockWriteAnnot for.
  for (size_t ind = 0; ind < info->write_cfgs.Size(); ind++) {
    const char *cur_name = info->write_cfgs[ind]->GetId()->Loop()->Value();
    if (!strcmp((const char*) annot_name, cur_name))
      found = true;
  }

  // optionally check the CFGs that are already in the database.
  for (size_t ind = 0; check_db &&  ind < info->database_cfgs.Size(); ind++) {
    const char *cur_name = info->database_cfgs[ind]->GetId()->Loop()->Value();
    if (!strcmp((const char*) annot_name, cur_name))
      found = true;
  }

  *result = new TOperandBoolean(t, found);
  return true;
}

bool BlockWriteAnnot(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  Assert(arguments[0]->Kind() == TO_String);
  TOperandString *list = (TOperandString*) arguments[0];

  LoadDatabases();

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  BlockCFG *annot_cfg = BlockCFG::Read(&read_buf);
  BlockId *id = annot_cfg->GetId();
  String *var_name = id->Function();
  data_buf.Reset();

  KeyAnnotationInfo *info = NULL;

  if (id->Kind() == B_AnnotationFunc)
    info = GetAnnotations(g_annot_body_xdb, g_annot_body, var_name);
  else if (id->Kind() == B_AnnotationInit)
    info = GetAnnotations(g_annot_init_xdb, g_annot_init, var_name);
  else if (id->Kind() == B_AnnotationComp)
    info = GetAnnotations(g_annot_comp_xdb, g_annot_comp, var_name);
  else
    Assert(false);

  // watch out for duplicate writes. this will only show up if there
  // are multiple workers trying to add this annotation, and we got
  // QUERYA;QUERYB;WRITEA;WRITEB sequence.
  if (!info->write_cfgs.Contains(annot_cfg))
    info->write_cfgs.PushBack(annot_cfg);

  return true;
}

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
      g_have_body = true;

      CompositeCSU *csu = CompositeCSU::Read(&read_buf);
      CompositeCSU::Write(&write_buf, csu);

      String *name = csu->GetName();
      Assert(g_write_comp.Lookup(name));

      Assert(g_comp_xdb);
      XdbReplaceCompress(g_comp_xdb, name, &write_buf);
      RemoveAnnotations(g_annot_comp_xdb, g_annot_comp, name);

      write_buf.Reset();
      break;
    }

    case TAG_UInt32: {
      g_have_body = true;

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

      // CFGs we read in should be unversioned.
      Assert(function_cfgs.Back()->GetVersion() == 0);

      // if we're doing an incremental build, we need to compute the
      // versions for the CFGs, and see if they represent a change from any
      // CFGs for the same function from the previous build.
      if (g_incremental && id->Kind() == B_Function) {
        // look for an old function and check if the new one is isomorphic.
        bool incremental_new = false;

        static Buffer compare_buf;
        if (XdbFindUncompressed(g_body_xdb, name, &compare_buf)) {
          // clone the old CFGs when reading them in to distinguish them
          // from the new CFGs we're writing out.
          Vector<BlockCFG*> old_cfgs;
          Buffer compare_read_buf(compare_buf.base,
                                  compare_buf.pos - compare_buf.base);
          BlockCFG::ReadList(&compare_read_buf, &old_cfgs, true);

          if (old_cfgs.Size() == function_cfgs.Size()) {
            for (size_t ind = 0; ind < old_cfgs.Size(); ind++) {
              if (!old_cfgs[ind]->IsEquivalent(function_cfgs[ind])) {
                // change in the contents of this function/loop.
                incremental_new = true;
              }
            }
          }
          else {
            // change in the number of loops.
            incremental_new = true;
          }

          // compute the version to use for the new CFGs. this is the old
          // version if the new CFGs are equivalent to the old CFGs,
          // otherwise the old version plus one.
          VersionId new_version = old_cfgs.Back()->GetVersion();
          if (incremental_new) new_version++;

          g_body_version.Insert(name, new_version);

          // update the versions for the CFGs before writing them out.
          for (size_t ind = 0; ind < count; ind++)
            function_cfgs[ind]->SetVersion(new_version);

          compare_buf.Reset();
        }
        else {
          // this is a new function, there is no old one to compare with.
          // the version number is zero.
          incremental_new = true;

          g_body_version.Insert(name, 0);
        }

        if (incremental_new)
          g_body_new.Insert(name);
      }

      // now that the CFGs have the correct version, write them out to disk.

      for (size_t ind = 0; ind < count; ind++)
        BlockCFG::Write(&write_buf, function_cfgs[ind]);

      if (id->Kind() == B_Function) {
        Assert(g_write_body.Lookup(name));

        Assert(g_body_xdb);
        XdbReplaceCompress(g_body_xdb, name, &write_buf);
        RemoveAnnotations(g_annot_body_xdb, g_annot_body, name);

        // remember the file this function was defined in.
        String *file = function_cfgs.Back()->GetBeginLocation()->FileName();
        g_body_file.Insert(name, file);
      }
      else if (id->Kind() == B_Initializer) {
        Assert(g_write_init.Lookup(name));

        Assert(g_init_xdb);
        XdbReplaceCompress(g_init_xdb, name, &write_buf);
        RemoveAnnotations(g_annot_init_xdb, g_annot_init, name);
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

bool BlockFlushAnnotations(Transaction *t, const Vector<TOperand*> &arguments,
                           TOperand **result)
{
  BACKEND_ARG_COUNT(0);
  FlushAnnotations();
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

  if (g_incremental) {
    // compare the preprocessed contents with the old data to look for changes.
    bool preproc_new = false;

    static Buffer compare_buf;
    if (XdbFindUncompressed(g_preproc_xdb, file, &compare_buf)) {
      if (compare_buf.pos - compare_buf.base != (int) preproc_length)
        preproc_new = true;
      else if (memcmp(compare_buf.base, preproc_data, preproc_length) != 0)
        preproc_new = true;
      compare_buf.Reset();
    }
    else {
      // entirely new file.
      preproc_new = true;
    }

    if (preproc_new && !g_file_changed.Contains(file))
      g_file_changed.PushBack(file);
  }

  static Buffer scratch_buf;

  scratch_buf.Append(source_data, source_length);
  XdbReplaceCompress(g_source_xdb, file, &scratch_buf);
  scratch_buf.Reset();

  scratch_buf.Append(preproc_data, preproc_length);
  XdbReplaceCompress(g_preproc_xdb, file, &scratch_buf);
  scratch_buf.Reset();

  return true;
}

bool BlockLoadWorklist(Transaction *t, const Vector<TOperand*> &arguments,
                       TOperand **result)
{
  BACKEND_ARG_COUNT(0);

  if (!g_stage_worklist.Empty()) {
    // ignore duplicate loads.
    *result = new TOperandInteger(t, g_stage_worklist.Size() - 1);
    return true;
  }

  Buffer worklist_buf;
  Vector<char*> worklist_strings;
  {
    FileInStream worklist_in(WORKLIST_FILE);
    ReadInStream(worklist_in, &worklist_buf);
    SplitBufferStrings(&worklist_buf, '\n', &worklist_strings);
  }

  Vector<String*> *stage_list = NULL;

  // we are incremental if the first stage is '#new'. in this case
  // functions under the subsequent '#old' will be ignored.
  bool incremental = false;

  for (size_t ind = 0; ind < worklist_strings.Size(); ind++) {
    char *str = worklist_strings[ind];
    if (*str == 0) continue;

    if (*str == '#') {
      // new stage, check its name for incremental analysis.
      // the names are ignored for initial analysis.

      if (incremental) {
        Assert(!strcmp(str, "#old"));
        break;
      }

      if (!strcmp(str, "#new"))
        incremental = true;

      stage_list = new Vector<String*>();
      g_stage_worklist.PushBack(stage_list);
      continue;
    }

    // get the function name from the 'file$function' format.
    char *separator = strchr(str, '$');
    Assert(separator);
    String *function = String::Make(separator + 1);

    stage_list->PushBack(function);
  }

  if (g_stage_worklist.Empty()) {
    // no functions at all. make an empty stage.
    g_stage_worklist.PushBack(new Vector<String*>());
  }

  *result = new TOperandInteger(t, g_stage_worklist.Size() - 1);
  return true;
}

bool BlockSeedWorklist(Transaction *t, const Vector<TOperand*> &arguments,
                       TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_LIST(0, functions);

  if (!g_stage_worklist.Empty()) {
    // ignore duplicate loads/seeds.
    return true;
  }

  Vector<String*> *seed_list = new Vector<String*>();
  g_stage_worklist.PushBack(seed_list);

  for (size_t ind = 0; ind < functions->GetCount(); ind++) {
    if (functions->GetOperand(ind)->Kind() != TO_String)
      BACKEND_FAIL(functions->GetOperand(ind));

    TOperandString *str = functions->GetOperand(ind)->AsString();
    if (!ValidString(str->GetData(), str->GetDataLength()))
      BACKEND_FAIL(str);

    seed_list->PushBack(String::Make((const char*) str->GetData()));
  }

  return true;
}

bool BlockCurrentStage(Transaction *t, const Vector<TOperand*> &arguments,
                       TOperand **result)
{
  BACKEND_ARG_COUNT(0);

  *result = new TOperandInteger(t, g_stage);
  return true;
}

bool BlockPopWorklist(Transaction *t, const Vector<TOperand*> &arguments,
                      TOperand **result)
{
  BACKEND_ARG_COUNT(0);

  Vector<String*> *worklist = (g_stage < g_stage_worklist.Size())
    ? g_stage_worklist[g_stage]
    : &g_overflow_worklist;

  if (!worklist->Empty()) {
    String *function = worklist->Back();
    worklist->PopBack();

    const char *new_function = t->CloneString(function->Value());
    *result = new TOperandString(t, new_function);
    return true;
  }

  // the current stage is exhausted, advance to the next stage.
  *result = new TOperandString(t, "");
  g_stage++;

  if (g_stage >= g_stage_worklist.Size()) {
    // we are fixpointing after the initial pass over the callgraph, load the
    // overflow worklist from the hash storing new functions to analyze.

    BackendStringHash *next_hash =
      GetNamedHash((const uint8_t*) WORKLIST_FUNC_NEXT);

    if (next_hash) {
      HashIteratePtr(next_hash)
        g_overflow_worklist.PushBack(next_hash->ItKey());
      next_hash->Clear();
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
  BACKEND_REGISTER(BlockQueryAnnot);
  BACKEND_REGISTER(BlockWriteAnnot);
  BACKEND_REGISTER(BlockQueryList);
  BACKEND_REGISTER(BlockWriteList);
  BACKEND_REGISTER(BlockFlushAnnotations);
  BACKEND_REGISTER(BlockQueryFile);
  BACKEND_REGISTER(BlockWriteFile);
  BACKEND_REGISTER(BlockLoadWorklist);
  BACKEND_REGISTER(BlockSeedWorklist);
  BACKEND_REGISTER(BlockCurrentStage);
  BACKEND_REGISTER(BlockPopWorklist);
}

static void finish_Block()
{
  Backend_IMPL::FinishBlockBackend();
}

TransactionBackend backend_Block(start_Block, finish_Block);

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

TAction* BlockQueryAnnot(Transaction *t, const char *db_name,
                         const char *var_name, const char *annot_name,
                         bool check_db, size_t var_result)
{
  BACKEND_CALL(BlockQueryAnnot, var_result);
  call->PushArgument(new TOperandString(t, db_name));
  call->PushArgument(new TOperandString(t, var_name));
  call->PushArgument(new TOperandString(t, annot_name));
  call->PushArgument(new TOperandBoolean(t, check_db));
  return call;
}

TAction* BlockWriteAnnot(Transaction *t, TOperand *annot_data)
{
  BACKEND_CALL(BlockWriteAnnot, 0);
  call->PushArgument(annot_data);
  return call;
}

TAction* BlockFlushAnnotations(Transaction *t)
{
  BACKEND_CALL(BlockFlushAnnotations, 0);
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

TAction* BlockLoadWorklist(Transaction *t, size_t var_result)
{
  BACKEND_CALL(BlockLoadWorklist, var_result);
  return call;
}

TAction* BlockSeedWorklist(Transaction *t, TOperandList *functions)
{
  BACKEND_CALL(BlockSeedWorklist, 0);
  call->PushArgument(functions);
  return call;
}

TAction* BlockCurrentStage(Transaction *t, size_t var_result)
{
  BACKEND_CALL(BlockCurrentStage, var_result);
  return call;
}

TAction* BlockPopWorklist(Transaction *t, size_t var_result)
{
  BACKEND_CALL(BlockPopWorklist, var_result);
  return call;
}

void MarkRoots()
{
    Backend_IMPL::MarkRoots();
}

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
