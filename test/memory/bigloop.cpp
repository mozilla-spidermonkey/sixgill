// Simulate the extremely slow behavior seen in Preferences.cpp that originates
// from having lots of `do { ... } while(false)` "loops".
//
// Note that this file must be compiled as C++, because when compiled as C,
// sixgill creates a CFG containing loops with back edges, so the optimization
// of discarding loop heads with a single incoming edge does not work.

#include <stdlib.h>

extern int XRE_IsParentProcess();
extern int XRE_IsContentProcess();
extern void MOZ_NoReturn(int line);

#define MOZ_CONCAT2(x, y) x##y
#define MOZ_CONCAT(x, y) MOZ_CONCAT2(x, y)

# define MOZ_UNLIKELY(x) (__builtin_expect(!!(x), 0))

#  define MOZ_FUZZING_HANDLE_CRASH_EVENT2(aType, aReason) \
    do {                                                  \
    } while (false)

#    define MOZ_REALLY_CRASH(line)                                  \
      do {                                                          \
        *((volatile int*)0x1) = line; /* NOLINT */ \
        abort();                                                    \
      } while (false)

#define MOZ_ASSERT_HELPER(kind, expr)                         \
  do {                                                         \
    if (MOZ_UNLIKELY(!expr)) {    \
      MOZ_FUZZING_HANDLE_CRASH_EVENT2(kind, #expr);            \
      MOZ_REALLY_CRASH(__LINE__);                              \
    }                                                          \
  } while (false)
#define MOZ_ASSERT_HELPER11(kind, expr, a) MOZ_ASSERT_HELPER(kind, expr)

#define MOZ_PASTE_PREFIX_AND_ARG_COUNT_GLUE(a, b) a b
#define MOZ_PASTE_PREFIX_AND_ARG_COUNT(aPrefix, ...) \
  MOZ_PASTE_PREFIX_AND_ARG_COUNT_GLUE(MOZ_CONCAT,    \
                                      (aPrefix, 11))
#define MOZ_ASSERT_GLUE(a, b) a b
#  define MOZ_DIAGNOSTIC_ASSERT(...)                                    \
    MOZ_ASSERT_GLUE(                                                    \
        MOZ_PASTE_PREFIX_AND_ARG_COUNT(MOZ_ASSERT_HELPER, __VA_ARGS__), \
        ("MOZ_DIAGNOSTIC_ASSERT", __VA_ARGS__))

#define ALWAYS_PREF \
    if (!XRE_IsParentProcess()) {                                             \
      MOZ_DIAGNOSTIC_ASSERT(                                                   \
          XRE_IsContentProcess(),             \
          "Should not access the preference 'test' in Content Processes"); \
    }

#define REPEAT_4(X) X X X X
#define REPEAT_1024(X) REPEAT_4(REPEAT_4(REPEAT_4(REPEAT_4(REPEAT_4(X)))))

void pref_test(char* a)
{
    a++;
    REPEAT_1024(ALWAYS_PREF)
}
