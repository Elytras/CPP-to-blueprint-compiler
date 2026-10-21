#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NaturalForms");

/*
Three forms written as C++ would write them, which must run as C++ runs them:
- A function library's static is called, not copied into its callers, unless it asks with [[gnu::always_inline]];
  an `inline` static has no function and expands.
- `==` / `!=` over bytes (uint8, byte enums, `(uint8)E::X`, a constant 0..255) compare the bytes; a constant that is no
  byte keeps C++'s int compare.
- A `switch` on an int, int64, byte or enum jumps straight to its case: fallthrough, break, default first or last, no
  default, a value no case names, nested switches, a switch in an inline function expanded twice and one in a function
  that waits.
*/
enum class ENfMood : uint8 { Calm, Angry = 7, Wild = 200 };
UE_ENUM(ENfMood);
enum ENfPlain : uint8 { NfA, NfB = 9 };
UE_ENUM(ENfPlain);
enum class ENfWide : int32 { Low = -5, Mid = 0, High = 100000 };
UE_ENUM(ENfWide);

class NaturalForms : public UBlueprintFunctionLibrary {
public:
  static int32 Twice(int32 V) { return V * 2; }
  [[gnu::always_inline]] static int32 Thrice(int32 V) { return V * 3; }
  static inline int32 Half(int32 V) { return V / 2; }
  static int32 UseLib(int32 V) { return Twice(V) + Thrice(V) + Half(V); }
};

class NfUser : public AActor {
public:
  int32  Stage;
  int32  Seen;
  uint8  Byte;
  ENfMood Mood;

  int32 CallsLib(int32 V) { return NaturalForms::Twice(V) + NaturalForms::Thrice(V); }

  /* Byte compares. */
  int32 ByteEq(uint8 A, uint8 B) { return (A == B ? 1 : 0) + (A != B ? 10 : 0); }
  int32 ByteConst(uint8 A) { return (A == 255 ? 1 : 0) + (A != 0 ? 10 : 0) + (A == 300 ? 100 : 0) + (A != -1 ? 1000 : 0); }
  int32 MoodIs(ENfMood M) { return ((uint8)M == (uint8)ENfMood::Wild ? 1 : 0) + (M != ENfMood::Angry ? 10 : 0); }
  int32 PlainIs(uint8 A) { return (A == NfB ? 1 : 0) + (NfA != A ? 10 : 0); }
  int32 MemberIs() { return (Byte == (uint8)Mood ? 1 : 0) + ((uint8)ENfMood::Angry == Byte ? 10 : 0); }

  /* Switches. */
  int32 Wide(int64 V) {
    switch (V) {
    case 1: return 10;
    case 5000000000LL: return 20;
    case -7: return 30;
    default: return 0;
    }
  }
  int32 OnMood(ENfMood M) {
    int32 R = 0;
    switch (M) {
    case ENfMood::Calm: R += 1; [[fallthrough]];
    case ENfMood::Angry: R += 10; break;
    case ENfMood::Wild: R += 100;
    }
    return R;
  }
  int32 OnWide(ENfWide W) {
    switch (W) {
    default: return -1;
    case ENfWide::Low: return 1;
    case ENfWide::High: return 3;
    }
  }
  int32 OnlyDefault(int32 V) {
    int32 R = 5;
    switch (V) { default: R = V * 2; }
    return R;
  }
  int32 Empty(int32 V) {
    switch (V) {}
    return V + 1;
  }
  int32 Nested(int32 A, int32 B) {
    switch (A) {
    case 0:
      switch (B) {
      case 0: return 1;
      case 1: break;
      default: return 2;
      }
      return 3;
    case 1: return 4;
    }
    return 5;
  }
  inline int32 Pick(int32 V) {
    switch (V) {
    case 2: return 20;
    case 3: return 30;
    }
    return 0;
  }
  int32 PickTwice(int32 A, int32 B) { return Pick(A) * 100 + Pick(B); }
  int32 InLoop(int32 N) {
    int32 R = 0;
    for (int32 I = 0; I < N; ++I) {
      switch (I % 4) {
      case 0: continue;
      case 1: R += 1; break;
      case 3: R += 100; [[fallthrough]];
      default: R += 10;
      }
    }
    return R;
  }
  /* The value is taken before the wait, the cases after it. */
  void Waits(int32 Code) {
    Stage = 1;
    UKismetSystemLibrary::Delay(this, 0.5f);
    switch (Code) {
    case 4: Stage = 40; break;
    case 9: Stage = 90; break;
    default: Stage = -1;
    }
    Seen = Seen + 1;
  }
};
