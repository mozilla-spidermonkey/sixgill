compile("source.cpp")

# Verify that we have a Cell struct annotated as a GC Thing.
cell = load_db_entry("src_comp", 'Cell')[0]
assert(cell['Kind'] == 'Struct')
annotations = cell['Annotation']
assert(len(annotations) == 1)
(tag, value) = annotations[0]['Name']
assert(tag == 'annotate')
assert(value == 'GC Thing')

# Make sure we don't lump together all detail::MaybeStorage::NonConstT.
assert(len(load_db_entry("src_comp", 'detail::MaybeStorage::NonConstT', require=False)) == 0)

# But we *should* have the type!
load_db_entry("src_comp", 'detail::MaybeStorage<CellStruct>::NonConstT')
load_db_entry("src_comp", 'detail::MaybeStorage<NonCellStruct>::NonConstT')
