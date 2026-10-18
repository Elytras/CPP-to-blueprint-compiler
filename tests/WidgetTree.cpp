#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/UMG.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/WidgetTree");

/*
S39: a widget class's designer tree from code. UWtFrame is a box with a named slot, Body, that whoever nests it fills.
UWtCard lays itself out in its constructor: a canvas root, a title, a button holding a label, and a frame whose Body holds
a note, each at the place and size it asks for. `pak mount` the mod, then `wrender /Game/_ElytrasMods/WidgetTree/UWtCard
desired` draws it, and the JSON's boxes read the numbers below: Title at 20,10 sized 200x40, Play at 20,60 sized 160x48
with Label centred in it, Frame at 20,120 with Note inside it. Title's text is bound to GetScoreText, so it reads
"0" where the layout said "Score", and Play's OnClicked is bound in the layout: each click counts. UWtCardKid declares no widgets and shows its
parent's tree, as a widget Blueprint below another does.
*/
class UWtFrame : public UUserWidget {
public:
  UE_WIDGET(UVerticalBox, Box);
  UE_WIDGET(UNamedSlot, Body);

  UWtFrame() { Box->AddChildToVerticalBox(Body); }
};

class UWtCard : public UUserWidget {
public:
  UE_WIDGET(UCanvasPanel, Root);
  UE_WIDGET(UTextBlock, Title);
  UE_WIDGET(UButton, Play);
  UE_WIDGET(UTextBlock, Label);
  UE_WIDGET(UWtFrame, Frame);
  UE_WIDGET(UTextBlock, Note);

  UWtCard() {
    UCanvasPanelSlot *At = Root->AddChildToCanvas(Title);
    At->SetPosition(FVector2D(20, 10));
    At->SetSize(FVector2D(200, 40));
    Root->AddChildToCanvas(Play)->SetOffsets(FMargin(20, 60, 160, 48));
    Cast<UButtonSlot>(Play->SetContent(Label))->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
    Root->AddChildToCanvas(Frame)->SetPosition(FVector2D(20, 120));
    UE_SLOT(Frame->Body, Note);
    Title->SetText(FText("Score"));
    Label->Text = FText("Play");
    Note->Text = FText("Inside");
    Play->OnClicked.Add(this, &UWtCard::OnPlay);
    UE_BIND(Title, Text, &UWtCard::GetScoreText);
  }

  int32 Clicks = 0;
  void OnPlay() { Clicks += 1; }
  FText GetScoreText() const { return UKismetTextLibrary::Conv_IntToText(Clicks, false, true, 1, 324); }
};

class UWtCardKid : public UWtCard {};
