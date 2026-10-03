#!/usr/bin/python3

# Compare the output of the gcc and clang front ends on a set of source files.
#
# Each file is compiled separately with each plugin into its own database, and
# the xdbfind text output for every body/init/comp entry is compared.
#
# Usage:
#   compare-frontends.py [--diff] [--outdir DIR] [--cflag FLAG]... FILE...

import argparse
import difflib
import json
import os
import re
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
        cmd = [clang] + ([] if is_cxx else CLANG_C_FLAGS)
        plugin = os.path.join(topdir, "clang", "xgill-clang.so")
    if is_cxx:
        cmd.append("-std=c++17")
    return cmd + extra_flags + ["-c", source, "-o", "/dev/null", f"-fplugin={plugin}"]


def compile_file(frontend, source, outdir, extra_flags):
    if os.path.exists(outdir):
        shutil.rmtree(outdir)
    os.makedirs(outdir)
    env = dict(os.environ, CCACHE_DISABLE="1")
    proc = subprocess.run(compiler_command(frontend, source, extra_flags), cwd=outdir, env=env,
                          capture_output=True, text=True)
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
    parser.add_argument("--cflag", action="append", default=[],
                        help="extra flag to pass to both compilers (repeatable)")
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()

    totals = {"same": 0, "different": 0, "gcc-only": 0, "clang-only": 0}

    for source in args.files:
        source = os.path.abspath(source)
        base = os.path.basename(source)
        dirs = {fe: os.path.join(os.path.abspath(args.outdir), base, fe)
                for fe in ("gcc", "clang")}

        ok = {}
        for fe, d in dirs.items():
            ok[fe], log = compile_file(fe, source, d, args.cflag)
            if not ok[fe]:
                print(f"{base}: {fe} compile FAILED")
                print("  " + "\n  ".join(log.strip().splitlines()[-15:]))

        for table in ("body", "init", "comp"):
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
                        print(f"--- {base} {table} {key}")
                        for line in difflib.unified_diff(g, c, "gcc", "clang", lineterm="", n=1):
                            print("    " + line)
                totals[status] += 1
                if status != "same":
                    print(f"{base}: {table} {key}: {status}")

    print("Totals: " + ", ".join(f"{k}={v}" for k, v in totals.items()))


if __name__ == "__main__":
    main()
