#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/UMG.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/WidgetTree");

/*
S39: a widget class's designer tree from code. The constructor lays it out: a canvas root, a title, and a button holding
a label, each at the place and size it asks for. `pak mount` the mod, then `wrender /Game/_ElytrasMods/WidgetTree/UWtCard
desired` draws it, and the JSON's boxes read the numbers below: Title at 20,10 sized 200x40, Play at 20,60 sized 160x48
with Label centred in it. UWtCardKid declares no widgets and shows its parent's tree, as a widget Blueprint below another
does.
*/
class UWtCard : public UUserWidget {
public:
  UE_WIDGET(UCanvasPanel, Root);
  UE_WIDGET(UTextBlock, Title);
  UE_WIDGET(UButton, Play);
  UE_WIDGET(UTextBlock, Label);

  UWtCard() {
    UCanvasPanelSlot *At = Root->AddChildToCanvas(Title);
    At->SetPosition(FVector2D(20, 10));
    At->SetSize(FVector2D(200, 40));
    Root->AddChildToCanvas(Play)->SetOffsets(FMargin(20, 60, 160, 48));
    Cast<UButtonSlot>(Play->SetContent(Label))->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
    Title->SetText(FText("Score"));
    Label->Text = FText("Play");
  }

  // A button event the layout does not bind yet is bound at run time; Play holds this widget's own button by then.
  int32 Clicks = 0;
  void Construct() { Play->OnClicked.Add(this, &UWtCard::OnPlay); }
  void OnPlay() { Clicks += 1; }
};

class UWtCardKid : public UWtCard {};
