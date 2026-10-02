#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ParmUnnamed");

/*
Pick and Both leave parameters unnamed, as C++ lets a function that never reads one. The editor names every pin, and
the override AssetGen synthesizes names such a parameter P<index>; the function's own parameter is named the same way,
so Both has two parameters of two names, not two of none. A local may take the name C++ leaves free beside an unnamed
parameter, Fill's P0; the parameter's name then steps aside (P0_), or the frame would hold two P0 and the local's every
read and write would reach the first, the caller's X through the reference: Kept gives 706, X kept 7, Store 6.
*/
class ParmUnnamed : public AActor {
public:
  [[gnu::noinline]] int32 Pick(int32, int32 B) { return B * 3; }
  [[gnu::noinline]] int32 Both(int32, int32) { return 4; }
  int32 Use() { return Pick(1, 2) * 10 + Both(5, 6); }

  int32 Store = 0;
  [[gnu::noinline]] void Fill(int32&, int32 B) { int32 P0 = B * 2; Store = P0; }
  int32 Kept() { int32 X = 7; Fill(X, 3); return X * 100 + Store; }
};
