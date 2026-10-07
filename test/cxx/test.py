# C++ function bodies. Most of these check the clang front end's output; the
# gcc front end represents several of these constructs differently (see
# local/clang-port-output-differences.md).

# Gecko builds with -fno-exceptions, so exception paths are not modeled.
compile("source.cpp", options="-fno-exceptions")


def body(name):
    return load_db_entry("body", name)[-1]


def exp_name(exp):
    """Name of the variable or field at the root of an expression."""
    while exp['Kind'] in ('Drf', 'Fld') and 'Variable' not in exp:
        if exp['Kind'] == 'Fld' and exp.get('Exp', [{}])[0].get('Kind') == 'Empty':
            return exp['Field']['Name'][0]
        exp = exp['Exp'][0]
    return exp['Variable']['Name'][0]


def calls(b, exp_name=exp_name):
    """The calls in a body, as (point, callee name, instance kind) in point order."""
    result = []
    for edge in b.get('PEdge', []):
        if edge['Kind'] != 'Call':
            continue
        callee = edge['Exp'][0]
        name = callee['Variable']['Name'][0] if callee['Kind'] == 'Var' else exp_name(callee)
        instance = edge.get('PEdgeCallInstance', {}).get('Exp')
        result.append((edge['Index'][0], name, instance['Kind'] if instance else None))
    return sorted(result)


def callees(b, calls=calls):
    return [name for (_, name, _) in calls(b)]


# Virtual calls go through the method's field, direct calls name the function.
calls_body = body('_Z5callsP4BaseR6Widget')
assert('vf' in callees(calls_body))
assert('_ZN4Base2mfEi' in callees(calls_body))

if frontend == "clang":
    # Constructor and destructor variants call each other.
    assert(callees(body('_ZN6WidgetC1EP8JSObject')) == ['_ZN6WidgetC2EP8JSObject'])
    assert(callees(body('_ZN6WidgetC2EP8JSObject')) == ['_ZN4BaseC2Ev', '_ZN6MemberC1Ev'])
    assert(callees(body('_ZN6WidgetD0Ev')) == ['_ZN6WidgetD1Ev', '_ZdlPvm'])
    assert(callees(body('_ZN6WidgetD1Ev')) == ['_ZN6WidgetD2Ev'])
    assert(callees(body('_ZN6WidgetD2Ev')) == ['_Z2GCv', '_ZN6MemberD1Ev', '_ZN4BaseD2Ev'])

    # Only the complete object constructor constructs virtual bases.
    assert(callees(body('_ZN7DiamondC1Ev')) == ['_ZN2VBC2Ev', '_ZN4LeftC2Ev'])
    assert(callees(body('_ZN7DiamondC2Ev')) == ['_ZN4LeftC2Ev'])

    # Locals are destroyed on every path out of their scope, with the
    # variable itself as the instance. The result of makeGuard() is
    # constructed directly in t.
    locals_calls = calls(body('_Z6localsb'))
    assert([name for (_, name, _) in locals_calls].count('_ZN5GuardD1Ev') == 3)
    assert(all(kind == 'Var' for (_, name, kind) in locals_calls if name.startswith('_ZN5Guard')))
    local_vars = [v['Variable']['Name'][0] for v in body('_Z6localsb')['DefineVariable']]
    assert(not any(name.startswith('__temp') for name in local_vars))

    # A temporary constructed on only one path is destroyed only on that path,
    # guarded by a flag.
    cond = body('_Z11conditionalb')
    dtor_edges = [e for e in cond['PEdge']
                  if e['Kind'] == 'Call' and e['Exp'][0].get('Variable', {}).get('Name', [''])[0] == '_ZN5GuardD1Ev']
    assert(len(dtor_edges) == 1)
    dtor_point = dtor_edges[0]['Index'][0]
    assume_into = [e for e in cond['PEdge'] if e['Kind'] == 'Assume' and e['Index'][1] == dtor_point]
    assert(len(assume_into) == 1)

    # Breaking out of a loop destroys the loop body's locals, as does the end
    # of each iteration. (After loop splitting, the break path is in the outer
    # body.)
    loop_bodies = load_db_entry("body", '_Z4loopi')
    loop_dtors = 0
    for b in loop_bodies:
        loop_dtors += callees(b).count('_ZN5GuardD1Ev')
    assert(loop_dtors == 2)

    # new constructs in the allocated memory, delete of a polymorphic object
    # calls the deleting destructor through its field.
    heap_calls = calls(body('_Z4heapP8JSObjectP4Base'))
    assert([name for (_, name, _) in heap_calls] ==
           ['_Znwm', '_ZN6WidgetC1EP8JSObject', '__dt_del ', '__dt_del '])

    # Captured variables are initialized from the closure at the start of the
    # lambda body.
    lam = load_db_entry("body", re.compile(r'_ZZ6lambdaP8JSObjectENK3\$_0clEi'))[-1]
    entry_edges = [e for e in lam['PEdge'] if e['Index'][0] == lam['Index'][0]]
    assert(len(entry_edges) == 1)
    assert(entry_edges[0]['Kind'] == 'Assign')
    assert(entry_edges[0]['Exp'][0]['Variable']['Name'][0] == 'o')

    # Static locals are initialized under a test of their guard variable, and
    # their destructor is registered.
    assert(callees(body('_Z7staticsv')) == ['_ZN5GuardC1Ei', '__cxa_atexit'])
