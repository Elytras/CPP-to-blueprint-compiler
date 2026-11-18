#include "../include/Objects.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SpawnWith");

/* The one-call deferred forms (SpawnActorWith, AddComponentWith, CreateWidgetWith, NewObjectWith) beside the two-step
   form each is written out as: each pair of functions must compile to the same bytecode. */

/* A member pointer through an inline helper of the mod's own: `O->*F = V` is `O->Tag = V`. */
template <class C, class M> inline void Put(C *O, M C::*F, M V) { O->*F = V; }

class UWithCharges : public UActorComponent {
public:
  int32 Left = 3;
  float Rate;
};

class UWithProbe : public UObject {
public:
  int32 Seen;
  FString Label;
};

class UWithHud : public UUserWidget {
public:
  int32 Score;
};

class SpawnWithBase : public AActor {
public:
  int32 Team;
};

class SpawnWith : public SpawnWithBase {
public:
  int32 Tag;
  float Health;
  SpawnWith *Last;
  UWithCharges *Charges;
  UWithProbe *Probe;
  UWithHud *Hud;

  /* Fields set in the order written, a parent's (Team) among them; Last keeps the actor past the spawn. */
  void ActorWith(FTransform Where) {
    SpawnWith *Twin = SpawnActorWith<SpawnWith>(SpawnWith::StaticClass(), Where, Assign(&SpawnWith::Health, 100.f),
                                                Assign(&SpawnWith::Tag, 7), Assign(&SpawnWith::Team, 2));
    Last = Twin;
  }
  void ActorHand(FTransform Where) {
    SpawnWith *Twin = SpawnActorDeferred<SpawnWith>(SpawnWith::StaticClass(), Where);
    Twin->Health = 100.f;
    Twin->Tag = 7;
    Twin->Team = 2;
    FinishSpawning(Twin, Where);
    Last = Twin;
  }

  void ComponentWith() {
    UWithCharges *C = AddComponentWith<UWithCharges>(this, Assign(&UWithCharges::Left, 5), Assign(&UWithCharges::Rate, 0.5f));
    Charges = C;
  }
  void ComponentHand() {
    UWithCharges *C = AddComponentDeferred<UWithCharges>(this);
    C->Left = 5;
    C->Rate = 0.5f;
    FinishComponent(this, C);
    Charges = C;
  }

  void WidgetWith(APlayerController *Player) {
    UWithHud *W = CreateWidgetWith<UWithHud>(Player, Assign(&UWithHud::Score, 42));
    Hud = W;
  }
  void WidgetHand(APlayerController *Player) {
    UWithHud *W = CreateWidget<UWithHud>(Player);
    W->Score = 42;
    Hud = W;
  }

  void PutTag() { Put(this, &SpawnWith::Tag, 3); }

  void ObjectWith() {
    UWithProbe *O = NewObjectWith<UWithProbe>(this, Assign(&UWithProbe::Seen, 9), Assign(&UWithProbe::Label, FString("made")));
    Probe = O;
  }
  void ObjectHand() {
    UWithProbe *O = NewObject<UWithProbe>(this);
    O->Seen = 9;
    O->Label = FString("made");
    Probe = O;
  }
};
