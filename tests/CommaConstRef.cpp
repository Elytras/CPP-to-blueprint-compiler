#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CommaConstRef");

/*
A comma, or an assignment used as a value, bound to a `const T&` parameter beside an argument that writes what it
names. The parameter is the variable itself, which the callee reads when it runs, after every argument: whichever
order C++ picks for the arguments, it sees the write. A temporary copy made before that argument would not.
*/
struct FCcSlot {
  UE_STRUCT;
  int32 A = 1;
  int32 B = 2;
};

class CommaConstRef : public AActor {
public:
  int32 Count;
  int32 B;
  FCcSlot S;

  void Bump() { Count += 1; }
  int32 ModS() { S.A = 50; return 1; }
  int32 SetB() { B = 50; return 1; }
  int32 CodeRef(int32 X, const FCcSlot &V) { return X * 1000 + V.A; }
  int32 IntRef(int32 X, const int32 &V) { return X * 1000 + V; }
  int32 RefAhead(const int32 &V, int32 X) { return X * 1000 + V; }

  int32 Struct(int32 M) { S.A = M; return CodeRef(ModS(), (Bump(), S)); }
  int32 Int(int32 M) { B = M; return IntRef(SetB(), (Bump(), B)); }
  int32 Ahead(int32 M) { B = M; return RefAhead((Bump(), B), SetB()); }
  int32 Assign(int32 M) { B = 0; return IntRef(SetB(), B = M); }
};
