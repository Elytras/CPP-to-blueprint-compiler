#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ConstObjects");

/* A constant of a plain C++ struct: REFERENCE.md#constant-objects. No Blueprint type stands for FTable or a handle, and
   none is cooked: each use is what the braces gave the member. */
struct FLimits {
  int32 Low;
  int32 High;
};
struct FTable {
  int32 X;
  float Y;
  FLimits Range;
  const char *Label;
};
constexpr FTable kTable = {5, 2.5f, {-3, 9}, "table"};
constexpr int32 kTwice = kTable.X * 2;

/* A handle: what it names is its members, read off its braces wherever a method of it is called. */
struct FIntHandle {
  const char *Name;
  int32 Default;
  int32 Get(int32 Stored) const { return Stored != 0 ? Stored : Default; }
  int32 Twice(int32 Stored) const { return Get(Stored) * 2; } // another method of the same constant
  FName Key() const { return Name; }
  int32 Spare() const { return (*this).Default - 1; }
  int32 Late(int32 Stored) const; // defined below the struct
};
inline int32 FIntHandle::Late(int32 Stored) const { return Twice(Stored) + Default; }
constexpr FIntHandle kWaves = {"Waves.Max", 12};

template <class T> struct THandle {
  const char *Name;
  T Default;
  T Or(T Stored, bool bHas) const { return bHas ? Stored : Default; }
  FName Key() const { return Name; }
};

class ConstObjects : public AActor {
public:
  static constexpr FIntHandle Lives{"Lives", 3};
  static constexpr THandle<float> Volume{"Audio.Volume", 0.5f};
  static constexpr THandle<int32> Count{"Count", 7};

  int32 Seed = kTable.X + Lives.Default + kTwice; // a default: 5 + 3 + 10

  int32 Members() { return kTable.X * 100 + kTable.Range.Low + kTable.Range.High; }
  float Real(float V) { return V * kTable.Y; }
  FName Label() { return kTable.Label; }
  int32 Shift(int32 V) { return V << kTable.X; } // a constant amount
  int32 Handle(int32 Stored) { return kWaves.Get(Stored) + kWaves.Twice(Stored) * 1000; }
  int32 Statics(int32 Stored) { return Lives.Get(Stored) * 10 + this->Lives.Default + kWaves.Spare() * 100; }
  int32 Below(int32 Stored) { return kWaves.Late(Stored) + Lives.Late(Stored) * 1000; }
  int32 Templated(int32 Stored, bool bHas) { return Count.Or(Stored, bHas); }
  float TemplatedReal(float Stored, bool bHas) { return Volume.Or(Stored, bHas); }
  FName Keys(int32 Which) { return Which == 0 ? kWaves.Key() : Which == 1 ? Volume.Key() : Count.Key(); }
};
