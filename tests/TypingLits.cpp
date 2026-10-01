#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TypingLits");

/*
Literals where the receiving property decides their width. Each const opcode writes a fixed size wherever it is
evaluated (ScriptCore.cpp execIntConst 3142, execByteConst 3160, ...), so the literal a member, a parameter or a
return value gets has to be the one of that property's type: an int32 constant stepped into a uint8 parameter writes
3 bytes past it. Fill stores into every scalar kind a class can hold, FillVector a struct; Calls passes one to a
script parameter of each kind; Natives passes a byte to a native uint8 parameter; Hide writes two native bitfield
bools of AActor, which only EX_LetBool masks into their byte; Flags writes two bitfield bools that share a byte of a
native struct member (FHitResult), through EX_StructMemberContext.
*/
enum class ETypingPick : uint8 { First, Second, Third };
UE_ENUM(ETypingPick);

class TypingLits : public AActor {
  uint8        B;
  ETypingPick  P;
  bool         F;
  int64        W;
  float        R;
  FName        N;
  FString      S;
  FText        T;
  FVector      V;
  int32        I;
  int32        Touched;
  FHitResult   Hit;

public:
  uint8       TakeByte(uint8 X) { return X; }
  int32       TakeInt(int32 X) { return X; }
  ETypingPick TakePick(ETypingPick X) { return X; }
  bool        TakeBool(bool X) { return X; }
  int64       TakeWide(int64 X) { return X; }
  float       TakeFloat(float X) { return X; }

  void Fill() {
    B = 200;
    P = ETypingPick::Third;
    F = true;
    W = 5000000000;
    R = 2.5f;
    N = FName("tag");
    S = "s";
    T = "t";
    I = 7;
  }

  void FillVector() { V = FVector(1, 2, 3); }

  int32 Calls() {
    return TakeByte(44) + TakeInt(-2) + (int32)TakePick(ETypingPick::Second) + (TakeBool(true) ? 10 : 0) +
           (int32)TakeWide(3) + (int32)TakeFloat(2);
  }

  int32 Natives(uint8 X) { return UKismetMathLibrary::Conv_ByteToInt(250) + UKismetMathLibrary::Conv_ByteToInt(X); }

  void Hide() {
    bHidden = true;
    bCanBeDamaged = false;
  }

  void Flags() {
    Hit.bStartPenetrating = true;
    Hit.bBlockingHit = false;
    Hit.Time = 0.5f;
  }

  /* `return` of a void call in a void function: the call runs, and nothing is written through the null result a
     ProcessEvent call of a function with no return value has (ScriptCore.cpp 1123-1133). */
  void Touch() { Touched = Touched + 1; }
  void Relay() { return Touch(); }
};
