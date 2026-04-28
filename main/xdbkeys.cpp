
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

NAMESPACE_XGILL_USING

const char *USAGE = "xdbkeys [options] <table> [substring]";

ConfigOption db_path(CK_String, "db", XDB_FILE,
                     "path to the .sqlite file (default: " XDB_FILE ")");

ConfigOption print_stats(CK_Flag, "print-stats", NULL,
                         "just print key statistics");

int main(int argc, const char **argv)
{
  db_path.Enable();
  print_stats.Enable();

  Vector<const char*> unspecified;
  bool parsed = Config::Parse(argc, argv, &unspecified);
  if (!parsed || unspecified.Size() < 1 || unspecified.Size() > 2) {
    Config::PrintUsage(USAGE);
    return 1;
  }

  const char *table     = unspecified[0];
  const char *substring = unspecified.Size() > 1 ? unspecified[1] : "";
  const char *file      = db_path.StringValue();

  XdbFile xf(file, XdbFile::kReadOnly);
  if (xf.HasError()) {
    return 1;  // XdbFile has already logged the error.
  }
  Xdb *xdb = xf.Table(table);

  if (!xdb->Exists()) {
    return 0;
  }

  if (print_stats.IsSpecified()) {
    xdb->PrintStats();
    return 0;
  }

  Buffer key;
  Xdb::KeyIter it = xdb->Keys();
  while (it.Next(&key)) {
    Assert(ValidString(key.base, key.pos - key.base));
    if (!substring || strstr((const char*)key.base, substring)) {
      logout << (const char *)key.base << endl;
    }
    key.Reset();
  }

  return 0;
}
