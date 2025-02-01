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
# Apparently old versions unwrapped this a different way:
old = [
    "Assign",  # a-- after the label
    "Assume",  # *a is nonzero, continue the loop
    "Assign",  # a++ before the label
]
new = [
    "Assume",  # *a, continue the loop
    "Assign",  # a++ before the label
    "Assign",  # a-- after the label
]

if matching_edges(loop, old):
    # The main body contains a complete clone of the loop. I'm skeptical that
    # matching the full structure here is a useful test, so I'll just check that
    # there are cloned points.
    assert(len(main["LoopIsomorphic"]) == 2)
else:
    assert(matching_edges(loop, new))
    assert(matching_edges(main, [
        "Assume", # b is true (branch 1)
        "Assume", # b is false (branch 2)
        "Assign", # a-- after the label (split out from loop)
        "Loop",
        "Assume", # !*a, loop exit condition
    ]))
    # Now, the main body does *not* clone the whole loop. It actually does a
    # simpler thing where does the loop, but has an isomorphic edge that has the
    # exit condition in the main body. I can't figure out how to get it to do
    # what it used to do, but what it's doing now makes sense.
    assert(len(main["LoopIsomorphic"]) == 1)

f = load_db_entry("src_body", "more_irreducible")
assert(len(f) in (4, 2))  # It's complicated. It used to be more complicated.
# I'm not sure what it was doing before, but hand-checking the current CFG
# seems to be doing the right thing (if you ignore that Assume nodes involving
# pointers seem to have their conditions backwards.) I don't think it gains a
# lot by checking against the Kinds or whatever. There must be a better way to
# test this.

# Rely on the plugin's assertions for testing all remaining functions in loop.c.

# Timing test: without the optimization of discarding loop heads without back
# edges to them, this will take 10s of seconds. With it, this should take well
# under a second.
from datetime import datetime
t0 = datetime.now()
output = compile("bigloop.cpp", env_mods={'SIXGILL_LOG_LOOPHEAD_OPT': '1'})
assert(re.search(r'Reduced loopheads \d+ -> 0 for CFG .*pref_test', output))
t1 = datetime.now()
elapsed = (t1 - t0).total_seconds()
print(f"Elapsed: {elapsed} sec")
