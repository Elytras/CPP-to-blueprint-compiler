#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/UpdateRef");

/*
A compound assignment or a prefix ++ passed to a reference parameter: the parameter is the variable itself, which the
callee reads, and for a `T&` writes, after every argument, as C++ binds a reference. IncRef writes 3 more into it,
so after `IncRef(1, B += 1)` B is M + 4. Beside an argument that writes B too (SetB), C++ allows either order of the
two arguments, and the callee sees the variable after both. A struct member read as a `const T&` (Ahead) is read
when the callee runs as well, after ModS wrote it.
*/
struct FUrSlot {
  UE_STRUCT;
  int32 A = 1;
  int32 B = 2;
};

class UpdateRef : public AActor {
public:
  int32 B;
  FUrSlot S;
  [[gnu::noinline]] int32 SetB() { B = 50; return 1; }
  [[gnu::noinline]] int32 IntRef(int32 X, const int32& V) { return X * 1000 + V; }
  [[gnu::noinline]] int32 IncRef(int32 X, int32& V) { V += 3; return X * 1000 + V; }
  [[gnu::noinline]] int32 ModS() { S.A = 50; return 1; }
  [[gnu::noinline]] int32 Ahead(const int32& V, int32 X) { return X * 1000 + V; }

  int32 Alone(int32 M) { B = M; return IncRef(1, B += 1); }
  int32 PreAlone(int32 M) { B = M; return IncRef(1, ++B); }
  int32 ConstBeside(int32 M) { B = M; return IntRef(SetB(), B += 1); }
  int32 PreBeside(int32 M) { B = M; return IntRef(SetB(), ++B); }
  int32 RefBeside(int32 M) { B = M; return IncRef(SetB(), B += 1); }
  int32 Member(int32 M) { S.A = M; return Ahead(S.A, ModS()); }
  int32 Read(int32 M) { B = M; int32 V = (B += 1) * 2; return V * 100 + B; }
};
