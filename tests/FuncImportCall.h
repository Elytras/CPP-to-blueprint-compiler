/* FuncImportCall.h: classes FuncImportOwner.cpp cooks and FuncImportUser.cpp imports. FicKid overrides FicBase's Bump;
   Fixed is final, so no subclass has one. */
#pragma once
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

class FicBase : public AActor {
public:
  UE_CLASS("/Game/_ElytrasMods/FuncImportOwner/FicBase", "FicBase_C");
  virtual int32 Bump(int32 V);
  virtual int32 Fixed(int32 V) final;
  static int32 Twice(int32 V);
};

class FicKid : public FicBase {
public:
  UE_CLASS("/Game/_ElytrasMods/FuncImportOwner/FicKid", "FicKid_C");
  int32 Bump(int32 V) override;
};
