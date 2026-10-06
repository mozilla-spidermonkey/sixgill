import re
compile("source.cpp")

# Verify that we have a Cell struct annotated as a GC Thing.
cell = load_db_entry("comp", 'Cell')[0]
assert(cell['Kind'] == 'Struct')
annotations = cell['Annotation']
assert(len(annotations) == 1)
(tag, value) = annotations[0]['Name']
assert(tag == 'annotate')
assert(value == 'GC Thing')

# Make sure we don't lump together all detail::MaybeStorage::NonConstT. The
# types are named by their real names, not by the typedef used to refer to
# them.
assert(len(load_db_entry("comp", 'detail::MaybeStorage::NonConstT', require=False)) == 0)
assert(len(load_db_entry("comp", re.compile('NonConstT'), require=False)) == 0)

# But we *should* have the types!
load_db_entry("comp", 'CellStruct')
load_db_entry("comp", 'NonCellStruct')

# We should also have two anonymous types for the variables captured by the
# lambdas, qualified with the name of the function containing them and the
# lambda's line (respecting #line).
load_db_entry("comp", "_Z12anonny_nonnyv:<lambda:248>")
load_db_entry("comp", "_Z12anonny_nonnyv:<lambda:1000>")
