
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

#include <stdio.h>
#include <xdb/xdb.h>
#include <util/config.h>

#include <imlang/storage.h>
#include <imlang/serial.h>

#include <backend/backend_xdb.h>

NAMESPACE_XGILL_USING

const char *USAGE = "xdbfind [options] <table> <key>";

ConfigOption db_path(CK_String, "db", XDB_FILE,
                     "path to the .sqlite file (default: " XDB_FILE ")");

ConfigOption plain_text(CK_Flag, "plain-text", NULL,
                        "print key data as plain text");

ConfigOption raw_tags(CK_Flag, "raw-tags", NULL,
                      "print key data as raw binary tags");

ConfigOption json(CK_Flag, "json", NULL,
                 "print key data as a JSON string");

// read a binary tag value from the specified buffer and get whatever
// hash object it represents from the buffer, returning a reference
// on that object. we will just match against the types of values that
// can appear at the top level of a database entry.
HashObject* ReadSingleValue(Buffer *buf)
{
  switch (PeekOpenTag(buf)) {

  case TAG_BlockCFG:         return BlockCFG::Read(buf);
  case TAG_CompositeCSU:     return CompositeCSU::Read(buf);

  default:
    logout << "ERROR: Unknown top-level tag in entry: "
           << PeekOpenTag(buf) << endl;
    Assert(false);
  }
}

int main(int argc, const char **argv)
{
  db_path.Enable();
  plain_text.Enable();
  raw_tags.Enable();
  json.Enable();

  Vector<const char*> unspecified;
  bool parsed = Config::Parse(argc, argv, &unspecified);
  if (!parsed || unspecified.Size() != 2) {
    Config::PrintUsage(USAGE);
    return 1;
  }

  AnalysisPrepare();

  const char *table = unspecified[0];
  const char *key   = unspecified[1];
  const char *file  = db_path.StringValue();

  XdbFile xf(file, XdbFile::kReadOnly);
  if (xf.HasError()) {
    return 1;  // XdbFile has already logged the error.
  }
  if (!xf.Exists()) {
    fprintf(logfile, "ERROR: no database file %s\n", file);
    return 1;
  }
  Xdb *xdb = xf.Table(table);

  if (!xdb->Exists()) {
    fprintf(logfile, "ERROR: no table %s in %s\n", table, file);
    return 1;
  }

  Buffer bkey((const uint8_t*) key, strlen(key) + 1);
  Buffer cdata;
  Buffer bdata;

  bool success = xdb->Find(&bkey, &cdata);
  if (success) {
    UncompressBufferInUse(&cdata, &bdata);
    size_t len = bdata.pos - bdata.base;

    if (plain_text.IsSpecified()) {
      for (size_t n = 0; n < len; n++)
        logout << (char) bdata.base[n];
      logout << endl;
    }
    else if (raw_tags.IsSpecified()) {
      size_t consumed = 0;
      while (consumed != len) {
        Buffer parse_data(bdata.base + consumed, len - consumed);
        parse_data.pos += len - consumed;

        size_t read_len = PrintPartialBuffer(logout, &parse_data);
        if (read_len == 0)
          break;
        consumed += read_len;
      }
    }
    else if (json.IsSpecified()) {
      Buffer parse_data(bdata.base, len);
      PrintJSONBuffer(logout, &parse_data);
    }
    else {
      Buffer read_buf(bdata.base, len);
      while (read_buf.pos != read_buf.base + len) {
        HashObject *value = ReadSingleValue(&read_buf);
        logout << value << endl;
      }
    }
  }

  ClearBlockCaches();
  AnalysisFinish(0);
  return 0;
}
