# Check whether we can link with cc1, not just cc1plus.
compile("source.c")

compile("rooting.cpp")
body = process_body(load_db_entry("src_body", re.compile(r'root_arg'))[0])

# Rendering positive and negative integers
marker1 = body.assignment_line('MARKER1')
equal(body.edge_from_line(marker1 + 2)['Exp'][1]['String'], '1')
equal(body.edge_from_line(marker1 + 3)['Exp'][1]['String'], '-1')

equal(body.edge_from_point(body.assignment_point('u1'))['Exp'][1]['String'], '1')
equal(body.edge_from_point(body.assignment_point('u2'))['Exp'][1]['String'], '4294967295')

assert('obj' in body['Variables'])
assert('random' in body['Variables'])
assert('other1' in body['Variables'])
assert('other2' in body['Variables'])

# Test function annotations
js_GC = process_body(load_db_entry("src_body", re.compile(r'js_GC'))[0])
annotations = js_GC['Variables']['void js_GC()']['Annotation']
assert(annotations)
found_call_tag = False
for annotation in annotations:
    (annType, value) = annotation['Name']
    if annType == 'annotate' and value == 'GC Call':
        found_call_tag = True
assert(found_call_tag)

# Test type annotations

# js::gc::Cell first
cell = load_db_entry("src_comp", 'js::gc::Cell')[0]
assert(cell['Kind'] == 'Struct')
annotations = cell['Annotation']
assert(len(annotations) == 1)
(tag, value) = annotations[0]['Name']
assert(tag == 'annotate')
assert(value == 'GC Pointer')

# Check JSObject inheritance.
JSObject = load_db_entry("src_comp", 'JSObject')[0]
bases = [ b['Base'] for b in JSObject['CSUBaseClass'] ]
assert('js::gc::Cell' in bases)
assert('Bogon' in bases)
assert(len(bases) == 2)

# Verify that function arguments remember whether they are references.

f = load_db_entry("src_body", re.compile(r'^void lambda_stuff'))
variables = f[0]['DefineVariable']

def param_type(name, variables=variables):
    for v in variables:
        if v['Variable']['Name'][1] == name:
            return v['Type']

func_var = [v for v in variables if v['Variable']['Name'][1] == 'lambda_stuff'][0]
args = func_var['Type']['TypeFunctionArgument']
assert(args[0]['Type']['Kind'] == 'Pointer')
assert(args[0]['Type']['Reference'] == 0)
assert(args[1]['Type']['Kind'] == 'Pointer')
assert(args[1]['Type']['Reference'] == 1)
assert(args[2]['Type']['Kind'] == 'Pointer')
assert(args[2]['Type']['Reference'] == 2)
assert(param_type('ptr')['Kind'] == 'Pointer')
assert(param_type('ptr')['Reference'] == 0)
assert(param_type('ref')['Kind'] == 'Pointer')
assert(param_type('ref')['Reference'] == 1)
assert(param_type('rref')['Kind'] == 'Pointer')
assert(param_type('rref')['Reference'] == 2)

# Regression test for reentrant XIL_GetFunctionFields calls.
compile("lazy.cpp")

# Prevent blowup from array initializers. This should clamp to 100 Assign expressions.
compile("array.cpp")
f = load_db_entry("src_body", re.compile(r'^void array_test'))
assigns = [e for e in f[0]['PEdge'] if e['Kind'] == 'Assign']
assert(len(assigns) == 100)
