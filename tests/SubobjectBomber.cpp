#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SubobjectBomber");

/*
Members of ABomber's own that two of its default subobjects fit, with neither named for them: GooSoundComponent (an
AudioComponent, like WingSound) points at GooAudioComponent, and AcidEmitterLeft / Right (ParticleSystemComponents) at
GooEmitterLeft / Right. The object dump has no values, so the type alone cannot tell which; the game's own
ENE_Bomber_C, whose parent is ABomber, says it on its default object: each member's tag names the subobject.
*/
class SubobjectBomber : public ABomber {
public:
  UE_DEFAULTS {
    GooSoundComponent->VolumeMultiplier = 0.5f;
    AcidEmitterLeft->SecondsBeforeInactive = 2.0f;
  }
};
