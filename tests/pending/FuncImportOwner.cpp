#include "UeApi/Types.h"

#include "UeApi/Engine.h"

#include "FuncImportCall.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncImportOwner");

/* The owner of FuncImportCall.h's classes. */
int32 FicBase::Bump(int32 V) { return V + 1; }
int32 FicBase::Fixed(int32 V) { return V + 2; }
int32 FicBase::Twice(int32 V) { return V * 2; }
int32 FicKid::Bump(int32 V) { return V + 100; }
