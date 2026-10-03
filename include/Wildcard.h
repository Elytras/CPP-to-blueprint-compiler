#pragma once
/*
Wildcard.h - the engine's wildcard property setters, callable with an address.

Each is the engine's own function, redeclared with an int64 Value so a __RefAt__(Addr) (Intrin.h) can be passed:

    WildcardSystemSetters::SetStructurePropertyByName(To, ToField, __RefAt__(Addr));

Each is a CustomThunk that steps Value with a null result and keeps only MostRecentPropertyAddress - which
__RefAt__(Addr) leaves pointing at Addr - then copies from it with the DESTINATION property's type. The int64 is
never read as a value. Engine classes, imported and never cooked, so any number of mods may include this.
*/
#include "UeApi/Engine.h"
#include "Intrin.h"

class WildcardSystemSetters : public UBlueprintFunctionLibrary {
public:
  UE_CLASS("/Script/Engine", "KismetSystemLibrary");
  static void SetStructurePropertyByName(class UObject *Object, FName PropertyName, int64 Value);
};

class WildcardArraySetters : public UBlueprintFunctionLibrary {
public:
  UE_CLASS("/Script/Engine", "KismetArrayLibrary");
  static void SetArrayPropertyByName(class UObject *Object, FName PropertyName, int64 Value);
};

class WildcardMapSetters : public UBlueprintFunctionLibrary {
public:
  UE_CLASS("/Script/Engine", "BlueprintMapLibrary");
  static void SetMapPropertyByName(class UObject *Object, FName PropertyName, int64 Value);
};

class WildcardSetSetters : public UBlueprintFunctionLibrary {
public:
  UE_CLASS("/Script/Engine", "BlueprintSetLibrary");
  static void SetSetPropertyByName(class UObject *Object, FName PropertyName, int64 Value);
};
