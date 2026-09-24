#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/IfaceTest");

/*
An interface the mod declares, and a class implementing it. ITargetable cooks as its own asset - a
BlueprintGeneratedClass whose super is UInterface, one empty UFunction per declared method - and
Turret lists it in the class's Interfaces array. Describe leaves GetPriority out on purpose: an
interface function a class does not implement still gets an empty stub, the same as for the game's
own interfaces.
*/
class ITargetable {
public:
  UE_INTERFACE;
  void  OnTargeted(class AActor *By);
  int32 GetPriority();
};

class Turret : public AActor, public ITargetable {
  int32 Hits = 0;

public:
  void  OnTargeted(class AActor *By) { Hits = Hits + 1; }
  int32 GetPriority() { return 7; }
};

/*
Testing an interface value: `if (I)` is EX_PrimitiveCast CST_InterfaceToBool, true when an object is behind
it. An object that does not implement the interface casts to an empty one, so this is also the cast's
success test, as the editor's "Cast To Interface" node uses it.
*/
class Spotter : public AActor {
  TScriptInterface<ITargetable> Current;

public:
  int32 Rate(class AActor *Other) {
    TScriptInterface<ITargetable> Seen = Other;
    if (!Seen) return -1;
    if (Seen != nullptr && Current == nullptr) Current = Seen;
    if (Current) return Current->GetPriority();
    return 0;
  }
};

/*
An interface extending another, and variables on an interface. Neither is something the editor offers.
IMarkable's super is ITargetable_C rather than UInterface; Beacon lists IMarkable alone, which the engine
reads as implementing both (UClass::ImplementsInterface tests IsChildOf), and gets a stub for ITargetable's
GetPriority, which it leaves out. `Marks` and `MarkedBy` are declared on the interface but are properties of
Beacon, the class that implements it - an interface holds no state - and LoudBeacon below reaches them on
its parent. Beacon's UE_DEFAULTS gives its own default for one.
*/
class IMarkable : public ITargetable {
public:
  UE_INTERFACE;
  int32          Marks = 3;
  class AActor  *MarkedBy;
  void           Mark(int32 Count);
};

class Beacon : public AActor, public IMarkable {
  UE_DEFAULTS { Marks = 12; }

public:
  void Mark(int32 Count) { Marks = Marks + Count; }
  void OnTargeted(class AActor *By) { MarkedBy = By; }

  /* A cast to the parent interface succeeds on a class that lists only the child. */
  int32 PriorityOf(class AActor *Other) {
    TScriptInterface<ITargetable> Seen = Other;
    return Seen ? Seen->GetPriority() : -1;
  }
};

class LoudBeacon : public Beacon {
  UE_DEFAULTS { Marks = 40; } // an inherited property here: a tag on this CDO

public:
  int32 Total(class Beacon *Other) { return Marks + Other->Marks; }
};

class Describe : public AActor, public ITargetable {
public:
  void OnTargeted(class AActor *By) {}
};

/*
The other direction, and the older half of this: implementing one of the GAME's interfaces. The row
in Interfaces is the same shape, but conformance is checked against UeApi/Events.json instead - a
native interface function that is not a BlueprintNativeEvent or BlueprintImplementableEvent cannot
be implemented from a Blueprint at all. Hummer leaves two of the three to their empty stubs.
*/
class Singer : public AActor, public ICurveSourceInterface {
public:
  FName GetBindingName() const { return FName("Singer"); }
  float GetCurveValue(FName CurveName) const { return 0.5f; }
  void  GetCurves(TArray<FNamedCurveValue> &OutValues) const {}
};

class Hummer : public AActor, public ICurveSourceInterface {
public:
  FName GetBindingName() const { return FName("Hummer"); }
};
