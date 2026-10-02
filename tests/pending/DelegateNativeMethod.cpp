/* DelegateNativeMethod: a delegate bound to a native class's function on this object, `&AActor::SetActorTickInterval`.
   Under the Microsoft ABI a member pointer gives every declaration of AActor an implicit MSInheritanceAttr, the
   `class AActor;` FSD.h declares after Engine.h's definition too, and AssetGen read that declaration as the class, an
   empty one: the mod's parent became "BP AActor", a /Game class no package holds, so the game cannot load it. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateNativeMethod");

class DelegateNativeMethod : public AActor {
public:
  UE_DISPATCHER(OnTick, float Delta);
  void Hook() { OnTick.Add(this, &AActor::SetActorTickInterval); }
};
