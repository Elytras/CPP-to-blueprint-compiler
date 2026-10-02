#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/UpdateLoop");

/*
A compound assignment or a prefix ++ passed to a reference parameter where no statement before the one holding it
fits: a loop condition, which reruns it on every trip (a do-while's too), the right side of && and the arms of ?:,
which may not run, and a call on another object, whose object C++ evaluates before the arguments. The parameter is
still the variable itself: IncRef's `V += 3` lands in B (or the local L), so each trip moves it by 4. Inline
(IncInline) or not, member or local.
*/
class UpdateLoop : public AActor {
public:
  int32 B;
  UpdateLoop* P;
  [[gnu::noinline]] int32 IncRef(int32 X, int32& V) { V += 3; return X * 1000 + V; }
  int32 IncInline(int32 X, int32& V) { V += 3; return X * 1000 + V; }

  int32 Loop(int32 M) { B = M; int32 N = 0; while (IncRef(0, B += 1) < 20) N += 1; return N * 100 + B; }
  int32 LoopPre(int32 M) { B = M; int32 N = 0; while (IncRef(0, ++B) < 20) N += 1; return N * 100 + B; }
  int32 LoopInline(int32 M) { B = M; int32 N = 0; while (IncInline(0, B += 1) < 20) N += 1; return N * 100 + B; }
  int32 LoopLocal(int32 M) { int32 L = M; int32 N = 0; for (; IncRef(0, L += 1) < 20; N += 1) {} return N * 100 + L; }
  int32 AndRight(int32 M) { B = M; bool R = M > 0 && IncRef(0, B += 1) > 5; return (R ? 1000 : 0) + B; }
  int32 DoLoop(int32 M) { B = M; int32 N = 0; do { N += 1; } while (IncRef(0, ++B) < 20); return N * 100 + B; }
  int32 Arms(int32 M) { B = M; int32 R = M > 0 ? IncRef(0, B += 1) : IncRef(1, ++B); return R * 100 + B; }
  int32 Other(int32 M) { B = M; P = this; int32 R = P->IncRef(1, B += 1); return R * 100 + B; }
};
