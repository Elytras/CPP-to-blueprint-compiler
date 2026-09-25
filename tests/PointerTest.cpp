#include "UeApi/Types.h"

#include "../include/Intrin.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PointerTest");

/* No FDeref here on purpose: the compiler synthesizes the read-hoist scratch for any mod that
   derefs without declaring one, and this mod is the check that it still does. */

class PointerTest : public AActor {
  int32 Failures = 0;

  void Check(bool bOk, FString What) {
    if (!bOk) {
      Failures = Failures + 1;
      UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage("PointerTest FAILED: " + What);
    }
  }

  /* A raw pointer parameter and return value are int64 addresses. */
  int32 *Advance(int32 *Base, int32 Count) { return Base + Count; }

  /* A T& parameter stays an out-parm. */
  void Bump(int32 &Value) { Value = Value + 1; }

  /* int8 and signed char are signed: a 0x80..0xFF byte reads as a negative int, a uint8 as 128..255. */
  int32 SignedByteMix(int8 *P) { return *P * 3 + P[1]; }
  int32 SignedCharAt(signed char *P, int32 I) { return P[I]; }
  bool SignedByteNegative(int8 *P) { return *P < 0; }
  int32 UnsignedByteAt(uint8 *P, int32 I) { return P[I]; }
  /* (uint8) of a signed byte read is 128..255 again, inside an int expression too. */
  int32 SignedByteAsUnsigned(int8 *P) { return (uint8)*P * 1000 + static_cast<uint8>(P[1]); }
  bool SignedByteIsMax(int8 *P) { return (uint8)*P == 255; }

  /* An intrinsic whose value goes nowhere, stored in a local nothing reads or discarded, is no call at all: it has no
     UFunction, and an EX_CallMath on null would crash the VM. */
  int32 DiscardedIntrinsics(int64 P, FName N) {
    int32 I = __NameIndex__(N);
    int64 A = __AddrOf__(this);
    int32 V = __Read32__(P);
    __NameIndex__(N);
    __Read32__(P);
    return 3;
  }

public:
  void ReceiveBeginPlay() {
    TArray<int32> Items;
    Items.Add(10);
    Items.Add(20);
    Items.Add(30);

    int32 *P = &Items[0];
    Check(P[1] == 20, "P[1] reads the second element");

    int32 *Q = P;
    Q += 2;
    Check(*Q == 30, "Q += 2 steps two elements");
    --Q;
    Check(*Q == 20, "--Q steps back one");
    Q++;
    Check(*Q == 30, "Q++ steps forward one");
    Check(sizeof(int32) == 4 && sizeof(FVector) == 12 && sizeof(*Q) == 4, "sizeof is the game's layout");
    *P = P[1] + 1;
    Check(Items[0] == 21, "*P writes the first element");

    int32 &Third = P[2]; // keeps the address in an int64 local
    Third = Third + 5;
    Check(Items[2] == 35, "a reference local writes through its address");

    Bump(*P); // the memory itself binds the out-parm
    Check(Items[0] == 22, "*P binds a T& parameter");

    *Advance(P, 1) = 50;
    Check(Items[1] == 50, "a pointer return value and pointer arithmetic");
    Check(&Items[2] - P == 2, "a pointer difference counts elements");

    int32 Local = 7;
    int32 &Alias = Local; // another name for Local
    Alias = 8;
    Check(Local == 8, "a reference to a variable aliases it");

    void *Opaque = P;
    int64 Addr = __PtrCast__<int64>(Opaque);
    __PtrCast__<int32 &>(Addr) = 60;
    Check(Items[0] == 60, "__PtrCast__ to a reference writes at the address");
    Check(__PtrCast__<int64>(Third) == Addr + 8, "__PtrCast__ of a reference local is its address");

    TArray<FVector> Points;
    Points.Add(FVector(1.0f, 2.0f, 3.0f));
    FVector *V = &Points[0];
    V->Y = 5.0f;
    Check(Points[0].Y == 5.0f, "P->Member writes a struct member");

    TArray<FString> Names;
    Names.Add("a");
    FString *S = &Names[0];
    *S = "hello";
    Check(*S == "hello", "an FString reads and writes through a pointer");

    TArray<bool> Flags;
    Flags.Add(false);
    bool *B = &Flags[0];
    *B = true;
    Check(Flags[0], "a bool writes through a pointer");
    Check(*B, "a bool reads through a pointer");

    AActor *Self = this;
    int64 SelfAddr = __PtrCast__<int64>(Self);
    Check(__PtrCast__<AActor *>(SelfAddr) == Self, "an object round-trips through its address");
    Check(P != nullptr, "a non-null pointer tests true");
    Check(Self, "an object pointer tests true through IsValid");

    if (Failures == 0)
      UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage("PointerTest: every pointer check passed");
  }
};
