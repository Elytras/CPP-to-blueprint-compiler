#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeAssets/UCurveFloat.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/TimelineCurves");

/*
Timeline tracks over curve assets, the editor's External Curve, and curve assets of the mod's own. Ramp, Curves::Path and
Curves::Glow are cooked as CurveFloat, CurveVector and CurveLinearColor assets with their keys unsorted as written; Fade
plays them, a game curve (Hologram_FadeIn, BP_Refinery_LandingZone's own) and a track of its own keys beside them. Picked
points at Ramp from a default, as any asset can be.
*/
UE_FLOAT_CURVE(Ramp, (2, 8), (0, 0), (1, 2, Cubic));
namespace Curves {
UE_VECTOR_CURVE(Path, (0, 0, 0, 0), (2, 0, 0, 50));
UE_COLOR_CURVE(Glow, (0, 1, 1, 1, 1), (2, 1, 0, 0, 1));
} // namespace Curves

class TimelineCurves : public AActor {
public:
  UE_TIMELINE(Fade, 2.0f);
  UE_FLOAT_TRACK_CURVE(Fade, Alpha, Ramp);
  UE_VECTOR_TRACK_CURVE(Fade, Lift, Curves::Path);
  UE_COLOR_TRACK_CURVE(Fade, Tint, Curves::Glow);
  UE_FLOAT_TRACK_CURVE(Fade, Fog, UeAssets::UCurveFloat::Game::LevelElements::Blueprints::Hologram_FadeIn);
  UE_FLOAT_TRACK(Fade, Own, (0, 0), (2, 1));

  UCurveFloat *Picked = &Ramp;
  float Seen = 0.0f;

  UE_TIMELINE_UPDATE(Fade) void OnFade() { Seen = Alpha + Fog; }
  float RampAt(float Time) { return (&Ramp)->GetFloatValue(Time); }
};
