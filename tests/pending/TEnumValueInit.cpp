#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TEnumValueInit");

/*
`{}` and `TEnum<E>()` value-initialise a TEnum<E> as they do an E: the zero enumerator. In an assignment, an argument,
a return value, a struct literal's member (where `{}` is the member's zero, not its default), an array's element and a
conditional's arm, for an own UE_ENUM and for a native enum, and as an argument of a game function, whose enum
parameters UeApi spells TEnum<E>.
*/
enum class ETvPick : uint8 { Zero, One, Two, Three };
UE_ENUM(ETvPick);

struct FTvSlot {
  UE_STRUCT;
  TEnum<ETvPick> P = ETvPick::One;
  int32 N = 5;
  TEnum<EAttachmentRule> R = EAttachmentRule::KeepWorld;
};

class TEnumValueInit : public AActor {
public:
  TEnum<ETvPick> Held = ETvPick::Two;
  TEnum<EAttachmentRule> Rule = EAttachmentRule::KeepWorld;

  int32 Take(TEnum<ETvPick> P) { return (int32)(ETvPick)P; }
  TEnum<ETvPick> Zero() { return {}; }
  TEnum<ETvPick> ZeroParens() { return TEnum<ETvPick>(); }
  int32 Code(const FTvSlot& S) { return (int32)(ETvPick)S.P * 100 + S.N * 10 + (int32)(EAttachmentRule)S.R; }

  int32 Assign(int32 M) { Held = {}; return (int32)(ETvPick)Held + M; }
  int32 AssignParens(int32 M) { Held = TEnum<ETvPick>(); Rule = {}; return (int32)(ETvPick)Held + (int32)(EAttachmentRule)Rule * 10 + M; }
  int32 Arg(int32 M) { return Take({}) + Take(TEnum<ETvPick>()) * 10 + Take(ETvPick::Three) * 100 + M; }
  int32 Return(int32 M) { return (int32)(ETvPick)Zero() + (int32)(ETvPick)ZeroParens() * 10 + M; }
  int32 Literal(int32 M) { FTvSlot S = {{}, M + 1, {}}; return Code(S); }
  int32 Designated(int32 M) { FTvSlot S = {.P = ETvPick::Three, .R = {}}; return Code(S) + M; }
  int32 Kept(int32 M) { FTvSlot S = {ETvPick::Three}; return Code(S) + M; }
  int32 Elements(int32 M) {
    TArray<TEnum<ETvPick>> A = {{}, ETvPick::Two, TEnum<ETvPick>()};
    return (int32)(ETvPick)A[0] + (int32)(ETvPick)A[1] * 10 + (int32)(ETvPick)A[2] * 100 + M;
  }
  int32 Choose(int32 M) { TEnum<ETvPick> T = M == 0 ? TEnum<ETvPick>{} : Held; return (int32)(ETvPick)T; }
  void Detach() { K2_DetachFromActor({}, {}, {}); }
};
