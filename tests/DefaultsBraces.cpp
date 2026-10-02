#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/DefaultsBraces");

/*
Designated braces of an engine struct whose header declares no constructor are the editor's Make Struct: a member they
leave out keeps what the engine's constructor sets (FHitResult's Time is 1, FHitResult::Init, EngineTypes.h), which no
header says. In a class's own default the fresh value holds it, so such a member is left untagged; in UE_DEFAULTS it is
left untagged where the parent's value is that fresh one (DbParent::Hit, and Held.Hit, a UE_STRUCT member with no
initializer), and refused over any other
(test_bytecode.py's defaults_braces). FTimerHandle's one member is Transient, which no default loads (Class.cpp 1452):
`T = FTimerHandle();` writes nothing, with a warning.
*/
struct FDbHeld {
  UE_STRUCT;
  FHitResult Hit;
  int32 N = 0;
};

class DbParent : public AActor {
public:
  FHitResult Hit;
  FDbHeld Held;
  FHitResult Hit2 = {.Time = 0.5f};
  FTimerHandle T;
};

class DefaultsBraces : public DbParent {
public:
  FHitResult Mine = {.Distance = 5.0f};
  FHitResult Mine2 = {0, 0.25f};

  UE_DEFAULTS {
    Hit = {.Distance = 6.0f};
    Held = {.Hit = {.Distance = 7.0f}};
    T = FTimerHandle();
  }
};
