
#include <stdlib.h>
#include "xgill.h"

// symbols defined by cc1plus but not cc1. we need these stubs so that
// we can use the plugin when compiling both C and CXX files. fortunately
// when this plugin is loaded by cc1plus these will be ignored and the real
// symbols called instead.
//
// decl_as_string is an exception. We *will* call this from C, so we provide a
// fallback implementation here.

EXTERN_BEGIN
void lang_check_failed() { abort(); }
const char * type_as_string(tree t, int x) { abort(); return NULL; }
const char * decl_as_string(tree t, int x) { return IDENTIFIER_POINTER(DECL_NAME(t)); }
const char * expr_as_string(tree t, int x) { abort(); return NULL; }
int cp_type_quals(const_tree t) { abort(); return 0; }
tree look_for_overrides_here(tree t, tree tt) { abort(); return NULL; }
tree get_template_info(const_tree t) { abort(); return NULL; }
tree get_innermost_template_args(tree t, int n) { abort(); return NULL; }
tree most_general_template(tree t) { abort(); return NULL; }
tree tsubst(tree t, tree args, tsubst_flags_t f, tree in) { abort(); return NULL; }
tree tsubst_expr(tree t, tree args, tsubst_flags_t f, tree in, bool b) { abort(); return NULL; }
int template_args_equal(tree a, tree b, bool p) { abort(); return 0; }
tree maybe_constant_value(tree t, tree d, bool m) { abort(); return NULL; }
tree cp_build_qualified_type_real(tree t, int q, tsubst_flags_t f) { abort(); return NULL; }
bool comptypes(tree a, tree b, int s) { abort(); return false; }
int uses_template_parms(tree t) { abort(); return 0; }
EXTERN_END
