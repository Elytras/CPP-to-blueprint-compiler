#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ValueInitScalar");

/*
Braces and `T()` around a value that is not a struct: `E R{};`, `int32 N = int32();` and `AActor* A{};` are the type's
zero (value-initialisation), `E R{E::B};` and `int32 N{7}` the value in the braces. In a function body - a local, an
assignment, an argument, a return value, an array's element, a member of a braced UE_STRUCT, where `{}` is that
member's zero and not its default - and as a member's or a UE_STRUCT member's default, and a braced asset's value. A
UE_STRUCT's own `T()` as a default is its defaults.
*/
enum class EValuePick : uint8 { Zero, One, Two };
UE_ENUM(EValuePick);

struct FValueSlot {
  UE_STRUCT;
  EAttachmentRule Zeroed{};
  EAttachmentRule Kept{EAttachmentRule::KeepWorld};
  int32 Nil{};
  int32 Five{5};
};

class ValueInitScalar : public AActor {
public:
  EAttachmentRule Rule{};
  EAttachmentRule Kept{EAttachmentRule::KeepWorld};
  int32 Count{};
  int32 Seven{7};
  float Half{0.5f};
  AActor* Who{};
  FValueSlot Slot;
  FValueSlot Cleared = {{}, {}, {}, {}};
  FValueSlot Fresh = FValueSlot();
  int32 Parens = int32();
  EAttachmentRule RuleParens = EAttachmentRule();
  EAttachmentRule Held = EAttachmentRule::KeepWorld;
  AActor* Seen;

  int32 Take(EAttachmentRule R) { return (int32)R; }
  EAttachmentRule Zero() { return {}; }
  EAttachmentRule Keep() { return EAttachmentRule{EAttachmentRule::KeepWorld}; }

  int32 EnumBraces(int32 M) { EAttachmentRule R{}; return (int32)R + M; }
  int32 EnumEqBraces(int32 M) { EAttachmentRule R = {}; return (int32)R + M; }
  int32 EnumParens(int32 M) { EAttachmentRule R = EAttachmentRule(); return (int32)R + M; }
  int32 EnumValue(int32 M) { EAttachmentRule R{EAttachmentRule::KeepWorld}; return (int32)R + M; }
  int32 EnumArg(int32 M) { return Take(EAttachmentRule{}) + Take({EAttachmentRule::SnapToTarget}) * 10 + M; }
  int32 EnumAssign(int32 M) { Held = {}; return (int32)Held + M; }
  int32 EnumReturn(int32 M) { return (int32)Zero() + (int32)Keep() * 10 + M; }
  int32 OwnEnum(int32 M) { EValuePick P{}; EValuePick Q{EValuePick::Two}; return (int32)P + (int32)Q * 10 + M; }
  int32 IntBraces(int32 M) { int32 I{}; int32 J{7}; return I + J + M; }
  int32 IntParens(int32 M) { int32 I = int32(); return I + M; }
  int32 WideBraces(int32 M) { int64 I{}; uint8 B{}; return (int32)I + B + M; }
  float FloatBraces(float F) { float X{}; float Y{1.5f}; return X + Y + F; }
  bool BoolBraces(int32 M) { bool B{}; return B; }
  int32 ObjBraces(int32 M) { AActor* A{}; return A == nullptr ? 1 : 0; }
  int32 ObjAssign(int32 M) { Seen = this; Seen = {}; return Seen == nullptr ? 1 : 0; }
  int32 Elements(int32 M) { TArray<EAttachmentRule> A = {EAttachmentRule{}, EAttachmentRule::KeepWorld}; return (int32)A[0] + (int32)A[1] * 10 + M; }
  int32 SlotBraces(int32 M) { FValueSlot S = {{}, {}, {}, {}}; return (int32)S.Kept * 10 + S.Five + M; }
};

class UValueDef : public UPrimaryDataAsset {
public:
  int32 Count = 5;
  EAttachmentRule Rule = EAttachmentRule::KeepWorld;
};

/* `{}` for a member an asset names is its zero, written, as `.Count = 0` is: not the class default. */
UValueDef VD_Braces = {.Count = {}, .Rule = {}};
