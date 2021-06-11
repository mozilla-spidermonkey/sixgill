// Compiling this file caused an Internal Compiler Error due to a reentrant
// call to XIL_GetFunctionFields.

class Child2;

class Base {
 public:
  virtual bool Shutdown() = 0;
  virtual Child2* AsChild2() = 0; // Child2 translated with no members.
};

class Trouble {
  void Kill();
  Base* mBase;
};

class Child1 : public Base {
  int GetMember() { return mMember; }
  int mMember;
};

class Child2 : public Base {
  explicit Child2() = default;
};

void Trouble::Kill() {
  // Need to look up the Shutdown vtable entry in Base, which calls
  // XIL_GetFunctionFields(Base), and something (Shutdown, probably) caused its
  // field list to be updated since the last call. Processing Base::AsChild2
  // triggers XIL_GetFunctionFields(Child2). Child2 while enumerating its base
  // classes calls XIL_GetFunctionFields(Base), reentrantly, which asserts.
  //
  // Intervening XIL_TranslateRecordType(Base) calls don't help, because it was
  // already translated and so does nothing. (XIL_TranslateRecordType calls
  // things in an order guaranteed to not run into problems with
  // XIL_GetFunctionFields; it's the call from XIL_GetVTableField that can
  // result in a bad ordering.) It's just that the original translation had an
  // incomplete set of fields, and so all fields need to be reprocessed.
  mBase->Shutdown();
}
