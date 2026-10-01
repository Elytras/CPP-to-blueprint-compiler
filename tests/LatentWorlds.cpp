#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/LatentWorlds");

/* Which classes a wait warns in. Delay finds its world through its world context, self here (KismetSystemLibrary.cpp
   2166, UnrealEngine.cpp 11320-11346): an actor or a component has one of its own, a plain UObject only through its
   Outer (Obj.cpp 846), so the compiler warns for it alone. All three wait and resume the same way. */
class ULatentWorldPart : public UActorComponent {
  int32 Done;

public:
  void Run(float Seconds) {
    UKismetSystemLibrary::Delay(Seconds);
    Done = 1;
  }
};

class ULatentWorldJob : public UObject {
  int32 Done;

public:
  void Run(float Seconds) {
    UKismetSystemLibrary::Delay(Seconds);
    Done = 2;
  }
};

class LatentWorlds : public AActor {
  int32 Done;

public:
  void Run(float Seconds) {
    UKismetSystemLibrary::Delay(Seconds);
    Done = 3;
  }
};
