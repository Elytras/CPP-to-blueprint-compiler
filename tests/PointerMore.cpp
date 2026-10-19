#include "UeApi/Types.h"

#include "../include/Intrin.h"
#include "../include/Objects.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PointerMore");

/* Whole structs through a pointer, the address of a struct member, reference returns, and memory reads in a function
   that waits. The functions taking an address run offline over runscript's MEM; the Self* ones point into a TArray
   of their own, so they mean the same in the game (bpcheck). */

struct FPair {
  UE_STRUCT;
  int32 A;
  int64 B; // 8 bytes in: A is padded to B's alignment
};

struct FHolder {
  UE_STRUCT;
  int32 Tag;
  FVector V; // 4 bytes in
};

class PointerMore : public AActor {
public:
  int32 Seen = 0;
  int32 SeenLater = 0;
  TArray<FVector> Points;

  /* A whole struct, read and written. */
  FVector ReadVec(FVector *P) {
    FVector V = *P;
    return V;
  }
  void WriteVec(FVector *P, float X) { *P = FVector(X, X + 1.0f, X + 2.0f); }
  void CopyVec(FVector *To, FVector *From) { *To = *From; }
  float SumAt(FVector *P, int32 I) {
    FVector V = P[I];
    return V.X + V.Y + V.Z;
  }
  FPair ReadPair(FPair *P) { return *P; }
  void SwapPair(FPair *P) {
    FPair Old = *P;
    FPair New;
    New.A = (int32)Old.B;
    New.B = Old.A;
    *P = New;
  }

  /* 16-bit values and doubles, which Kismet has no type for: an int32, and the float nearest the double. */
  int32 ReadU16(uint16 *P) { return *P; }
  int32 ReadI16(int16 *P, int32 I) { return P[I]; }
  void WriteU16(uint16 *P, int32 V) { *P = (uint16)V; }
  void BumpI16(int16 *P) { *P += 1; }
  float ReadDouble(double *P) { return (float)*P; }
  void WriteDouble(double *P, float V) { *P = V; }

  /* The address of a struct member: its offset in the game's layout. */
  int64 AddrOfY(FVector *P) { return (int64)&P->Y; }
  int64 AddrOfB(FPair *P) { return (int64)&(*P).B; }
  int64 AddrOfNested(FHolder *P) { return (int64)&P->V.Z; }
  int64 AddrOfElementMember(FHolder *P, int32 I) { return (int64)&P[I].V.Y; }
  void SetZ(FVector *P, float Z) {
    float *Pz = &P->Z;
    *Pz = Z;
  }
  int64 AddrThroughRef(FVector *P) {
    FVector &R = *P;
    return (int64)&R.Y;
  }

  /* A reference return: the address of what the body returns, read or written where the call stands. */
  int32 &Slot(int32 *P, int32 I) { return P[I]; }
  int32 SlotUse(int32 *P) {
    Slot(P, 2) = 7;
    Slot(P, 0) += 5;
    int32 &R = Slot(P, 1);
    R = R * 2;
    return Slot(P, 2) + Slot(P, 1);
  }
  const int32 &FirstOf(int32 *P) { return P[0]; } // a const T& returns a copy
  int32 ConstRefUse(int32 *P) {
    const int32 &R = FirstOf(P);
    return R + FirstOf(P);
  }
  FVector &VecAt(FVector *P, int32 I) { return P[I]; }
  float VecAtUse(FVector *P) {
    FVector V = VecAt(P, 1);
    V.Y = 40.0f;
    VecAt(P, 0) = V;
    FVector Back = VecAt(P, 0);
    return Back.X + Back.Y;
  }

  /* Memory reads and writes in a function that waits: before and after the wait. */
  void ReadAcrossDelay(int32 *P) {
    Seen = *P;
    UKismetSystemLibrary::Delay(this, 0.5f);
    SeenLater = __Read32__((int64)P + 4);
    P[2] = SeenLater + 1; // and a write
    FVector *V = (FVector *)P;
    V[1] = FVector((float)Seen, 0.5f, 0.25f);
  }
  UObject *OuterSeen = nullptr;
  AActor *OwnerSeen = nullptr;
  void OuterAcrossDelay(UObject *O) {
    UKismetSystemLibrary::Delay(this, 0.5f);
    OuterSeen = O->GetOuter();
    OwnerSeen = GetTypedOuter<AActor>(O);
  }

  /* Self-contained: the memory is a TArray this object owns, so the game means them as C++ does (bpcheck cases written
     by hand in test_bytecode.py; runscript's MEM has no TArray in it). SelfLatent's members are read after its wait. */
  void SelfLatent() {
    Points.Clear();
    Points.Add(FVector(7.0f, 8.0f, 9.0f));
    FVector *P = &Points[0];
    FVector First = *P;
    Seen = (int32)First.X; // 7
    UKismetSystemLibrary::Delay(this, 0.5f);
    FVector Later = *P;
    SeenLater = (int32)(Later.Z * 10.0f) + __Read32__((int64)&P->Y) - 0x41000000; // 90 + (8.0f's bits - 0x41000000)
    OuterSeen = GetOuter();
  }
  float SelfWhole() {
    Points.Clear();
    Points.Add(FVector(1.0f, 2.0f, 3.0f));
    Points.Add(FVector(4.0f, 5.0f, 6.0f));
    FVector *P = &Points[0];
    FVector Second = P[1];
    *P = FVector(Second.Z, Second.Y, Second.X); // Points[0] = (6, 5, 4)
    P[1] = *P;                                  // Points[1] = (6, 5, 4)
    return Points[0].X * 100.0f + Points[1].Z;  // 604
  }
  int64 SelfMemberAddr() {
    Points.Clear();
    Points.Add(FVector(1.0f, 2.0f, 3.0f));
    Points.Add(FVector(4.0f, 5.0f, 6.0f));
    FVector *P = &Points[0];
    float *Z = &P[1].Z;
    *Z = 9.0f;
    return (int64)&Points[1].Y - (int64)P + (int64)Points[1].Z; // 16 + 9
  }
  float SelfRefReturn() {
    Points.Clear();
    Points.Add(FVector(1.0f, 2.0f, 3.0f));
    VecAt(&Points[0], 0).X = 10.0f;
    float &Y = VecAt(&Points[0], 0).Y;
    Y = Y + 20.0f;
    return Points[0].X + Points[0].Y; // 10 + 22
  }
};
