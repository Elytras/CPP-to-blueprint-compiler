#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/Game/UI_KPI_Tab_C.h"
#include "UeApi/UMG.h"

#include "../include/Objects.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/WidgetTest");

/*
A widget's Tick and OnPaint in game. The engine calls them only while the default object's bHasScriptImplementedTick and
bHasScriptImplementedPaint are set (UserWidget.cpp 1377, 1602), and AssetGen sets each where the class overrides the event
itself, as the editor's widget compiler does (test_bytecode.py widget_event_flags reads them offline). `pak spawn
/Game/_ElytrasMods/WidgetTest/WidgetTest` puts one widget of each class in the viewport, and each Tick or OnPaint that
runs counts: `prop obj <widget> get Ticks`, or Paints. UWgTick ticks; UWgBare has no Tick, and its flag is off; UWgKid
inherits UWgTick's Tick, which never runs for it (AssetGen warns); UWgTabTick ticks below UI_KPI_Tab_C, a game widget
Blueprint whose own default object holds the flag off; UWgPaint paints and UWgPaintKid, which inherits its OnPaint, does
not. Unmounting the pak destroys the actor, whose EndPlay takes the widgets off the screen.
*/
class UWgBare : public UUserWidget {};

class UWgTick : public UUserWidget {
public:
  int32 Ticks = 0;
  void Tick(FGeometry MyGeometry, float InDeltaTime) { Ticks += 1; }
};

class UWgKid : public UWgTick {};

class UWgTabTick : public UI_KPI_Tab_C {
public:
  int32 Ticks = 0;
  void Tick(FGeometry MyGeometry, float InDeltaTime) { Ticks += 1; }
};

// OnPaint is const, so it counts through Me, the widget itself, which Construct sets before the first paint.
class UWgPaint : public UUserWidget {
public:
  int32 Paints = 0;
  UWgPaint *Me = nullptr;
  void Construct() { Me = this; }
  void OnPaint(FPaintContext &Context) const { Me->Paints += 1; }
};

class UWgPaintKid : public UWgPaint {};

class WidgetTest : public AActor {
public:
  TArray<UUserWidget *> Shown;

  void Show(UUserWidget *Widget) {
    if (!Widget) return;
    Widget->AddToViewport(0);
    Shown.Add(Widget);
  }

  void ReceiveBeginPlay() {
    APlayerController *Player = UGameplayStatics::GetPlayerController(this, 0);
    Show(CreateWidget<UWgBare>(Player, UWgBare::StaticClass()));
    Show(CreateWidget<UWgTick>(Player, UWgTick::StaticClass()));
    Show(CreateWidget<UWgKid>(Player, UWgKid::StaticClass()));
    Show(CreateWidget<UWgTabTick>(Player, UWgTabTick::StaticClass()));
    Show(CreateWidget<UWgPaint>(Player, UWgPaint::StaticClass()));
    Show(CreateWidget<UWgPaintKid>(Player, UWgPaintKid::StaticClass()));
  }

  void ReceiveEndPlay(EEndPlayReason EndPlayReason) {
    for (UUserWidget *Widget : Shown) Widget->RemoveFromParent();
  }
};
