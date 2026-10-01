#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ParmUnnamed");

/*
Pick and Both leave parameters unnamed, as C++ lets a function that never reads one. The editor names every pin, and
the override AssetGen synthesizes names such a parameter P<index>; the function's own parameter is named the same way,
so Both has two parameters of two names, not two of none.
*/
class ParmUnnamed : public AActor {
public:
  [[gnu::noinline]] int32 Pick(int32, int32 B) { return B * 3; }
  [[gnu::noinline]] int32 Both(int32, int32) { return 4; }
  int32 Use() { return Pick(1, 2) * 10 + Both(5, 6); }
};
