#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OpndRefArgs");

/*
Operands the VM resolves against the object they run on. A script callee's reference parameter gets no copy: the VM
steps its argument with no result buffer and keeps only the address that argument leaves (ScriptCore.cpp
ProcessScriptFunction), so each form a C++ lvalue takes - a local, a member, a native struct's member, an array
element, another object's member - must reach the callee as that very variable, and an rvalue bound to a const
reference becomes a variable first. Under EX_Context only the one inner expression runs on the other object: a call's
arguments there still run on this one.
*/
class IOpndPing {
public:
  UE_INTERFACE;
  int32 Ping(int32 X);
};

class OpndPinger : public AActor, public IOpndPing {
public:
  int32 Ping(int32 X) { return X * 3; }
};

class OpndRefArgs : public AActor {
public:
  int32                       Member;
  FIntPoint                   Spot;
  TArray<int32>               Slots;
  OpndRefArgs                *Other;
  TScriptInterface<IOpndPing> Pal;

  [[gnu::noinline]] void  Fill(int32 &Out, int32 V) { Out = V; }
  [[gnu::noinline]] int32 Peek(const int32 &V) { return V + 1; }
  [[gnu::noinline]] void  Swap(int32 &A, int32 &B) {
    int32 T = A;
    A = B;
    B = T;
  }

  /* Every lvalue form on this object, and two rvalues for a const reference. */
  int32 Forms(int32 V) {
    int32 Local = 0;
    Fill(Local, V);
    Fill(Member, V + 1);
    Fill(Spot.Y, V + 2);
    Fill(Slots[1], V + 3);
    return Local + Peek(5) + Peek(V * 2);
  }

  /* A const reference ahead of an argument that writes its variable: the callee reads the variable when it runs, after
     every argument, so it sees the write (V + 5). */
  [[gnu::noinline]] int32 PeekAfter(const int32 &V, int32 X) { return V * 10 + X; }
  int32                   SetMember(int32 V) { Member = V; return 1; }
  int32                   ReadLate(int32 V) { Member = V; return PeekAfter(Member, SetMember(V + 5)); }

  /* Two reference parameters at once, parameters and members both. */
  int32 Swapped(int32 A, int32 B) {
    Swap(A, B);
    Swap(Spot.X, Slots[0]);
    return A * 1000 + B;
  }

  /* On another object: a call by name finds that object's function and a member read is that object's, while the
     call's argument - Member, unqualified - is still this object's. */
  virtual int32 Virt(int32 X) { return X + 100; }
  int32         OnOther(int32 X) { return Other ? Other->Virt(Member + X) * 1000 + Other->Member : -1; }

  /* Another object's members, and its struct's and array's, as reference arguments. */
  void FillOther(int32 V) {
    if (!Other) return;
    Fill(Other->Member, V);
    Fill(Other->Spot.X, V + 1);
    Fill(Other->Slots[0], V + 2);
  }

  /* An interface value a call returns, as the object of a call through the interface. */
  TScriptInterface<IOpndPing> GetPal() { return Pal; }
  int32                       PingPal(int32 X) { return GetPal() ? GetPal()->Ping(X) : -1; }
};
