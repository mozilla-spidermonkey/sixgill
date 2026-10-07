#!/usr/bin/python3

import glob
import json
import os
import re
import sys
import typing
from subprocess import check_output

topdir = os.path.abspath(os.path.join(os.path.abspath(os.path.dirname(__file__)), ".."))
bindir = os.path.join(topdir, "bin")
plugin = os.path.join(topdir, "gcc", "xgill.so")


def execfile(thefile, globals=None):
    exec(__builtins__.compile(open(thefile).read(), thefile, "exec"), globals)


for d in ("test-output", "test-output-clang"):
    if not os.path.exists(d):
        os.mkdir(d)

with open(os.path.join(topdir, "config.json")) as fh:
    config = json.load(fh)


# Simplified version of the body info.
class Body(dict):
    def __init__(self, body):
        self['BlockIdKind'] = body['BlockId']['Kind']
        if 'Variable' in body['BlockId']:
            self['BlockName'] = body['BlockId']['Variable']['Name'][0]
        self['LineRange'] = [ body['Location'][0]['Line'], body['Location'][1]['Line'] ]
        self['Filename'] = body['Location'][0]['CacheString']
        self['Edges'] = body.get('PEdge', [])
        self['Points'] = { i+1: body['PPoint'][i]['Location']['Line'] for i in range(len(body['PPoint'])) }
        self['Index'] = body['Index']
        self['Variables'] = { x['Variable']['Name'][0]: x['Type'] for x in body['DefineVariable'] }

        # Indexes
        self['Line2Points'] = {}
        for point, line in self['Points'].items():
            self['Line2Points'].setdefault(line, []).append(point)
        self['SrcPoint2Edges'] = {}
        for edge in self['Edges']:
            (src, dst) = edge['Index']
            self['SrcPoint2Edges'].setdefault(src, []).append(edge)
        self['Line2Edges'] = {}
        for (src, edges) in self['SrcPoint2Edges'].items():
            line = self['Points'][src]
            self['Line2Edges'].setdefault(line, []).extend(edges)

    def edges_from_line(self, line):
        return self['Line2Edges'][line]

    def edge_from_line(self, line):
        edges = self.edges_from_line(line)
        assert(len(edges) == 1)
        return edges[0]

    def edges_from_point(self, point):
        return self['SrcPoint2Edges'][point]

    def edge_from_point(self, point):
        edges = self.edges_from_point(point)
        assert(len(edges) == 1)
        return edges[0]

    def assignment_point(self, varname):
        for edge in self['Edges']:
            if edge['Kind'] != 'Assign':
                continue
            dst = edge['Exp'][0]
            if dst['Kind'] != 'Var':
                continue
            if dst['Variable']['Name'][0] == varname:
                return edge['Index'][0]
        raise Exception("assignment to variable %s not found" % varname)

    def assignment_line(self, varname):
        return self['Points'][self.assignment_point(varname)]


# Usage: run-test.py [--compiler=gcc|clang] [test...]
_, *tests = sys.argv
frontend = "gcc"
for arg in list(tests):
    if arg.startswith("--compiler="):
        frontend = arg.split("=", 1)[1]
        tests.remove(arg)
if not tests:
    tests = ['gchazard', 'gccbugs', 'memory', 'cxx']

# clang errors out on some old-style C that gcc only warns about.
CLANG_C_FLAGS = ("-Wno-error=implicit-function-declaration -Wno-error=implicit-int "
                 "-Wno-error=int-conversion -Wno-error=incompatible-pointer-types")

for name in tests:
    indir = os.path.join(topdir, "test", name)
    outdir = os.path.join(topdir, "test", "test-output" if frontend == "gcc" else f"test-output-{frontend}", name)
    if os.path.exists(outdir):
        # Clobber old database files to allow tests that count the number of
        # occurrences of certain symbols.
        for pat in ["*.sqlite", "*.sqlite-wal", "*.sqlite-shm", "*.xdb"]:
            for f in glob.glob(os.path.join(outdir, pat)):
                os.remove(f)
    else:
        os.mkdir(outdir)

    def compile(source, env_mods={}, options=""):
        if frontend == "clang":
            clang_bin = os.path.join(config["CLANG_DIR"], "bin")
            if source.endswith(".c"):
                compile = f"{clang_bin}/clang {CLANG_C_FLAGS}"
            else:
                compile = f"{clang_bin}/clang++ -std=c++17"
            sixgill = os.path.join(topdir, "clang", "xgill-clang.so")
        elif source.endswith(".c"):
            compile = os.environ.get("CC", f"{config['TARGET_CC']} -x c")
            sixgill = plugin
        else:
            compile = os.environ.get("CXX", f"{config['TARGET_CC']} -x c++")
            compile += " -std=c++17"
            sixgill = plugin
        cmd = "{compile} -c {source} -fplugin={sixgill} {options}".format(
            source=os.path.join(indir, source),
            compile=compile,
            sixgill=sixgill,
            options=options
        )
        print("Running %s" % cmd)
        os.environ['CCACHE_DISABLE'] = '1'
        os.environ.update(env_mods)
        return check_output(["/bin/sh", "-c", cmd], text=True)

    def load_db_entry(table, pattern, require=True):
        output = check_output([os.path.join(bindir, "xdbkeys"), table], universal_newlines=True)
        entries = output.splitlines()
        if not isinstance(pattern, typing.Pattern):
            pattern = re.compile(r'^' + pattern + '$')
        matches = [f for f in entries if re.search(pattern, f)]
        if len(matches) == 0:
            if require:
                raise Exception(f"entry /{pattern.pattern}/ not found in table {table}")
            return []
        if len(matches) > 1:
            raise Exception(f"multiple entries found for /{pattern.pattern}/ in table {table}")

        output = check_output([os.path.join(bindir, "xdbfind"), "-json", table, matches[0]], universal_newlines=True)
        return json.loads(output)

    def process_body(body):
        return Body(body)

    def process_bodies(bodies):
        return [process_body(b) for b in bodies]

    def equal(got, expected):
        if got != expected:
            print("Got '%s', expected '%s'" % (got, expected))

    os.chdir(outdir)
    execfile(os.path.join(indir, "test.py"))
