#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/Timelines");

/*
Timelines, UE_TIMELINE and its tracks (UeMeta.h). What the editor's Timeline node cooks: a TimelineTemplate in the
class's Timelines, a curve object per track, the timeline's and the tracks' variables and a direction variable, from which
UBlueprintGeneratedClass::CreateTimelineComponent makes the actor's component and binds the marked methods by name. Fade
has a track of each kind, a cubic, a constant and an unsorted key, and every pin; Pulse has every option and no method;
TimelinesChild overrides an update method and declares a timeline of its own; TimelinesPlain has none.
*/
class Timelines : public AActor {
public:
  UE_TIMELINE(Fade, 2.0f);
  UE_FLOAT_TRACK(Fade, Alpha, (0, 0), (1, 1, Cubic), (2, 4));
  UE_VECTOR_TRACK(Fade, Lift, (2, 0, 0, 100), (0, 1, 2, 3, Constant));
  UE_COLOR_TRACK(Fade, Tint, (0, 1, 1, 1, 1), (1.5f, 1, 0, 0, 0.5));
  UE_TIMELINE(Pulse, 5, AutoPlay, Loop, Replicated, IgnoreTimeDilation, LastKeyframe);
  UE_FLOAT_TRACK(Pulse, Beat, (0, 1, Cubic), (0.25, 0));

  int32 Updates = 0, Ends = 0, Blinks = 0;
  float Seen = 0.0f;

  UE_TIMELINE_UPDATE(Fade) void OnFade() {
    Updates += 1;
    Seen = Alpha;
  }
  UE_TIMELINE_FINISHED(Fade) void OnFaded() { Ends += 1; }
  UE_TIMELINE_EVENT(Fade, 1.5, 0.5) void Blink() { Blinks += 1; }
  void ReceiveBeginPlay() { Fade->PlayFromStart(); }
};

class TimelinesChild : public Timelines {
public:
  UE_TIMELINE(Spin, 1);
  UE_FLOAT_TRACK(Spin, Turn, (0, 0), (1, 360));
  void OnFade() { Updates += 100; }
  UE_TIMELINE_UPDATE(Spin) void OnSpin() { Seen = Turn; }
};

class TimelinesPlain : public Timelines {
public:
  void OnFaded() { Ends = -1; }
};
