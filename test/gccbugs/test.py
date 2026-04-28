compile("source.cpp")
import re

# Verify that we have a Cell struct annotated as a GC Thing.
cell = load_db_entry("comp", 'Cell')[0]
assert(cell['Kind'] == 'Struct')
annotations = cell['Annotation']
assert(len(annotations) == 1)
(tag, value) = annotations[0]['Name']
assert(tag == 'annotate')
assert(value == 'GC Thing')

# Make sure we don't lump together all detail::MaybeStorage::NonConstT.
assert(len(load_db_entry("comp", 'detail::MaybeStorage::NonConstT', require=False)) == 0)

# But we *should* have the type!
load_db_entry("comp", 'detail::MaybeStorage<CellStruct>::NonConstT')
load_db_entry("comp", 'detail::MaybeStorage<NonCellStruct>::NonConstT')

# We should also have two anonymous types for the variables captured by the
# lambda, and they should be qualified with their filenames.
load_db_entry("comp", re.compile(r"^\w+#source.cpp:\._anon_"))
load_db_entry("comp", re.compile(r"^\w+#fakesource.cpp:\._anon_"))
