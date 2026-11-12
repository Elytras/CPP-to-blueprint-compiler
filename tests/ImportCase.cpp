#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ImportCase");

UE_ASSET_AT(UEnemyDescriptor, Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");
UE_ASSET_AT(UEnemyDescriptor, GruntLow, "/game/enemies/spider/grunt/ed_spider_grunt");

class ImportCase : public AActor {
public:
  UEnemyDescriptor *A = nullptr;
  UEnemyDescriptor *B = nullptr;
  void Keep() { A = &Grunt; B = &GruntLow; }
};
