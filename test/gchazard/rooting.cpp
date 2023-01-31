#define ANNOTATE(tagname) __attribute__((annotate(tagname)))

namespace js {
namespace gc {
struct Cell { int f; } ANNOTATE("GC Pointer");
}
}

struct Bogon {
};

struct JSObject : public js::gc::Cell, public Bogon {
    int g;
};

extern void js_GC() ANNOTATE("GC Call") ANNOTATE("Slow");

void js_GC() {}

void root_arg(JSObject *obj, JSObject *random)
{
  obj = random;

  JSObject *other1 = obj;
  js_GC();

  float MARKER1 = 0;
  JSObject *other2 = obj;
  other1->f = 1;
  other2->f = -1;

  unsigned int u1 = 1;
  unsigned int u2 = -1;
}

template <typename T, typename Process>
void dostuff(T t, Process&& process) {
    process(t);
}

void lambda_stuff(int* ptr, int& ref, int&& rref) {
    dostuff(1, [](auto x) {
        return lambda_stuff;
    });
    int local;
    dostuff(1.0, [&](auto x) {
        return x + local;
    });
}
