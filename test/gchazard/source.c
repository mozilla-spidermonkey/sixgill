// Dummy C source file.
int foo(int);

struct dummy {
    int x;
    struct dummy* self;
};

int bar() {
    struct dummy d;
    foo(sizeof(d));
}
