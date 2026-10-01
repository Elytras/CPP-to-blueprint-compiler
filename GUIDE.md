# AssetGen guide

This guide shows how to write, build and run a Deep Rock Galactic mod with AssetGen. [REFERENCE.md](REFERENCE.md)
covers every construct the compiler accepts, warns about or refuses. [examples/](examples/) holds complete mods that
compile against the public SDK. The top comment of each says what it does in game. Only HelloWorld's shape has run in
DRG; what the others do there follows from the game API they call, not from a run. The [README](README.md) says how
to build the compiler.

## How AssetGen works

A mod is C++20 source written against the SDK headers, which declare the game's reflected classes, structs, enums and
functions. AssetGen runs clang on the source. clang checks it and hands AssetGen its syntax tree; nothing is compiled
to machine code. AssetGen lowers each class to a Blueprint class and each function body to Kismet bytecode, the
instructions Blueprint's virtual machine runs, and writes cooked packages (`.uasset` + `.uexp`), the form the shipped
game loads: one per class, struct, enum, interface and data asset, plus an `AssetRegistry.bin` that lists them.
`tools/bpbuild.py` runs those compiles for every mod in a `mods.yaml` and packs each mod into `<Mod>_P.pak` with
UnrealPak. The game mounts the pak and loads the classes like any other cooked Blueprint. There is no editor step, and
no DLL.

```
HelloWorld.cpp + UeApi/ ──assetgen compile──► HelloWorld.uasset/.uexp, ... + AssetRegistry.bin
mods.yaml ──tools/bpbuild.py + UnrealPak──► out/HelloWorld_P.pak ──► the game
```

Each piece of C++ becomes the Blueprint construct the editor would have made:

| In C++ you write | In Blueprint it is |
| --- | --- |
| `UE_MOD_PACKAGE("/Game/_MyMods/Hello");` | The content folder that the source's classes, structs, enums, interfaces and data assets are cooked into |
| `class Hello : public AActor { ... };` | A Blueprint class `Hello_C` whose parent is Actor, at `/Game/_MyMods/Hello/Hello` |
| `namespace Weapons { class Rifle : public AActor {}; }` | A subfolder: `Rifle` is cooked at `/Game/_MyMods/Hello/Weapons/Rifle`. A namespace that starts at `Game` is a `/Game` folder of its own |
| A member variable, `int32 Count = 0;` | A variable. The initializer is its default under Class Defaults |
| A method with a body, `int32 AddCharges(int32 By) { ... }` | A function (a function graph) that Blueprints can call and a subclass can override |
| A method named after a parent event, `void ReceiveBeginPlay() { ... }` | That event in the event graph, here Event BeginPlay |
| An `inline` method or function | No function at all: each call pastes the body in, like a macro |
| A local, `int32 Sum = 0;` | A local variable of the function |
| A `T&` parameter, `void Around(int32 A, int32 &Lo, int32 &Hi)` | A pass-by-reference pin: the function writes the caller's variable |
| `UE_PURE int32 Doubled() const { ... }` | A pure function, drawn without exec pins |
| Static methods of a class deriving `UBlueprintFunctionLibrary` | A Blueprint Function Library |
| A variable at namespace scope, `int32 Total = 0;` | No Blueprint has globals: a class that AssetGen generates holds it in its default object |
| `if`, `switch`, `for`, `while` | The Branch node, Switch on Int / Enum, the ForLoop and WhileLoop macros, compiled to jumps |
| `Cast<APawn>(Obj)` | The Cast To node |
| `UKismetSystemLibrary::Delay(2.0f);` | A latent node: the function becomes part of the class's event graph and resumes when the delay ends |

Components, class defaults, dispatchers, interfaces, structs, enums, data assets and replication map to their editor
equivalents the same way; [Writing mods](#writing-mods) takes them one at a time.

### What changes because it is Blueprint

The source is C++, but what runs is a Blueprint. These are the differences that catch a C++ programmer:

- **Your C++ never runs as C++.** A constructor is dropped without a message; member initializers and `UE_DEFAULTS`
  are read when the mod is built, and run-time setup goes in `ReceiveBeginPlay`. See
  [Classes and variables](#classes-and-variables).
- **A method is a Blueprint function unless it is `inline`**, and a free function must be `inline`. See
  [Functions and events](#functions-and-events).
- **Only Blueprint's types exist.** There is no `double`, and the only integer types are `uint8`, `int32` and
  `int64`: write `0.5f`, not `0.5`. See [Types](REFERENCE.md#types).
- **The engine's C++ is out of reach.** A mod calls the reflected functions in the SDK headers. There is no `FMath`
  and no standard library, and Print String shows nothing in the retail game. See
  [Talking to the game](#talking-to-the-game).
- **Locals have no address and no scope.** Two locals of one name and type in different blocks are one variable, which
  correct C++ cannot notice. Give locals of different types different names: `{ int32 X; } { FString X; }` makes two
  variables both named X. See [Locals](REFERENCE.md#locals).
- **Structs and containers are values, and `Map[Key]` is a copy.** Passing one by value copies it; a `T&` parameter
  writes back. See [Structs, enums and containers](#structs-enums-and-containers).
- **A function that waits runs in the event graph.** It returns to its caller at its first wait, resumes later and
  keeps one set of locals per object. See [Timers, waiting and input](#timers-waiting-and-input).
- **What is not exact warns; what cannot be done is refused.** See
  [Warnings, refusals and compiler bugs](#warnings-refusals-and-compiler-bugs). The reference marks every construct
  Yes, Warns, Refused or Not yet ([Reading this reference](REFERENCE.md#reading-this-reference)).

## Your first mod

This section builds [examples/HelloWorld.cpp](examples/HelloWorld.cpp), the smallest mod that runs on its own, and
packs it into a pak.

### Get the compiler and the SDK

Build AssetGen as the [README](README.md) describes. You get `x64\Release\assetgen.exe` on Windows or `build/assetgen`
on Linux. The compiler parses every mod with LLVM's `clang++`, so that has to be on `PATH`. The build driver,
`tools/bpbuild.py`, needs Python 3 with `pyyaml`, and UE 4.27's UnrealPak to make the pak.

Then clone the SDK beside the AssetGen folder:

```
git clone https://github.com/Elytras/DRG-Blueprint-Cpp-SDK ../DRG-Blueprint-Cpp-SDK
```

Its `UeApi/` folder is what a mod includes and what every compile takes as its include dir: one header per engine or
game module (`Engine.h`, `FSD.h`, ...), one per game Blueprint under `Game/`, and the JSON tables the compiler reads.
The SDK's README names the AssetGen commit that generated it. Build that commit of AssetGen or a later one, because the
JSON tables in `UeApi/` are part of the contract between the headers and the compiler. [The SDK](#the-sdk) says more.
The commands below run from the AssetGen folder.

### Write the mod

Make a folder for your mods, `mymods/` here, and copy [examples/HelloWorld.cpp](examples/HelloWorld.cpp) into it:

```cpp
/* HelloWorld: the smallest mod that runs on its own.

   One actor class greets you when the game spawns it, then counts to three, one step every two seconds. In game
   you see "Hello from AssetGen" posted as a game message, followed by "HelloWorld counted to 1", 2 and 3. The game
   spawns InitSpacerig in the Space Rig and InitCave in a mission; both are HelloWorld under another name. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_AssetGenExamples/HelloWorld");

/* The mod's actor, cooked as the Blueprint class HelloWorld_C whose parent is Actor. The game never spawns it
   under this name: it spawns the two empty subclasses at the bottom, which inherit all of it. */
class HelloWorld : public AActor {
  /* Blueprint variables: each initializer is the variable's default under Class Defaults. */
  int32 Count = 0;
  int32 Goal = 3;

public:
  /* The engine calls this when the actor starts play: it is Event BeginPlay in the class's event graph. */
  void ReceiveBeginPlay() {
    Say("Hello from AssetGen");

    /* Then we count. Delay is a latent call, like the Delay node in an event graph: ReceiveBeginPlay hands control
       back to the engine at the first Delay, and each round of the loop resumes here two seconds later. */
    while (Count < Goal) {
      UKismetSystemLibrary::Delay(2.0f);
      Count += 1;
      Say(FString("HelloWorld counted to ") + Count);
    }
  }

private:
  /* ReceiveBeginPlay's way to show text. Print String shows nothing in the retail game, so we post the text
     through the game state instead. Say is inline, so it is no Blueprint function: each call pastes this body in. */
  inline void Say(FString Msg) { UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg); }
};

/* The game spawns a mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both
   are empty, so whichever one the game spawns runs HelloWorld's ReceiveBeginPlay. */
class InitSpacerig : public HelloWorld {};
class InitCave : public HelloWorld {};
```

What each part does:

- A mod starts with `UeApi/Types.h`, then the headers of the modules it uses: `Engine.h` for the engine's classes,
  `FSD.h` for the game's.
- `UE_MOD_PACKAGE` names the `/Game` folder the source is cooked into. `HelloWorld` becomes the Blueprint class
  `HelloWorld_C` at `/Game/_AssetGenExamples/HelloWorld/HelloWorld`, with Actor as its parent. When you start a mod of
  your own, change this line to a folder of its own, such as `/Game/_MyMods/<Mod>`: two mods with one folder hold
  packages at the same paths, `InitSpacerig` included, and only one of them is read.
- `Count` and `Goal` are Blueprint variables, and their initializers are their defaults.
- `ReceiveBeginPlay` has the name of Actor's BeginPlay event, so it is this class's Event BeginPlay, and the engine
  calls it.
- `UKismetSystemLibrary::Delay(2.0f)` is the Delay node. You leave out the world context (the object that tells the
  engine which world the call is for) and the `FLatentActionInfo`, and the compiler fills them in. The loop waits two
  seconds before each round.
- `Say` is `inline`, so the class has no `Say` function: each call pastes its body in. It posts the text through the
  game state's `PostGameMessage`, because Print String shows nothing in the retail game.
- `FString("HelloWorld counted to ") + Count` turns `Count` into a string with Blueprint's ToString node and joins the
  two with the Append node.
- A pak only adds classes, so something has to spawn one before any code runs: the game spawns a mod's
  `InitSpacerig` in the Space Rig and its `InitCave` in a mission. Declared as empty subclasses of `HelloWorld`,
  with exactly those names, they run its `ReceiveBeginPlay` in both places.

### Compile it once

A pak does not need this step, but it shows what AssetGen writes:

```
assetgen compile mymods/HelloWorld.cpp ../DRG-Blueprint-Cpp-SDK/UeApi out/HelloWorld
```

`assetgen` stands for `x64\Release\assetgen.exe` on Windows and `build/assetgen` on Linux. The arguments are the
source, the SDK's `UeApi` folder and the out folder. AssetGen creates the out folder if it is missing, and prints one
line per package:

```
  HelloWorld     -> HelloWorld.uasset  (extends AActor)
  InitCave       -> InitCave.uasset  (extends BP HelloWorld)
  InitSpacerig   -> InitSpacerig.uasset  (extends BP HelloWorld)
  registry       -> ./AssetRegistry.bin  (3 assets)
```

`out/HelloWorld` then holds seven files:

| File | What it is |
| --- | --- |
| `HelloWorld.uasset`, `HelloWorld.uexp` | The class `HelloWorld_C`: its variables, its functions and their bytecode |
| `InitCave.uasset`, `InitCave.uexp` | `InitCave_C`, whose parent is `HelloWorld_C` (`extends BP` marks a Blueprint parent) |
| `InitSpacerig.uasset`, `InitSpacerig.uexp` | `InitSpacerig_C`, the same |
| `AssetRegistry.bin` | One row per class, which the game reads from the pak |

A compile that fails prints `  FAILED: <reason>` and exits with 1. clang's own errors come first and end with
`clang rejected <source> (diagnostics above)`.
[Warnings, refusals and compiler bugs](#warnings-refusals-and-compiler-bugs) explains the messages.

### Build the pak

`tools/bpbuild.py` compiles every mod that a `mods.yaml` lists and packs each one into a pak. Write
`mymods/mods.yaml`:

```yaml
mods:
  - name: HelloWorld           # -> mymods/out/HelloWorld_P.pak
    sources: [HelloWorld.cpp]
```

and run it:

```
python tools\bpbuild.py mymods ..\DRG-Blueprint-Cpp-SDK\UeApi x64\Release\assetgen.exe
```

or on Linux `python3 tools/bpbuild.py mymods ../DRG-Blueprint-Cpp-SDK/UeApi build/assetgen`. bpbuild reads the
package from the first source's `UE_MOD_PACKAGE`, compiles into
`mymods/build/HelloWorld/FSD/Content/_AssetGenExamples/HelloWorld/`, and packs the whole `FSD` folder. On Windows it
prints:

```
  HelloWorld     -> HelloWorld.uasset  (extends AActor)
  InitCave       -> InitCave.uasset  (extends BP HelloWorld)
  InitSpacerig   -> InitSpacerig.uasset  (extends BP HelloWorld)
  registry       -> .../mymods/build/HelloWorld/FSD/AssetRegistry.bin  (3 assets)
HelloWorld       compiled -> /Game/_AssetGenExamples/HelloWorld
HelloWorld       packed   -> ...\mymods\out\HelloWorld_P.pak
built 1, packed 1, up to date 0, failed 0
```

On Windows, bpbuild looks for UnrealPak at `C:\Program Files\Epic Games\UE_4.27\Engine\Binaries\Win64\UnrealPak.exe`,
and the `UNREALPAK` variable points it elsewhere. The README's *Packing on Linux* covers Linux. Without UnrealPak,
bpbuild prints `UnrealPak not found at <path> - skipping the pak.` and counts the mod as failed. A second run with
nothing changed prints `HelloWorld       up to date`. [Building mods](#building-mods) covers the rest of `mods.yaml`.

### What you should see

Install `HelloWorld_P.pak` the way you install any DRG mod.

Enter the Space Rig. The game spawns `InitSpacerig`, and its `ReceiveBeginPlay` posts `Hello from AssetGen`, then
`HelloWorld counted to 1`, `2` and `3`, two seconds apart. A mission spawns `InitCave`, which does the same. Look for
the messages in the chat feed, the most likely place; where the game shows posted messages has not been checked.

If nothing appears, [Running in the game](#running-in-the-game) says what to check. From here,
[Writing mods](#writing-mods) goes through what a mod can do, one topic at a time, and
[examples/WaitForPlayer.cpp](examples/WaitForPlayer.cpp) shows how a mod finds the player.

## Writing mods

A tour of what a mod can do, one topic at a time. Each topic ends with links to its full rules in REFERENCE.md and to
an example mod. Snippets leave out the `#include` lines and `UE_MOD_PACKAGE`, and a method shown on its own is a member
of a class that derives from `AActor`.

### Classes and variables

A class that derives from a UE class is cooked as a Blueprint class whose parent is that class. The parent can be an
engine or game class such as `AActor`, one of the game's Blueprints, or another class of your mod. The package is the
`UE_MOD_PACKAGE` path, then a folder for each namespace, then the class name, and the class inside it is `<Name>_C`. A
class with no base is plain C++ and is not cooked.

Each non-static data member is a Blueprint variable, and its initializer is its default under Class Defaults. A
variable has one of Blueprint's types: `bool`, `uint8`, `int32`, `int64`, `float`, `FName`, `FString`, `FText`, an
enum, an object, class, soft or interface reference, a struct, or a `TArray`, `TSet` or `TMap`. A default is read when
the mod is built, so it must be a literal, a constant expression, an enum constant, a braced struct or list, or
`&Asset`. Anything else is refused with `<Member>: a default is a value known when the mod is built - ...`; set such a
value in `ReceiveBeginPlay`. A class as the default of a `TSubclassOf` member is not built yet and is refused the same
way. A `const` member is read-only, and a `static constexpr` or `static inline const` member is a constant with no
variable behind it. To change the default of a property that a parent class declares, assign it in `UE_DEFAULTS`,
which is what editing Class Defaults does.

```cpp
class Turret : public AActor {
public:
  static constexpr int32 kMaxAmmo = 50;   // a constant: no variable is made for it
  int32 Ammo = kMaxAmmo;                  // a variable whose default is 50
  float Range = 1500.0f;
  const FName Team = "Dwarves";           // read-only: a Get node and no Set node
  FString Label;                          // no initializer: empty
  AActor *Target = nullptr;               // an object reference
  TArray<AActor *> Seen;
  TMap<FName, int32> HitsByName;
  TSubclassOf<AActor> Projectile;         // a class reference; it starts null

  UE_DEFAULTS {
    InitialLifeSpan = 120.0f;             // a property that AActor declares
  }
};

class HeavyTurret : public Turret {       // a Blueprint whose parent is Turret
  UE_DEFAULTS { Ammo = 200; }             // Ammo is Turret's: set its default here
};
```

With `UE_MOD_PACKAGE("/Game/_MyMods/Turrets")` this writes `/Game/_MyMods/Turrets/Turret` and
`/Game/_MyMods/Turrets/HeavyTurret`. A variable at namespace scope is shared by every class of the source
([Global variables](REFERENCE.md#global-variables)).

Watch for:

- A constructor is ignored, with no message. Set defaults with initializers and `UE_DEFAULTS`, and do run-time setup
  in `ReceiveBeginPlay`.
- Redeclaring an inherited property, such as `float InitialLifeSpan = 3.0f;` in a class derived from `AActor`,
  compiles with no warning. It adds a second variable that hides the parent's, and the engine never reads it. Use
  `UE_DEFAULTS`.
- UE 4.27 Blueprint has no `double`, `int8`, `int16`, `uint16`, `uint32` or `uint64`, and a variable of any of them is
  refused. `X * 0.5` with a `float X` is a `double` operation in C++, so it is refused too, with
  `no Kismet conversion from float to double`. Write `0.5f`.

Full rules: [Classes and variables](REFERENCE.md#classes-and-variables), [Types](REFERENCE.md#types),
[Constants](REFERENCE.md#constants), [Class defaults](REFERENCE.md#class-defaults). Example:
[examples/HelloWorld.cpp](examples/HelloWorld.cpp).

### Functions and events

A method with a body is a Blueprint function of the class, a function graph in editor terms. Other Blueprints can call
it, and a subclass can override it. Defining the body inside the class does not make it inline; only the `inline`
keyword does. An `inline` method is never a function: each call copies its body in, as a macro does. It costs no
call, but it cannot be bound to a dispatcher, called on another object or called from another Blueprint.

An event is a method named after an event the parent class exposes, such as `ReceiveBeginPlay`, `ReceiveTick` or
`ReceiveActorBeginOverlap`. The engine calls it as it calls the event node you add in the editor, and overriding
`ReceiveTick` also turns ticking on for the actor or component. Inside an override, `Base::Method()` runs the parent's
version, the editor's Add call to parent function; C++ has no `Super`, so name the class you derive from. In a class
that does not declare Method, `Base::Method()` copies Base's body in, or, where it cannot, runs Base's function through
an override of Method that AssetGen adds to your class, which only calls the parent's; a multicast goes by name with a
warning.
Every other call to a method goes by name, so the most derived override runs, also when the parent's own code makes
the call. A `final` class or method has no override, so its calls reach the one function, and on `this` its
body is copied in.

`UE_PURE` makes a pure function, drawn without exec pins. A `T&` parameter is an output, a pass-by-reference pin. A
`static` method runs on the class default object (the instance that holds Class Defaults), which has no world, so give
it a `UObject *WorldContextObject` parameter if it calls the engine. A method that waits follows extra rules, in
[Timers, waiting and input](#timers-waiting-and-input).

```cpp
class Pinger : public AActor {
public:
  int32 Pings = 0;
  float Elapsed = 0.0f;

  void ReceiveBeginPlay() { Ping(3); }        // an event: the name makes it the override
  void ReceiveTick(float DeltaSeconds) {      // overriding it also turns ticking on
    Elapsed += DeltaSeconds;
  }

  int32 Ping(int32 Times) {                   // a Blueprint function
    Pings += AtMost10(Times);
    return Pings;
  }
  UE_PURE bool IsBusy() const { return Pings > 10; }                     // a pure node
  void Around(int32 A, int32 &Lo, int32 &Hi) { Lo = A - 1; Hi = A + 1; }   // two outputs
  inline int32 AtMost10(int32 V) { return V > 10 ? 10 : V; }             // expanded, no function
};

class LoudPinger : public Pinger {
public:
  int32 Ping(int32 Times) { return Pinger::Ping(Times * 2); }   // an override calling the parent
};
```

Watch for:

- Do not write `override`: clang refuses it with `only virtual member functions can be marked 'override'`. The name
  alone makes the override, and AssetGen refuses a parameter list other than the event's:
  `void ReceiveTick(int32 X)` fails with "the AActor::ReceiveTick it replaces is void (float)". Copy the declaration
  from UeApi.
- A Blueprint class has one function per name. Two non-inline methods with the same name are refused ("a second
  function of that name"). Rename one, or make the extra overloads `inline`.
- A function outside a class must be `inline`. Without it the call is refused with
  `call to an unknown function: Helper`. Lambdas and function pointers are refused.

Full rules: [Functions](REFERENCE.md#functions), [Overrides and parent calls](REFERENCE.md#overrides-and-parent-calls),
[Inline functions and templates](REFERENCE.md#inline-functions-and-templates). Examples:
[examples/HelloWorld.cpp](examples/HelloWorld.cpp), [examples/GameBlueprintChild.cpp](examples/GameBlueprintChild.cpp).

### Talking to the game

Engine and game functions are called as in C++: statics through their class (`UGameplayStatics`,
`UKismetSystemLibrary`, DRG's `UGameFunctionLibrary`), members through a pointer. A parameter named `WorldContext...`
can be left out. The compiler then passes `this`, which is what the editor wires to the hidden pin; inside a static
function it passes the function's own `WorldContext...` parameter. UeApi declares no default arguments for engine
functions, so pass all the others, except a latent function's `FLatentActionInfo`.

DRG's getters are in `UGameFunctionLibrary`. `GetLocalPlayerCharacter()` returns the dwarf this machine controls,
already an `APlayerCharacter`, and `GetFSDGameState()` returns the game state, whose `PostGameMessage` posts text as a
game message. The dwarf is null until it exists; [Timers, waiting and input](#timers-waiting-and-input) shows how to
wait for it. The engine's getters, such as `UGameplayStatics::GetPlayerController(0)`, work too.
`UGameplayStatics::GetAllActorsOfClass` fills a `TArray<AActor *>` that you pass in, and `Cast<T>` is the Cast To
node: the object if it is a `T`, else null. Math is in `UKismetMathLibrary` (`Clamp`, `FMin`, `Sqrt`,
`RandomInteger`, ...); there is no `FMath` and no C++ standard library.

Reading another object's property is the Get node with a Target: `Me->HealthComponent->GetHealth()`. Unlike C++, a
read or a call through a null object does not crash: the game logs `Accessed None`, and a value reads as zero.
`if (Obj)` is the IsValid node, which is also false for an object that is being destroyed. A subsystem is its class's
static `Get()`, the editor's Get node for it: `UTracerManager::Get()`, or `GetSubsystem<UTracerManager>()`. Left out,
the world context is `this`. See [Working with other objects](REFERENCE.md#working-with-other-objects) for the kinds
and for spelling the getter by hand.

```cpp
void ReceiveBeginPlay() {
  AFSDGameState *GS = UGameFunctionLibrary::GetFSDGameState();      // world context left out: this
  APlayerCharacter *Me = UGameFunctionLibrary::GetLocalPlayerCharacter();
  if (!GS || !Me) return;                                           // the dwarf is null until it exists

  TArray<AActor *> Found;                                           // the call fills it
  UGameplayStatics::GetAllActorsOfClass(APlayerCharacter::StaticClass(), Found);
  int32 Running = 0;
  for (AActor *A : Found)
    if (APlayerCharacter *P = Cast<APlayerCharacter>(A))
      if (P->IsRunning) Running += 1;

  float Health = Me->HealthComponent->GetHealth();
  GS->PostGameMessage(FString("Dwarves: ") + Found.Num() + ", running: " + Running +
                      ", my health: " + Health);
}
```

Watch for:

- `UKismetSystemLibrary::PrintString` compiles but shows nothing. The retail game is a shipping build, and the
  function's body is compiled out of it. Use `PostGameMessage`.
- An engine or game static that UeApi marks `UE_AUTHORITY_ONLY` or `UE_COSMETIC`, such as
  `UGameplayStatics::ApplyDamage`, `UActorFunctionLibrary::GetPlayersInRange` or `UWidgetBlueprintLibrary::Create`
  (which `CreateWidget` calls), runs on every machine. The editor's node skips an authority-only call on a client and
  a cosmetic one on a dedicated server; AssetGen does not do this yet, and prints nothing. Guard such a call with
  `HasAuthority()` or `UKismetSystemLibrary::IsServer(this)` (see [Multiplayer](#multiplayer)).
- A pure function whose result you do not use is removed, and clang warns
  `ignoring return value of function declared with pure attribute`. An array the engine fills must have the
  parameter's exact type: `TArray<AActor *>`, not `TArray<APlayerCharacter *>`.

Full rules: [Calling engine and game functions](REFERENCE.md#calling-engine-and-game-functions),
[Working with other objects](REFERENCE.md#working-with-other-objects). Examples:
[examples/HelloWorld.cpp](examples/HelloWorld.cpp), [examples/WaitForPlayer.cpp](examples/WaitForPlayer.cpp).

### Components and spawning

`UE_COMPONENT(Type, Name)` adds a component, as Add Component in the Components panel does. `Name` is an object
variable, and each spawned actor gets its own instance, in place by the time its construction script and BeginPlay
run. The first scene component is the root, and every later scene component attaches to it. To place one elsewhere,
write `Tip->SetupAttachment(Glow);` in `UE_DEFAULTS`, as a C++ constructor does: under another of the class's
components, at a socket with `SetupAttachment(Glow, FName("Muzzle"))`, or under a component the class inherits, from
a mod or game Blueprint parent or a native one (`SetupAttachment(Mesh)` in an `ACharacter` or `APlayerCharacter`
child), or under the actor's root, whichever that is (`SetupAttachment(RootComponent)`). A component that is not a
scene component, such as a movement component, attaches to nothing. `SetupAttachment` in a function attaches at once,
keeping the relative transform, with a warning: the engine's own does nothing once the actor is constructed, so write
`AttachToComponent` there.

Set a component's defaults in `UE_DEFAULTS`, one `Comp->Field = value;` each, as you would in its Details panel.
Assign a struct whole (`FVector(...)`, `FColor(R, G, B)`), and an asset with `&Asset`
(see [Data assets and game assets](#data-assets-and-game-assets)). The class must be an engine or game component
class; add a component class of your own at run time with `AddComponentByType<T>(this)`.

The spawn and construct nodes are inline helpers in `Objects.h`, in AssetGen's `include` folder; include it by its
path or copy it beside your source. `SpawnActor<T>(Class, Transform, Owner)` is Spawn Actor from Class, and the new
actor's construction script and BeginPlay run inside the call. `SpawnActorDeferred` with `FinishSpawning` is the same
node with Expose on Spawn pins, `NewObject<T>(Outer)` is Construct Object from Class, and there are also
`AddComponentByType`, `AddComponentDeferred`, `AttachToComponent`, `AttachToActor` and `CreateWidget`. A helper costs
only the engine calls inside it. The wrong kind of class for a helper, such as `NewObject<AActor>`, fails in clang on
the calling line.

```cpp
#include "../include/Objects.h"   // AssetGen's include/ folder: SpawnActor, NewObject, AddComponentByType, ...

UE_ASSET_AT(UStaticMesh, SM_Crate_B, "/Game/Art/Environments/SpaceRig/SM_Crate_B");   // a game mesh

class Crate : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);        // the first scene component is the root
  UE_COMPONENT(UStaticMeshComponent, Mesh);   // attached to Root
  UE_COMPONENT(UPointLightComponent, Lamp);   // attached to Root

  UE_DEFAULTS {                               // what the Details panel would set
    Mesh->StaticMesh = &SM_Crate_B;
    Lamp->Intensity = 1500.0f;
    Lamp->LightColor = FColor(255, 128, 0);
    Lamp->RelativeLocation = FVector(0.0f, 0.0f, 80.0f);
  }
};

class CrateDropper : public AActor {
public:
  Crate *Last = nullptr;

  void ReceiveBeginPlay() {
    APlayerCharacter *Me = UGameFunctionLibrary::GetLocalPlayerCharacter();
    if (!Me) return;
    FTransform Where = Me->K2_GetActorLocation() + FVector(0.0f, 0.0f, 200.0f);
    Last = SpawnActor<Crate>(Crate::StaticClass(), Where, this);   // Spawn Actor from Class
    Last->Lamp->SetIntensity(3000.0f);                             // that crate's own light
  }
};
```

Watch for:

- The engine puts the root at the spawn transform and ignores the root's own location, rotation and scale. A plain
  `USceneComponent` root hands them on to the components attached to it. (A class whose parent already has a root,
  such as a Blueprint parent or `ACharacter`, adds no root: its first scene component keeps its transform.) A mesh or a light as the root keeps them,
  and AssetGen warns that they are not applied. Declare a `USceneComponent` first.
- `UE_DEFAULTS` is read when the mod is built and never runs. A call, an `if`, a `+=`, `nullptr`, or a member path
  such as `Lamp->RelativeLocation.Z = 50.0f;` is refused; the member path gets a misleading message about genueapi,
  the SDK generator ([The SDK](#the-sdk)). A variable the class declares itself takes an initializer instead.
- `Weapons::Turret::StaticClass()`, for a mod class in a namespace, names the engine class that `Turret` inherits
  `StaticClass` from, with no message: the qualified form is not built yet. Inside the namespace, write
  `Turret::StaticClass()`. Where the value goes straight into a `TSubclassOf<Weapons::Turret>`, such as the class
  argument of `SpawnActor<Weapons::Turret>`, it comes out right.

Full rules: [Components](REFERENCE.md#components), [Class defaults](REFERENCE.md#class-defaults),
[Creating objects](REFERENCE.md#creating-objects). Example: [examples/Beacon.cpp](examples/Beacon.cpp).

### Timers, waiting and input

`UKismetSystemLibrary::Delay(0.5f)` pauses the method it is in, as the Delay node does in an event graph. The method's
locals and parameters keep their values, and the code after the call runs when the delay ends; in a loop, each round
waits before the next one starts. An object in a local is kept only weakly across the wait, though: one the method
made or loaded can be garbage-collected meanwhile and read None after it. The compiler warns where that can happen;
keep such an object in a member. The caller gets control back at the method's first wait. Leave out the world
context and the `FLatentActionInfo`: the compiler supplies both, and `RetriggerableDelay`, `MoveComponentTo` and the
other latent functions are called the same way. Never pass the `FLatentActionInfo` yourself. With the world context
the call is refused, and without it (`Delay(1.0f, Info)`) it is not caught yet and compiles to a broken call.

A timer calls a method at a fixed rate. `K2_SetTimerDelegate({this, &Class::Method}, Time, bLooping, 0.0f, 0.0f)` is
Set Timer by Event; keep the `FTimerHandle` it returns in a member, and `K2_ClearAndInvalidateTimerHandle(Handle)`
stops the timer. The method takes no parameters and must not be `inline`. Setting a timer for the same method again
restarts it and returns a new handle. Use `ReceiveTick` for work every frame, a `Delay` loop to wait for something and
then carry on, and a timer for a fixed rate that you pause or stop.

Key events are not built yet: poll the player controller in `ReceiveTick` with `WasInputKeyJustPressed` or
`IsInputKeyDown`. Write the key with braces and the engine's key name, `FKey{"F5"}` (`"LeftShift"`, `"SpaceBar"`,
`"One"`); `FKey("F5")` is not built yet and is refused with `no Kismet conversion from FString to FKey`. To wait for an
event rather than a time, `UE_AWAIT(Obj->Dispatcher)` continues the method at the dispatcher's next broadcast and, for
a dispatcher with one parameter, gives its value: `Image = UE_AWAIT(Task->OnSuccess);`. It stays bound, so a later
broadcast runs the rest of the method again. `UKismetSystemLibrary::LoadAsset(Soft)` waits for an async load and
returns the object.

```cpp
class Blinker : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);
  FTimerHandle Blink;

  void ReceiveBeginPlay() {
    APlayerCharacter *Me = UGameFunctionLibrary::GetLocalPlayerCharacter();
    while (Me == nullptr) {                   // wait for the dwarf, half a second at a time
      UKismetSystemLibrary::Delay(0.5f);
      Me = UGameFunctionLibrary::GetLocalPlayerCharacter();
    }
    K2_AttachToActor(Me, FName(), EAttachmentRule::SnapToTarget, EAttachmentRule::SnapToTarget,
                     EAttachmentRule::SnapToTarget, false);
    Blink = UKismetSystemLibrary::K2_SetTimerDelegate({this, &Blinker::Toggle}, 0.5f, true, 0.0f, 0.0f);
  }

  void Toggle() { Lamp->ToggleVisibility(false); }   // every 0.5 s while the timer runs

  void ReceiveTick(float DeltaSeconds) {             // no key events: poll the key
    APlayerController *PC = UGameplayStatics::GetPlayerController(0);
    if (PC && PC->WasInputKeyJustPressed(FKey{"F5"}))
      UKismetSystemLibrary::K2_ClearAndInvalidateTimerHandle(Blink);
  }
};
```

Watch for:

- A method that waits returns nothing, takes no non-const reference parameters (a `const&` is copied when it is
  called) and is not static. Anything else is refused: a return value or a `T&` parameter with
  `a function that resumes later returns nothing and takes no non-const reference parameters`, a static method with
  `a static function has no object whose ubergraph frame could keep its locals`. Store a result in a member, or
  broadcast a dispatcher when the method is done.
- Each object keeps one copy of a waiting method's locals, not one per call. Calling the method again before it
  resumes starts it over with the new arguments, and its `Delay` is ignored while the first one is pending, so in
  `ReceiveTick` the code after a `Delay` runs at most once per delay. A local declared without an initializer keeps
  what the previous call left in it, so initialize it.
- A timer set by name, `K2_SetTimer(this, "Poll", ...)`, is not checked: an inline, misspelled or parameter-taking
  method compiles and sets no timer. The `{this, &Class::Method}` form is checked. A time of zero or less sets no
  timer.

Full rules: [Latent calls](REFERENCE.md#latent-calls), [Timers and input](REFERENCE.md#timers-and-input),
[Waiting on events](REFERENCE.md#waiting-on-events). Examples: [examples/WaitForPlayer.cpp](examples/WaitForPlayer.cpp),
[examples/Beacon.cpp](examples/Beacon.cpp), [examples/AwaitEvents.cpp](examples/AwaitEvents.cpp).

### Dispatchers and interfaces

`UE_DISPATCHER(OnScored, int32 Total)` declares an event dispatcher. `OnScored.Broadcast(Total)` is Call OnScored,
and every bound handler runs before `Broadcast` returns. `OnScored.Add(this, &Class::Method)` is Bind Event,
`Remove(this, &Class::Method)` is Unbind Event, and `Clear()` is Unbind all Events. A dispatcher has these four
methods and no others: `IsBound`, `AddUnique` and `Contains` are clang errors. Adding the same handler twice binds it
once.

`Add` and `Remove` also work on the game's dispatchers, on this actor, on another object or on a component:
`OnDestroyed`, `Me->OnFlareThrown`, `Box->OnComponentBeginOverlap`. The handler returns `void` and takes exactly the
parameters the header declares, spelled the same way, and clang checks this at the `Add`. A function that takes a
single delegate, such as a timer, takes `{this, &Class::Method}`.

A class with `UE_INTERFACE` is a Blueprint Interface, cooked as an asset of its own. A class implements it by listing
it after its parent and defining its functions; a function it leaves out gets an empty one that returns zero. The
game's interfaces are implemented the same way when their functions are Blueprint events; one with native-only
functions, such as `IHealth`, is refused. Hold an object through `TScriptInterface<IScorable>`: assigning an object is
Cast To IScorable and gives an empty value when the object does not implement it, `if (S)` tests it, and
`S->Points()` calls the object's function.

```cpp
class IScorable {                             // a Blueprint Interface
public:
  UE_INTERFACE;
  int32 Points();
};

class Nugget : public AActor, public IScorable {   // parent first, then interfaces
public:
  int32 Points() { return 5; }
};

class Scorer : public AActor {
public:
  UE_DISPATCHER(OnScored, int32 Total);       // an event dispatcher
  int32 Total = 0;

  void ReceiveBeginPlay() {
    OnScored.Add(this, &Scorer::Announce);                        // Bind Event to OnScored
    APlayerCharacter *Me = UGameFunctionLibrary::GetLocalPlayerCharacter();
    if (Me) Me->OnFlareThrown.Add(this, &Scorer::FlareThrown);    // one of the game's dispatchers
  }

  void FlareThrown() { AddPoints(1); }

  void Collect(AActor *Thing) {
    TScriptInterface<IScorable> S = Thing;                        // empty if Thing does not implement it
    if (S) AddPoints(S->Points());
  }

  void AddPoints(int32 N) {
    Total += N;
    OnScored.Broadcast(Total);                                    // Call OnScored
  }

  void Announce(int32 NewTotal) {
    FString T = NewTotal;
    UGameFunctionLibrary::GetFSDGameState()->PostGameMessage("Score: " + T);
  }
};
```

Watch for:

- A handler is a method of `this` that is not `inline`. An inline one is refused with
  ``a delegate cannot bind Handle: an inline function is expanded where it is called, no UFunction (drop `inline`)``.
  Binding a method of another object is not built yet and is refused with
  ``TODO: a delegate can only bind a function of `this` ``; bind a method of `this` that forwards the call.
- `Broadcast` works only on a `UE_DISPATCHER` of the class being compiled. Broadcasting one of the game's dispatchers,
  or one that a mod parent declares, is not built yet. For a parent's, give the parent a method that broadcasts, and
  call it. `Clear()` on a game dispatcher also removes the game's own bindings and those of other mods, so use
  `Remove` there.
- An interface function is matched by name only. An implementation marked `inline` compiles with no message and
  leaves the empty function in its place, and one with different parameters also compiles with no message, so copy
  the interface's declaration. Hold interfaces in `TScriptInterface`: `Cast<IScorable>` is refused for a mod
  interface. Do not reuse a game interface's name, such as `ITargetable`, which FSD.h already declares.

Full rules: [Event dispatchers](REFERENCE.md#event-dispatchers), [Interfaces](REFERENCE.md#interfaces). Examples:
[examples/Scoreboard.cpp](examples/Scoreboard.cpp), [examples/WaitForPlayer.cpp](examples/WaitForPlayer.cpp).

### Multiplayer

In a multiplayer game one machine is the server, the host, and the others are clients. A replicated variable is one the
server sends to every client. Declare it with a macro in place of the plain declaration:
`UE_REPLICATED(Type, Name)` is the editor's Replicated, `UE_REPLICATED_USING(Type, Name, OnRep)` is RepNotify, and
`UE_REPLICATED_IF(Type, Name, Condition)` adds a replication condition such as `SkipOwner`. An initializer after the
macro is the class default. A class that declares a replicated variable or an RPC of its own replicates
(`bReplicates`) without being asked. A class with neither sets it with `UE_DEFAULTS { bReplicates = true; }`.

An RPC is a method marked `UE_SERVER` (Run on Server), `UE_CLIENT` (Run on owning Client) or `UE_MULTICAST`, plus
`UE_RELIABLE` for the editor's Reliable checkbox. `UE_AUTHORITY_ONLY` skips a method on machines without authority, and
`UE_COSMETIC` skips it on a dedicated server. The SDK marks the game's own RPCs the same way, so you call them like any
other function and the engine routes the call. An RPC returns `void`, an OnRep takes no parameters, and a `TMap` or
`TSet` does not replicate; the compiler refuses each of these.

To branch at run time, `HasAuthority()` asks whether this machine has authority over an actor,
`UKismetSystemLibrary::IsServer(this)` asks whether it is the host (it is also true in a solo game), and
`IsLocallyControlled()` asks whether a pawn is this machine's player. Authority is not the same as being the host: an
actor that each machine spawns for itself has authority on every machine.

```cpp
class LightSwitch : public AActor {
  UE_REPLICATED_USING(bool, bOn, OnRep_On);
  int32 Flips = 0;

public:
  void OnRep_On() { Flips += 1; }   // on clients when bOn arrives, and after every write here

  UE_SERVER UE_RELIABLE void ServerSetOn(bool bValue) { bOn = bValue; }

  UE_MULTICAST void MultiSpark() { Flips += 100; }

  void Flip() {
    if (HasAuthority()) {
      bOn = !bOn;                   // FlushNetDormancy, the write, then OnRep_On
      MultiSpark();                 // runs on the server and on every client
    } else {
      ServerSetOn(!bOn);            // runs on the server, if this client owns the actor
    }
  }
};
```

Watch for:

- Assigning a RepNotify variable calls its OnRep right after the write, on the machine that writes it, the server
  included. This is the editor's Set w/ Notify, and it differs from native UE C++. Only an assignment to the whole
  variable does this. `Slots.Add(3)` on a replicated array and `Target.Z = 1.0f` on a replicated struct get no flush and
  no OnRep. Assign the whole value, or call the OnRep yourself.
- The engine drops a Server RPC unless the client that calls it owns the actor.
- Keep `inline` off every method that carries a marker, and off every OnRep. An inline method is no UFunction, so
  both are refused.

Full rules: [Replication](REFERENCE.md#replication), [RPCs](REFERENCE.md#rpcs). Example:
[examples/NetworkedSwitch.cpp](examples/NetworkedSwitch.cpp).

### Structs, enums and containers

The game's structs are ordinary types. `FVector(1, 2, 3)` makes one value, a literal when its members are constants or
variables, and the editor's Make Struct when one computes something (`FVector2D(X, M ? 1.0f : 2.0f)`), each member then
its own statement, left to right. Braces, positional or with designated members, are the editor's Make Struct:
`FHitResult Hit = { .Time = 0.5f };`. Members the braces leave out keep their defaults; a member given `{}` is a fresh
value of its type, as in C++: zero, empty, None. Member reads and writes, nested to any depth, act on the struct in
place. A struct of your own carries `UE_STRUCT;` in its body. It is cooked as a Structure asset in the mod package, and
its members' initializers are its default values.

An enum of your own is an `enum class` with a `uint8`, `int32` or `int64` underlying type, followed by `UE_ENUM(Name);`.
It is cooked as an Enumeration asset and then works like any Blueprint enum: variables, parameters, constants and
`switch`. Without `UE_ENUM` its constants still fold to numbers, but a variable of it is refused. `UE_ENUM_MAP(E)` as
the default of a `TMap<E, FName>` member fills in a name table at build time. For enum to name, declare the variable
as `TEnum<E>`: it is still an E, and `.Name()` / `.String()` ask the engine. The game's enum fields and return values
already come as `TEnum<E>` in UeApi.

`TArray`, `TSet` and `TMap` are the Blueprint Array, Set and Map. Their methods are the editor's container nodes, not
UE's C++ API: `Add`, `Contains`, `Find(Key, Out)`, `Keys(OutArray)`, `Num()` and so on, with results coming back
through reference parameters. `Map[Key]` reads with the Find node: you get a copy of the value, or the value type's
default for a missing key, and nothing is added. `Map[Key] = V`, `+=` and `++` store with the Add node. A range-for
with `auto& [Key, Value]` walks the map where it lives, so a write to `Value` changes the map. A container inside a
container works through a generated wrapper struct ([Containers](REFERENCE.md#containers) says where it is cooked).

```cpp
enum class ERank : uint8 { Rookie, Veteran, Legend };
UE_ENUM(ERank);

struct FEntry {
  UE_STRUCT;
  FName Player;
  int32 Kills = 0;
  ERank Rank = ERank::Rookie;
};

class Tally : public AActor {
  TMap<FName, int32> Kills;
  TArray<FEntry> Board;

public:
  void AddKill(FName Player) { Kills[Player] += 1; }   // a missing key reads as 0

  void Rebuild() {
    Board.Clear();
    for (const auto &[Player, Count] : Kills) {
      FEntry E = {.Player = Player, .Kills = Count};
      switch (Count / 10) {
      case 0: break;
      case 1: E.Rank = ERank::Veteran; break;
      default: E.Rank = ERank::Legend; break;
      }
      Board.Add(E);
    }
  }
};
```

Watch for:

- `Map[Key]` is a copy, because Blueprint has no reference to a map element. `Lists[K].Add(X)` still works: the element
  is copied, changed and stored back. A `T&` parameter bound to `Map[K]` gets the same copy, stored back after the
  call, and the compiler warns, since code that reads the map during the call sees the old value.
- `TArray::Remove(I)` is the Remove Index node: it removes the element at index `I`. To remove by value, call
  `RemoveItem(V)`. On a `TArray<int32>` both compile, so C++ code ported with `Remove(Value)` removes the wrong element.
- A mod struct has no methods and no operators. A non-inline method on a `UE_STRUCT` compiles without a word into a
  call that cannot work, so write a free `inline` function that takes the struct instead. `==` on a mod struct or on
  two containers is a clang error: compare the members, or use `A.Identical(B)` for arrays.
- A container that a non-inline function returns has to be stored in a local before you call a method on it, index it
  or loop over it. `Append` and `Union` take one directly, and an inline function's result already counts as a
  variable.

Full rules: [Structs](REFERENCE.md#structs), [Enums](REFERENCE.md#enums), [Containers](REFERENCE.md#containers),
[Loops](REFERENCE.md#loops), [Functions](REFERENCE.md#functions). Example:
[examples/Scoreboard.cpp](examples/Scoreboard.cpp).

### Data assets and game assets

A data asset class is a mod class that derives from `UPrimaryDataAsset` or `UDataAsset`. Keep its members public, so
that C++ accepts braces for it. A namespace-scope variable of that class with a braced initializer is cooked as an
asset at `<mod package>/<Name>`, as creating a Data Asset in the Content Browser and filling in its details would. Only
the members the braces name are written; the rest keep the class defaults. The class can also be a game class, such as
`UEnemyDescriptor`. `&Name` points at the asset: in a variable's default, in a container default, or in a function
body.

A game asset is named with `UE_ASSET_AT(Class, Name, "/Game/Path/Package")`, after which `&Name` points at it as well.
The UeAssets headers (see [The SDK](#the-sdk)) declare the game's assets this way, in namespaces that follow their
folders: `&UeAssets::UEnemyDescriptor::Game::Enemies::Spider::Grunt::ED_Spider_Grunt`. A game asset's values are read at
run time, through the pointer.

A `TSoftObjectPtr<T>` or `TSoftClassPtr<T>` holds a path and loads nothing with the class. Its default is the path as a
string. `(T *)Soft` gives the object only if it is already loaded, and null otherwise. `UKismetSystemLibrary::LoadAsset`
and `LoadAssetClass` are the editor's Async Load nodes: the function waits at the call, and the call's value is the
loaded object or class. `LoadAsset_Blocking` is an ordinary call that loads at once.

```cpp
class UCaveTuning : public UPrimaryDataAsset {
public:
  float HealthScale = 1.0f;
  int32 Waves = 3;
  TSoftObjectPtr<UTexture2D> Icon;
};

UCaveTuning CT_Easy = {.HealthScale = 0.5f};   // cooked as <mod package>/CT_Easy
UCaveTuning CT_Hard = {.HealthScale = 2.0f, .Waves = 6,
                       .Icon = "/Game/LevelElements/RoomObjects/Hazards/StickySpiderWeb/T_StickySpiderWeb_Corner"};

UE_ASSET_AT(UEnemyDescriptor, ED_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");

class CaveRules : public AActor {
  UCaveTuning *Tuning = &CT_Hard;
  TArray<UCaveTuning *> Levels = {&CT_Easy, &CT_Hard};
  float Spread = 0;
  UObject *IconObj = nullptr;

public:
  void ReceiveBeginPlay() {
    Spread = (&ED_Grunt)->SpawnSpread;                        // a game asset's value, read at run time
    IconObj = UKismetSystemLibrary::LoadAsset(Tuning->Icon);   // waits here until the texture loads
  }
};
```

A data asset class declared in a header that several mods include needs an owner; see
[Several mods and shared code](#several-mods-and-shared-code).

Watch for:

- An asset is an object, not a C++ value. `CT_Hard.Waves` is refused ("CT_Hard is an asset, not a value: point at it
  with &CT_Hard"). Write `(&CT_Hard)->Waves`, or keep a pointer. The same holds for game assets, whose values are not
  available when the mod is built.
- Paths are not checked at build time, neither in `UE_ASSET_AT` nor in a soft default. A soft class of a Blueprint takes
  the class's full path, ending in `.BP_Name_C`. The short form `"/Game/Dir/Pkg"` means the object `Pkg.Pkg`, which for
  a Blueprint is the asset and not its class.
- Not yet: cooking a changed copy of a game data asset. Write the loaded asset's members through its pointer at run
  time, or extend a game Blueprint and set its defaults.

Full rules: [Data assets](REFERENCE.md#data-assets), [Game assets](REFERENCE.md#game-assets),
[Types](REFERENCE.md#types) (soft references), [Latent calls](REFERENCE.md#latent-calls) (the loads). Example:
[examples/EnemyInfo.cpp](examples/EnemyInfo.cpp).

### Extending a game Blueprint

The SDK's `UeApi/Game/` headers declare the game's Blueprint classes, each in the namespace of its folder. Include the
header and derive from the class. In the same namespace, your class is cooked beside the one it extends, here at
`/Game/GameElements/GameEvents/ExplosiveBarrelsEvent/BP_ExplosiveBarrel_Big`. Outside it, your class stays in your
mod's folder. A package outside your mod's folder is written under the `Content` folder that the out dir is in, which
bpbuild sets up for you.

To override an event or a function, declare a method with its name and the SDK's parameter list, copied from the
header and without `override`. `Parent::Method()` runs the parent's own implementation, as the editor's Add call to
parent function does. `UE_DEFAULTS` sets the defaults of inherited variables, as the editor's Class Defaults do, and of
inherited components, including the ones the game Blueprint adds in its Components panel.

```cpp
#include "UeApi/Game/BP_ExplosiveBarrel_C.h"

namespace Game::GameElements::GameEvents::ExplosiveBarrelsEvent {
class BP_ExplosiveBarrel_Big : public BP_ExplosiveBarrel_C {
  UE_DEFAULTS {
    SpeedThreshold = 2000.0f;                                  // a variable the game Blueprint declares
    StaticMesh->RelativeScale3D = FVector(2.0f, 2.0f, 2.0f);   // a component it adds
  }

public:
  int32 Starts = 0;

  void ReceiveBeginPlay() {                     // an event the game Blueprint implements
    BP_ExplosiveBarrel_C::ReceiveBeginPlay();   // run the game's own graph first
    Starts += 1;
  }

  void Throw(FVector force) {                   // a function it declares
    BP_ExplosiveBarrel_C::Throw(force * 2.0f);
  }
};
}
```

Watch for:

- Change an inherited variable's default in `UE_DEFAULTS`. Redeclaring it (`float SpeedThreshold = 2000.0f;`) compiles
  without a warning into a second variable that hides the parent's, and the game never reads it.
- A component the game Blueprint adds is matched by its construction-script node, which the SDK records beside the
  member as `<Comp>__UeScsNode`. Only an SDK generated from a [Dumper-7 fork](https://github.com/Elytras/Dumper-7) dump
  has these markers. Without one, the override is refused: "... its header does not say which SCS node it is -
  re-dump the game with the Dumper-7 fork (ScsNode=) and regenerate UeApi".
- Your class is a new class. The game keeps spawning its own, so spawn yours yourself (see
  [Components and spawning](#components-and-spawning)). Not yet: changing the game Blueprint's own defaults in the pak.

Full rules: [Overrides and parent calls](REFERENCE.md#overrides-and-parent-calls),
[Class defaults](REFERENCE.md#class-defaults), [Mod sources and packages](REFERENCE.md#mod-sources-and-packages).
Example: [examples/GameBlueprintChild.cpp](examples/GameBlueprintChild.cpp).

### Several mods and shared code

Every mod that includes a header compiles what is in it. Without an owner, a class, struct or enum declared in a shared
header is cooked by each of those mods, as separate copies that do not match. Name the one mod that cooks it:
`UE_CLASS("<package>", "<Name>_C")` inside a class, `UE_STRUCT_IN("<owner's UE_MOD_PACKAGE>")` inside a struct, and
`UE_ENUM_IN(Enum, "<owner's UE_MOD_PACKAGE>")` after an enum. Every other mod imports it.

A source cooks a `UE_CLASS` class only when the package is exactly the path that source gives the class: its
`UE_MOD_PACKAGE`, then any namespace folders, then the class name. For `UE_STRUCT_IN` and `UE_ENUM_IN`, the path is the
owner's `UE_MOD_PACKAGE` itself. Static functions of a `UBlueprintFunctionLibrary` are then called like an engine
library's, and a world context you leave out is the caller's self. A shared actor class works the same way: the header
declares its methods and dispatchers, the owner's source defines the methods, and another mod finds an instance, for
example with `GetAllActorsOfClass(Turret::StaticClass(), Found)` and `Cast<Turret>`, then calls `T->Fire(2)` or binds
`T->OnFired`. See [Sharing a class between mods](REFERENCE.md#sharing-a-class-between-mods-ue_class). The owner's
assets must be in the game as well. Namespace-scope variables are not shared this way: each one lives under its own
source's `UE_MOD_PACKAGE`, so mods with different packages never share one, even with the same name.

```cpp
// Shared.h, included by both mods
#pragma once
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

enum class ETeam : uint8 { Red, Blue };
UE_ENUM_IN(ETeam, "/Game/_MyMods/Core");

struct FTeamScore {
  UE_STRUCT_IN("/Game/_MyMods/Core");
  ETeam Team;
  int32 Score = 0;
};

class CoreLib : public UBlueprintFunctionLibrary {
public:
  UE_CLASS("/Game/_MyMods/Core/CoreLib", "CoreLib_C");
  static int32 Bonus(FTeamScore S, class UObject *WorldContextObject = nullptr);
};

// Core.cpp: cooks ETeam, FTeamScore and CoreLib
#include "Shared.h"
UE_MOD_PACKAGE("/Game/_MyMods/Core");

int32 CoreLib::Bonus(FTeamScore S, class UObject *WorldContextObject) { return S.Score * 2; }

// Arena.cpp: imports all three
#include "Shared.h"
UE_MOD_PACKAGE("/Game/_MyMods/Arena");

class Arena : public AActor {
  FTeamScore Best = {.Team = ETeam::Blue, .Score = 7};

public:
  int32 Total() { return CoreLib::Bonus(Best); }
};
```

In `mods.yaml`, `needs` builds the owner first, and `embed` packs the owner's assets and registry rows into the user's
pak, so that one pak works alone. Without `embed`, ship both paks. List the header in `sources`, so that an edit to it
rebuilds every mod that uses it.

```yaml
mods:
  - name: Core
    sources: [Core.cpp, Shared.h]    # the first source holds the UE_MOD_PACKAGE line
  - name: Arena
    sources: [Arena.cpp, Shared.h]
    needs: [Core]                    # build Core first
    embed: true                      # and pack Core's assets into Arena_P.pak too
```

Watch for:

- A path that does not match cooks nothing, and a lone `assetgen compile` does not notice. A `UE_CLASS` path without
  the class's namespace folder leaves the class imported and produced by no mod. bpbuild fails the build with
  "`<asset>` imports `<package>`, which no mod in this build produces"; in game the import is null. A mismatched
  `UE_STRUCT_IN` or `UE_ENUM_IN` path cooks nothing, and the compiler says nothing. A `UE_CLASS` package that does not
  end in the class name, or a cooked class not named `<Name>_C`, is refused, and the message gives the expected value.
- Pointing at another mod's asset whose class a shared header declares without an owner is refused, because the
  reference would load as null. The message gives the `UE_CLASS` line to add.
- A component of a parent class that another mod owns cannot be set in `UE_DEFAULTS`. It is refused with the
  construction-script message meant for game Blueprints, whose advice to re-dump the game does not apply. Set the
  component in `ReceiveBeginPlay` instead.

Full rules: [Mod sources and packages](REFERENCE.md#mod-sources-and-packages), [Structs](REFERENCE.md#structs),
[Enums](REFERENCE.md#enums), [Global variables](REFERENCE.md#global-variables), and [Building mods](#building-mods) for
`mods.yaml`. Examples: [examples/MathLib.h](examples/MathLib.h), [examples/MathLib.cpp](examples/MathLib.cpp),
[examples/LibraryUser.cpp](examples/LibraryUser.cpp).

### Reading memory

A pointer to anything that is not a UObject (`int32*`, `FVector*`, `void*`, even `UObject**`) is an `int64` address in
the Blueprint. `*P`, `P[I]`, `P->Member`, `P + N` and `T& R = *P` read and write the memory at that address, as in C++,
and need no include. A pointer to a UObject class stays an object reference. `(int64)Obj` and `(uint8 *)Obj` give an
object's address, and `(AActor *)Addr` reads an address back as an object. `sizeof` and `alignof` give the game's
layout, so pointer arithmetic steps by the game's sizes. `&Items[I]` is the address of a `TArray` element. For explicit
reads and casts, AssetGen's `include/Intrin.h` adds `__PtrCast__`, `__AddrOf__` and the `__Read64__` family.

A read takes the pointee's size. `int32`, `int64`, `float`, `uint8`, `bool`, enums, `FName`, `FString`, `FText` and
object or class pointers can be read. A whole struct can be neither read nor written; reach its members with `P->X`,
which touches only that member. A `uint32` read is refused, since Blueprint has no unsigned 32-bit integer. A Blueprint
variable has no address, so `&Local` is refused. Every access goes through a small scratch struct, `FDeref`, which the
compiler cooks beside your class the first time you use a pointer; you do not declare it.

```cpp
TArray<FVector> Points = {{1, 2, 3}, {4, 5, 6}};

float SecondY() {
  FVector *P = &Points[0];                         // the array's storage, as an int64 address
  return (P + 1)->Y;                               // steps sizeof(FVector) = 12 bytes, reads 4
}

bool ClassMatches(UObject *Obj) {
  if (!Obj) return false;
  UClass *C = *(UClass **)((uint8 *)Obj + 0x10);   // UObject::ClassPrivate, per CoreUObject.h
  return C == Obj->GetClass();
}
```

Watch for:

- Nothing checks an address. A wrong address or offset reads garbage or crashes the game, with no soft failure. Take
  offsets from the game you target, and prefer a reflected property or function whenever one exists.
- A pointer read in a function that makes a latent call (`Delay`, `LoadAsset`, `UE_AWAIT`) is refused. Move the read
  into a function that does not wait, and call it.
- `&Obj->Member`, `FText` through a pointer and `__ClassOf__` call helper code that AssetGen does not ship.
  [Pointers and memory](REFERENCE.md#pointers-and-memory) says what to supply. Without it, only `&Obj->Member` fails
  the build; the other two compile without a warning.

Full rules: [Pointers and memory](REFERENCE.md#pointers-and-memory), [Intrinsics](REFERENCE.md#intrinsics). Example:
[examples/MemoryRead.cpp](examples/MemoryRead.cpp).

## Building mods

Two tools build mods. `assetgen compile` compiles one source file into cooked packages. `tools/bpbuild.py` builds every
mod that a `mods.yaml` lists, recompiles only what changed and packs each mod into a pak. You normally run bpbuild. It
runs `assetgen compile` once per source, and you run that by hand to look at what one file produces.

### assetgen

```
assetgen compile <source.cpp> <include-dir> <out-dir> [--api <api-dir>]
assetgen registry <out AssetRegistry.bin> <AssetRegistry.bin>...
assetgen verify <out-dir> <reference-dir>
```

With no arguments, or too few, `assetgen` prints this usage and exits with 2. `compile` and `registry` exit with 0 on
success. On failure they print `FAILED: <reason>` and exit with 1, and an unexpected error inside the compiler is
reported the same way, as `internal error: ...`. `verify` is a self-check of the package writer: it rebuilds one known
class and compares it byte for byte with an Epic-cooked original, which is not distributed. It plays no part in
building a mod.

| Argument | Meaning |
| --- | --- |
| `<source.cpp>` | One translation unit. It must contain `UE_MOD_PACKAGE("/Game/...")`, which names the mod's own package folder. |
| `<include-dir>` | The SDK's `UeApi` folder. It must hold `Conv.json`, `Ops.json`, `Types.json` and `Events.json`; otherwise the compile stops with `missing or invalid <dir>/<file> (run genueapi.py)`. |
| `<out-dir>` | Where the mod's own packages go. The compiler creates it, and the folder of every package it writes, when missing. |
| `--api <api-dir>` | Optional, after the out dir. Also writes an editor stub for each class, struct and enum into this folder (see [Editor API stubs](#editor-api-stubs)). It is an output folder for the editor, not the SDK folder. It must already exist: without it a class gets no stub and prints `<Class> -> no API asset: cannot write .uasset`, and a struct or enum fails the compile. |

**Parsing.** AssetGen runs `clang++` from `PATH` with `-std=c++20 -fsyntax-only` and reads the syntax tree that clang
dumps as JSON. The include dir and its parent are both on the include path, so `#include "Engine.h"` and
`#include "UeApi/Engine.h"` both work, and so does `#include "UeAssets/USoundWave.h"` when `UeAssets/` sits beside
`UeApi/`. A quoted include relative to the source's own folder works as in any clang build. On Linux, clang still parses
for the game's Windows target; the [README](README.md) says what that means for includes. clang's own errors go to the
console, and the compile then ends with `clang rejected <source> (diagnostics above)`. The syntax tree can run to
hundreds of MB; it streams from clang through a pipe and is never written to disk, and several compiles can run at once.

**What it writes.** One cooked package, a `.uasset` and a `.uexp`, for each of these:

| Package | When |
| --- | --- |
| each class, `UE_STRUCT`, interface and `UE_ENUM` the source declares | always |
| each namespace-scope data asset (`UCaveTuning CT_Easy = {...};`) | always |
| `<Ns>__<Name>`, a class whose default object holds a namespace-scope variable | when a function uses the variable |
| `FDeref`, the scratch struct that raw pointer reads go through | when the source reads through a pointer and does not declare `FDeref` itself |
| two structs per map type walked in place, `FMapSlot_TMap_<K>_<V>` and `FMapSlots_TMap_<K>_<V>`, and a wrapper struct per container nested in another container | when the code needs one |

It prints one line per package and one for the registry. Compiled into the bpbuild layout, a source with one class
prints:

```
  RunDemo        -> RunDemo.uasset  (extends AActor)
  registry       -> <root>/AssetRegistry.bin  (1 asset)
```

`extends BP <Class>` marks a Blueprint parent, and a struct prints `(struct, N members)`.

A source with nothing to cook fails with `the source declares no UE_STRUCT, UE_ENUM or class deriving from a UE class`.
Two declarations that would land in the same package fail with `<A> and <B> would both be cooked as <package>`; package
names are compared ignoring case. [What a failed compile leaves behind](#what-a-failed-compile-leaves-behind) says
what a failure deletes.

**Where it writes.** A package in the mod's own folder goes under the out dir and keeps its subfolders: a class in
`namespace Weapons` goes to `<out-dir>/Weapons/`. Any other `/Game` package is written relative to the `Content` folder
that the out dir is assumed to sit in. To find it, the compiler walks up one folder from the out dir for each level of
the `UE_MOD_PACKAGE` path below `/Game`. So compile into `<root>/Content/<package path without /Game>`, as bpbuild does.
Two kinds of package land outside the mod's folder: a class in a namespace that starts at `Game` (see
[Extending a game Blueprint](#extending-a-game-blueprint)), and the wrapper structs for nested containers, which every
mod shares in one fixed folder (see [Containers](REFERENCE.md#containers)).

### The asset registry

A cooked package carries no asset-registry data of its own. So every compile also writes rows into an
`AssetRegistry.bin` for everything it cooked, each with its class, and bpbuild packs it with the mod so that the
game's asset registry lists the mod's assets.

- When the out dir ends in `/Content/<package path without /Game>` (compared ignoring case), the registry goes to
  `<root>/AssetRegistry.bin`, beside `Content`, which is where a cooked pak keeps it. Any other out dir gets its own
  `<out-dir>/AssetRegistry.bin`, printed as `./AssetRegistry.bin`.
- An existing registry is merged, not overwritten. The rows for each package this compile wrote replace that
  package's old rows, so several sources compiled into one pak share one registry.

`assetgen registry <out> <in>...` merges registries that AssetGen wrote into `<out>`, and creates `<out>` when it is
missing. Each input's rows replace the rows `<out>` already had for the same packages. It refuses a registry that has
tags or dependencies, such as an editor-cooked one, rather than read half of it. bpbuild uses this command for `embed`.

### Reproducible output

The same source, compiled against the same `UeApi`, gives the same bytes on every run: the compiler reads no clock and
draws no random numbers, a cooked package's GUIDs are zeros, and an editor stub's GUIDs are hashed from its package
name. The output is also meant not to depend on what built `assetgen`. One comparison found the Windows and Linux
builds' output for the test mods byte-identical; it was made before the last fix that removed such a dependence, and
CI does not compare bytes. A different `clang++` version has not been measured. The pak is UnrealPak's output, not
AssetGen's.

Because unchanged sources rebuild to the same bytes, a byte comparison of two builds shows exactly which packages a
compiler change touched.

### bpbuild

```
python tools/bpbuild.py <mods dir> <UeApi dir> <assetgen> [--force] [--no-pak]
```

| Argument | Meaning |
| --- | --- |
| `<mods dir>` | The folder that holds `mods.yaml`. Every relative path in `mods.yaml` is relative to it. Without a manifest, bpbuild stops with `no manifest at <path>`. |
| `<UeApi dir>` | The include dir that every compile gets. |
| `<assetgen>` | The compiler. bpbuild makes the path absolute first, so a relative one works. |
| `--force` | Recompile every mod, changed or not. |
| `--no-pak` | Compile only, and leave the staged files for you to pack. `embed` is not applied either: dependencies are copied in only when a pak is made. |

With fewer than three arguments, bpbuild prints its usage line and stops.

#### mods.yaml

```yaml
mods:
  - name: MathLib                  # -> out/MathLib_P.pak
    sources: [MathLib.cpp, MathLib.h]
    generate_api: true             # also write editor stubs

  - name: LibraryUser              # -> out/LibraryUser_P.pak
    sources: [LibraryUser.cpp, MathLib.h]
    needs: [MathLib]               # build MathLib first
    embed: true                    # and pack MathLib's assets into LibraryUser_P.pak too
```

Top-level keys:

| Key | Meaning |
| --- | --- |
| `mods` | The list of mods. If it is empty or missing, bpbuild prints `mods.yaml lists no mods.` and exits with 0. |
| `api_dir` | The default `api_dir` for every mod that sets `generate_api`. |

Keys of a mod:

| Key | Default | Meaning |
| --- | --- | --- |
| `name` | required | Names the build folder `build/<name>/` and the pak `out/<name>_P.pak`. Other mods refer to the mod by this name in `needs`. |
| `sources` | required | Files relative to the mods dir. Each `.cpp` is compiled, in order, into the mod's package folder. Any other file only counts for staleness, so list the headers the `.cpp` files include. The **first** entry must hold the mod's `UE_MOD_PACKAGE` line: bpbuild finds it with a text search and stops the whole build with `<file> declares no UE_MOD_PACKAGE` otherwise, so list the `.cpp` first. With `embed`, the same check runs on the first source of each dependency. A mod with no sources, or with a source that does not exist, prints `SKIP - no such source` and counts as failed. |
| `needs` | none | Mods to build before this one. An unknown name stops the build with ``mods.yaml: `needs` names an unknown mod: <name>``. A cycle is not an error: bpbuild breaks it, and since every mod compiles before any mod packs, both sides still see each other's assets. |
| `embed` | `false` | Also packs every mod reachable through `needs`, directly or not, into this mod's pak, and merges their registries into its registry. The pak then works on its own. |
| `generate_api` | `false` | Also writes the editor stubs for this mod's classes, structs and enums (see [Editor API stubs](#editor-api-stubs)). |
| `api_dir` | top-level `api_dir`, else `<mods dir>/out/api/Content` | Where the stubs go: the `Content` folder of a UE project. A mod's stubs land in `<api_dir>/<package path without /Game>/`. Only read when `generate_api` is set. |

`api_dir` takes any of these forms:

```yaml
api_dir: ../MyProject/Content                      # one path, relative to the mods dir or absolute
api_dir: [D:/ProjA/Content, D:/ProjB/Content]      # several projects
api_dir:
  - { path: D:/ProjA/Content, ue: 4.27 }           # with the project's engine version
  - D:/ProjB/Content
```

- Environment variables in a path are expanded, with Python's `os.path.expandvars`: `%NAME%`, `$NAME` and `${NAME}` on
  Windows, `$NAME` and `${NAME}` elsewhere.
- `ue` defaults to 4.27, the only version AssetGen writes. Any other value stops the build with
  `mods.yaml: api_dir <path> wants UE <v>; assetgen writes 4.27 only`. An entry with no `path` stops it with
  ``mods.yaml: an `api_dir` entry has no `path` ``.
- With several entries, `assetgen` writes into the first one and bpbuild copies the stubs to the rest.

#### What a build does

1. **Compile.** Mods are taken in `mods.yaml` order, except that the mods a mod `needs` go first. For each mod that is
   out of date (see below), bpbuild deletes `build/<name>/FSD/Content` and `build/<name>/FSD/AssetRegistry.bin`, then
   runs `assetgen compile <source> <UeApi dir> build/<name>/FSD/Content/<package path>` for each `.cpp`, adding
   `--api <first api dir>/<package path>` when `generate_api` is set. A failed compile prints `<name> FAILED`, skips the
   mod's remaining sources and moves on to the next mod.
2. **Pack.** Every mod's assets exist by now, so an `embed` mod can copy in its dependencies' assets. Each mod whose pak
   is out of date is packed with UnrealPak.
3. **Check imports between mods.** If nothing failed, bpbuild reads the import table of every `.uasset` in each mod's
   package folder, namespace subfolders included. An import of a package inside one of this build's mod folders that no mod produced fails the
   build with `<asset> imports <package>, which no mod in this build produces`. In game, such an import loads without an
   error and resolves to null. The usual cause is a `UE_CLASS` path that names the folder instead of
   `<mod package>/<Class>`. This check also runs with `--no-pak`.

A build of the `mods.yaml` above prints this, with each compile's own lines above it:

```
MathLib          compiled -> /Game/_AssetGenExamples/MathLib
MathLib          api      -> out\api\Content\_AssetGenExamples\MathLib
LibraryUser      compiled -> /Game/_AssetGenExamples/LibraryUser
MathLib          packed   -> ...\out\MathLib_P.pak
LibraryUser      packed   -> ...\out\LibraryUser_P.pak
built 2, packed 2, up to date 0, failed 0
```

The last line counts the mods built, packed, up to date and failed. The exit status is 1 if anything failed. The
`api ->` line of a `generate_api` mod is printed on every run, and a second run with nothing changed prints
`up to date` for each mod. LibraryUser's registry then lists its own classes and the embedded `MathLib_C`.

#### When a mod is rebuilt

| bpbuild... | when |
| --- | --- |
| recompiles the mod | `--force` is given; the mod's package folder holds no staged asset yet; `generate_api` is set and a stub folder holds no `.uasset`/`.uexp`; or one of the `sources`, the files directly in the UeApi dir, or the `assetgen` binary is newer than the **oldest** staged asset. |
| repacks without recompiling | the newest staged asset of the mod, or of a dependency it embeds, is newer than the mod's pak (not with `--no-pak`). This picks up a dependency that changed, or a pak that an earlier `--no-pak` run skipped. |
| prints `up to date` | neither applies. |

The oldest asset is the reference, not the newest, so a half-finished earlier run does not look current.

#### The build tree

```
<mods dir>/
  mods.yaml
  build/<name>/
    autogen.txt                          the UnrealPak response file
    FSD/AssetRegistry.bin                the pak's one registry
    FSD/Content/<package path>/*.uasset|*.uexp
    FSD/Content/<dependency package>/... copied in at pack time when embed is set
  out/<name>_P.pak
  out/api/Content/<package path>/        default stub folder, with a .assetgen list of the stubs written there
```

#### Packing

bpbuild finds UnrealPak in this order:

1. the `UNREALPAK` environment variable, if set;
2. on Windows, `C:\Program Files\Epic Games\UE_4.27\Engine\Binaries\Win64\UnrealPak.exe` (there is no `PATH` lookup on
   Windows);
3. on other systems, `UnrealPak` on `PATH`.

On Linux that is a native UnrealPak or a wrapper that runs the Windows one under Wine; the [README](README.md)'s
section on packing on Linux has the script. bpbuild writes the response file
`"<mods dir>\build\<name>\FSD\*.*" "..\..\..\FSD\*.*"` and runs
`UnrealPak <pak> -platform=Windows -create=<response file> -compress`. The whole `build/<name>/FSD` tree goes into the
pak under `../../../FSD/`, the layout of the game's own pak. When UnrealPak fails, bpbuild prints
`UnrealPak failed (<code>):` and the last six lines of its output. To pack by hand after `--no-pak`, put the same tree
under the same mount point.

#### bpbuild pitfalls

- **`embed` and the import check look only inside a mod's package folder.** Its namespace subfolders count, but a
  package the mod writes outside its folder (a class in a `Game::` namespace, a nested-container wrapper struct) is
  only packed into the mod's own pak: `embed` does not copy it from a dependency, and an import of it is not checked.
- **A missing UnrealPak counts as a failure.** bpbuild prints `UnrealPak not found at <path> - skipping the pak.` and
  still compiles every mod, but each mod it wanted to pack counts as failed, the exit status is 1 and the import check
  is skipped. Pass `--no-pak` when you mean to pack yourself.
- **Keep your own assets out of the stub folders.** Before it recompiles a `generate_api` mod, bpbuild deletes the files
  listed in each stub folder's `.assetgen` file; a folder without that file is left alone. After compiling, it rewrites
  the list from every `.uasset` and `.uexp` in the folder, not only the stubs it wrote. A hand-made asset kept beside
  the stubs is therefore listed after one build and deleted by the next recompile.
- **bpbuild installs nothing.** It stops at `out/<name>_P.pak`; install the pak as you install any DRG mod.

## Running in the game

A pak only adds classes. Nothing runs until something creates an instance of one. DRG mods handle this with two actor
classes in the mod's folder, named `InitSpacerig` and `InitCave`: the game spawns a mod's `InitSpacerig` in the Space
Rig and its `InitCave` in a mission. Declare both as empty subclasses of your main actor, so that whichever one the
game spawns runs its `ReceiveBeginPlay`:

```cpp
class Greeter : public AActor {
public:
  int32 Starts = 0;

  void ReceiveBeginPlay() { Starts += 1; }
};

class InitSpacerig : public Greeter {};   // spawned in the Space Rig
class InitCave : public Greeter {};       // spawned in a mission
```

This is what [examples/HelloWorld.cpp](examples/HelloWorld.cpp) does. The class must be named exactly `InitSpacerig` or
`InitCave`, in a folder at any depth. So many mods declare classes of these names that the SDK gives them no global
alias. The pak's `AssetRegistry.bin`, which bpbuild puts in every pak, is loaded with it.

### When nothing happens

Make the mod show that it ran. Print String shows nothing in the shipped game, so post a message through the game
state's `PostGameMessage` in `ReceiveBeginPlay`, as HelloWorld does. Then check:

- **The pak's contents.** The pak holds what bpbuild staged in `build/<name>/FSD`. Look there for the `InitSpacerig` and
  `InitCave` packages in the mod's folder, and run `python tools/dumpar.py build/<name>/FSD/AssetRegistry.bin` to list
  the registry's rows (see [Testing and inspecting](#testing-and-inspecting)).
- **The game's log.** It is `FSD/Saved/Logs/FSD.log`; an earlier run's log is kept beside it as
  `FSD-backup-<time>.log`. Search it for your mod's folder. The spawn itself logs nothing.

## The SDK

The headers a mod includes are the SDK. The published one is the
[DRG-Blueprint-Cpp-SDK](https://github.com/Elytras/DRG-Blueprint-Cpp-SDK) repo, generated from an unmodded game. Its
README names the game build it describes and the AssetGen commit that generated it; use that compiler or a later one,
because the JSON tables in `UeApi/` are part of the contract between the headers and the compiler. After a game update,
use a newer SDK or regenerate your own.

### What UeApi/ holds

| File | What it holds |
| --- | --- |
| `<Module>.h` | One per native package `/Script/<Module>` (`Engine.h`, `FSD.h`, ...): its enums, structs and classes, with the operators on its structs. |
| `Game/<Name>_C.h` | One per game Blueprint class. The class sits in a namespace that mirrors its `/Game` folder. |
| `UeApi.h` | Includes every native package header, not the `Game/` ones. It is slow to parse; include the headers you use instead. |
| `Conv.h`, `Conv.json` | Every Kismet `Conv_XToY` the compiler can use as an implicit conversion or an explicit cast. |
| `Ops.json` | The Kismet functions behind operators on structs and soft pointers (`==`, `+`, ...). |
| `Containers.h` | The Kismet `Array_*`, `Set_*` and `Map_*` functions, as methods of `TArray`, `TSet` and `TMap`. |
| `Types.json` | Every enum and struct: package, engine name, size, alignment and fields, and whether an enum is an `enum class` (`form`), read off how the dump's properties of it are reflected. |
| `Events.json` | The function flags of every `BlueprintEvent`, which an override inherits. |
| `UeMeta.h` | The `UE_*` macros. Written by hand. |
| `Types.h` | The integer spellings, `FString`, `FName`, `FText` and the container templates. Written by hand. |

A header holds a member if and only if the compiler can compile a use of it, so clang accepting your source and
AssetGen being able to compile it are the same statement. A member whose type the generator cannot map yet is left out,
and a member that is not in the headers cannot be named. For scale, the published SDK declares 9191 classes, 34053
functions and 57060 properties across 4758 headers, 4628 of them Blueprint headers under `Game/`.

Functions in the headers carry more than their C++ signature. The net and authority marks (`UE_SERVER`, `UE_CLIENT`,
`UE_MULTICAST`, `UE_RELIABLE`, `UE_AUTHORITY_ONLY`, `UE_COSMETIC`) and `UE_PURE` are written on the game's own
functions. A function that takes a world context also has an overload without it, and a latent function has one without
its `FLatentActionInfo`. When a latent function also takes a one-parameter completion delegate and returns nothing,
another overload drops both and returns the delegate's value. See
[Calling engine and game functions](REFERENCE.md#calling-engine-and-game-functions) and
[Latent calls](REFERENCE.md#latent-calls).

### Game Blueprints

`UeApi/Game/` declares each game Blueprint class in the namespace of its folder:
`/Game/GameElements/GameEvents/ExplosiveBarrelsEvent/BP_ExplosiveBarrel` is
`Game::GameElements::GameEvents::ExplosiveBarrelsEvent::BP_ExplosiveBarrel_C`, in `Game/BP_ExplosiveBarrel_C.h`. In a
folder name, a character that C++ does not allow becomes `_`, and a leading digit gets a `_` in front. A class name that
only one asset uses is also aliased at global scope, so `BP_ExplosiveBarrel_C` works bare. A name that several assets
share gets no alias, and naming it bare fails to compile: write the namespaced spelling. See
[Extending a game Blueprint](#extending-a-game-blueprint).

### Names the SDK respells

Dumper-7, which the SDK is generated from, renames members whose engine names C++ cannot use. When two members collide
it respells one (`Name` becomes `Name_0`), it turns a character C++ does not allow into `_` (`Audio Flying` becomes
`Audio_Flying`), and it turns a leading digit into a word (`3P` becomes `ThreeP`). Write the SDK's spelling. The header
keeps the engine's name beside the member, and everything AssetGen cooks (defaults, property references, function
names) uses the engine's name:

```cpp
static constexpr const char* Name_0__UeName = "Name";
```

Parameter names keep the C++ spelling, which only an editor stub's pin names would show. The headers carry a few other
markers of this kind, all written by the generator and read by the compiler. You never write them:

| Marker | What it records |
| --- | --- |
| `<Member>__UeName` | The engine's name for a member or function the dumper respelled. |
| `<Member>__UeScsNode` | The construction-script node of a component that a game Blueprint adds, through which a child class overrides the component's defaults. |
| `<Member>__UeSubobject` | The default subobject that a native component member points at, which a mod class overrides by that name. |
| `UeDefaultSubobjects` | Every default subobject a game Blueprint's default object exports, which a child class is loaded after. |
| `<Member>__Replicated` | That a property replicates, and its RepNotify function. |
| `<Function>__UeForward` | What `GetOuter`, `GetClass` and `GetName` really call. |

### UeAssets/

`UeAssets/<Class>.h` declares every game asset of one class, named by its content path:

```cpp
#include "UeAssets/UEnemyDescriptor.h"

UEnemyDescriptor *Grunt = &UeAssets::UEnemyDescriptor::Game::Enemies::Spider::Grunt::ED_Spider_Grunt;
```

Each asset is a `UE_ASSET_AT` in a namespace that mirrors its folder, and `UeAssets::<Class>::All` holds every one of
them as soft pointers. Names that are not C++ identifiers are fixed up: another character becomes `_`, a leading digit
gets a `_` in front, a C++ keyword or a name starting with `__` gets a trailing `_`, and a name that clashes with a
subfolder or an earlier asset in the same folder gets more `_`s. Left out are Blueprint classes (`UeApi/Game` names
those), redirectors, the editor-only `Blueprint`, `WidgetBlueprint` and `AnimBlueprint` objects, classes that `UeApi`
has no header for, and class names that two packages share. The largest headers take a few seconds to parse, so include
only the classes you use. See [Game assets](REFERENCE.md#game-assets).

### Generating your own

The [README](README.md) lists the steps: dump the game with the Dumper-7 fork, run `tools/genueapi.py`, copy `UeMeta.h`
and `Types.h` from the SDK repo, and optionally run `tools/genueassets.py`. What the steps leave out:

- **Dump an unmodded game.** A mod manager's pak replaces some game classes (the SDK repo names `BP_GameInstance` and
  the player controllers for mint's), and a dump taken with mods loaded declares those modded versions and every mod's
  classes. The published SDK was dumped with `FullAssetPaths=1` and `PreloadAssets=1`, which loads every asset in the
  registry before the dump.
- **Run genueapi on the dump itself.** `python tools/genueapi.py <dump>/SDK/SDK <UeApi dir>` reads the SDK folder's
  `*_structs.hpp`, `*_classes.hpp`, `*_functions.cpp` and `*_parameters.hpp`, and the object dump
  `GObjects-Dump-WithProperties.txt` two folders up. The object dump is how it learns the engine's real name for each
  member the dumper renamed. Without it, genueapi stops with
  `no <path>: run genueapi on the SDK inside its Dumper-7 dump, not a copy of it`. The SDK folder and the object dump
  must come from the same dump.
- **Give it the game's content with `--game <extracted Content dir>`.** The dump carries neither a class's flags,
  ClassWithin and config name nor a native class's interfaces. genueapi reads the game's cooked Blueprints for them
  (about 20 seconds): each game Blueprint's own tail, and the native interfaces a native class implements, as the
  Blueprints that override one's function show. Without it, a mod deriving from a game Blueprint gets its nearest
  native ancestor's tail, and an override of a native ancestor's interface function is taken for a new function.
- **Your own mods are left out.** genueapi skips every class whose package a mod in the folder above `<UeApi dir>`
  cooks (any `.cpp` or `.h` directly in that folder with a `UE_MOD_PACKAGE`), and the shared nested-container structs.
  Keep `UeApi/` inside your mods folder, and a dump taken with your mods loaded does not declare them a second time.
- **Put `UeAssets/` beside `UeApi/` before you generate it.** Each `UeAssets` header includes `UeMeta.h` and its class's
  `UeApi` header by a path relative to where it was written, so the two folders move together.
- **A mod pak has no registry.** `--pak <pak or extracted folder>` (repeatable) adds its assets to `UeAssets/` from each
  `.uasset`'s own export table. They replace the registry's rows at the same path, as the pak's files replace the
  game's in game. A `.pak` is extracted with UnrealPak, found through `UNREALPAK` or else the Windows default install
  path. Only `/Game` content is taken. genueassets ends with one line: how many assets and headers it wrote, and how
  many rows it left out, by reason.

genueapi prints one line per table it writes, then a summary of counts. These lines report something left out:

| Line | What it means | What to do |
| --- | --- | --- |
| `container method held back: <function>` | A Kismet `Array_`, `Set_` or `Map_` function has a type genueapi cannot map, so that one method is missing from `TArray`, `TSet` or `TMap`. | Nothing. |
| `NOT named back, ...: <n>, e.g. ...` | For these members, the object dump's name at the member's offset is not one that the dumper's renaming rules explain. genueapi writes no `__UeName` rather than a wrong one, and the member keeps the SDK's spelling; if the engine's name really differs, reads, writes and defaults of it miss in game without an error. | This almost always means the SDK folder and the object dump come from different dumps. Dump once and run genueapi on that dump. |
| `SDK helpers left out, no UFunction behind them: <n> (...)` | Functions in the dump's headers that are the dumper's own C++ helpers, not engine functions. No Blueprint can call them. | Nothing. |
| `out of reach: <kind> <n>, ...` | Class functions and properties held back because one of their types has no mapping yet, by kind: `enum`, `struct`, `container` or `other`. `UeApi.h` records the same numbers. | Nothing a mod can do. |
| `blueprint classes dropped for want of a /Game path: <n> (...)` | The dump was taken without `FullAssetPaths=1`, so these Blueprint classes have no path to import them by. | Set `FullAssetPaths=1` in `Dumper-7.ini`, dump again with the fork, and rerun genueapi. |

genueapi stops with exit status 1 when the object dump is missing, and on
`base-class cycle between packages: <package> -> <package> -> ...`, which a mod cannot fix: report it with the dump.
Run with fewer than two arguments, it prints only the last sentence of its help text, not a usage line; the usage is
`genueapi.py <SDK dir> <UeApi dir> [--game <Content dir>]`.

## Editor API stubs

`generate_api: true` in `mods.yaml`, or `--api <dir>` on `assetgen compile`, also writes the editor side of a mod: one
uncooked UE 4.27 package per class, struct and enum, with signatures and no bodies. Someone working in the UE 4.27
editor can then call your mod's functions from their own Blueprints. At run time, your mod's pak supplies the bodies.

```yaml
mods:
  - name: MathLib
    sources: [MathLib.cpp, MathLib.h]
    generate_api: true
    api_dir: D:/UeProjects/MyProject/Content    # optional; the default is out/api/Content in the mods dir
```

Each stub is written straight into the `--api` folder as `<Name>.uasset`, one file with no `.uexp`. bpbuild points
`--api` at `<api_dir>/<package path without /Game>/`, so a mod's top-level classes sit at the same `/Game` path as the
cooked classes they stand for, and what an editor Blueprint references is what the pak provides. `api_dir` can name
several projects; see [Building mods](#building-mods) for its forms. Only UE 4.27 has a writer, and an `api_dir` entry
that asks for another version stops the build.

A class stub holds:

- one function graph per function a Blueprint can call, with an entry node for the inputs and a result node for the
  outputs. A `UE_PURE` function has no exec pins. The graphs are empty; the editor recompiles them when it loads the
  asset. Static functions have no world-context parameter, because the editor adds its own hidden one;
- each function's access, static, const and net flags, and its `UE_CATEGORY`;
- the class's variables that are not private, with their flags, replication, RepNotify, category and literal defaults;
- an empty generated class and default object, which the editor fills in on load, and registry tags, so that the
  Content Browser lists the asset.

For example, with `--api` this class gets a stub with the variable `Charges` and the functions `Recharge` and `Left`,
all three carrying the category `Teleporter`:

```cpp
class StubDemo : public AActor {
public:
  UE_CATEGORY("Teleporter");
  int32 Charges = 3;                          // a variable: in the stub, with its default
  void Recharge(int32 By) { Charges += By; }  // a function graph with one input
  UE_PURE int32 Left() { return Charges; }    // pure: no exec pins
  UE_CATEGORY("");

  void ReceiveBeginPlay() { Charges += 1; }   // an event override: not in the stub

private:
  int32 Seed = 0;                             // private: not in the stub
};
```

An event override such as `ReceiveBeginPlay` is not a callable function, so it is not in the stub, and an `inline`
method is no function at all. A private function is kept, flagged private, so the editor refuses calls to it from
other classes. A struct stub is a `UserDefinedStruct` whose member names match the cooked struct's, so the editor
resolves them to the same members. An enum stub is a `UserDefinedEnum` whose enumerators keep their C++ names.

The compile prints a line for each thing a stub leaves out, and none of them fails the build:

- `<Function> skipped: parameter X has no editor-side type yet`, and
  `<Variable> skipped: variable has no editor-side type yet`;
- a `TODO:` line when a class whose stub has variables also has a `ReceiveBeginPlay` or a construction script, such as
  `TODO: ReceiveBeginPlay may set variables from calls; ...`: defaults those functions compute are not in the stub,
  only literal ones;
- `<Class> -> no API asset: <Class> exposes no callable function or variable`. `InitSpacerig` and `InitCave` print this,
  since they add nothing. A struct or enum stub that cannot be written, by contrast, fails the compile.

No stub is written for the compiler's own helper structs (`FDeref`, the `TMap` walk structs and the nested-container
wrappers). Not yet: a stub for an interface the mod declares, so a Blueprint made in the editor cannot implement one; a
mod class written in C++ can.

To use the stubs:

1. Point `api_dir` at your UE 4.27 project's `Content` folder, or copy the stubs from `out/api/Content`.
2. Open the project. Stubs of a function library, of actor classes, and of structs and enums have been loaded and
   re-saved in UE 4.27.2 with no errors.
3. Call the mod's functions from your Blueprints.
4. Do not ship the stubs. Keep their folder out of your cook, and install the mod's own pak beside yours. UE's
   "Directories to never cook" packaging setting should keep them out; that has not been tested.

Not checked yet: whether the editor files members under their `UE_CATEGORY`, which is written into the stub, and
whether it accepts the stub of a class the compiler cooks into a subfolder (one in a C++ namespace), which is still
written into the mod's top folder.

## Testing and inspecting

The scripts in `tools/` check the compiler, run compiled functions without the game, and print what is inside a cooked
package. Those below need only Python 3's standard library.

### The test suite

```
python tools/test_bytecode.py [--assetgen <assetgen>] [--ueapi <UeApi dir>] [--cases <file>] [--no-prefetch | --check-prefetch]
```

It compiles every `tests/*.cpp` and every `examples/*.cpp`, runs the compiled functions offline against expected
results, and checks what the engine reads off the cooked files. It deletes `tests/build` first, so two runs in one
checkout break each other. Pass `--ueapi` with the SDK's `UeApi`. `--cases <file>` also writes every offline run as
JSON (arguments, members before and after, return value), for replaying the calls in game with a harness of your own.
CI runs it on Linux and Windows.

After those first compiles, the tests compile about 200 small mods one at a time. The suite makes them ahead: each run
lists them in `assetgen-suite-prefetch.json` in the temp folder (one file for every checkout on the machine), and the
next run starts them all at once in staging folders, so a test usually finds its compile done. A result is used only
when the compile is the same in everything it reads, and its output is moved into place as if the test had compiled
there; the line before the last says how many were ready. `--no-prefetch` compiles each one when the test asks;
`--check-prefetch` also compiles each prefetched one directly and stops the run if anything differs. Deleting the
JSON file only costs the next run its head start.

### Running a function offline

`tools/runscript.py` runs one function's bytecode straight from the cooked files and prints its return value:

```
python tools/runscript.py <base path without extension> <Function> [Parm=value ...]
```

Take this class, built by bpbuild as a mod whose `UE_MOD_PACKAGE` is `/Game/_MyMods/RunDemo`:

```cpp
class RunDemo : public AActor {
  int32 Count = 0;

public:
  int32 SumTo(int32 N) {
    int32 Total = 0;
    for (int32 I = 1; I <= N; I++) Total += I;
    return Total;
  }

  float Half(float X) { return X * 0.5f; }

  void Bump(int32 By) { Count += By; }

  void ReceiveBeginPlay() {
    Count += 1;
    UKismetSystemLibrary::Delay(1.0f);
    Count += 10;
  }
};
```

```
> python tools\runscript.py mods\build\RunDemo\FSD\Content\_MyMods\RunDemo\RunDemo SumTo N=4
10
> python tools\runscript.py mods\build\RunDemo\FSD\Content\_MyMods\RunDemo\RunDemo Half X=3.0
1.5
```

On the command line, a value containing `.` is a float and anything else an integer. A parameter you leave out starts
at 0, as it does in the VM's frame. As a Python module, `run(base, function, self_vars=None, **parms)` returns the
return value and the locals, takes any Python value, and reads and writes the object's members in `self_vars`.

`tools/runvm.py`, a module with no command line, adds what runscript cannot do alone: latent calls and the event
graph's persistent frame, delegate binds and broadcasts, and calls and member writes on other objects. The members of
the object being run live in `vm.self.vars`:

```python
import sys
sys.path.insert(0, 'tools')
from runscript import run
from runvm import VM, latent_call

base = r'mods\build\RunDemo\FSD\Content\_MyMods\RunDemo\RunDemo'
members = {'Count': 5}
print(run(base, 'Bump', self_vars=members, By=2))   # (None, {'By': 2})
print(members)                                      # {'Count': 7}

vm = VM(base, natives={'Delay': latent_call}, Count=0)
vm.call('ReceiveBeginPlay')
print(vm.self.vars)                                 # {'Count': 1}: waiting in the Delay
vm.fire(0)                                          # finish the first pending latent action
print(vm.self.vars)                                 # {'Count': 11}
```

`VM(base, natives=None, isa=None, objects=None, **members)` also takes `isa`, the class test the run uses, and
`objects`, the objects that an object constant names. Engine calls are recorded in `vm.log` as
`(name, context, args)`, beside `('set', object, member)` for each member write, and answered by the `natives` you pass,
each a function of `(vm, context object, *args)`. A call with no native returns nothing, except `IsValid`, which tests
for non-null. `vm.broadcast(obj, 'OnDone', 5)` calls every handler bound to that dispatcher.

The interpreters model locals, jumps, `switch` and the execution-flow stack; `int32` wraparound; strings, names and
texts as Python strings (with `==` on names and strings ignoring case); `TArray`, `TSet`, `TMap` and their library
functions; the Kismet math, conversion and string functions in their tables; calls within the class; and raw pointer
reads and writes against a fake memory. A few engine calls have stand-ins: `GetFSDGameState` returns a placeholder,
`PostGameMessage` collects its messages and `RandomInteger` returns 0. In runscript, any other opcode or call stops the
run with `unsupported op ...` or `unsupported call <name>`, so a run that finishes has executed every instruction it
reached. A run also stops on code that would misbehave in the real VM, such as a computed value passed to a native that
reads that argument by address. runscript stops a loop past a million steps as a runaway, and runvm stops a function
after 100,000 statements. In runvm an unknown opcode still stops the run, but a call with no native returns nothing
and the run goes on, so look in `vm.log` for calls you did not answer.

An offline run has no engine: no real objects, no spawning, no ticking, no world, no garbage collection and no
replication, and its memory is fake. What it models of the engine was written in by hand, from reading the engine. A
function that passes offline can still misbehave in game, most of all when it depends on engine state.

### Looking inside a package

Most of these tools take a package's **base path**, the path without `.uasset` or `.uexp`. Export indexes count from 0,
and `dumpedl.py` lists every export with its index.

| Tool | Usage | Prints |
| --- | --- | --- |
| `dumpedl.py` | `dumpedl.py <file.uasset>` | Every export with its index and its preload dependencies. Takes the `.uasset` file itself, not a base path. |
| `walkscript.py` | `walkscript.py <base> <export-index>` | A function's bytecode, expression by expression, with disk and memory sizes, and whether the totals match the header. `loader would STOP here` means the engine would stop reading early. |
| `dumpstruct.py` | `dumpstruct.py <base> <export-index>` | A function or class export's body: its super, children, every property with flags and type, the bytecode sizes, then the function flags or the class trailer. |
| `dumptags.py` | `dumptags.py <base> <export-index> [byte-offset]` | The export's tagged properties, decoded, such as a default object's defaults. A struct's default instance starts after the struct's body, so pass that offset. |
| `dumpexp.py` | `dumpexp.py <base> <export-index> [count]` | The export's class, super and outer, then its payload as 4-byte words, each annotated with the name or import it may stand for. Reads cooked packages and single-file uncooked ones, such as editor stubs. |
| `dumpar.py` | `dumpar.py <AssetRegistry.bin>` | Every row of a registry, AssetGen's or the game's, then `OK - consumed the whole file`. It exits with an error unless the whole file parses. |
| `genenums.py` | `genenums.py <UE_4.27/Engine/Source/Runtime>` | Regenerates the compiler's `UeEnums.h` (bytecode tokens and flag values) from the engine source. For compiler development only. |

To read one function, find its index with `dumpedl.py`, then walk it. For `Half` above, export 3:

```
> python tools\walkscript.py mods\build\RunDemo\FSD\Content\_MyMods\RunDemo\RunDemo 3
Half: header ScriptBytecodeSize=26 ScriptStorageSize=30, script at +126
  + 126 mem    0  disk  29 mem  25  Return
  + 127 mem    1  disk  28 mem  24    CallMath             imp[3]:Function'Multiply_FloatFloat'
  + 132 mem   10  disk  17 mem   9      LocalVariable      X@exp[3]:Half
  + 149 mem   19  disk   5 mem   5      FloatConst
  + 154 mem   24  disk   1 mem   1      EndFunctionParms
  + 155 mem   25  disk   1 mem   1  EndOfScript
walked: disk 30 (header 30)  mem 26 (header 26)
```

and the mod's registry:

```
> python tools\dumpar.py mods\build\RunDemo\FSD\AssetRegistry.bin
version 8
1 asset(s), 5 name(s)
  /Game/_MyMods/RunDemo/RunDemo.RunDemo_C              BlueprintGeneratedClass
     package /Game/_MyMods/RunDemo/RunDemo   path /Game/_MyMods/RunDemo   asset RunDemo_C
     flags 0x80000000  tagmap 0x0  bundles 0  chunks 0
dependencies: 0 node(s)
package data: 0 entr(y/ies)
OK - consumed the whole file (323 bytes)
```

The bytecode's exact shape changes as the compiler improves, so compare what a function does, not its opcodes. When you
report a compiler bug, attach the `.uasset` and `.uexp` it produced, as the README's bug form asks.

## Warnings, refusals and compiler bugs

Some C++ has no exact Blueprint equivalent. Where a close one exists, AssetGen compiles it, prints a `warning:` that
says what differs, and the build goes on. What it cannot compile faithfully, it refuses, rather than write a Blueprint
that quietly does something other than your C++. Some gaps are not caught yet; [Limits](#limits) lists them.

### Reading a refusal

The compile stops at the first problem it finds. It prints one line that starts with `FAILED:` and exits with 1, so a
compile reports one refusal at a time: fix it and compile again. In a class `Pickup`:

```cpp
float Half(float X) { return X * 0.5; }   // refused: 0.5 is a double
```

```
  FAILED: Pickup::Half: no Kismet conversion from float to double
```

The start of the message says where the problem is:

- `<Class>::<Function>: ` for anything in a function body, here the method `Half` of `Pickup`. When the problem sits
  inside an inline function that the body calls, `inline <Class>::<Method>: ` follows and names the inline function.
- `<Class>::UE_DEFAULTS: ` for a statement in a `UE_DEFAULTS` block, and `<Class>::<Member>: ` for a declaration such
  as a `UE_COMPONENT` or a replicated variable.
- the member's name alone for a default value (`Kind: a default is a value known when the mod is built - ...`), and
  nothing at all for some member types (`TODO: unimplemented property Mask: uint32`).

A message with `TODO:` in it names a construct that AssetGen has no translation for. Some of these are gaps that may
close; others, such as a `uint32` variable, have no Blueprint type to become. [Diagnostics](REFERENCE.md#diagnostics)
lists every message with its cause and its fix.

When clang rejects the C++, nothing else runs. Its own errors come first, then AssetGen's line. In a class `Spinner`:

```cpp
void ReceiveTick(float DeltaSeconds) override {}   // refused by clang
```

```
mods\Spinner.cpp:8:40: error: only virtual member functions can be marked 'override'
    8 |   void ReceiveTick(float DeltaSeconds) override {}   // refused by clang
      |                                        ^~~~~~~~
1 error generated.
  FAILED: clang rejected mods\Spinner.cpp (diagnostics above)
```

bpbuild runs the same compile and shows the same lines. After a failed compile it prints `<Mod>  FAILED` (for
example `HelloWorld       FAILED`), skips the mod's remaining sources and goes on with the other mods. The last line
counts the failures, and bpbuild exits with 1.

### Warnings

A warning starts with `warning:`, names the class and the function, and says what differs from C++. The compile still
writes every package and exits with 0, and bpbuild builds and packs the mod. In a class `Scores`:

```cpp
TMap<int32, int32> ByPlayer;
void Add5(int32 &V) { V += 5; }
void Bump(int32 Player) { Add5(ByPlayer[Player]); }   // warns
```

```
  warning: Scores::Bump: Add5's reference parameter V is bound to a map element: Blueprint has no reference to it, so V gets a copy, stored back after the call
  Scores         -> Scores.uasset  (extends AActor)
  registry       -> ./AssetRegistry.bin  (1 asset)
```

Read each warning: whether the difference matters depends on your code. AssetGen has five warnings, and the table
below has the three you meet first. clang prints warnings of its own, with the file and line in front, such as
`-Wunused-value` for a pure call whose result you ignore. They do not stop the build either.

### What a failed compile leaves behind

A failed compile does not write or update `AssetRegistry.bin`. When a class, struct or interface fails, AssetGen
deletes every class, struct and interface package of that source before it exits, including the ones it wrote before
the failure. A problem found later, while AssetGen writes a `UE_ENUM`, a data asset, a global variable's holder class
or a helper struct, can leave the source's class packages in the out dir. Do not pack an out dir by hand after a failed
compile.

bpbuild clears a mod's whole staging folder before it compiles the mod again, and it does not pack a mod whose compile
failed: its `out/<Mod>_P.pak` stays as the last successful build left it. A mod that embeds it is the exception: when
that mod is out of date it is still packed, with whatever the failed compile left in the dependency's staging folder,
which may be none of its packages. Fix the failure before you ship such a pak. The check of cross-mod imports runs
only when nothing failed.

### Messages you will meet first

Each row gives the part of the message to look for, the reason, and what to write instead.
[Diagnostics](REFERENCE.md#diagnostics) has the rest.

| Message | Why | Write instead | See |
| --- | --- | --- | --- |
| `no Kismet conversion from float to double` | In C++, `0.5` is a `double`, and Blueprint has no double. | `X * 0.5f` | [Literals and conversions](REFERENCE.md#literals-and-conversions) |
| `TODO: unimplemented local D: double`, `TODO: unimplemented property Mask: uint32` | A variable, parameter or return needs a Blueprint type. `double`, `int8`, `int16`, `uint16`, `uint32` and `uint64` have none. | `float`, `int32` or `int64` | [Types](REFERENCE.md#types) |
| `call to an unknown function: Helper` | A Blueprint has no free functions. A free function or template without `inline` is not compiled. | `inline int32 Helper(int32 X) { ... }`, or a method of the class | [Inline functions and templates](REFERENCE.md#inline-functions-and-templates) |
| clang: `use of undeclared identifier 'FMath'`, `no member named 'min' in namespace 'std'` | There is no FMath and no standard library. | `UKismetMathLibrary::FClamp(V, 0.0f, 10.0f)`, `UKismetMathLibrary::Min(A, B)` | [Calling engine and game functions](REFERENCE.md#calling-engine-and-game-functions) |
| clang: `only virtual member functions can be marked 'override'` | The SDK declares engine functions non-virtual. The method's name alone makes the override. | `void ReceiveTick(float DeltaSeconds) { ... }` | [Overrides and parent calls](REFERENCE.md#overrides-and-parent-calls) |
| `<Member>: a default is a value known when the mod is built - ...` | A member initializer is read when the mod is built and never runs, so it takes only a value known then. A class reference is not one yet. | Set it in `ReceiveBeginPlay`: `Kind = AActor::StaticClass();` | [Classes and variables](REFERENCE.md#classes-and-variables) |
| `<Class>::UE_DEFAULTS: Charges is declared here - give it an initializer instead` | `UE_DEFAULTS` sets inherited variables and components, not the class's own. | `int32 Charges = 3;` | [Class defaults](REFERENCE.md#class-defaults) |
| `latent call Delay: a function that resumes later returns nothing and takes no non-const reference parameters` | The code after a wait resumes in the event graph, which has no return value and no out parameters. | Return `void`, take parameters by value or `const&` and keep results in member variables | [Latent calls](REFERENCE.md#latent-calls) |
| `static Calls lives in the ubergraph's frame, which only a function that makes a latent call runs in; make Calls a member` | A Blueprint function keeps nothing between calls (the ubergraph is the class's event graph). | A member variable | [Latent calls](REFERENCE.md#latent-calls) |
| ``a delegate cannot bind Handle: an inline function is expanded where it is called, no UFunction (drop `inline`)`` | An inline method is pasted into its callers and is no function of the class. | Drop `inline` from the handler | [Event dispatchers](REFERENCE.md#event-dispatchers) |
| ``TODO: a delegate can only bind a function of `this` `` | Not yet: the delegate binds the object whose code is running. | Bind a method of `this` that calls the other object: `void Forward(int32 P) { Other->Handle(P); }` | [Event dispatchers](REFERENCE.md#event-dispatchers) |
| `a container operation needs a variable, not a computed value: Length` | A container node works on a variable, not on the result of a call. | `TArray<int32> L = GetItems(); return L.Num();` | [Containers](REFERENCE.md#containers) |
| `TODO: unimplemented local Inc: (lambda at ...)` | Lambdas are not compiled, in any form. | An `inline` method or a free `inline` function | [Functions](REFERENCE.md#functions) |
| `warning: ... reference parameter V is bound to a map element: ... V gets a copy, stored back after the call` | Blueprint has no reference to a map element. | Nothing, unless the callee reads the map while it runs: it sees the old value there | [Functions](REFERENCE.md#functions) |
| `warning: <Class>::UE_DEFAULTS: Body is the actor's root, ... so its RelativeScale3D is not applied` | The engine puts the root component at the spawn transform. | Declare `UE_COMPONENT(USceneComponent, Root);` first, so the other components attach to it | [Components](REFERENCE.md#components) |
| `warning: LatentJob::Run waits, and an object of this class finds its world only through its Outer ...` | A plain `UObject` has no world of its own, and a wait needs one. | Create it with an actor or a component as Outer: `NewObject<LatentJob>(this)` from `Objects.h` | [Latent calls](REFERENCE.md#latent-calls) |

### Internal errors and compiler bugs

Some messages point at a bug in AssetGen, not in your mod:

- `FAILED: internal error: <text>`: an exception escaped the compiler, for example on a shape of clang's syntax tree
  that it did not expect.
- a message that starts with `internal:` after its prefix, or one of the other guards that
  [Diagnostics](REFERENCE.md#diagnostics) lists as internal errors. Many of those have no prefix and read like ordinary
  refusals.
- a message from `tools/runscript.py` or `tools/runvm.py` saying that the code would misbehave in the real VM.
- a mod that compiles without a message but fails to load, or does something other than its C++ says, where
  [Limits](#limits) does not explain it.

Report it with the **Compiler bug** issue form of the AssetGen repository. Before you file, build the latest `main`
and try again. The form asks for:

- the AssetGen commit (`git rev-parse --short HEAD`); the SDK commit, or the dump you generated `UeApi` from and
  whether you edited any of it; the game version, for a problem in the game; the first line of `clang++ --version`;
  and the system `assetgen` ran on;
- the exact command, with the mod's `mods.yaml` entry if bpbuild ran it;
- the smallest source that still shows the problem, with any headers of your own that it includes;
- the full output, unedited;
- what you expected and what happened;
- for a problem in the game, the relevant part of the game's log or the crash report's call stack;
- for anything that compiled, the generated `.uasset` and `.uexp`, zipped, and the `_P.pak` if you have it. The
  output of `python tools/walkscript.py <package without extension> <export index>` for the function that misbehaves
  helps too. `python tools/dumpedl.py <file.uasset>` lists every export with its index; see
  [Testing and inspecting](#testing-and-inspecting).

To cut the source down, start from the function that the message's `<Class>::<Function>: ` prefix names, and delete
everything the failure does not need. Until a fix lands, write the statement the message points at in another form.

## Limits

This section collects what AssetGen does not do. [REFERENCE.md](REFERENCE.md) marks each item in its own section; here
they are in one place: the gaps in the compiler, which may close; the helper code that some low-level features need;
and the platforms. What Blueprint, the engine and the game decide, which no compiler can change, is summed up in
[What changes because it is Blueprint](#what-changes-because-it-is-blueprint) and set out in the REFERENCE section of
each topic.

### Not yet

**Refused with a message.** The compile stops, and the section linked says what to write instead.

- Binding a method of another object, `OnHit.Add(Other, &AOther::Handle)` or `{ Other, &AOther::Handle }`. Bind a
  method of `this` that calls the other object. See [Event dispatchers](REFERENCE.md#event-dispatchers).
- Broadcast on a dispatcher that the class did not declare with `UE_DISPATCHER`, such as `OnDestroyed` or a parent
  class's dispatcher. Add, Remove and Clear work on any dispatcher. Give the declaring class a method that broadcasts,
  and call it. See [Event dispatchers](REFERENCE.md#event-dispatchers).
- A class as a default: `TSubclassOf<AActor> Kind = AActor::StaticClass();`, or a class value in a data asset's
  braces. Set it in `ReceiveBeginPlay`, or use a `TSoftClassPtr` with a path. See
  [Classes and variables](REFERENCE.md#classes-and-variables).
- `UE_ENUM_MAP` inside a function body. Keep the table in a member and read the member. See
  [Enums](REFERENCE.md#enums).
- A shift by a named constant, `X << kShift`: only a bare integer literal is accepted as the amount. See
  [Operators](REFERENCE.md#operators).
- The address of a struct member, `&S.Member` or `&Pv->X`. Through a pointer, add the member's offset yourself:
  `(int64)Pv + 4` is the address of `Pv->Y` for an `FVector *Pv`. See
  [Pointers and memory](REFERENCE.md#pointers-and-memory).
- A reference return, `int32 &Slot(int32 I)`. Return a value or a pointer. See [Functions](REFERENCE.md#functions).
- A read through a pointer, `GetOuter()` and `GetTypedOuter` included, in a function that also waits. Read in a
  separate method that does not wait, or keep the value in a member before the wait. See
  [Pointers and memory](REFERENCE.md#pointers-and-memory).
- A one-argument struct constructor in parentheses, `FKey("F5")` or `FFrameNumber(5)`. Write the braces out:
  `FKey{"F5"}`. See [Timers and input](REFERENCE.md#timers-and-input).
- Key and input-action events. `BindKey` and `BindAction` are not in the SDK, so clang rejects them. Poll the player
  controller in `ReceiveTick`, `PC->WasInputKeyJustPressed(FKey{"F5"})`, or bind the player character's action
  dispatchers, such as `OnFirePressed`. See [Timers and input](REFERENCE.md#timers-and-input).
- A type alias declared inside a function, used as a local's type: `using T = int32; T A = X;`. See
  [Locals](REFERENCE.md#locals).
- A game asset's values as build-time constants. The `UeAssets` headers name every asset but hold none of its values.
  Read a value in the game through the asset's pointer, `(&UeAssets::...::ED_Spider_Grunt)->SpawnAmountModifier`. See
  [Game assets](REFERENCE.md#game-assets).

**Compiled with no message, but wrong.** Nothing catches these yet, so avoid them.

- A constructor. Its body is dropped: it makes no function and no default. Use member initializers, `UE_DEFAULTS` and
  `ReceiveBeginPlay`. See [Class defaults](REFERENCE.md#class-defaults).
- A method of a `UE_STRUCT` that is not inline. A Blueprint struct holds no functions, so the call names a function
  that exists nowhere. Write a free `inline` function that takes the struct. See [Functions](REFERENCE.md#functions).
- Two non-inline methods with the same name. A Blueprint class has one function per name: only the overload with the
  most parameters is cooked, and calls to the other are miscompiled. Rename one, or make the extra overloads `inline`.
  See [Functions](REFERENCE.md#functions).
- A method declared and never defined. A call to it names a function the class does not have. See
  [Functions](REFERENCE.md#functions).
- FString methods such as `S.Len()`. Call `UKismetStringLibrary::Len(S)` and the other string library functions. See
  [Strings and text](REFERENCE.md#strings-and-text).
- `Weapons::Turret::StaticClass()` for a mod class in a namespace. It names the engine class that Turret inherits
  `StaticClass` from. Write `Turret::StaticClass()` inside the namespace, or let a `TSubclassOf<Weapons::Turret>`
  parameter, such as SpawnActor's, supply the class. See [Creating objects](REFERENCE.md#creating-objects).
- A mod widget class. It has no designer layout, so it shows nothing of its own. For visible UI, create one of the
  game's widget Blueprints. See [Classes and variables](REFERENCE.md#classes-and-variables).
- An RPC, authority-only or cosmetic marker on an `inline` method. The marker is ignored, and the call runs locally.
  See [RPCs](REFERENCE.md#rpcs).
- An engine or game static marked authority-only or cosmetic, such as `UGameplayStatics::ApplyDamage` or
  `UGameplayStatics::PlaySound2D`. The editor's node skips an authority-only function on a client and a cosmetic one
  on a dedicated server; AssetGen's call runs both on every machine. Guard such a call with `HasAuthority()` or
  `UKismetSystemLibrary::IsServer(this)`. See [Working with other objects](REFERENCE.md#working-with-other-objects).
- `UKismetSystemLibrary::Delay(1.0f, Info)`. Passing the `FLatentActionInfo` without the world context compiles into
  a broken call. Leave the `FLatentActionInfo` out: the compiler supplies it. See
  [Latent calls](REFERENCE.md#latent-calls).
- `UE_AWAIT` on a dispatcher with two or more parameters. The method resumes, but the values are not available. Bind
  a handler to read them. See [Waiting on events](REFERENCE.md#waiting-on-events).

A few mistakes also compile without a message, because the construct itself works:

- An interface function implemented as an `inline` method. The class gets the interface's empty function instead. See
  [Interfaces](REFERENCE.md#interfaces).
- An interface implementation whose parameters differ from the interface's. Functions are matched by name only, so
  copy the signature from the header. See [Interfaces](REFERENCE.md#interfaces).
- `Items.Remove(2)` on a `TArray`. It is the Remove Index node; `RemoveItem(2)` removes the elements equal to 2. See
  [Containers](REFERENCE.md#containers).
- `UKismetSystemLibrary::K2_SetTimer(this, "Poll", ...)` naming an inline method, a method that takes parameters or a
  misspelled name. The engine sets no timer. Prefer `K2_SetTimerDelegate({ this, &AMine::Poll }, ...)`, which clang
  and AssetGen check. See [Timers and input](REFERENCE.md#timers-and-input).
- A `UE_CLASS` path that leaves out the class's namespace folder. The owning mod imports the class instead of cooking
  it. bpbuild reports the import that no mod produces; a lone `assetgen compile` does not. See
  [Mod sources and packages](REFERENCE.md#mod-sources-and-packages).

**Missing, with nothing to write yet.**

- Editor stubs for the mod's interfaces. A Blueprint made in the editor cannot implement an interface that the mod
  declares; a C++ class can. See [Editor API stubs](#editor-api-stubs).
- A check of a game component class's own native data. A `UE_COMPONENT` of one of the game's component classes gets
  the native data of its engine ancestors, which matches every game component class that appears as a template in the
  game's content. 177 never appear there, mostly abstract bases, objectives and character states; one of those that
  serialized more data would fail to load *(inferred)*, and nothing checks for this. See
  [Components](REFERENCE.md#components).
- Keywords and a ToolTip for a function in the editor stub. `UeMeta.h` has no macro for them. See
  [Classes and variables](REFERENCE.md#classes-and-variables).
- Changing a game data asset, or a game Blueprint's defaults, in place in the pak. Change the loaded asset through its
  pointer in the game, or subclass the Blueprint and set its defaults in `UE_DEFAULTS`. See
  [Game assets](REFERENCE.md#game-assets).
- The form of a game enum that no property uses. genueapi learns whether a game enum is an `enum class` from how the
  dump's properties of it are reflected, and 249 of the SDK's 1445 enums have none; a variable of one is a Byte, which
  differs from the editor's Enum variable only in type, and only if the enum is an `enum class`. See
  [Enums](REFERENCE.md#enums).
- Walking a `TSet` in place. A range-for over a `TSet` walks a copy, while a `TMap` walks its own slots. Only the cost
  differs. See [Loops](REFERENCE.md#loops).
- Reusing a repeated pure call. It is evaluated each time it appears. To compute it once, keep the result in a local.
  See [The optimizer](REFERENCE.md#the-optimizer).
- Game members the SDK generator cannot map. genueapi leaves out every function and property with a type it has no
  mapping for yet, and a mod cannot name what the headers do not declare. The published SDK leaves out 539 of them,
  and its `UeApi.h` records the count by kind. See [The SDK](#the-sdk).

### Code you supply

Three low-level features call code that AssetGen neither generates nor ships:

- `&Obj->Member`, and a reference local bound to another object's member, call `GetPropertyAddress` on a class named
  `ReadProperty`, because a cooked property carries no offset.
- An `FText` read or written through a pointer goes through a struct at the fixed path
  `/Game/_ElytrasMods/ReadProperty/FDerefTextView`.
- `__ClassOf__` calls a `GetParmClassName` method on the class that uses it.

Apart from refusing `&Obj->Member` when no class named `ReadProperty` is in scope, the build does not check that this
code exists or ships with your mod. [Pointers and memory](REFERENCE.md#pointers-and-memory) shows the helper mod to
write once and the method to paste.

### Platforms

- **Windows and Linux.** `assetgen` builds and runs on both. One comparison found their output byte-identical, and CI
  checks behaviour on both systems, not bytes; [Reproducible output](#reproducible-output) says what that comparison
  covered. A different `clang++` version parsing the mod has not been compared.
- **Standard headers.** On Linux, clang still parses a mod for the game's target, `x86_64-pc-windows-msvc`, so
  `sizeof`, `long` and `wchar_t` mean what they mean in the game. That target has no C++ standard library on Linux:
  `<initializer_list>`, which the SDK needs and AssetGen supplies, is the only standard header a mod can include
  there. On any system, a call into the standard library is refused, even with its header included. Use clang builtins
  such as `__is_same(A, B)` in `if constexpr` or `static_assert` in place of `<type_traits>`, and `UKismetMathLibrary`
  for math. See
  [Calling engine and game functions](REFERENCE.md#calling-engine-and-game-functions).
- **Packing on Linux.** bpbuild needs UE 4.27's UnrealPak: a native Linux build on `PATH`, or the Windows
  `UnrealPak.exe` under Wine, named by the `UNREALPAK` variable as the README shows. See
  [Building mods](#building-mods).
- **The game under Proton.** It reads paks from the same folders as on Windows, under
  `steamapps/common/Deep Rock Galactic/FSD/`.
