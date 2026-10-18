#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/InputEvents");

/*
Key and input-action events, UE_INPUT_KEY / UE_INPUT_ACTION (UeMeta.h). What the editor's InputKey and InputAction nodes
cook: an entry of the class's InputKeyDelegateBinding or InputActionDelegateBinding, which the class lists in
DynamicBindingObjects, naming the function the input calls by name (UInputDelegateBinding::BindInputDelegates). InputEvents
marks a key, its release, a chord, both edges of one key on a method taking the key, and an action with every option;
InputEventsChild overrides a marked method and marks an action of its own; InputEventsPlain marks nothing and so lists no
binding object: the engine binds each class's own up the chain, the parent's included.
*/
class InputEvents : public AActor {
public:
  int32 Presses = 0;
  bool bOn = false;
  FName Last;

  UE_DEFAULTS { AutoReceiveInput = EAutoReceiveInput::Player0; }

  UE_INPUT_KEY("F5") void Toggle() {
    bOn = !bOn;
    Presses += 1;
  }
  UE_INPUT_KEY("F5", Released) void Stop() { bOn = false; }
  UE_INPUT_KEY("S", Ctrl, Shift) void Save() { Presses = 0; }
  UE_INPUT_KEY("G", Pressed, Released) void Hold(FKey Key) { Last = Key.KeyName; }
  UE_INPUT_ACTION("Use", NoConsume, WhenPaused, KeepParent) void Used(const FKey &Key) {
    Last = Key.KeyName;
    Presses += 10;
  }
};

class InputEventsChild : public InputEvents {
public:
  void Toggle() { Presses += 100; }
  UE_INPUT_ACTION("Fire") void Fired() { Presses += 1000; }
};

class InputEventsPlain : public InputEvents {
public:
  void Stop() { Presses = -1; }
};
