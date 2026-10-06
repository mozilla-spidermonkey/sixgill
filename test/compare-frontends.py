#!/usr/bin/python3

# Compare the output of the gcc and clang front ends on a set of source files.
#
# Each file is compiled separately with each plugin into its own database, and
# the xdbfind text output for every body/init/comp entry is compared.
#
# Usage:
#   compare-frontends.py [-j N] [--diff] [--outdir DIR] [--cflag FLAG]... FILE...
#
# See --help for comparing files from a compile_commands.json.

import argparse
import concurrent.futures
import difflib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

topdir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
bindir = os.path.join(topdir, "bin")

with open(os.path.join(topdir, "config.json")) as fh:
    config = json.load(fh)

CXX_EXTENSIONS = (".cc", ".cpp", ".C", ".cxx")

# clang errors out on some old-style C that gcc only warns about.
CLANG_C_FLAGS = [
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=int-conversion",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=return-type",
]


def compiler_command(frontend, source, extra_flags):
    is_cxx = source.endswith(CXX_EXTENSIONS)
    if frontend == "gcc":
        cmd = [config["TARGET_CC"], "-x", "c++" if is_cxx else "c"]
        plugin = os.path.join(topdir, "gcc", "xgill.so")
    else:
        clang = os.path.join(config["CLANG_DIR"], "bin", "clang++" if is_cxx else "clang")
        # Use the same C++ standard library headers as gcc.
        gcc_root = os.path.dirname(os.path.dirname(config["TARGET_CC"]))
        cmd = [clang, f"--gcc-toolchain={gcc_root}"] + ([] if is_cxx else CLANG_C_FLAGS)
        plugin = os.path.join(topdir, "clang", "xgill-clang.so")
    if is_cxx:
        cmd.append("-std=c++17")
    return cmd + extra_flags + ["-c", source, "-o", "/dev/null", f"-fplugin={plugin}"]


# Flags in a compile database that clang doesn't accept or that are gcc-only.
GCC_ONLY_FLAGS = ("-Wduplicated-cond", "-Wlogical-op", "-Wno-multistatement-macros",
                  "-Wno-error=class-memaccess", "-Wformat-overflow=2", "-Wno-psabi")
# Flags from a clangd compile database that gcc doesn't accept.
CLANG_ONLY_FLAGS = ("-ferror-limit=0",)


PATH_MAP = []
EXTRA_FLAGS = []
REMOVE_FLAGS = set()


def compile_db_command(frontend, entry):
    """Build a command for a compile database entry. The database's compiler is
    used for gcc, and clang++/clang for clang."""
    args = entry.get("arguments") or shlex.split(entry["command"])
    for old, new in PATH_MAP:
        args = [a.replace(old, new) for a in args]
    compiler, rest = args[0], args[1:]
    rest = [a for a in rest if a != entry["file"] and not a.endswith(entry["file"])]
    if frontend == "gcc":
        rest = [a for a in rest if a not in CLANG_ONLY_FLAGS]
        cmd = [compiler] + rest
        plugin = os.path.join(topdir, "gcc", "xgill.so")
    else:
        is_cxx = not entry["file"].endswith(".c")
        clang = os.path.join(config["CLANG_DIR"], "bin", "clang++" if is_cxx else "clang")
        rest = [a for a in rest if a not in GCC_ONLY_FLAGS]
        cmd = [clang, "-Wno-unknown-warning-option"] + rest
        plugin = os.path.join(topdir, "clang", "xgill-clang.so")
    cmd = [a for a in cmd if a not in REMOVE_FLAGS]
    return cmd + EXTRA_FLAGS + ["-DXGILL_PLUGIN", entry["file"], f"-fplugin={plugin}"]


def compile_file(frontend, source, outdir, extra_flags, db_entry=None):
    if os.path.exists(outdir):
        shutil.rmtree(outdir)
    os.makedirs(outdir)
    env = dict(os.environ, CCACHE_DISABLE="1")
    if db_entry:
        cmd = compile_db_command(frontend, db_entry)
    else:
        cmd = compiler_command(frontend, source, extra_flags)
    # Compile in the output directory so that xdb.sqlite ends up there, with -o
    # pointing away from the build. Plugin messages go to a log file; by
    # default they go to stdout, which breaks -pipe.
    cmd = [("/dev/null" if prev == "-o" else a) for prev, a in zip([None] + cmd, cmd)]
    cmd.append("-fplugin-arg-xgill-log=" + os.path.join(outdir, "plugin.log"))
    proc = subprocess.run(cmd, cwd=outdir, env=env, capture_output=True, text=True)
    log = proc.stdout + proc.stderr
    with open(os.path.join(outdir, "compile.log"), "w") as fh:
        fh.write(log)
    return proc.returncode == 0, log


def keys(dbdir, table):
    db = os.path.join(dbdir, "xdb.sqlite")
    if not os.path.exists(db):
        return []
    out = subprocess.run([os.path.join(bindir, "xdbkeys"), f"--db={db}", table],
                         capture_output=True, text=True).stdout
    return out.splitlines()


def entry(dbdir, table, key, exact):
    db = os.path.join(dbdir, "xdb.sqlite")
    out = subprocess.run([os.path.join(bindir, "xdbfind"), f"--db={db}", table, key],
                         capture_output=True, text=True).stdout
    # The command differs between the compilers and doesn't matter.
    lines = [line for line in out.splitlines()
             if not line.startswith(("command:", "command "))]
    if not exact:
        # gcc computes most field offsets as zero, so ignore them.
        lines = [re.sub(r"^field: \d+ ", "field: ", line) for line in lines]
    return lines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--diff", action="store_true", help="print diffs of differing entries")
    parser.add_argument("--outdir", default="test-output/compare-frontends")
    parser.add_argument("--exact", action="store_true",
                        help="also compare field offsets, which gcc gets wrong")
    parser.add_argument("--tables", default="body,init,comp",
                        help="comma-separated tables to compare")
    parser.add_argument("--cflag", action="append", default=[],
                        help="extra flag to pass to both compilers (repeatable)")
    parser.add_argument("--compile-db", help="compile_commands.json to take commands from; "
                        "FILE arguments are then substrings of the files to compile")
    parser.add_argument("--remove-flag", action="append", default=[],
                        help="flag to remove from compile database commands (repeatable)")
    parser.add_argument("--path-map", action="append", default=[],
                        help="OLD=NEW: replace OLD with NEW in compile database commands, "
                        "eg to use a different objdir's generated headers")
    parser.add_argument("-j", "--jobs", type=int, default=1,
                        help="number of files to process in parallel")
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()
    for mapping in args.path_map:
        PATH_MAP.append(tuple(mapping.split("=", 1)))
    EXTRA_FLAGS.extend(args.cflag)
    REMOVE_FLAGS.update(args.remove_flag)

    db_entries = {}
    if args.compile_db:
        with open(args.compile_db) as fh:
            db = json.load(fh)
        files = []
        for pattern in args.files:
            for db_entry in db:
                if pattern in db_entry["file"] and db_entry["file"] not in db_entries:
                    db_entries[db_entry["file"]] = db_entry
                    files.append(db_entry["file"])
        args.files = files

    def process(source):
        """Compile and compare one file. Returns its output lines and totals."""
        out = []
        counts = {"same": 0, "different": 0, "gcc-only": 0, "clang-only": 0}
        source = os.path.abspath(source)
        base = os.path.basename(source)
        dirs = {fe: os.path.join(os.path.abspath(args.outdir), base, fe)
                for fe in ("gcc", "clang")}

        for fe, d in dirs.items():
            ok, log = compile_file(fe, source, d, args.cflag, db_entries.get(source))
            if not ok:
                out.append(f"{base}: {fe} compile FAILED")
                out.append("  " + "\n  ".join(log.strip().splitlines()[-15:]))

        for table in args.tables.split(","):
            gcc_keys = set(keys(dirs["gcc"], table))
            clang_keys = set(keys(dirs["clang"], table))

            for key in sorted(gcc_keys | clang_keys):
                if key not in clang_keys:
                    status = "gcc-only"
                elif key not in gcc_keys:
                    status = "clang-only"
                else:
                    g = entry(dirs["gcc"], table, key, args.exact)
                    c = entry(dirs["clang"], table, key, args.exact)
                    status = "same" if g == c else "different"
                    if status == "different" and args.diff:
                        out.append(f"--- {base} {table} {key}")
                        for line in difflib.unified_diff(g, c, "gcc", "clang", lineterm="", n=1):
                            out.append("    " + line)
                counts[status] += 1
                if status != "same":
                    out.append(f"{base}: {table} {key}: {status}")
        return out, counts

    totals = {"same": 0, "different": 0, "gcc-only": 0, "clang-only": 0}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as executor:
        for out, counts in executor.map(process, args.files):
            for line in out:
                print(line)
            sys.stdout.flush()
            for k, v in counts.items():
                totals[k] += v

    print("Totals: " + ", ".join(f"{k}={v}" for k, v in totals.items()))


if __name__ == "__main__":
    main()
