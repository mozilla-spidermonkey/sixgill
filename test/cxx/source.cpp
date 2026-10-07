// C++ constructs in function bodies: constructor and destructor variants,
// temporaries, cleanups, virtual calls, new/delete, lambdas, static locals.

void GC();
struct JSObject;

struct Base {
  int x;
  virtual ~Base();
  virtual int vf(int);
  int mf(int);
};

struct Guard {
  int v;
  Guard(int);
  ~Guard();
  bool ok() const;
};

struct Member {
  Member();
  ~Member();
};

struct Widget : Base {
  Member m;
  JSObject *obj;
  Widget(JSObject *o) : Base(), obj(o) {}
  ~Widget() { GC(); }
};

struct VB {
  VB();
  ~VB();
};

struct Left : virtual VB {
  Left();
  ~Left();
};

struct Diamond : Left {
  Diamond() {}
};

Guard makeGuard();
void use(JSObject *);

int calls(Base *b, Widget &w) {
  return b->vf(1) + w.mf(2);
}

void locals(bool c) {
  Guard g(1);
  if (c)
    return;
  Guard t = makeGuard();
  GC();
}

int conditional(bool c) {
  return (c ? Guard(3).ok() : false) ? 1 : 2;
}

void loop(int n) {
  for (int i = 0; i < n; i++) {
    Guard g(i);
    if (i == 3)
      break;
    GC();
  }
}

void heap(JSObject *o, Base *b) {
  Widget *w = new Widget(o);
  delete w;
  delete b;
}

void lambda(JSObject *o) {
  auto f = [o](int) { use(o); };
  f(1);
}

int statics() {
  static Guard sg(5);
  return sg.v;
}
