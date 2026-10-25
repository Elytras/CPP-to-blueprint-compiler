#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/GameBlueprintFlags");

/*
Another mod's Blueprint classes, as the header it shares declares them (UE_CLASS): no UeApi table has a row for them.
Every function of a Blueprint is a BlueprintEvent, so its interface can be implemented and its functions overridden,
and the header's marks give the flags an override copies, as genueapi writes them for the game's Blueprints.
*/
namespace Game { namespace _OtherMod {
class IOtherHub_C {
public:
  UE_CLASS("/Game/_OtherMod/IOtherHub", "IOtherHub_C");
  void Opened(int32 By);
  UE_PROTECTED void Closed(int32 Why);
  void GetName(FString &Name);
};

class OtherBase_C : public AActor {
public:
  UE_CLASS("/Game/_OtherMod/OtherBase", "OtherBase_C");
  UE_PRIVATE void Hidden();
  UE_BLUEPRINT_PURE void Peek(int32 &Out) const;
  UE_COSMETIC void Flash();
  UE_BLUEPRINT_CALLABLE void UserConstructionScript();
};
}}

class GameBlueprintFlags : public Game::_OtherMod::OtherBase_C, public Game::_OtherMod::IOtherHub_C {
public:
  int32 Count = 0;
  UE_DISPATCHER(OnPing);
  UE_DISPATCHER(OnUse, bool Using);
  UE_DISPATCHER(OnTick, float Delta);
  AActor *A = nullptr;
  AItem *I = nullptr;

  void Opened(int32 By) { Count += By; }
  void Closed(int32 Why) { Count -= Why; }
  void Hidden() { Count = 0; }
  void Peek(int32 &Out) const { Out = Count; }
  void Flash() {}
  void UserConstructionScript() {}

  /* Functions the engine does not mark BlueprintCallable, bound all the same: a delegate binds a UFunction by name. */
  void BindOnRep() { OnPing.Add(A, &AActor::OnRep_Instigator); }
  void BindRpc() { OnUse.Add(I, &AItem::Server_StartUsing); }
  void BindEvent() { OnTick.Add(A, &AActor::ReceiveTick); }
};
