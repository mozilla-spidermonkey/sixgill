import re

compile("autoptr.cc")
compile("blowup.c")
compile("callgraph.c")
compile("char_array.c")
compile("cil.c")
compile("diamond.cc")
compile("enum.c")
compile("fixpoint.c")
compile("fixpoint_indirect.c")
compile("inherit.cc")
compile("mem.c")
compile("virt_diamond.cc")
compile("virt_modset.cc")

# Hm... there is a missing parameter name, which is a hard error in C, and yet the filename implies that it is expected.
#compile("missing.c")

compile("loop.c")

def summarized_edges(body):
    return [f"{e['Kind']}({e['Index'][0]},{e['Index'][1]})" for e in body["PEdge"]]

def matching_edges(body, expect):
    edges = body["PEdge"]
    if len(edges) != len(expect):
        return False
    for edge, want in zip(body["PEdge"], expect):
        if edge["Kind"] != want:
            return False
    return True

f = load_db_entry("src_body", "nested")
assert(len(f) == 3)  # outer body, two loops
inner, outer, main = f
assert(matching_edges(main, ["Loop", "Assume"]))
assert(matching_edges(outer, ["Assume", "Loop", "Assume", "Assign"]))
assert(matching_edges(inner, ["Assume", "Assign"]))

f = load_db_entry("src_body", "noloop")
assert(len(f) == 1)  # do { ... } while(0) not treated as a loop

f = load_db_entry("src_body", "basic")
assert(len(f) == 2)  # outer body, loop
loop, main = f
assert(matching_edges(main, [
    "Assign",  # a++
    "Loop",    # while
    "Assume",  # *a is zero (while loop end condition)
]))
assert(matching_edges(loop, [
    "Assume",  # *a is nonzero (while loop body should run)
    "Assign",  # a++
]))

f = load_db_entry("src_body", "gotos")
assert(len(f) == 2)  # loop manufactured from goto
loop, main = f
assert(matching_edges(loop, [
    # sixgill sorts in source order, so this is different from the basic loop above.
    "Assign",  # a++
    "Assume",  # *a is nonzero
]))
assert(matching_edges(main, [
    # The body starts with the loop head here, unlike the basic case that has a leading a++.
    "Loop",    # destination of goto
    "Assign",  # a++
    "Assume",  # *a is zero (termination condition)
]))

f = load_db_entry("src_body", "irreducible")
assert(len(f) == 2)  # Single loop head (label 'L:')
loop, main = f
assert(matching_edges(loop, [
    "Assign",  # a++ after the label
    "Assume",  # *a is nonzero, continue the loop
    "Assign",  # a++ before the label
]))
# The main body contains a complete clone of the loop. I'm skeptical that
# matching the full structure here is a useful test, so I'll just check that
# there are cloned points.
assert(len(main["LoopIsomorphic"]) == 2)

f = load_db_entry("src_body", "more_irreducible")
assert(len(f) == 4)  # It's complicated.

# Rely on the plugin's assertions for testing all remaining functions in loop.c.

# Timing test: without the optimization of discarding loop heads without back
# edges to them, this will take 10s of seconds. With it, this should take well
# under a second.
from datetime import datetime
t0 = datetime.now()
output = compile("bigloop.cpp", env_mods={'SIXGILL_LOG_LOOPHEAD_OPT': '1'})
t1 = datetime.now()
elapsed = (t1 - t0).total_seconds()
print(f"Elapsed: {elapsed} sec")
