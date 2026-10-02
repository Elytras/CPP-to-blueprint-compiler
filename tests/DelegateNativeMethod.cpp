/* DelegateNativeMethod: a delegate bound to a native class's function on this object, `&AActor::SetActorTickInterval`.
   Under the Microsoft ABI a member pointer gives every declaration of AActor an implicit MSInheritanceAttr, the
   `class AActor;` FSD.h declares after Engine.h's definition too, which AssetGen read as the class, an empty one, until
   the record walk learnt to skip a declaration that holds nothing but attributes: the mod's parent was "BP AActor", a
   /Game class no package holds. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DelegateNativeMethod");

class DelegateNativeMethod : public AActor {
public:
  UE_DISPATCHER(OnTick, float Delta);
  void Hook() { OnTick.Add(this, &AActor::SetActorTickInterval); }
};
