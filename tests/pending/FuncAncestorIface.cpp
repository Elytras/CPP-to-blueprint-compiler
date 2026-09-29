#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncAncestorIface");

/*
An override of a function a native ancestor gets from a native interface: AWoodLouse implements ITriggerAI in C++,
and the game's own ENE_Woodlouse overrides OnMessageAI with /Script/FSD.TriggerAI:OnMessageAI as its SuperStruct.
The Kismet compiler takes the super from ParentClass->FindFunctionByName, which looks through each class's
Interfaces (Class.cpp:5299; KismetCompiler.cpp:1774), and the override inherits its FUNC_FuncInherit flags
(KismetCompiler.cpp:1855). UeApi does not list a native class's interfaces, so the compiler must learn them elsewhere
(the game's Blueprints that override one show the pairs) to link the override and check its parameters.
*/
class FuncAncestorIface : public AWoodLouse {
  FName Heard;

public:
  void OnMessageAI(FName TriggerName) { Heard = TriggerName; }
};
