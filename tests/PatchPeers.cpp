// The "game" of test_bytecode.py patch_import_kept: a Blueprint class holding a set of objects, and a child that
// inherits it. A patch of the child assigns Peers = {B}: A is listed removed, and B, the archetype's own, is written
// nowhere, so the child's package needs an import of A and none of B.
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PatchPeers");

UE_ASSET_AT(UEnemyDescriptor, PpA, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");
UE_ASSET_AT(UEnemyDescriptor, PpB, "/Game/Enemies/Spider/Exploder/ED_Spider_Exploder");

class PatchPeersBase : public AActor {
public:
  TSet<UObject *> Peers = {&PpA, &PpB};
};

class PatchPeersKid : public PatchPeersBase {
public:
  int32 Marker = 0;
};
