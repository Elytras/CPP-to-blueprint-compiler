# AssetGen language reference

This reference covers every C++ construct that AssetGen compiles, warns about or refuses, and says what each one
becomes in the cooked Blueprint. [GUIDE.md](GUIDE.md) walks through writing, building and running a mod, and
[examples/](examples/) holds complete mods that build as they are (`python tools/bpbuild.py examples <UeApi dir>
<assetgen>` builds them all). Come here to look a construct up. Find a construct
by topic in the table below, a macro or intrinsic by name in [Macro and intrinsic index](#macro-and-intrinsic-index),
and a compiler message in [Diagnostics](#diagnostics).

## Contents

| Topic | Sections |
|---|---|
| Declarations | [Mod sources and packages](#mod-sources-and-packages), [Classes and variables](#classes-and-variables), [Global variables](#global-variables) |
| Types | [Types](#types), [Literals and conversions](#literals-and-conversions), [Strings and text](#strings-and-text), [Constants](#constants), [Enums](#enums), [Structs](#structs), [Containers](#containers) |
| Code | [Operators](#operators), [Statements and control flow](#statements-and-control-flow), [Loops](#loops), [Locals](#locals), [The optimizer](#the-optimizer) |
| Functions | [Functions](#functions), [Calling engine and game functions](#calling-engine-and-game-functions), [Overrides and parent calls](#overrides-and-parent-calls), [Inline functions and templates](#inline-functions-and-templates) |
| Objects | [Components](#components), [Class defaults](#class-defaults), [Creating objects](#creating-objects), [Working with other objects](#working-with-other-objects), [Data assets](#data-assets), [Game assets](#game-assets) |
| Events and waiting | [Event dispatchers](#event-dispatchers), [Latent calls](#latent-calls), [Waiting on events](#waiting-on-events), [Timers and input](#timers-and-input), [Interfaces](#interfaces) |
| Network | [Replication](#replication), [RPCs](#rpcs) |
| Low level | [Pointers and memory](#pointers-and-memory), [Intrinsics](#intrinsics) |
| Lookup | [Macro and intrinsic index](#macro-and-intrinsic-index), [Diagnostics](#diagnostics) |

## Reading this reference

Most sections open with what they cover and the one rule most worth remembering. Tables follow, one row per construct:
what you write, what it does, and its status. Snippets and notes come after the tables.

### Status

Some C++ has no exact Blueprint equivalent. AssetGen then compiles the nearest equivalent and prints a `warning:` that
says what differs. It refuses only what it cannot compile faithfully.

| Status | What it means |
|---|---|
| Yes | AssetGen compiles it. The row says what it becomes, and where Blueprint behaves differently from C++, it says how. |
| Warns | AssetGen compiles the nearest Blueprint equivalent and prints a line that starts `warning:`, names the class and function, and says what differs. The build succeeds. |
| Refused | AssetGen cannot compile it faithfully, so the compile stops with `FAILED: <message>` and exit code 1. "Refused by clang" means clang rejects the source first: its errors come first, and AssetGen ends with `FAILED: clang rejected <source> (diagnostics above)`. The row gives the message and what to write instead. |
| Not yet | A gap in the compiler today. Most such constructs are refused with a message, and many of those messages start with `TODO:`. A few are not caught: they compile with no diagnostic into code that does not do what the C++ says, and the row says so. Each row gives a way around it. |

[Warnings, refusals and compiler bugs](GUIDE.md#warnings-refusals-and-compiler-bugs) shows a warning and a refusal
as the compiler prints them, and [Diagnostics](#diagnostics) lists every message.

### Snippets

Every mod source starts with the same lines. `UeApi/Types.h` holds the integer spellings, the string types and the
container templates. `UeApi/FSD.h` declares the game's classes and includes the engine's headers (`Engine.h`, `UMG.h`
and the rest), so it is the only SDK header most snippets need. `UE_MOD_PACKAGE` names the `/Game` folder the source is
cooked into.

```cpp
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_MyMods/MyMod");

class MyMod : public AActor {
public:
  // the snippet's members go here
};
```

A snippet leaves these lines out. Unless it shows its own `class`, it is the members of a class like `MyMod`, which
derives from `AActor` and starts with `public:`. A snippet that declares classes, structs, enums, assets or variables
at namespace scope shows them as they sit in the file, after the `UE_MOD_PACKAGE` line. `/Game/_MyMods/...` is an
example path; use your own.

Some snippets need one more include, and show it:

- `Objects.h` (`SpawnActor`, `NewObject`, `AddComponentByType` and the other helpers) and `Intrin.h` (the `__Name__`
  intrinsics) are in AssetGen's `include/` folder, not in the SDK. Include them by a path that reaches that folder from
  your source, as the snippets do with `#include "../include/Objects.h"`, or copy them beside your source.
- A game Blueprint class needs its header from `UeApi/Game/`, such as `#include "UeApi/Game/WPN_GrapplingGun_C.h"`,
  and a game asset named by path needs its `UeAssets/` header, such as `#include "UeAssets/USoundWave.h"`.

A comment in a snippet marks a line that warns or is refused, or says what a line becomes.

### Blueprint terms

The rows name each construct's Blueprint equivalent as the editor names it: a node (Cast To, Make Array, Bind Event),
a panel (the Components panel, Class Defaults) or a setting (Replicates: Run on Server). The code AssetGen writes
behaves as that node or setting does in a Blueprint made in the editor, and a row says so where it differs. Some C++
has no editor counterpart at all, such as a variable at namespace scope or a raw pointer; its section says what
AssetGen writes instead.

## Mod sources and packages

A mod source is one `.cpp` file. Its `UE_MOD_PACKAGE` names the `/Game` folder the source is cooked into, and every
class, `UE_STRUCT`, interface, `UE_ENUM` and braced asset in the source becomes one package in that folder. A namespace
is a subfolder. The rule to remember: a class declared in a header that several mods include is cooked by one mod only,
the one its `UE_CLASS` names. The others import it.

### The mod package

| You write | What it does | Status |
|---|---|---|
| `UE_MOD_PACKAGE("/Game/_MyMods/Hello");` | Names the `/Game` folder for everything the source cooks. Write it once, at file scope. It is the only place the path is written: the bpbuild manifest does not repeat it. | Yes |
| a source with no `UE_MOD_PACKAGE` | Refused: "the source declares no UE_MOD_PACKAGE". | Refused |
| a second `UE_MOD_PACKAGE` in one source | Refused by clang: "redefinition of 'UeModPackage'". The macro declares a variable. | Refused |
| a source with nothing to cook | Refused: "the source declares no UE_STRUCT, UE_ENUM or class deriving from a UE class". A class with no base does not count. | Refused |
| `class Hello : public AActor {};` | One package per class, named after it: `/Game/_MyMods/Hello/Hello`. The class inside is `Hello_C`, as the editor names a Blueprint's generated class, and the asset registry lists `/Game/_MyMods/Hello/Hello.Hello_C`. The name is the C++ name as written, prefix included: `UTurretDef` becomes `UTurretDef_C`. | Yes |
| several classes in one source | Each class is its own package in the mod folder. The classes can name each other's types and share [global variables](#global-variables). | Yes |
| two declarations that land on one package | Refused, naming both: "would both be cooked as". Package names are compared ignoring case. | Refused |

```cpp
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_MyMods/Hello");

class Hello : public AActor {       // /Game/_MyMods/Hello/Hello, class Hello_C
public:
  int32 Count = 0;
};

class HelloPeer : public AActor {   // /Game/_MyMods/Hello/HelloPeer
public:
  Hello *Other;
  int32 Read() { return Other->Count; }
};

class Helper { public: int X; };    // no base: nothing is cooked
```

Notes:

- A package in the mod folder is written under the out dir, subfolders included. A package outside it, such as a class
  in a `Game::` namespace (below), is written under the `Content` folder that the out dir sits in. So compile into
  `<root>/Content/<package path without /Game>`, as bpbuild does. See [Building mods](GUIDE.md#building-mods).
- The wrapper structs for a container inside a container are the one kind of package written outside the mod folder
  whatever `UE_MOD_PACKAGE` says. See [Containers](#containers).

### Namespaces and folders

| You write | What it does | Status |
|---|---|---|
| `namespace Weapons { class Rifle : public AActor {}; }` | A namespace is a subfolder. `Rifle` is cooked at `/Game/_MyMods/Hello/Weapons/Rifle`, and child classes, calls, variable types and the registry all use that path. The same holds for a `UE_STRUCT`, an interface, a `UE_ENUM` and a braced asset. | Yes |
| `Rifle` inside `namespace Weapons`, `Weapons::Rifle` outside it | Both name the same class, as in C++. | Yes |
| `namespace Game::WeaponsNTools::GrapplingGun { ... }` | A namespace that starts at `Game` is an absolute `/Game` path. The class leaves the mod folder and is cooked at `/Game/WeaponsNTools/GrapplingGun/<Class>`. The SDK declares each game Blueprint in the namespace of its folder, so a class that extends one can sit beside it. | Yes |
| `namespace Tally { int32 Hits; }` | A global variable in a namespace gets no subfolder. See [Global variables](#global-variables). | Yes |
| `namespace Game::_MyMods::Hello { class Hello : public AActor {}; }` beside a plain `class Hello` | Both land on `/Game/_MyMods/Hello/Hello`. Refused: "would both be cooked as". | Refused |
| `class Pistol : public Rifle` in `namespace Weapons`, when another namespace also declares a `Rifle` | Refused: "derives from an undeclared class: Rifle". Write `Weapons::Rifle`. | Refused |

```cpp
namespace Weapons {
class Rifle : public AActor {};        // /Game/_MyMods/Hello/Weapons/Rifle
class Pistol : public Rifle {};        // the short name inside the namespace
}

class Holster : public AActor {
public:
  Weapons::Rifle *Held;                // the qualified name outside it
};
```

```cpp
#include "UeApi/Game/WPN_GrapplingGun_C.h"

namespace Game::WeaponsNTools::GrapplingGun {
class WPN_GrapplingGun_Long : public WPN_GrapplingGun_C {   // /Game/WeaponsNTools/GrapplingGun/WPN_GrapplingGun_Long
  int32 Pulls = 0;
};
}
```

Notes:

- When two namespaces each declare a class of the same name, write the qualified name everywhere, also inside the
  class's own namespace. The short name is refused as a base class. As a variable's type it compiles with no warning,
  but the variable becomes an address (an Integer64 variable), not an object reference.

### Sharing a class between mods: UE_CLASS

| You write | What it does | Status |
|---|---|---|
| `UE_CLASS("/Game/_MyMods/Turrets/UTurretDef", "UTurretDef_C");` in a class of a shared header | Records the class's package and its UE name. A source cooks the class only when that package is exactly the path the source would give it: its `UE_MOD_PACKAGE`, then any namespace, then the class name. Every other source that includes the header imports the class and cooks no copy. | Yes |
| `UE_CLASS_IN("/Game/_MyMods/Turrets");` in UTurretDef | The same as the row above, the class written once: it names the owning mod's `UE_MOD_PACKAGE`, and the compiler appends the namespaces and class name for the package and `_C` for the UE name. Use it for a mod's own classes; `UE_CLASS` stays for an engine class or a name that differs from the C++ one. | Yes |
| `UE_CLASS("/Game/_MyMods/Turrets", "UTurretDef_C");` | The package names the folder, not the class's own asset. Refused: "does not end in the asset that declares UTurretDef_C", with the path it should be. | Refused |
| `UE_CLASS("/Game/_MyMods/Turrets/UTurretDef", "UTurretDef");` in the mod that cooks it | A cooked Blueprint class is named `<Class>_C`. Refused: "cooking it here requires", with the right name. | Refused |
| `UE_CLASS("/Script/Engine", "KismetSystemLibrary");` | Names an engine class: a `/Script` package and the native name, with no `_C`. The class is imported, never cooked, and needs no base. The SDK headers declare every class this way. Write one by hand for an engine class or function the SDK leaves out. | Yes |
| `UE_STRUCT_IN("/Game/_MyMods/Turrets");`, `UE_ENUM_IN(EMood, "/Game/_MyMods/Turrets");` | The same one-owner rule for a struct and an enum. These name the owning mod's `UE_MOD_PACKAGE`, not the type's own package. See [Structs](#structs) and [Enums](#enums). | Yes |
| `T->Fire(2);`, `T->OnFired.Add(this, &Gunner::OnHeard);` on a `Turret *T`, where a shared header declares `class Turret : public AActor { public: UE_CLASS("/Game/_MyMods/Turrets/Turret", "Turret_C"); int32 Shots = 0; int32 Fire(int32 N); UE_DISPATCHER(OnFired, int32 Total); };` | Calls and binds another mod's actor class. The header declares the methods, the owner's source defines them (`int32 Turret::Fire(int32 N) { ... }`), and a user finds an instance as it would a game actor, for example with `UGameplayStatics::GetAllActorsOfClass(Turret::StaticClass(), Found)`, then `Cast<Turret>`. Compiled, not run in game. | Yes |
| `&OtherData`, where `OtherData` is another mod's asset of a class that a shared header declares without `UE_CLASS` | Refused: the reference would load as null. The message names the `UE_CLASS` line that fixes it. See [Game assets](#game-assets). | Refused |

```cpp
// Turrets.h, included by both mods
class UTurretDef : public UPrimaryDataAsset {
public:
  UE_CLASS("/Game/_MyMods/Turrets/UTurretDef", "UTurretDef_C");
  float Range = 800.0f;
};
```

```cpp
// Turrets.cpp: cooks UTurretDef as /Game/_MyMods/Turrets/UTurretDef
#include "Turrets.h"
UE_MOD_PACKAGE("/Game/_MyMods/Turrets");
```

```cpp
// Picker.cpp: imports UTurretDef from the Turrets mod
#include "Turrets.h"
UE_MOD_PACKAGE("/Game/_MyMods/Picker");

class Picker : public AActor {
public:
  UTurretDef *Def;
  float RangeOf() { return Def->Range; }
};
```

An engine class declared by hand. `UeApi` already has this one as `UKismetSystemLibrary`; the form is the same for a
class it lacks:

```cpp
class KismetSystemLibrary {
public:
  UE_CLASS("/Script/Engine", "KismetSystemLibrary");
  static bool IsServer(class UObject *WorldContextObject);
};
// in a class: bool OnHost() { return KismetSystemLibrary::IsServer(this); }
```

Notes:

- Without `UE_CLASS`, each mod that includes the header cooks its own copy of the class, and a reference from one mod
  into another mod's copy loads as null in game. The compiler catches this only for a reference to an asset (the last
  row above).
- Put a hand-written declaration under `public:`. A class starts private, and clang refuses a call to a private static.
- The same pattern serves a library's static functions ([examples/MathLib.h](examples/MathLib.h)) and an actor's
  methods and dispatchers (the `Turret` row above): the shared header declares, the owner's source defines.
- The folder check compares only the last part of the package with the class name. A folder that happens to have the
  class's name, such as `UE_CLASS("/Game/_MyMods/Lib", "Lib_C")` for a class cooked at `/Game/_MyMods/Lib/Lib`, is not
  caught: it compiles, and the package it names does not exist.
- The owning mod's pak has to be loaded too. List the owner under `needs` in the using mod's `mods.yaml` entry. Then
  ship both paks, or set `embed: true` so that the using mod's pak carries the owner's assets. See
  [Several mods and shared code](GUIDE.md#several-mods-and-shared-code), [examples/MathLib.h](examples/MathLib.h) and
  [examples/LibraryUser.cpp](examples/LibraryUser.cpp).

## Classes and variables

A class that derives from a UE class is a Blueprint class. Each of its non-static data members is a Blueprint variable,
and the member's initializer is the variable's default in Class Defaults. The rule to remember: a default is data
written when the mod is built, so a value the game computes belongs in `ReceiveBeginPlay`. Mind that a C++ `class`
starts private.

### Blueprint classes

| You write | What it does | Status |
|---|---|---|
| `class Hello : public AActor { ... };` | A Blueprint class (a BlueprintGeneratedClass) whose parent is `AActor`, as if made in the editor with that parent. | Yes |
| `class Helper { ... };` | A class with no base is plain C++. Nothing is cooked for it. | Yes |
| `class X : public ANotIncluded {};` | Refused by clang, "expected class name", when no included header declares the base. Include the SDK header that declares it. | Refused |
| `class InitCave : public Hello {};` | A child of another class of the mod. A parent in the same source is used from there. A parent pinned with `UE_CLASS` to another mod is imported from that mod. | Yes |
| `class Turret final : public AActor { ... };` | A class with no subclass. Its functions, overrides and interface implementations aside, are cooked Final, which the editor does not let a Blueprint override, and a call to one of them reaches that function directly instead of by name; on `this` the body is usually copied in. See [Calling your own functions](#calling-your-own-functions). | Yes |
| `UE_FINAL_AS(UTurretBase, Turret);` at namespace scope, after UTurretBase | Final through one leaf: declares `class Turret final : public UTurretBase {}`, the class that is made, in the namespace it is written in (its package path, as for any class). UTurretBase's own code is then compiled as if it were final: its own functions, overrides and interface implementations aside, are cooked Final, and its calls on `this` to them are direct and usually copied in. A call to one of its overrides, or to a function it inherits, goes by name, as the editor calls a function that is not Final; with Turret the only class made, it reaches the same function. UTurretBase is cooked Abstract, and any other class deriving from it is refused, so put the macro in the header beside UTurretBase: a mod that includes the header is refused a subclass too. When UTurretBase has a `UE_CLASS` (a header mods share), Turret is pinned beside it, so only the mod that owns UTurretBase cooks Turret and every other mod imports it, and a second UE_FINAL_AS on the same base is refused ("has two UE_FINAL_AS leaves"). Such a base is final only through the UE_FINAL_AS in the header that declares it, which its owner sees too: one written in another mod's own source, its `.cpp` or a header of its own that re-declares the base, is refused ("only the UE_FINAL_AS in the header that declares it ..."), since its owner would never cook that leaf. The header counts as the owner's when a source beside it includes it whose `UE_MOD_PACKAGE` is the owner's, or that has none in a folder whose only `UE_MOD_PACKAGE` is the owner's (a mod of several sources). A game Blueprint base is refused ("is the game's Blueprint ..."): no mod cooks it, so it stays as the game has it. A base left with a `= 0` method is refused ("would be abstract"): Turret brings no method of its own, so it would be abstract too and nothing could be made. | Yes |
| `class WPN_GrapplingGun_Long : public WPN_GrapplingGun_C` | A child of one of the game's Blueprint classes. Include the parent's `UeApi/Game/` header and derive from it; the parent is imported from the game. Put the child in the parent's `Game::` namespace to cook it beside the parent (see [Mod sources and packages](#mod-sources-and-packages)). The child loads after every default subobject the parent's default object exports, which UeApi lists only when genueapi had `--game`: against a UeApi made without it, the class is refused, saying to regenerate with `--game`. | Yes |

Notes:

- The compiler cooks any class that has a base, not only one with a UE base. A class that derives from a plain C++
  class compiles with no warning, but its parent is a Blueprint that is never written. Derive from a UE class, or from
  a mod class that has one.
- An unqualified call reaches the most-derived override, and `Base::Method()` runs the parent's body. See
  [Overrides and parent calls](#overrides-and-parent-calls) and
  [examples/GameBlueprintChild.cpp](examples/GameBlueprintChild.cpp).
- A child changes an inherited default, including one of a component that a game Blueprint parent adds, in
  `UE_DEFAULTS`. See [Class defaults](#class-defaults).
- The game starts a mod through its classes named `InitCave` and `InitSpacerig`. See
  [Running in the game](GUIDE.md#running-in-the-game) and [examples/HelloWorld.cpp](examples/HelloWorld.cpp).

### Parent classes

| You write | What it does | Status |
|---|---|---|
| `: public AActor`, or any actor class | An actor Blueprint. Only a class whose parents reach `AActor` has a construction script, and so components. | Yes |
| `: public UObject` | A plain object Blueprint. Create one at run time; see [Creating objects](#creating-objects). | Yes |
| `: public UBlueprintFunctionLibrary` | A Blueprint Function Library, a class of static functions. See [Functions](#functions). | Yes |
| `: public UPrimaryDataAsset`, `: public UDataAsset` | A data asset class. Its instances are braced variables at namespace scope; see [Data assets](#data-assets). | Yes |
| `: public USaveGame`, `: public UFSDSaveGame` | A save game class. | Yes |
| `: public UActorComponent`, `: public USceneComponent` | A component Blueprint, with the component class flags. Add one to an actor at run time with `AddComponentByType<T>(Owner)`, the editor's Add Component by Class node. Its own events to override are `ReceiveBeginPlay`, `ReceiveEndPlay` and `ReceiveTick`. It cannot be a `UE_COMPONENT` of a mod actor, which takes engine component classes only; see [Components](#components). | Yes |
| `void ReceiveTick(float DeltaSeconds)` in a component class | Turns the component's ticking on: it sets `PrimaryComponentTick.bCanEverTick` on the class default object, as the editor's compiler does. UActorComponent leaves it off. | Yes |
| `: public UUserWidget` | A widget with logic and no layout. `CreateWidget<T>` makes one. Its events, such as `Construct`, are overrides. `Tick` runs only if `bHasScriptImplementedTick` is set in `UE_DEFAULTS`, which the editor's widget compiler would do. | Yes |
| a designer layout (a widget tree) for a mod widget | Not yet, and nothing warns. A mod widget has no widget tree, so it shows nothing of its own. For visible UI, create one of the game's widget Blueprints. | Not yet |

```cpp
class Turret : public AActor { public: int32 Shots = 0; };
class TurretLib : public UBlueprintFunctionLibrary {
public:
  static int32 Twice(int32 X) { return X * 2; }
};
class UTurretDef : public UPrimaryDataAsset { public: float Range = 800.0f; };
class UTurretTable : public UDataAsset { public: int32 Rows = 4; };
class UTurretSave : public USaveGame { public: int32 Kills = 0; };
class UTurretJob : public UObject { public: int32 Done = 0; };
```

A component class and the actor that adds it:

```cpp
class UCharges : public UActorComponent {
public:
  int32 Left = 3;
  void ReceiveBeginPlay() { Left = 5; }
};

class Turret : public AActor {
public:
  UCharges *Charges;
  void ReceiveBeginPlay() { Charges = AddComponentByType<UCharges>(this); }
};
```

A widget class and the actor that shows it:

```cpp
class UScoreWidget : public UUserWidget {
  UE_DEFAULTS { bHasScriptImplementedTick = true; }   // needed only if you override Tick
public:
  int32 Shown = 0;
  float Elapsed = 0;
  void Construct() { Shown += 1; }
  void Tick(FGeometry MyGeometry, float InDeltaTime) { Elapsed += InDeltaTime; }
};

class Hud : public AActor {
public:
  void ReceiveBeginPlay() {
    UScoreWidget *W = CreateWidget<UScoreWidget>(UGameplayStatics::GetPlayerController(this, 0),
                                                 UScoreWidget::StaticClass());
    if (W) W->AddToViewport(0);
  }
};
```

Notes:

- `AddComponentByType` and `CreateWidget` come from `Objects.h` in AssetGen's `include/` folder, which is not part of
  `UeApi`. The compiler gives clang two include paths, the `UeApi` folder and the folder that holds it, so include
  `Objects.h` by its path from your source (the examples write `#include "../include/Objects.h"`) or from that folder,
  or copy it beside your source. A bare `#include "Objects.h"` finds it only there.
- The class takes its parent's class flags, ClassWithin and config name, as the editor copies them: an `AHUD` child
  reads `Game.ini`, a `UCheatManager` child is within PlayerController, a `UStatusEffect` child is `EditInlineNew`.
  UeApi states them per class (`UeClassTail`): a native class's from what the game's Blueprint children of it carry and
  UE 4.27's UCLASS specifiers, a game Blueprint's read off its package. A native parent neither describes gets the
  flags of the kind of parent (actor, actor component, function library, anything else), within Object, config
  Engine; so does every parent with a UeApi generated before the markers existed.
- A class that is not an actor has no construction script, so `UE_COMPONENT` in it is refused: "only an actor has a
  construction script". See [Components](#components).
- A latent call such as `Delay` finds its world by itself in an actor, actor component, user widget, game instance or
  subsystem class. In any other class the compiler warns, because the object finds its world only through its Outer.
  See [Latent calls](#latent-calls).
- The component and widget behaviour above follows from the engine's source. No mod component or widget has run yet.

### Member variables

| You write | What it does | Status |
|---|---|---|
| `int32 Health = 100;` | A Blueprint variable of the class. Blueprints get and set it, and it is editable in Class Defaults but not on a placed instance. | Yes |
| `bool`, `uint8`, `int32`, `int64`, `float`, `FName`, `FString`, `FText` | Blueprint's Boolean, Byte, Integer, Integer64, Float, Name, String and Text variables. | Yes |
| `EMood Mood;` | An enum variable. A `uint8` enum is cooked as a Byte of that enum, except a game `enum class`, which is an enum property over a byte as in the editor; an `int32` or `int64` enum is an enum property over that integer. See [Enums](#enums). | Yes |
| `AActor *Target;`, `UClass *Cls;`, `TSubclassOf<AActor> Kind;` | Object and class references. See [Types](#types). | Yes |
| `USceneComponent *Spare;`, `TArray<UStaticMeshComponent *> Pieces;`, `FHitResult LastHit;` | A reference to a component or widget, or a container or struct that holds one, is flagged instanced, as the editor flags it, so an actor spawned from the class gets its own copy of what the class default points at, not the default's. | Yes |
| `TSoftObjectPtr<T>`, `TSoftClassPtr<T>`, `TScriptInterface<I>` | Soft object and soft class references, and an interface reference. | Yes |
| `TEnum<EMood>` | The enum itself, plus `.Name()` and `.String()`. See [TEnum](#tenum). | Yes |
| `FVector Home;`, `FAmmo Ammo;` | An engine or game struct, or a mod `UE_STRUCT`. See [Structs](#structs). | Yes |
| `TArray<FVector>`, `TSet<int32>`, `TMap<FName, int32>` | Array, Set and Map variables. A container inside a container goes through a generated wrapper struct; see [Containers](#containers). | Yes |
| `TSet<bool>`, `TMap<FText, int32>`, `TSet<FRotator>` | Refused: "cannot hash, and the engine hashes each one". A set element or map key must hash: not a bool, an FText or a delegate, and an engine struct only if it has a GetTypeHash (FVector, FGuid, FGameplayTag and others do; FRotator, FHitResult, FTransform do not). A `UE_STRUCT` always hashes. | Refused |
| `UE_DISPATCHER(OnScored, int32 Score);` | An event dispatcher. See [Event dispatchers](#event-dispatchers). | Yes |
| `int32 *Raw;` | A pointer to anything but a UObject is an address, held in an Integer64 variable. See [Pointers and memory](#pointers-and-memory). | Yes |
| `uint16 W;`, `double D;` | Refused: "unimplemented property W: uint16". UE 4.27 Blueprint has no such type; see [Types](#types). | Refused |
| a member whose type is a game class the source only forward-declares | Refused, with the `#include "UeApi/Game/<Class>.h"` line to add. | Refused |
| `static constexpr int32 kSlots = 12;`, `static inline const TArray<int32> kPrimes = {2, 3, 5};` | No variable is cooked. Each use is the value, and a braced list is built where it is used. See [Constants](#constants). | Yes |
| `static inline float Loose = 0.25f;` | A static that is not const is refused where it is used: a Blueprint class has no static storage. See [Constants](#constants). | Refused |
| `const int32 Limit = 3;` | A read-only variable. See Access, read-only and categories below. | Yes |

```cpp
enum class EMood : uint8 { Calm, Angry };
UE_ENUM(EMood);
struct FAmmo { UE_STRUCT; int32 Count; };

class Loadout : public AActor {
public:
  bool bArmed = true;
  uint8 Charges = 3;
  int32 Health = 100;
  int64 Stamp;
  float Ratio = 0.5f;
  FName Tag = "alpha";
  FString Label;
  FText Caption;
  EMood Mood;                          // a UE_ENUM, or one of the game's enums
  AActor *Target;
  TSubclassOf<AActor> Kind;
  TSoftObjectPtr<UTexture2D> Icon;
  FVector Home;
  FAmmo Ammo;                          // a UE_STRUCT
  TArray<FVector> Points;
  TMap<FName, int32> Counts;
  UE_DISPATCHER(OnScored, int32 Score);
  int32 *Raw;                          // an address, not an object
};
```

### Defaults

| You write | What it does | Status |
|---|---|---|
| `int32 Budget = kSlots * 2 + 1;` | The compiler works the initializer out when the mod is built and writes it into the class default object: the variable's default value in Class Defaults. Nothing runs in game to set it. | Yes |
| `int32 Hits = 0;`, `int32 Hits;` | A zero default writes nothing, as the engine cooks it, and the variable starts at zero. The same holds for the zero enumerator, `nullptr` and an empty string. | Yes |
| `int32 Hits{};`, `EMood Mood = EMood();`, `int32 Budget{25};` | `{}` and `T()` are the zero, and write nothing; braces around one value are that value. | Yes |
| `int32 X = UKismetMathLibrary::RandomInteger(5);` | Refused: "a default is a value known when the mod is built". Set such a value in `ReceiveBeginPlay` or `UserConstructionScript`. | Refused |
| `static constexpr int32 kSeed = Fnv("types");` then `int32 Seed = kSeed;` | A call counts as a default only through a `consteval` function, which clang runs itself. A call to a plain `constexpr` function is refused like any call. See [Constants](#constants). | Yes |
| `TSubclassOf<AActor> Kind = AActor::StaticClass();` | Not yet. A class reference has no build-time value, so a `TSubclassOf` or `UClass*` member always starts null, and this initializer is refused with the build-time message. `= nullptr` compiles. Set the class in `ReceiveBeginPlay`, or use a `TSoftClassPtr` member, which takes a path. | Not yet |
| `TSoftClassPtr<AActor> Soft = "/Game/A/BP_A.BP_A_C";` | A soft reference's default is its path. `"/Game/Dir/Pkg"` means `Pkg.Pkg`, so write the full path for a Blueprint class. See [Types](#types). | Yes |
| `TArray<uint8> Payload = __EmbedFile__("data/blob.bin");` | Reads the file when the mod is built, from a path relative to the source's folder, and writes its bytes into the default, one element per byte. The compile prints `embed          -> Payload  (3 bytes)`. The file is not needed at run time. `__EmbedFile__` comes from `Intrin.h` in AssetGen's `include/` folder, included by its path as `Objects.h` is. | Yes |
| `__EmbedFile__("missing.bin")`, `__EmbedFile__(kPath)` | Refused: a missing or empty file ("cannot read (or empty)"), and a path that is not a string literal ("the path must be a string literal"). It works only as the initializer of a `TArray<uint8>` member; clang refuses it for any other array type. | Refused |
| `UE_ENUM_MAP(EMood, FName, Names);` | Declares a `TMap<EMood, FName>` member holding a name table that the compiler fills in when the mod is built, as a member default only. See [Enums](#enums). | Yes |
| `AMyActor() { Charges = 5; }` | A constructor is dropped with no message. See [Class defaults](#class-defaults). | Not yet |

```cpp
// in a class; EMood is a UE_ENUM
static constexpr int32 kSlots = 12;
int32 Budget = kSlots * 2 + 1;            // 25
int32 Bits = ~(1 << 4 | 3) & 0xFF;        // 236
float Reach = 1.5f;
FString Greeting = "hi";
FText Label = "label";
EMood Mood = EMood::Angry;                // written by name
FVector Home = { 1, 2, 3 };
FVector Away = FVector(4, 5, 6);
TArray<int32> Scores = { 1, 2, 3 };
TMap<FName, int32> Ranks = { { "a", 1 }, { "b", 2 } };
TSoftObjectPtr<UTexture2D> Icon = "/Game/UI/Icons/T_Icon";
AActor *Target = nullptr;                 // writes nothing
int32 Hits = 0;                           // writes nothing
```

Notes:

- An initializer can be a literal; a constant expression over literals, enum constants and `constexpr` variables; an
  enum constant, which is written by name; `nullptr`; a braced or constructed struct; a braced list for a container; or
  `&Asset`, which points an object member at an asset (see [Data assets](#data-assets) and
  [Game assets](#game-assets)). How constants fold is in [Constants](#constants).
- In `UE_DEFAULTS`, in a struct's member defaults and in a braced asset, an explicit zero is written, because there the
  value is compared with a base that need not be zero.
- A member with the name of an inherited variable does not change the inherited default. It is a second variable that
  hides the parent's, and nothing warns. Use `UE_DEFAULTS`; see [Class defaults](#class-defaults).

### Access, read-only and categories

| You write | What it does | Status |
|---|---|---|
| `public:`, `protected:`, `private:` before a function | The function is cooked as public, protected or private, and the editor allows or refuses a call node to it the same way. An override keeps its parent's access. Nothing is checked at run time. | Yes |
| `private:` before a variable | The variable is still cooked on the class, but the editor API stub leaves it out. A Blueprint variable has no other access: a protected one and a public one look the same. | Yes |
| `const int32 Limit = 3;` | A read-only variable (BlueprintReadOnly): the editor offers a Get node and no Set node. The initializer is the default. clang refuses a write in C++, and the Blueprint VM does not check it. | Yes |
| `UE_READONLY int32 Cap = 5;` | Read-only the same way (BlueprintReadOnly), but it can still be written: a subclass's `UE_DEFAULTS` may give it a default, and a write from code compiles with `warning: <Class>::<Function>: Cap is BlueprintReadOnly; the editor would not set it`, because the VM allows the write (for example, setting a deferred spawn's members before FinishSpawning). UeApi declares the engine's and game's BlueprintReadOnly properties this way. | Yes |
| `UE_CATEGORY("Turret\|Setup");` | Gives the functions and variables declared after it that category. `\|` starts a subcategory, and `UE_CATEGORY("")` ends the category. It is positional, like an access specifier. | Yes |
| a keyword or a tooltip for a function | Not yet: there is no macro for it. | Not yet |

```cpp
class Turret : public AActor {
public:
  UE_CATEGORY("Turret|Setup");
  void Configure() {}
  int32 Charges;
  UE_CATEGORY("");                  // back to none
  int32 Plain;
  const int32 Limit = 3;            // Get node only
protected:
  int32 Step() { return 1; }
private:
  int32 Seed;                       // cooked, but not in the API stub
  int32 Double() { return Step() + Step(); }
};
```

Notes:

- A C++ `class` starts private. A function declared before the first `public:` is a private Blueprint function, and a
  variable there is left out of the API stub.
- Categories and access matter for the editor API stub that `generate_api` writes, so that a Blueprint made in the
  editor can call the mod. `UE_CATEGORY` is written only there, as the category of each variable and function: the
  cooked class has no categories, and without `generate_api` the macro does nothing. The stub keeps a private function,
  flagged private, and leaves out interfaces the mod declares. Whether the editor lists the members under their
  category has not been checked. See [Editor API stubs](GUIDE.md#editor-api-stubs).

### Names

| You write | What it does | Status |
|---|---|---|
| `Index_0 = 7;`, for a member the SDK spells `Index_0` | Write the SDK's spelling. Dumper-7 respells names that C++ cannot use: a clash gets a suffix (`Name` becomes `Name_0`), an illegal character becomes `_` (`Audio Flying` becomes `Audio_Flying`), and a leading digit becomes a word (`3P` becomes `ThreeP`). The SDK keeps the real name beside the member, and everything the compiler cooks uses the real name. | Yes |
| `class Größe : public AActor { FName Umlaut = "Größe"; };` | Non-ASCII class, member and asset names, and non-ASCII name and string defaults, are cooked and registered correctly. | Yes |
| `using FTarget = AActor;`, then `FTarget *Aimed;` | An alias of a UObject class is still an object reference, not an address. It works at namespace scope and at class scope. | Yes |
| `using FMoods = TArray<EMood>;`, `using Factory = TScriptInterface<IFoo>;` | A global alias of a template type is that type wherever it is used, in out-of-line method definitions too. A global alias of a class is that class the same way, so an override may spell a parameter through it. | Yes |
| `using Grapple = Game::WeaponsNTools::GrapplingGun::WPN_GrapplingGun_C;` | An alias to a game class's full `Game::` path. The SDK gives a game class a short name at global scope only when no other game package has a class of that name. For one that does, write such an alias. | Yes |

```cpp
class NameTest : public UFSDSaveGame {
  UE_DEFAULTS { Index_0 = 7; Name_0 = "Karl"; }   // cooked as Index and Name
public:
  int32 Next() { return ++Index_0; }
};
```

Notes:

- Parameter names keep the C++ spelling. Calls pass arguments by position, so only an editor stub's pin names show it.
- A real name that is not ASCII and that Dumper-7 respelled (three members in the DRG dump) is not supported yet.
- When a value of an alias type turns up in another class's function, the compiler finds the class by its name. If two
  classes share that name, the source is refused rather than compiled with the value typed as an address.

## Global variables

A Blueprint has no global variables. For each variable at namespace scope that a function uses, AssetGen generates a
class that holds the variable in its default object, and every class of the source reads and writes that one object.
The rule to remember: define a global in the source that uses it.

### Namespace-scope variables

| You write | What it does | Status |
|---|---|---|
| `int32 Counter = 5;` | A variable that every class of the source shares. Its holder is a generated `UObject` class at `<UE_MOD_PACKAGE>/Counter`, with one member of the same name and type. The initializer is its default. | Yes |
| `namespace Tally { int32 Hits; }` | A namespace gives no subfolder here: the holder is `<UE_MOD_PACKAGE>/Tally__Hits`. A global with no initializer starts at zero, as in C++. | Yes |
| `Counter += By;`, `++Tally::Hits;`, `return Counter++;` | Assignment, compound assignment, `++` and a postfix read all work on a global. | Yes |
| `constexpr int32 kStep = 3;`, `const int32 kMask = 1 << 4;` | A `const` or `constexpr` global is not stored: each use is its value. See [Constants](#constants). | Yes |
| a global that no function uses | Gets no holder class. Nothing is written for it. | Yes |
| `UMoodDef GlobalDef;`, a variable of a UObject class by value | Refused where it is used: "is an asset, not a value". Declare a braced asset of the mod or name an existing one with `UE_ASSET_AT`, and point at it with `&`. See [Data assets](#data-assets). | Refused |
| `extern int32 Shared;`, used but not defined in this source | Refused: "declared extern but not defined in this source". | Refused |

```cpp
UE_MOD_PACKAGE("/Game/_MyMods/Tally");

int32   Counter  = 5;                 // held at /Game/_MyMods/Tally/Counter
FString Greeting = "hi";              // held at /Game/_MyMods/Tally/Greeting
namespace Tally { int32 Hits; }       // held at /Game/_MyMods/Tally/Tally__Hits
int32   Unused   = 3;                 // no function uses it: nothing is written

class Scorer : public AActor {
public:
  int32 Bump(int32 By) { Counter += By; ++Tally::Hits; return Counter; }
  int32 Take() { return Counter++; }
  FString Greet() { return Greeting + "!"; }
};

class ScoreReader : public AActor {
public:
  int32 Read() { return Counter * 100 + Tally::Hits; }   // the same Counter
};
```

Notes:

- `UE_ASSET_ALL`'s `All` is a global too, held the same way. It is the one exception to the extern rule, because the
  compiler fills it in. See [Game assets](#game-assets).

### Globals across sources

| You write | What it does | Status |
|---|---|---|
| `int32 Counter = 5;` in two sources with different `UE_MOD_PACKAGE`s | Two separate variables. Each holder sits under its own source's package, so they never collide. | Yes |
| `int32 Counter = 5;` in two sources with the same `UE_MOD_PACKAGE` | Both sources write the same holder package, byte for byte, so both name one variable. | Yes |
| `extern int32 Counter;` to reach another source's global | Refused, as above. Each source that uses a global cooks its holder, so the global is defined where it is used. | Refused |

## Types

This section covers the types a member, parameter, local or return value can have, and the Blueprint variable type each
becomes: numbers, strings, object and class references, and soft references. Enums, structs and containers have
sections of their own. The rule to remember: a variable can only have a type that UE 4.27 Blueprint has, so there is no
`double`, no `uint32` and no weak pointer.

### Variable types

| You write | What it does | Status |
|---|---|---|
| `bool bArmed = true;` | A Boolean variable. | Yes |
| `uint8 Charges = 3;` | A Byte variable. `unsigned char` is the same type. | Yes |
| `int32 Kills;` | An Integer variable. `int` is the same type. | Yes |
| `int64 Stamp;` | An Integer64 variable. `long long` is the same type. | Yes |
| `float Ratio = 0.5f;` | A Float variable. | Yes |
| `FString Label;` | A String variable. A `const char *` member or parameter is a String too. | Yes |
| `FName Key = "alpha";` | A Name variable. | Yes |
| `FText Caption;` | A Text variable. | Yes |
| `AActor *Target;` | An Object Reference variable. See the next table. | Yes |
| `EMood Mood;` | An enum variable: [Enums](#enums). | Yes |
| `FVector Home;` | A struct variable: [Structs](#structs). | Yes |
| `TArray<int32> Scores;` | An Array, Set or Map variable: [Containers](#containers). | Yes |
| `uint32 Mask;` | Refused (`unimplemented property Mask: uint32`): Blueprint 4.27 has no such type. The same holds for `int8`, `int16`, `uint16` and `uint64`, as a member, parameter, local or return value. Use `int32` or `int64`. | Refused |
| `double D;` | Refused (`unimplemented property D: double`): Blueprint reals in 4.27 are `float`. Use `float`. | Refused |

Notes:
- A member's initializer is its default in Class Defaults: [Classes and variables](#classes-and-variables).
- A cast to a narrow or unsigned type inside an expression is refused too: `(uint32)X >> 1` gives "no Kismet conversion
  from int to uint32". So there is no logical (unsigned) right shift.
- These types still work inside a `consteval` function, which clang runs while it compiles the mod: see
  [Constants](#constants).
- A local declared without an initializer starts at zero, empty, None or null: [Locals](#locals).

### Object and class references

| You write | What it does | Status |
|---|---|---|
| `APawn *Spotted;` | An Object Reference variable of that class. Its default can only be `nullptr` or `&Asset`. | Yes |
| `using FTarget = AActor;` then `FTarget *Aimed;` | An alias of a class is still an Object Reference: [Classes and variables](#classes-and-variables). | Yes |
| `int32 *P;` | A pointer to a type that is not a UObject is an int64 address: [Pointers and memory](#pointers-and-memory). | Yes |
| `TSubclassOf<AActor> Kind;` | A Class Reference limited to `AActor` and its subclasses. | Yes |
| `UClass *Raw;` | A Class Reference that takes any class. | Yes |
| `Raw = Kind; Kind = Raw;` | Each converts to the other. Nothing checks at run time that the class in `Raw` derives from `AActor`. | Yes |
| `TSubclassOf<AActor> Kind = AActor::StaticClass();` | Not yet: a class reference member starts null, and a class as its default is refused ("a default is a value known when the mod is built"). `= nullptr` compiles. See [Classes and variables](#classes-and-variables). | Not yet |
| `TScriptInterface<IHealth> Health;` | An Interface variable: [Interfaces](#interfaces). | Yes |
| `TWeakObjectPtr<AActor> Last;` | Refused by clang (`no template named 'TWeakObjectPtr'`): the SDK has no weak or lazy pointer. Keep an object pointer and test it with `if (Obj)`, or keep a soft reference. | Refused |

Notes:
- A class that the headers only forward-declare cannot be a variable's type. The message says which `UeApi/Game/...`
  header to `#include`.

### Casts and class values

| You write | What it does | Status |
|---|---|---|
| `Cast<APawn>(Obj)` | The Cast To node: the object if it is an `APawn`, otherwise null. The class must be one the headers declare, or the cast is refused with "Cast<> to an unknown class". | Yes |
| `static_cast<APawn *>(A)` | Compiles to nothing. The reference passes through unchecked, as in C++. Use `Cast<T>` when the object may not be a `T`. | Yes |
| `AActor *U = P;` | An upcast. It compiles to nothing. | Yes |
| `AActor::StaticClass()` | The class as a constant, like a class picked on a pin. On one of the mod's own classes it names that class: [Creating objects](#creating-objects). | Yes |
| `Other->GetClass()` | The Get Class node. The other object helpers: [Working with other objects](#working-with-other-objects). | Yes |
| `if (Target)` | Is Valid, so an object that is being destroyed tests false. `Target != nullptr` is a plain compare. See [Working with other objects](#working-with-other-objects). | Yes |
| `A < B` on object pointers | Refused: Blueprint compares object references only with `==` and `!=`. See [Operators](#operators). | Refused |

### Soft references

| You write | What it does | Status |
|---|---|---|
| `TSoftObjectPtr<UTexture2D> Icon = "/Game/UI/Icons/T_Icon.T_Icon";` | A Soft Object Reference. Its default is the asset's path. Nothing is loaded with the class. | Yes |
| `TSoftClassPtr<AActor> Kind = "/Game/A/BP_A.BP_A_C";` | A Soft Class Reference. For a Blueprint class the path names the generated class, `<Name>_C`. | Yes |
| `TArray<TSoftClassPtr<AActor>> Kinds = {"/Game/A/BP_A.BP_A_C"};` | An array of soft references, with paths as its default. | Yes |
| `= "/Game/Dir/Pkg"` in a default | A path with no object name means `Pkg.Pkg`. | Yes |
| `Icon = "/Game/UI/Icons/T_Icon.T_Icon";` in a function | A soft path constant, written exactly as given. | Yes |
| `Seen = A;` | An object converts to a soft reference (To Soft Object Reference); a class converts to a soft class reference. | Yes |
| `Kinds.Contains(A->GetClass())` | The class converts to a soft class reference where one is wanted, as the editor does. | Yes |
| `(AActor *)Seen`, `(UClass *)Kind` | Resolve Soft Reference: the object or class, or null if it is not loaded. The cast is explicit so that the null is visible in the source. | Yes |
| `FString Path = Seen;` | The path as a string, and through it an FName or FText. | Yes |
| `FString(TSoftClassPtr<AActor>(A->GetClass()))` | A class's path. `FString(A->GetClass())` would give its name. | Yes |
| `Kind == Other` | Compares the two paths (Equal (Soft Class Reference)), whatever class each one is of. Nothing is loaded. | Yes |
| `TSoftObjectPtr<UObject> Any = Icon;` | A soft reference to a subclass passes where one to its parent is wanted, with no conversion node. | Yes |
| `UKismetSystemLibrary::LoadAsset_Blocking(Icon)` | Loads the asset now and returns it. `LoadClassAsset_Blocking(Kind)` does the same for a class. | Yes |
| `UKismetSystemLibrary::LoadAsset(Icon)` | The latent Async Load Asset node: the function waits, and the call's value is the object. `LoadAssetClass` loads a class. See [Latent calls](#latent-calls). | Yes |
| `UKismetSystemLibrary::IsValidSoftObjectReference(Icon)` | Whether a path is set. It loads nothing. | Yes |

```cpp
TSoftClassPtr<AActor> Kind = "/Game/A/BP_A.BP_A_C";
TSoftObjectPtr<UTexture2D> Icon = "/Game/UI/Icons/T_Icon.T_Icon";
TArray<TSoftClassPtr<AActor>> Kinds = {"/Game/A/BP_A.BP_A_C", "/Game/B/BP_B.BP_B_C"};
TSoftObjectPtr<AActor> Seen;

void Remember(AActor *A) { Seen = A; }
bool KnowsClassOf(AActor *A) { return Kinds.Contains(A->GetClass()); }
AActor *Recall() { return (AActor *)Seen; } // null unless the object is loaded
FString SeenPath() { return Seen; }
UClass *KindNow() { return UKismetSystemLibrary::LoadClassAsset_Blocking(Kind); }
```

Notes:
- The `Pkg.Pkg` shorthand applies to defaults only. In a function the path is written as given, so write the full
  object path there.
- For a Blueprint class, `"/Game/Dir/BP_A"` expands to `BP_A.BP_A`, the Blueprint asset, not the class `BP_A_C`. Write
  the full `"/Game/Dir/BP_A.BP_A_C"` in a `TSoftClassPtr` default.
- A soft reference inside a data asset, and a soft reference to one of the game's assets by name:
  [Data assets](#data-assets) and [Game assets](#game-assets).

## Literals and conversions

Literals become constants of the type the code wants, and numbers convert the way C++ converts them, through
Blueprint's conversion nodes (Conv_IntToFloat, Truncate and the like). The rule to remember: a float literal next to a
float value needs its `f`. `X * 0.5` is a double operation in C++, and Blueprint 4.27 has no double.

### Number literals

| You write | What it does | Status |
|---|---|---|
| `int32 Mask = 0xFF;` | An int32 constant. | Yes |
| `int64 Big = 1234567890123;` | An int64 constant when the value does not fit in int32. A literal is never truncated. | Yes |
| `(int64)18446744073709551615ULL` | A uint64 literal past int64's range keeps its bits, as the C++ cast does: this is -1. | Yes |
| `float Reach = 300.f;` | A Float constant. A literal beyond float's range, `(float)1e39`, is infinity. | Yes |
| `X * 0.5`, `X > 0.5` | Refused (`no Kismet conversion from float to double`): C++ makes this double math. Write `0.5f`. | Refused |
| `float Y = 0.5;`, `return 0.25;`, `FVector(1.0, 2.0, X)` | A double literal on its own converts to float where a float is wanted. So does a default, `float Delay = 0.75;`. | Yes |
| `'A'`, `('a' - 'A')` | The int constant of its code unit. A plain `char` is signed, so `'\xff'` is -1. | Yes |
| `nullptr` | None for an object or class. For a `TScriptInterface` it is the null interface. Where an int64 address is wanted it is 0. | Yes |
| `int32 N{7};`, `EMood M{EMood::Angry};` | Braces around one value of a type that is not a struct are that value. | Yes |
| `EMood M{};`, `int32 N = int32();`, `AActor* A{};`, `Mood = {};`, `return {};` | C++'s value-initialisation: the type's zero, the zero enumerator or None. In a function and as a default, where a zero writes nothing. In `UE_DEFAULTS` the zero is written, as `= 0` is (see [Class defaults](#class-defaults)). | Yes |
| `"text"`, `L"wide"` | A String, Name or Text constant: [Strings and text](#strings-and-text). | Yes |

```cpp
float Delay = 0.75;                        // a lone double literal is fine
float Half(float X) { return X * 0.5f; }   // X * 0.5 is refused
int64 AllOnes() { return (int64)18446744073709551615ULL; }
```

Notes:
- The message for `X * 0.5` does not tell you to add the `f`. Look for a double literal in the expression it names.

### Number conversions

| You write | What it does | Status |
|---|---|---|
| `Ratio = Count;` | int32 to float (Conv_IntToFloat). Every widening is the matching Kismet conversion node: int32 to int64, uint8 to int32, bool to int32, uint8 to float. A literal converts with no node. | Yes |
| `Big = Count;` | int32 to int64. A uint8 or bool reaches int64 through int32. | Yes |
| `int32 N = X;`, `(int32)X`, `(int64)X` | float to integer, truncated toward zero as in C++ (the Truncate node): -2.75 becomes -2. | Yes |
| `(uint8)X` with `float X` | Refused (`no Kismet conversion from float to uint8`). Write `(uint8)(int32)X`, which then wraps the int as C++ does. | Refused |
| `(uint8)V`, `(int32)Big` | A narrowing cast wraps: 511 becomes 255, and 2^32+5 becomes 5. It happens even in the middle of a chain of casts, and an implicit int64 to int32 assignment wraps the same way. | Yes |
| `(float)Big` with `int64 Big` | Warns: UE 4.27 has no int64 to float node, so the value goes through int32. A value outside int32's range wraps (2^32+5 becomes 5.0). | Warns |
| `Charges += 200;` with `uint8 Charges` | The byte is promoted to int32 for the math, and the store back wraps modulo 256, as in C++. | Yes |
| `Sum / N` with two int32 | Integer division, as in C++. Each operation takes the type C++ promotes to: float if either side is float, otherwise int64 if either side is int64, otherwise int32. | Yes |
| `A / B` with `int32 A`, `float B` | `A` converts to float, then a float divide. | Yes |
| `(bool)F`, `bool B = Count;` | True when nonzero, as in C++. An int32 goes through ToBool (integer) (Conv_IntToBool); a float, int64, byte or enum is compared with zero, so a NaN is true. Conditions of every type: [Statements and control flow](#statements-and-control-flow). | Yes |
| `+`, `%`, `<<`, `~` and the other operators | The Kismet math nodes: [Operators](#operators). A shift amount must be a bare integer literal, and `%` on int64 is refused. | Yes |

```cpp
int32 Count;
float Ratio;
int64 Big;

void Convert(float X) {
  Ratio = Count;              // Conv_IntToFloat
  Big = Count;                // Conv_IntToInt64
  int32 N = X;                // truncated toward zero
  uint8 B = (uint8)(int32)X;  // a float reaches a byte through int32
  Count = N + B;
}
float Avg(int32 Sum, int32 N) { return Sum / N; }  // integer division, then float
float Scale(int32 A, float B) { return A / B; }    // float division
```

Notes:
- A float literal as a bool default, `bool Halfway = 0.5f;`, is true, and clang warns that the conversion changes the
  value. A `constexpr float` constant as the default draws no warning.

## Strings and text

FString, FName and FText are Blueprint's String, Name and Text. They convert into one another and from numbers
implicitly, `+` joins them, and `==` compares them. The rule to remember: they have no methods. String functions are
the `UKismetStringLibrary` nodes, and text functions the `UKismetTextLibrary` nodes, called as statics.

### String literals

| You write | What it does | Status |
|---|---|---|
| `Label = "Ready";` | A literal where an FString, FName or FText is wanted is a constant of that type, like a value typed on a pin. No conversion node runs. | Yes |
| `FName Named = "plain name";`, `FText Literal = "plain text";` | The same as a default. | Yes |
| `FString()`, `FName()`, `FText()` | The empty string, None and the empty text. | Yes |
| `FName Umlaut = "Größe";` | Non-ASCII text is written as UTF-16 and reads back exactly as written, in code and in defaults. It is the same FName you get by converting the string at run time. | Yes |
| `FString Wide = L"wide";` | An `L`, `u` or `U` literal is a Unicode string constant. | Yes |

Notes:
- A text literal, in code or as a default, is culture-invariant. It is not gathered for localization.

### Joining strings

| You write | What it does | Status |
|---|---|---|
| `FString("Kills: ") + Count + ", ratio " + Ratio` | Every `+` that involves a string converts each operand to a string (the ToString nodes) and joins them with Append. Strings, names, texts, numbers, bools, objects and structs with a ToString node can all be mixed. | Yes |
| `Key = Label + "_key";` | The joined string converts to the destination: String to Name here. | Yes |
| `Caption = Label + Key + Caption;` | String to Text. The text is culture-invariant, since it comes from a string. | Yes |
| `"Kills: " + Count`, `Count + " left"` | In C++ this is pointer arithmetic; AssetGen reads it as the concatenation you meant. | Yes |
| `"a" + N + "b"`, `"a" + Ratio` | Refused by clang (pointer + pointer, pointer + float). Only one literal and an integer can start a chain. Start it with `FString("a")`. | Refused |
| `FString(" (") + this + ")"` | An object converts to its name (ToString (Object)). A class converts to its name too, not its path. | Yes |
| `FString("Home: ") + Home` | A struct with a Kismet ToString node (FVector, FRotator, FTransform, FLinearColor, FVector2D, FIntPoint...) converts to that node's text. | Yes |

```cpp
FString Label;
FName Key;
FText Caption;
int32 Count;
float Ratio;
FVector Home;

void Describe() {
  Label = FString("Kills: ") + Count + ", ratio " + Ratio;
  Key = Label + "_key";             // String to Name
  Caption = Label + Key + Caption;  // String to Text
  Label = "Kills: " + Count;        // one literal and an integer can start a chain
  Label = FString("Home: ") + Home + " (" + this + ")";
}
FName MakeKey(FText Prefix, int32 Index) { return Prefix + "_" + Index; }
```

### Converting strings

| You write | What it does | Status |
|---|---|---|
| `Key = Label;` | FString to FName (String to Name). | Yes |
| `Caption = Key;` | FName to FText (Name to Text). | Yes |
| `FString S = Caption;` | FText to FString (Text to String). FText to FName goes through FString. | Yes |
| `FString A = Count;`, `FString C = bDone;` | A number or bool converts through its ToString node: 3 becomes "3", false becomes "false". | Yes |
| `Caption = Big;` with `int64 Big` | UE 4.27 has no int64 to string node, so an int64 goes Int64 to Text, then Text to String when a string is wanted. Integer text has no digit grouping. | Yes |
| `FName N = Big;` with `int64 Big` | Refused. clang calls the conversion ambiguous, and `FName(FString(Big))` is refused with "no Kismet conversion from int64 to FName". Convert through a local: `FString S = Big; Key = S;`. | Refused |
| `Count = int(Label);`, `float F = float(Label);` | String to Int, String to Float. Text that is not a number gives 0. The cast must be explicit. FName and FText parse by way of FString. | Yes |
| `FString Path = Soft;` | A soft reference's path: [Types](#types). | Yes |

### Comparing and testing strings

| You write | What it does | Status |
|---|---|---|
| `S == "gold"` | FString `==` ignores case (Equal, Case Insensitive), as UE's own FString `==` does. | Yes |
| `UKismetStringLibrary::EqualEqual_StrStr(A, B)` | The case-sensitive compare. | Yes |
| `A != B` with two FNames | Names compare ignoring case. | Yes |
| `A == B` with two FTexts | The Equal, Case Insensitive (text) node: texts compare ignoring case too. | Yes |
| `A < B` with two FStrings | Refused by clang (`invalid operands to binary expression`): no Kismet node orders strings. | Refused |
| `if (N)`, `!N` with `FName N` | Tests `N != None`. Conditions of every type: [Statements and control flow](#statements-and-control-flow). | Yes |

### String functions

| You write | What it does | Status |
|---|---|---|
| `UKismetStringLibrary::Len(S)` | The Len node. Every string operation is a `UKismetStringLibrary` static: `Mid`, `Replace`, `ToUpper`, `ParseIntoArray` and the rest. | Yes |
| `S.Len()` | Not yet. It compiles with no error and no warning, into a call that cannot work in the game. Call `UKismetStringLibrary::Len(S)`. | Not yet |

Notes:
- The SDK declares `Len()` on FString only so that the struct type-checks. No other FString method is declared, so any
  other method call is a clang error.

## Constants

This section covers constants, `consteval` functions and inline class variables. A constant has no storage in a
Blueprint. Each use of it is its value, worked out when the mod is built, and a member default computed from constants
is folded the same way. The rule to remember: build-time evaluation covers constants, constant expressions and
`consteval` calls, and a default reaches a `consteval` call only through a constant. A plain `constexpr` function call
is not run at build time.

### Constants and constant expressions

| You write | What it does | Status |
|---|---|---|
| `constexpr int32 kStep = 3;` at namespace scope | No storage. Each use, in a function or a default, is the value 3. | Yes |
| `const int32 kMask = 1 << 4 \| kStep;` at namespace scope | A top-level `const` variable is a constant too. | Yes |
| `static constexpr int32 kSlots = kStep * 4;` in a class | A class-scope constant. `static const` works the same way. | Yes |
| `int32 Budget = kSlots * 2 + 1;` | A default from a constant expression is folded when the mod is built. Each step is rounded to its C++ type: `1 / 2.f` is 0.5 and `1 << 31` wraps. | Yes |
| `bool Ready = kStep > 2 && kStep < 10;` | Comparisons, `&&`, `\|\|` and `?:` fold too, as do casts, enum constants and unary `-`, `~` and `!`. | Yes |
| `int32 Counter = 5;` at namespace scope | Not a constant: a variable every class of the mod shares. See [Global variables](#global-variables). | Yes |
| `TArray<int32> Rolls = {UKismetMathLibrary::RandomInteger(3)};` | Refused (`a default is a value known when the mod is built`): a default cannot call a function. Compute it in `ReceiveBeginPlay`. See [Classes and variables](#classes-and-variables). | Refused |
| `sizeof(FVector)`, `alignof(FTransform)` in code | The game's layout: [Pointers and memory](#pointers-and-memory). Refused in a default. | Yes |

Notes:
- How constant subexpressions fold inside a function body: [The optimizer](#the-optimizer).

### consteval functions

| You write | What it does | Status |
|---|---|---|
| `static constexpr int32 kSeed = Fnv("types");` | clang runs a `consteval` function while it compiles the mod, and AssetGen uses the result. The body can be any C++: loops, `uint32` math, pointers. | Yes |
| `int32 Seed = kSeed;` | The result as a default, through the constant. | Yes |
| `int32 Seed = Fnv("types");` | Refused (`a default is a value known when the mod is built`): a call in a default counts only through a constant, `constexpr int32 kSeed = Fnv("types");`. | Refused |
| `return N + Fnv("angry");` | The result as a constant in code. | Yes |
| `Fnv(Name)` with a run-time argument | Refused by clang (`call to consteval function ... is not a constant expression`). The arguments must be constants. | Refused |
| `int32 V = Twice(3);` with a `constexpr` (not `consteval`) `Twice` | Refused with the default rule's message, which points at `consteval`. A plain `constexpr` call is not evaluated at build time, in a default or in code: [Inline functions and templates](#inline-functions-and-templates). | Refused |

```cpp
constexpr int32 kStep = 3;
const int32 kMask = 1 << 4 | kStep;

consteval int32 Fnv(const char *S) {
  uint32 H = 2166136261u;
  while (*S) H = (H ^ uint32(*S++)) * 16777619u;
  return int32(H & 0x7FFFFFFF);
}

class Tuning : public AActor {
public:
  static constexpr int32 kSlots = kStep * 4;
  static constexpr int32 kSeed = Fnv("types");
  int32 Budget = kSlots * 2 + 1;  // 25 in Class Defaults
  bool Ready = kStep > 2 && kStep < 10;
  int32 Seed = kSeed;

  int32 ConstSum(int32 N) { return N * kStep + kSlots + kMask; }
  int32 Salted(int32 N) { return N + Fnv("angry"); }
};
```

### Inline class variables

| You write | What it does | Status |
|---|---|---|
| `static inline const float kHold = 0.75f;` | A value that is not a Blueprint variable. It cooks nothing: no variable and no default. Each use is its initializer, evaluated where it is used, here the literal 0.75. | Yes |
| `static inline const FName kTag = "types";` | Each use is the Name constant. | Yes |
| `static inline const TArray<int32> kPrimes = {2, 3, 5, 7, 11};` | Each use is a Make Array. `kPrimes[I]` and `kPrimes.Num()` each make the array. | Yes |
| `static inline const TMap<int32, float> kRates = {{1, 0.5f}, {3, 1.5f}};` | Each use is a Make Map; a TSet is a Make Set. | Yes |
| `kPrimes.Contains(N)` | Makes no array: `N` is evaluated once and compared with each element in turn. This holds for an inline TArray or TSet of constants whose `==` is exact: numbers, bools, enums, names, strings, objects, classes and soft references. | Yes |
| `for (int32 P : kPrimes)` | Makes no array when an inline TArray holds at most 16 constants and the loop variable has the element type: each pass picks its element by the loop index. Anything else (17 or more elements, a TSet or TMap, elements that are not all constants, a loop variable of another type) makes the container once, before the loop. | Yes |
| `kHold`, `this->kHold` | Both name the constant. | Yes |
| `Me()->kHold` | Refused (`kHold is static: name it without the object in front`): the object would never be evaluated. | Refused |
| `static inline float Loose = 0.25f;` | Refused wherever it is used (`a Blueprint class has no static storage`). Make it `static inline const` or `static constexpr`, or a plain member if it has to keep a value. | Refused |
| `kHold = 1.0f;` | Refused by clang: the member is `const`. | Refused |

```cpp
static inline const float kHold = 0.75f;
static inline const TArray<int32> kPrimes = {2, 3, 5, 7, 11};
static inline const TMap<int32, float> kRates = {{1, 0.5f}, {3, 1.5f}};
static inline const TArray<TSoftClassPtr<AActor>> kKinds = {"/Game/A/BP_A.BP_A_C", "/Game/B/BP_B.BP_B_C"};

float HoldFor(int32 N) { return N * kHold; }
float RateOf(int32 K) { return kRates.Contains(K) ? kRates[K] : -1.0f; }  // each use makes the map
bool Known(AActor *A) { return kKinds.Contains(A->GetClass()); }          // no array
int32 PrimeSum() {
  int32 T = 0;
  for (int32 P : kPrimes) T += P;  // no array
  return T;
}
```

Notes:
- C++ runs the initializer once. Here every use runs it, so an initializer that calls something, such as
  `RandomInteger`, calls it at every use. A range-for makes its array only once.
- `Contains` on an inline TMap, or on a list of texts or structs, makes the container, because their `==` is not exact
  (a vector's `==` has a tolerance). `UE_NO_OPTIMIZE` on the function turns the no-array `Contains` off.
- A function-local `static` is something else again: [Latent calls](#latent-calls).

## Enums

A variable of a game enum is what the editor makes of it: an Enum variable (an enum property over a byte) for an
`enum class`, a Byte bound to the enum for any other. A mod's own enum needs `UE_ENUM`, which cooks it as a
UserDefinedEnum asset in the mod's package. The rule to remember: a mod enum without `UE_ENUM` is only a set of number
constants, not a Blueprint type.

### Game enums

| You write | What it does | Status |
|---|---|---|
| `EEndPlayReason LastReason;` | A variable of the game's enum: a Byte bound to its UEnum, as the editor makes a variable of an enum that is no `enum class` (here a namespaced one, which the game's C++ holds as `TEnumAsByte`). Its enumerators are byte constants. | Yes |
| `EAttachmentRule Rule = EAttachmentRule::KeepWorld;` | A variable of a game `enum class`: an EnumProperty over a ByteProperty named UnderlyingType, as the editor makes it and as the game's own members and parameters of it are. The same goes for a parameter, a return value, a local, a container's element or key, a UE_STRUCT member, a delegate's parameter and a `UE_DEFAULTS` tag on a native member: an override or a delegate of a native signature then has the native's types. The value is the same byte, its enumerators byte constants, and a default is written as the enumerator's name in an EnumProperty tag. | Yes |
| A game enum that no property uses | UeApi's `Types.json` gives each enum's form, which genueapi reads off how the Dumper-7 dump's properties of the enum (a native delegate's parameters among them), and the game's Blueprints' variables, locals and containers' elements, are reflected (`form`: `EnumClass` or `TEnumAsByte`). 240 of the SDK's 1445 enums have no such property, and a variable of one is a Byte bound to the enum. One the game uses, ESteamVRInputStringBits, is only the element of a native function's `TArray` parameter, whose inner property neither the dump nor Dumper-7's usmap tells apart and no game package holds. Their form is not read yet rather than unknown: the editor takes it from the enum itself (`UEnum::GetCppForm`), which the dump does not carry yet. | Yes |
| `LastReason == EEndPlayReason::Quit`, `<` | Comparisons work. | Yes |
| `switch (Reason)` | A switch works on any enum. | Yes |
| `EEndPlayReason Last = EEndPlayReason::Quit;` as a default | The default is written as the enumerator's name. | Yes |
| `ECreatureSize Size;` | Refused (`a uint8, int32 or int64 enum only`). 36 of the SDK's 1445 enums (35 uint32, 1 uint16) have an underlying type other than uint8, int32 or int64, and none of them can be a variable. | Refused |

### Mod enums: UE_ENUM

| You write | What it does | Status |
|---|---|---|
| `enum class EMood : uint8 { Calm, Angry = 5, Sleepy };` `UE_ENUM(EMood);` | Cooks EMood as a UserDefinedEnum asset in the mod's package, with the C++ names and values and a closing `EMood_MAX` one past the largest value. From then on it is an ordinary Blueprint enum for variables, parameters, constants and switch. | Yes |
| `EMood Mood = EMood::Angry;` | A default is written as the enumerator's name. | Yes |
| `enum class ESpan : int32 { Tiny = -3, Wide = 70000 };` `UE_ENUM(ESpan);` | An int32 or int64 enum is cooked the same way with its wide values. A variable of it holds the full width, as UHT reflects a native `enum class : int32`. | Yes |
| `enum class EBad : int16 { A, B };` `UE_ENUM(EBad);` | Refused (`declare it : uint8 (a Blueprint enum), : int32 or : int64`), even if no variable uses it. An enum with no fixed type is refused too. | Refused |
| `enum class EO : uint8 { A = 255 };` `UE_ENUM(EO);` | Refused (`A is out of range`): `_MAX` needs the next value, so a uint8 enum's largest value is 254. | Refused |
| `enum class EGear : uint8 { Low, High, EGear_MAX };` | The UE C++ idiom: a declared `EGear_MAX` one past the largest value is the closing `_MAX` itself, not a second entry. Any other value for it is refused. | Yes |
| `UE_ENUM(EMissing);` | Refused (`names no enum with enumerators`). | Refused |
| `UE_ENUM(EDialogRestriction);` for a mod enum named like a game or engine enum, in any namespace | Refused (`/Script/FSD already has an enum EDialogRestriction`). The engine keeps every enumerator's name, `EDialogRestriction::None`, in one global table where the first enum loaded wins, so lookups by name would answer with the game's. Rename it. Two mods' enums of one name clash the same way, which the compiler cannot see; give each mod's enums names of their own. | Refused |
| `UE_ENUM_IN(ESystems, "/Game/_MyMods/Shared");` | For an enum in a header several mods include. Only the source whose `UE_MOD_PACKAGE` is exactly that path cooks it; every other mod imports it from there. `UE_STRUCT_IN` does the same for structs: [Structs](#structs). | Yes |
| `ELocal L;` with no `UE_ENUM` on `ELocal` | Refused (`unimplemented property L: ELocal`). The enum's constants still fold to numbers, `(int32)ELocal::B`, but it is not a Blueprint type. Add `UE_ENUM`. | Refused |
| `enum { kCap = 1000, kFar = 5000000000 };` | Constants of an enum with no fixed type are int-sized, or int64 when a value needs it, as C++ makes them. They fold to their values. | Yes |
| `EMood Mood;` of a `uint8` UE_ENUM | A Byte bound to the enum, which is what the editor makes of a UserDefinedEnum: its form is namespaced, never `enum class`, whatever the C++ says. | Yes |

```cpp
enum class EMood : uint8 { Calm, Angry = 5, Sleepy };
UE_ENUM(EMood);

enum class ESpan : int32 { Tiny = -3, Wide = 70000 };
UE_ENUM(ESpan);

class Moody : public AActor {
public:
  EMood Mood = EMood::Angry;
  ESpan Span = ESpan::Wide;

  int32 Score(EMood M) {
    using enum EMood;
    switch (M) {
    case Calm: return 1;
    case Angry: return 2;
    case Sleepy: break;
    }
    return 0;
  }
};
```

Notes:
- An enum's zero value, what a variable of it starts at, is its enumerator whose value is 0, or the first one if none
  is 0.
- The editor cannot make an int32 or int64 enum. Such an enum has not been tested in game.
- With `UE_ENUM_IN`, a path that matches no mod's `UE_MOD_PACKAGE` cooks the enum nowhere, and no message says so.
  Shared headers and the mods that own them: [Mod sources and packages](#mod-sources-and-packages).

### Enum conversions and switch

| You write | What it does | Status |
|---|---|---|
| `(int32)M` | Byte to Int. | Yes |
| `(EMood)X` | Int to Byte. It wraps modulo 256 and does not check that the value is an enumerator, as the C++ cast does not. | Yes |
| `(bool)M` | True when the value is nonzero. (`if (M)` on an `enum class` is a clang error, as in C++.) | Yes |
| `switch (M)` with `using enum EMood;` | Works, `using enum` included. A value that matches no case falls through past the switch, as in C++. How a switch runs: [Statements and control flow](#statements-and-control-flow). | Yes |

### UE_ENUM_MAP

| You write | What it does | Status |
|---|---|---|
| `UE_ENUM_MAP(EMood, FName, MoodNames);` | Declares the member `TMap<EMood, FName> MoodNames` with a default the compiler fills in when the mod is built: one pair per enumerator in declaration order, with the C++ name as the text. The closing `_MAX` is left out. | Yes |
| `UE_ENUM_MAP(FString, EMood, MoodsByName);` | The enum can be on either side, with FName or FString on the other. | Yes |
| `UE_ENUM_MAP(EEndPlayReason, FName, Reasons);` | Any enum the source can see works, the game's included. | Yes |
| `TMap<EMood, FName> MoodNames = UE_ENUM_MAP(EMood);` | The default alone, for a member you declare yourself: the same table. | Yes |
| `UE_ENUM_MAP(int32, FName, M);`, `UE_ENUM_MAP(EMood, EMood, M);`, `UE_ENUM_MAP(EMood, int32, M);` | Refused by clang, naming both types: "static assertion failed due to requirement '__EnumMapPair__<int, FName>': UE_ENUM_MAP(Key, Value, Name): one of Key and Value is an enum and the other FName or FString". | Refused |
| `UE_ENUM_MAP(TEnum<EMood>, FName, M);` | Refused the same way: write the enum itself, `UE_ENUM_MAP(EMood, FName, M);`. | Refused |
| `TMap<int32, FName> M = UE_ENUM_MAP(EMood);` | Refused by clang, "no viable conversion from '__EnumMapInit__<EMood>' to 'TMap<int32, FName>'": the one-argument form is the default of a TMap between that enum and FName or FString. | Refused |
| `UE_ENUM_MAP(FName, EMood);`, `UE_ENUM_MAP(FName, EMood, A, B);` | Refused by clang: "UE_ENUM_MAP takes (Enum), or (Key, Value, Name) to declare the member". Written as an initializer, `= UE_ENUM_MAP(FName, EMood)`, clang says only "expected expression". | Refused |
| `UE_ENUM_MAP(EMood)` in a function body | Not yet: refused with `unimplemented intrinsic __EnumMap__`. Keep the table as a member and read the member. | Not yet |

```cpp
UE_ENUM_MAP(EMood, FName, MoodNames);      // Calm, Angry, Sleepy
UE_ENUM_MAP(FString, EMood, MoodsByName);

FName NameOf(EMood M) { return MoodNames[M]; }
EMood Parse(FString Text) { return MoodsByName[Text]; }  // Calm for an unknown name
```

For enum to name, `TEnum` below needs no table.

### TEnum

`TEnum<E>` (UeApi `Types.h`) is `E` to the compiler: the same property and the same bytes. It converts to and from `E`,
and adds the two lookups the editor's Enum to Name and Enum to String nodes make. UeApi spells every game enum field,
return value and by-value parameter as `TEnum<E>`, so `Actor->Mode.String()` works on game data. An `E&` out-parameter
stays `E`, because an `E` variable cannot bind to a `TEnum<E>&`.

| You write | What it does | Status |
|---|---|---|
| `TEnum<EMood> Mood = EMood::Calm;` | An EMood variable, member, parameter or local. It switches, compares and assigns like a plain EMood. | Yes |
| `Mood = {};`, `TEnum<EMood>()`, `K2_DetachFromActor({}, {}, {})` | The zero enumerator, as for a plain EMood: assigned, passed (to a game function too), returned, as a struct literal's member or an array element. | Yes |
| `Mood.Name()` | The enumerator's name as an FName, from `KismetNodeHelperLibrary::GetEnumeratorName` on EMood's UEnum. | Yes |
| `Mood.String()` | Its display name as an FString, from `GetEnumeratorUserFriendlyName`: the label typed in the editor for a Blueprint enum. | Yes |
| `.Name()` / `.String()` on an `int32` or `int64` enum | Refused ("a uint8 enum only"): the engine's two lookups take a uint8. | Refused |
| `TMap<TEnum<EMood>, TSubclassOf<AActor>> Spawns;` | A map keyed by EMood, its value any type a map takes, a template too. | Yes |
| `Moods.Num()` on a `TArray<TEnum<EMood>>`; `UE_DISPATCHER(OnMood, TEnum<EMood> M)` | A container of EMood with a container's methods, and a dispatcher whose Add and Broadcast are a dispatcher's. Only `TEnum<E>` itself has `.Name()` / `.String()`. | Yes |

```cpp
TEnum<EMood> Mood = EMood::Angry;

FString Describe() { return "Mood: " + Mood.String(); }   // "Mood: Angry"
FName MoodName(TEnum<EMood> M) { return M.Name(); }        // an EMood argument converts
```

## Structs

Any reflected struct of the game or the engine, and a mod's own `UE_STRUCT`, can be a variable, a parameter, a local, a
return value or a container element. The rule to remember: a struct is a value. Assigning it or passing it by value
copies it, as in C++.

### Struct variables and members

| You write | What it does | Status |
|---|---|---|
| `FVector Home;`, `FHitResult Hit;`, `FTransform Placed;` | A struct variable. All 2983 reflected structs in the DRG SDK can be one. | Yes |
| `Home.Z = Twice.X + 1;` | Reads and writes a member in place (Break Struct, Set Members in Struct). | Yes |
| `Nested.Inner.Kills += 1;` | Nested members work to any depth, in place. | Yes |
| `K2_GetActorLocation().X` | A member of a struct a call returns. The value is stored in a temporary and read from there. | Yes |
| `&S.Kills` | Not yet: a Blueprint variable has no address. See [Pointers and memory](#pointers-and-memory). | Not yet |

### Making struct values

| You write | What it does | Status |
|---|---|---|
| `FVector(1, 2, 3)`, `FLinearColor(1, 0.5f, 0, 1)` | A whole-struct constructor: one struct value built from the arguments. Constants, locals and the object's own variables make one literal value, as the editor's literal pin does. | Yes |
| `FVector2D(1.0f, M == 0 ? 2.0f : 3.0f)`, `{Twice(M), 1}`, `V = {V.Y, V.X}` | A member that computes anything else (a call, `?:`, `&&`, a member of a struct) makes the editor's Make Struct instead: a fresh value, then one store per member, left to right as C++ runs a braced list. The variable assigned is written after all of them, so `V = {V.Y, V.X}` swaps. | Yes |
| `FColor(255, 128, 0)` | R, G, B and A, with A 255 when left out, like the engine's constructor. Each argument lands on the member its parameter is named after, although FColor stores B, G, R, A. | Yes |
| `FLightmassDirectionalLightSettings(1.5f, 2.5f, true, 4.5f)` | A struct with a parent struct: the arguments come in C++ order, the parent's members first, and each lands on its member (the engine lists the struct's own members first). | Yes |
| `FMaterialAttributesInput(3, "In", "Ex", 0)`, `Handle = FTimerHandle();` | A struct with a member the engine marks Transient (PropertyConnectedBitmask here, FTimerHandle's one member Handle) is the Make Struct above, never a literal: the engine's literal skips that member and leaves it as the variable assigned held it, which a variable assigned again, or a local in a loop, still holds. The Make Struct sets it, to the value given or zero, so `Handle = FTimerHandle();` resets a live handle, as in C++. | Yes |
| `FVector()`, `FVector4()`, `FTransform()`, `FTransform T{};` | What the engine's constructor makes. `FVector`, `FVector2D`, `FRotator`, `FLinearColor`, `FColor` and the `FVector_NetQuantize` types, whose constructor sets nothing, are their zeros, a literal. `FVector4()` is the literal (0, 0, 0, 1), its constructor's W; so is an FVector4 declared with no initializer, each time a loop comes round, since the engine makes a fresh FVector4 variable as zeros. Any other engine struct, `FTransform()` (the identity) or `FQuat()` among them, is the editor's Make Struct with nothing set: a temporary the function's frame constructs as the engine does. So `SpawnActor<T>(Class, FTransform())` spawns at the origin at scale 1. | Yes |
| `FHitResult()`, `FStats{}` | Make Struct with nothing set, for a struct without a whole-struct constructor: the struct keeps its own defaults (`FHitResult::Time` is 1). | Yes |
| `FStats S = {.Kills = K, .Alive = true};` | Make Struct: a fresh value, then one store for each member given. Members left out keep the struct's defaults. | Yes |
| `KillsOf({.Kills = K})` | A braced value as an argument. | Yes |
| `FStats S = {K, 2.0f};` | Positional braces go by member declaration order. | Yes |
| `FStats S = {{}, 2.0f};` | A `{}` for a member is a fresh value of its type, as in C++, not the member's default: zero, an empty container, None, an engine struct's zeros, or a `UE_STRUCT`'s own defaults. | Yes |

```cpp
FVector Home;
FLinearColor Tint;
ULightComponent *Lamp;

void Paint(float D) {
  Home = FVector(1, 2, 3);
  Tint = FLinearColor(1, 0.5f, 0, 1);
  Lamp->LightColor = FColor(255, 128, 0);  // orange, A = 255
  FHitResult Hit = {.Time = 0.5f, .Distance = D};
  FTransform Identity = FTransform(FQuat(0, 0, 0, 1), FVector(0, 0, 0), FVector(1, 1, 1));
}
```

Notes:
- 944 of the 2983 structs have fields AssetGen cannot write, such as weak pointers, delegates or bitfields, so the SDK
  gives them no whole-struct constructor. Build them with braces.
- A braced value is made afresh each time the code runs, so a loop never sees the previous pass's values.
- For an identity transform, write `FTransform(FQuat(0, 0, 0, 1), FVector(0, 0, 0), FVector(1, 1, 1))` or call
  `UKismetMathLibrary::MakeTransform`.

### Struct operators

| You write | What it does | Status |
|---|---|---|
| `Home + Home * 2.0f`, `(A + B) / 2`, `Tint * 0.5f` | The Kismet math node for that pair of types (Add_VectorVector, Multiply_VectorFloat, Divide_VectorInt...). Only the pairs Kismet has exist. | Yes |
| `2.0f * V` | The float converts to a vector first, which gives the same result. | Yes |
| `Home == Twice` | For FVector, FVector2D, FVector4, FRotator and FQuat this is Kismet's Equal node with an error tolerance of 1e-4, so nearly equal values compare equal. UE C++'s FVector `==` is exact. No warning is printed. | Yes |
| `Tint == Other` | FLinearColor, FTransform and other structs use their own Kismet Equal node. FLinearColor's has no tolerance. | Yes |
| `Home += W;` | Refused by clang (`no viable overloaded '+='`): structs have no compound assignment. Write `Home = Home + W;`. | Refused |
| `-V` | Refused by clang (`invalid argument type 'FVector' to unary expression`). Call `UKismetMathLibrary::NegateVector(V)`. | Refused |
| `A + B` with two FRotators | Kismet has no such node. Stored in or returned as an FRotator it is a clang error. Call `UKismetMathLibrary::ComposeRotators(A, B)`. | Refused |
| `A == B` with two `UE_STRUCT`s | Refused by clang (`invalid operands to binary expression`). The editor has no Equal node for them either. Compare the members that matter. | Refused |

```cpp
FVector Mid(FVector A, FVector B) { return (A + B) / 2; }
FVector Away(FVector V) { return UKismetMathLibrary::NegateVector(V); }
FRotator Turn(FRotator A, FRotator B) { return UKismetMathLibrary::ComposeRotators(A, B); }
bool Same(FVector A, FVector B) { return A == B; }  // with a tolerance of 1e-4
```

Notes:
- Two FRotators with `+` are joined as strings: both convert to FString, so `auto S = A + B;` or `FString S = A + B;`
  compiles, and `S` is text, not a rotation. Only storing the result in an FRotator shows the mistake.

### Struct conversions

| You write | What it does | Status |
|---|---|---|
| `Facing = Twice;` (FVector to FRotator) | The rotation of the direction, not a copy of X, Y and Z. | Yes |
| `FTransform Placed = Home;` | A translation. | Yes |
| `FVector V = 2.0f;` | A float fills all three components. | Yes |
| `FLinearColor L = FColor(255, 0, 0);` | FColor and FLinearColor convert both ways through sRGB. | Yes |
| `FVector V = Flat;` (FVector2D) | Z is 0. | Yes |
| `FVector V = Facing;` (FRotator) | Refused by clang: left out on purpose, because a rotator is not a direction. Call `UKismetMathLibrary::GetForwardVector(Facing)`. | Refused |

Notes:
- A struct converts implicitly wherever Kismet has a conversion node, and the conversion means what that node means.

### Struct copies

| You write | What it does | Status |
|---|---|---|
| `FStats T = Stats;` | A copy. Changing `T` leaves `Stats` alone. | Yes |
| `SetKills(K, T)` with `void SetKills(int32 K, FStats &S)` | Writes through the reference into `T`, the copy. | Yes |
| `for (FStats S : Many)` | `S` is a copy; `for (FStats &S : Many)` writes the element. See [Loops](#loops). | Yes |
| `Weights.Find(Key, Out)` | `Out` receives a copy; writing it leaves the map alone. | Yes |

```cpp
// in a class; FStats is the UE_STRUCT under Mod structs: UE_STRUCT
FStats Stats;
TArray<FStats> Many;

void SetKills(int32 K, FStats &S) { S.Kills = K; }
int32 CopyToRef(int32 K) {
  FStats T = Stats;
  SetKills(K, T);  // changes T, not Stats
  return Stats.Kills;
}
void ZeroAll() {
  for (FStats &S : Many) SetKills(0, S);  // a reference writes the element
}
```

### Struct defaults

| You write | What it does | Status |
|---|---|---|
| `FFloatInterval Delay = FFloatInterval(1, 5);` | A constructor call as a member default. It is written into Class Defaults. | Yes |
| `FVector Offset = {0, 0, 50};` | Positional braces as a default. | Yes |
| `FNested Deep = {.Inner = {.Time = 1.5f}, .Stamp = 7};` | Designated braces, nested. Members left out are zero, or take their own default initializer. | Yes |
| `FHitResult Mine = {.Distance = 5.0f};` | Designated (or positional) braces of an engine struct whose header has no constructor: the members given are written, the ones left out are not, so they hold what the engine's constructor sets (`FHitResult::Time` is 1), as the editor's Make Struct leaves them. | Yes |
| `FHeld H = {.Hit = {.Distance = 5.0f, .Time = 1.0f}};` where FHeld declares `FHitResult Hit = {.Time = 0.5f};` | New braces for a struct member with an initializer of its own. The value starts as the struct's default instance, which holds that initializer, so a member both braces leave out (FaceIndex) is the engine's in either and stays unwritten. A member the initializer gives and the new braces leave out would load the initializer's value instead of the engine's: `H = {.Hit = {.Distance = 5.0f}}` is refused, naming `H.Hit.Time`. The same holds for a `UE_STRUCT` or another mod's `UE_STRUCT_IN` struct, in a struct's member initializer and in a container's element. | Yes |
| `FColor Lamp = FColor(255, 128, 0);` | FColor's argument order holds in defaults too. | Yes |
| `FStats Fresh = FStats();`, `FStats Cleared = {{}, {}};` | A `UE_STRUCT`'s `T()` is its defaults, and writes nothing. A `{}` for a member is a fresh value of its type (zero, empty, None, a `UE_STRUCT`'s own defaults), written even where the member's default differs. In a function, `Code(FStats())` and `FStats S = FStats();` are the Make Struct of its defaults, as `FStats S{}` is. | Yes |
| `FHitResult Hit = {};`, `FHeld H = {FHitResult(), 5};`, `FTransform T = FTransform();` | An engine struct by `{}` or `T()`: none of its members is written, so it holds what the engine's constructor sets (`FHitResult::Time` is 1, `FTransform()` is the identity, `FFindFloorResult()`'s HitResult has Time 1), however deep it sits in a struct value. The UeApi header's `T() = default;` says nothing of those values. Only `FVector`, `FVector2D`, `FRotator`, `FLinearColor`, `FColor` and the `FVector_NetQuantize` types, whose constructor sets nothing, are written as zeros. `FVector4` is written as (0, 0, 0, 1), its constructor's W, by `{}`, `T()` or no initializer at all, a `UE_STRUCT`'s member too: the engine makes a fresh one as zeros. A `UE_STRUCT` member with an initializer of its own, given `T()` in braces (`{FHitResult(), 5}` where the member is `FHitResult Hit = {.Time = 0.5f};`), is refused as in `UE_DEFAULTS`: see [Class defaults](#class-defaults). | Yes |

Notes:
- Every value in a default must be known when the mod is built: [Classes and variables](#classes-and-variables).

### Mod structs: UE_STRUCT

| You write | What it does | Status |
|---|---|---|
| `struct FStats { UE_STRUCT; int32 Kills; ... };` | Cooks the struct as a UserDefinedStruct asset in the mod's package, the editor's Structure asset. It can then be a member, parameter, local, container element or member of another struct. | Yes |
| `float Time = 1.0f;` inside the struct | A member initializer is the struct's default value for that member. | Yes |
| `TArray<int32> Hits;` inside the struct | A member can have any Blueprint type, containers and enums included. | Yes |
| `UE_STRUCT_IN("/Game/_MyMods/Shared");` | For a struct in a header several mods include. Only the source whose `UE_MOD_PACKAGE` is exactly that path cooks it; every other mod imports it from there. | Yes |

```cpp
struct FStats {
  UE_STRUCT;
  int32 Kills;
  float Time = 1.0f;
  bool Alive;
  UObject *Owner;
  TArray<int32> Hits;
};

struct FNested {
  UE_STRUCT;
  FStats Inner;
  int64 Stamp;
};

class Tally : public AActor {
public:
  FStats Stats;
  TArray<FStats> Many;
  FNested Deep = {.Inner = {.Time = 1.5f}, .Stamp = 7};

  int32 KillsOf(FStats S) { return S.Kills; }
  int32 Made(int32 K) {
    FStats S = {.Kills = K, .Alive = true};  // Time keeps 1.0
    return KillsOf(S) + KillsOf({.Kills = K * 2});
  }
};
```

A header that several mods include names the owner:

```cpp
// Shared.h
enum class ESystems : uint8 { Unknown = 0, Enemies = 1 };
UE_ENUM_IN(ESystems, "/Game/_MyMods/Shared");

struct FShared {
  UE_STRUCT_IN("/Game/_MyMods/Shared");
  int32 X;
};
```

Notes:
- A struct carries `UE_STRUCT` or `UE_STRUCT_IN`, never both.
- Each member is cooked under an editor-style unique name, `Kills_<n>_<32 hex digits>`, made from the struct's
  package and the member's name. Renaming a member, or moving the struct to another package, changes that name.
- With `UE_STRUCT_IN`, a path that matches no mod's `UE_MOD_PACKAGE` cooks the struct nowhere, and no message says so.

## Containers

`TArray`, `TSet` and `TMap` are Blueprint's Array, Set and Map. Their methods are the Blueprint Array, Set and Map
nodes, not UE's C++ API. The rule to remember: a container is a value. Assigning one copies it, and a container node
needs a container variable, not one a call returns.

### Container variables

| You write | What it does | Status |
|---|---|---|
| `TArray<int32> Scores;`, `TArray<FVector> Points;` | An Array variable, as a member, parameter or local. | Yes |
| `TSet<int32> Seen;` | A Set variable. | Yes |
| `TMap<FName, float> Weights;` | A Map variable. | Yes |
| `TMap<APlayerCharacter *, int32> Kills;`, `TMap<AFSDPlayerState *, float> Damage;` | A Map keyed by an object reference, such as one entry per player: `Kills[Player] += 1`, `Kills.Remove(Player)` and `for (auto &[P, N] : Kills)` work as with any key. The game replaces a player's character (see [examples/WaitForPlayer.cpp](examples/WaitForPlayer.cpp)), so an entry keyed by the old character stops matching. Key by the player state, or by its `GetPlayerName()` as an `FString`, to keep one entry per player. | Yes |
| `TArray<TSoftClassPtr<AActor>> Kinds;` | The element type can be any Blueprint type: numbers, strings, enums, structs, objects, class and soft references, interfaces. | Yes |
| `TMap<FName, TArray<FName>> Groups;` | A container of containers, through a generated wrapper struct. See below. | Yes |
| `UE_REPLICATED(TSet<int32>, Seen);` | Refused (`a TMap or TSet does not replicate`): [Replication](#replication). | Refused |

### Container methods

| You write | What it does | Status |
|---|---|---|
| `Scores.Add(7)` | A method is the Blueprint node, with the container as its target. The methods are listed under the table. `Add` returns the new index. | Yes |
| `Scores.Num()` | The same as `Length()`. | Yes |
| `Weights.Find("alpha", W)` | Results come back through reference parameters: `Find(Key, Out)` on a map, `Get(Index, Out)`, `Keys(OutArray)`, `Random(OutItem, OutIndex)`, `Seen.Union(Other, Result)`. | Yes |
| `Seen.ToArray(List)`, `Weights.Keys(Names)`, `GetAllActorsOfClass(C, Found)` | An array an engine function only fills is emptied just before the call, as the editor does, so afterwards it holds exactly what the call put there: `ToArray` itself would add to what `List` held. An array the function also reads (`UPARAM(ref)`, such as RunAssetsThroughFilter's) is passed as it is. | Yes |
| `Items.Remove(2)` | Removes the element at index 2 (the Remove Index node). UE C++'s `Remove(Item)` removes by value; this does not. | Yes |
| `Items.RemoveItem(2)` | Removes the elements equal to 2 (the Remove node). | Yes |
| `Items.Find(5)` | The index of 5, or -1. | Yes |
| `Items.Insert(9, 0)` | The item, then the index. | Yes |
| `Items.Set(0, 9, true)` | The index, the item, then `bSizeToFit`. | Yes |
| `A.Identical(B)` | Whether two arrays hold the same elements. `A == B` is refused by clang: containers have no `==`. | Yes |
| `Items.Empty()`, `Items.Sort()`, `Items.Pop()` | Refused by clang. `Empty`, `Emplace`, `FindOrAdd`, `IsEmpty`, `Last`, `Pop`, `Sort` and the like are UE C++, not Blueprint nodes, and the SDK does not declare them. | Refused |
| `Off = Active - Wanted;`, `A + B`, `A & B` on TSets | Set_Difference, Set_Union and Set_Intersection, the result in a temp that is the value. Both sides must be set variables or another of these (`(A + B) - C`). | Yes |
| `Seen[0]` on a TSet | Refused by clang: a set has no `[]`. | Refused |
| `GetItems().Num()` | Refused (`a container operation needs a variable, not a computed value`). Store the result in a local first. | Refused |
| `TArray<int32> R = GetItems(); return R.Num();` | The local stays a variable, read once or not: a container function reads its container where it lies, so the call is never folded into its place. The same for a parameter of an `inline` function. | Yes |

```cpp
TArray<int32> Scores;
TSet<int32> Seen;
TMap<FName, float> Weights;

void Use() {
  int32 I = Scores.Add(7);       // the new index
  float W = 0;
  if (Weights.Find("alpha", W))  // the value comes back through W
    Seen.Add(I);
  TArray<FName> Names;
  Weights.Keys(Names);
  int32 First = 0;
  Scores.Get(0, First);
  Scores.Remove(0);              // the element at index 0
  Scores.RemoveItem(7);          // the elements equal to 7
}
```

Notes:
- `TArray`: `Add`, `AddUnique`, `Append`, `Clear`, `Contains`, `Find`, `Get`, `Identical`, `Insert`, `IsValidIndex`,
  `LastIndex`, `Length`, `Random`, `RandomFromStream`, `Remove`, `RemoveItem`, `Resize`, `Reverse`, `Set`, `Shuffle`,
  `Swap`.
- `TSet`: `Add`, `AddItems`, `Clear`, `Contains`, `Difference`, `Intersection`, `Length`, `Remove`, `RemoveItems`,
  `ToArray`, `Union`.
- `TMap`: `Add`, `Clear`, `Contains`, `Find`, `Keys`, `Length`, `Remove`, `Values`.
- On a `TArray<int32>`, both `Remove(Value)` and `Remove(Index)` compile. C++ code that removes by value removes by
  index here.
- The "needs a variable" rule also applies to `[]` and to range-for. A braced list, an inline class variable and an
  inline function's result already count as variables.

### Indexing

| You write | What it does | Status |
|---|---|---|
| `Scores[0] = Scores[0] + 1;` | `A[i]` is the Get (a ref) node: it reads or writes the element in place. | Yes |
| `Points[0].Z = 5;`, `Grid[0][0] = 7;` | A member or a nested array of an element changes in place. | Yes |
| `float W = Weights["alpha"];` | A map read is the Find node into a temporary: a copy of the value, or the value type's default (0, empty, None, null) for a missing key. It never adds the key. | Yes |
| `Counts[Key] = Seed;` | A map store is the Add node: insert or replace. | Yes |
| `Counts[Key] += 2;`, `Local[7]++` | Reads with Find, then stores with Add. On an empty map, `Local[7]++` leaves 7 -> 1. | Yes |
| `Table[NextKey()].Inner.Kills = K;` | Blueprint has no reference to a map element, so this becomes: copy the element, set the member, store it back. The value is evaluated first, as C++17 does, and the key only once. A missing key is added. | Yes |
| `Lists[K].Add(X);` | A container method on a map element runs on a copy of the element. A method that writes stores the copy back; a read such as `Num` or `Contains` stores nothing. | Yes |
| `AddTo(Table[1].Inner.Kills, 10)` with `void AddTo(int32 &V, int32 By)` | Warns: `V` gets a copy of the map element's member, stored back after the call. See [Functions](#functions). | Warns |

```cpp
TMap<FName, int32> Counts;
TMap<int32, TArray<int32>> Lists;

void Count(FName Key, int32 X) {
  Counts[Key] += 1;           // 1 for a new key
  int32 N = Counts["alpha"];  // 0 for a missing key, which is not added
  Lists[X].Add(N);            // on a copy, stored back
}
```

Notes:
- `Lists[K].Add(X)` prints no warning, because the Add node runs none of the mod's code while it holds the copy. Its
  arguments are evaluated before the element is copied.

### Copies and braced lists

| You write | What it does | Status |
|---|---|---|
| `TArray<int32> T = Items; T.Add(K);` | Assigning a container copies it. `Items` is unchanged. | Yes |
| `Items.Append(Twice());` | A container that a call returns, passed to `Append` or `Union`, is stored in a local first. | Yes |
| `TArray<int32> Primes = {2, 3, 5};` | A braced member default, written into Class Defaults element by element. | Yes |
| `TMap<FName, int32> Cost = {{"Gold", 5}};` | A map's elements are `{key, value}` pairs. | Yes |
| `TMap<FString, int32> M = {{"a", 5}, {"A", 1}};`, `TSet<int32> S = {3, 3};` in a default | A key given twice is written once, where it first appears, holding the last element given, as C++'s value holds it. An FString key compares without case, as an FName does, so this M is `{"A": 1}`. | Yes |
| `TArray<FVector> Points = {{1, 2, 3}, FVector(4, 5, 6)};` | Struct elements can be braces or constructor calls. | Yes |
| `TArray<int32> Rolls = {UKismetMathLibrary::RandomInteger(3)};` | Refused: every element of a default must be known when the mod is built. See [Classes and variables](#classes-and-variables). | Refused |
| `TArray<int32> L = {4, 5, 6};` in a function | The Make Array node: a temporary filled at once, made afresh each time the code runs. | Yes |
| `TSet<int32> S = {1, 2};`, `TMap<int32, float> M = {{1, 0.5f}};` in a function | Make Set and Make Map. | Yes |
| `Items = {};`, `Count({})`, `TArray<int32>()`, `return {};` in a function | An empty container: a Make Array (Set, Map) with no element. | Yes |
| `Size(TArray<int32>{1, 2, M})`, `TSet<int32>{1, M}`, `TArray<int32>({1, 2})` in a function | The typed list, braced or in parentheses: the same Make Array (Set, Map) as the untyped braces. | Yes |

```cpp
TArray<int32> Primes = {2, 3, 5};
TMap<FName, int32> Cost = {{"Gold", 5}};
TArray<FVector> Points = {{1, 2, 3}, FVector(4, 5, 6)};
TMap<FName, FVector> Spots = {{"home", {7, 8, 9}}};

int32 LocalList(int32 I) {
  TArray<int32> L = {4, 5, 6};  // Make Array, each time this runs
  return L[I] + L.Num();
}
```

Notes:
- A set or map list is written exactly as listed, duplicates included. What the engine makes of a duplicate has not
  been checked, so keep such lists free of them.

### Containers of containers

| You write | What it does | Status |
|---|---|---|
| `TArray<TArray<int32>> Grid;`, `TMap<FName, TArray<FName>> Groups;` | Blueprint has no container of containers, so the inner container becomes the `Value` member of a generated wrapper struct, such as `FNC_TArray_FName`. The C++ reads as usual. | Yes |
| `Grid[0].Add(5);`, `Grid[0][0] = 7;` | An array's inner container changes in place. | Yes |
| `TArray<FName> Again = Groups["first"];` | A map's value is read as a copy, like any map read. | Yes |
| `Groups.Find(Key, Out)` with a container `Out` | Goes through a wrapper temporary; `Out` gets the inner container. | Yes |
| `UE_REPLICATED(TArray<TSet<int32>>, Sets);` | Refused: a container that holds a TMap or TSet does not replicate. See [Replication](#replication). | Refused |

```cpp
TMap<FName, TArray<FName>> Groups;
TArray<TArray<int32>> Grid;

void Build(TArray<FName> Members, TArray<int32> Row) {
  Groups.Add(FName("first"), Members);
  TArray<FName> Again = Groups[FName("first")];  // a copy
  Grid.Add(Row);
  Grid[0].Add(5);                                // in place
  Grid[0][0] = 7;
}
```

Notes:
- The wrapper structs are written to one fixed package folder, `/Game/_ElytrasMods/_NestedContainerStructs`, which
  every mod shares, whatever its `UE_MOD_PACKAGE`. It lies outside the mod's own folder, so the compiler writes it
  relative to the `Content` folder the output is in. bpbuild puts the mod's whole staging tree into its pak, so the
  wrappers go in with the mod. If you pack by hand, pack them too.
- Other Blueprints see the member as a container of the wrapper struct.

### Walking a container

| You write | What it does | Status |
|---|---|---|
| `for (int32 P : Scores)`, `for (auto &[K, V] : Counts)` | Range-for over a TArray, TSet or TMap: [Loops](#loops). | Yes |

## Operators

This section covers the C++ operators in a function body: arithmetic, bitwise, comparison, logical, conditional and
assignment operators, and the order in which their parts run. Each operator becomes the Kismet math node the editor
places for the same types, and the type is the one C++ gives after its usual promotions. The rule to remember:
Blueprint math never traps. Integer overflow wraps, and a division by zero gives 0.

### Arithmetic

| You write | What it does | Status |
|---|---|---|
| `Kills * 3 + Assists - 1` | The editor's +, - and x nodes for the operand type (Add_IntInt, Subtract_Int64Int64, Multiply_FloatFloat). C++'s promotions pick the type: a uint8 or bool operand becomes int, int32 mixed with int64 is int64, and anything mixed with float is float. | Yes |
| `X + 1` with X at the int32 maximum | Wraps around (two's complement), for int32 and int64 alike. C++ leaves signed overflow undefined. | Yes |
| `Players / Squads`, `Players % Squads` | The / and % nodes (Divide_IntInt, Divide_Int64Int64, Divide_FloatFloat, Percent_IntInt). Integer division truncates toward zero and the sign of % follows the dividend, as in C++: -7 / 3 is -2 and -7 % 3 is -1. | Yes |
| `Hits / Shots` with Shots at 0 | Gives 0 and logs a script warning ("Divide by zero", or "Modulo by zero" for %). The mod does not crash, but it quietly gets 0 where C++ would trap or give inf or NaN. | Yes |
| `Id % Count` on int64 | Refused ("unimplemented binary operator % on Int64Int64"): UE 4.27 has no int64 % node. `%=` on an int64 is refused the same way. Write `Id - (Id / Count) * Count`. | Refused |
| `-X`, `-F` | An integer is `0 - X`. A float is `-0.0 - F`, so the sign of zero flips as C++'s `-F` does. A literal such as `-5` is the constant itself. | Yes |
| `+X` | Compiles to X. | Yes |

Notes:

- A uint8 result stored back into a uint8 wraps as in C++: 255 + 1 stored in a uint8 is 0.
- double, int16 and uint32 are not Blueprint types, and a parameter of one of them is refused. See [Types](#types).
- `++`, `--` and the compound assignments are under Assignment and updates below.

### Bitwise operators and shifts

| You write | What it does | Status |
|---|---|---|
| `(A & 0xFF) \| (B ^ 1)` | The Bitwise AND, OR and XOR nodes (And_IntInt, Or_IntInt, Xor_IntInt, or the Int64Int64 versions). | Yes |
| `Any \|= Hit`, `A & B`, `A ^ B` on bools | The OR, AND and XOR Boolean nodes (BooleanOR, BooleanAND, BooleanXOR). Both sides run, as in C++. A constant side decides the value or drops out, but a side that calls something still runs. Under UE_NO_OPTIMIZE the bools are promoted to int as in C++ (Conv_BoolToInt) and the result converted back (Conv_IntToBool). | Yes |
| `~B & 0xFF` | `~` is the Bitwise NOT node (Not_Int, Not_Int64). | Yes |
| `Hi << 16` | Kismet has no shift node. `X << N` is a multiply by 2^N, which wraps as a shift does. | Yes |
| `X >> 1` | Clears the low N bits and then divides, so it floors as C++ does: -3 >> 1 is -2 and -7 >> 1 is -4. A uint8 is promoted to int first, so 255 >> 1 is 127. | Yes |
| `X << 3LL`, `X << SHIFT` | The amount may be any integer literal, with any suffix, or a macro that expands to one. The width comes from the left side alone, so `int32 >> 1LL` is int32 math. | Yes |
| `1 << kShift` | When both sides are constants the whole shift folds to a constant before the amount is checked, so a named amount works here. | Yes |
| `X << 32` | Refused ("bit shift amount out of range"). The amount must be 0..31 for an int32 and 0..63 for an int64 left side. | Refused |
| `X << N`, `X <<= N` with a variable N | Refused ("bit shift with a non-constant amount"): Kismet has no shift node, and a run-time amount cannot become a constant multiplier. Multiply or divide by a power of two instead. | Refused |
| `X << kShift`, `X >> (2)` | Refused today with the same message, although the amount is a constant: only a bare integer literal is read as the amount. The same goes for a constexpr or const variable, a const local, an enumerator, a cast such as `(int32)3`, `sizeof(...)`, a template parameter and an inline function's parameter, even when the caller passes a literal. Write the literal. | Not yet |

```cpp
static constexpr int32 kShift = 3;
int32 Pack(int32 Hi, int32 Lo) { return (Hi << 16) | (Lo & 0xFFFF); }
int32 Half(int32 X) { return X >> 1; }                    // -3 >> 1 is -2, as in C++
int32 Eight(int32 X) { return (X << 3) | (1 << kShift); } // 1 << kShift folds to 8
```

Notes:

- There is no unsigned (logical) shift, because uint32 is not a Blueprint type.
- `X << -1` gets the non-constant message, not the out-of-range one. Clang adds its own warnings for both
  (-Wshift-count-negative, -Wshift-count-overflow).

### Comparisons

| You write | What it does | Status |
|---|---|---|
| `Health < 25.f`, `A >= B`, `A == B` | The comparison node for the operand type: Less_IntInt, GreaterEqual_FloatFloat, EqualEqual_Int64Int64 and so on. This covers numbers and bools. | Yes |
| `M < EMood::Calm` | An enum compares through its underlying type. A uint8 enum uses the Byte nodes (Less_ByteByte), and an int64-backed enum compares all 64 bits. | Yes |
| `A == this`, `P == nullptr` | Object identity, the Equal and Not Equal (Object) nodes. How `P != nullptr` differs from `if (P)` is in [Working with other objects](#working-with-other-objects). | Yes |
| `A < B` on object pointers | Refused ("unimplemented binary operator < on ObjectObject"): Kismet has only == and != for objects. | Refused |
| `A == B` on FString or FName | Compares case-insensitively, so "abc" == "ABC" is true for both. See [Strings and text](#strings-and-text). | Yes |

### Logical operators and ?:

| You write | What it does | Status |
|---|---|---|
| `!bFiring` | The NOT Boolean node. Many negations cost nothing instead; see [The optimizer](#the-optimizer). | Yes |
| `X != 0 && 10 / X > 2` | C++ short-circuiting is kept: the right side's calls, reads and divisions run only when C++ would run them, so this never divides by zero. `\|\|` runs its right side only when the left is false. | Yes |
| `X > 0 ? X * 2 : X < -5 ? -1 : 7` | Only the chosen arm runs. The operator compiles to an if/else that stores into a temp, or straight into the variable being assigned, and `return C ? A : B;` becomes two returns. Nested ternaries work. | Yes |
| `(C ? Left : Right) = 5` | Refused ("assignment to ConditionalOperator"): Blueprint has no reference to whichever variable the condition picks. Write `if (C) Left = 5; else Right = 5;`. | Refused |
| `Add5(C ? A : B)` into a `T&` parameter | The callee gets a copy, stored back into the picked variable after the call, with a warning. See [Functions](#functions). | Warns |
| `A = 1, B = 2;`, `++I, --J` | The comma operator as a statement: each side runs in turn, as two statements. A side that does nothing (`I, J++`) is dropped. A declaration of several variables, `int32 I = 0, J = N;`, is not the comma operator and works too. | Yes |
| `if (bool bOk; Get(Out, bOk), !bOk)`, `switch (N += 10, N)` | An if or switch condition with a comma: the left side runs once, before the test, as an init-statement does, and the right side is the condition. This is how an out-parameter call is tested in one line. | Yes |
| `int32 N{(Bump(), M)};`, `F((Bump(), M))`, `return (N += 1, N * 2);`, `switch (Bump(), T)` over a `TEnum<E>`, `S = {(Bump(), M), 2}` | A comma inside a statement's expression: the left side runs first, as a statement of its own, and the comma is worth its right side, read where C++ reads it. This holds wherever nothing in the statement runs before the comma: an initialiser, a return value, an assignment's right side, an argument, a braced list's first member. Beside another argument that is no constant (C++ leaves their order open, a parenthesised constructor's `FVector(Get(), (Bump(), M), 0.f)` included; braces fix theirs) the whole argument holding the comma runs first, into a temporary. A left-out argument whose default is a constant counts as that constant. Passed to a reference parameter, `const T&` included, the comma is its right side's variable itself, which the callee reads when it runs, after every argument: only the left side runs first, and the right side must be a variable (see the refusal below). | Yes |
| `while (Next(X), X > 0)`, `while ((Bump(), N) < 5)`, `A && (Bump(), B)`, `C ? (Bump(), X) : Y`, `Obj->F((Bump(), M))` | Where no statement before the one holding the comma fits, the comma runs in place: its left side, then a copy of its right side, read right after it. That is a loop condition, which runs it again on every trip; the right side of `&&` / `\|\|` and an arm of `?:`, which run it only when C++ does; and an argument of a call on another object, whose object C++ evaluates first. Passed to a reference parameter there, `while (IncRef(0, (Bump(), N)) < 20)`, it is the right side's variable itself, which must then be a plain variable. | Yes |
| `{G(), (Bump(), M)}`, `L[(Bump(), I)] = G()` | Refused ("the comma operator after something its statement runs first ..."). C++ runs G first: braces evaluate their members in order, and an assignment's right side runs before its left. The comma's left side would run before G. Write the left side as a statement of its own. | Refused |
| `while ((Bump(), S).X < 3)` | Refused ("the comma operator used as a place, not a value ..."): a member taken of the comma, or the comma bound to an operator's or a constructor's reference, needs the place itself, and a loop condition has no statement before it to hold the left side. Write the left side as a statement of its own. A comma assigned to or updated there works, and so does a member or an element of one, `while (((Bump(), S).X += 1) < 3)`: see [Assignment and updates](#assignment-and-updates). | Refused |
| `F(G(), (Bump(), L[I]))` where F takes `int32&` or `const int32&`, `Get() + (Bump(), L[0])` over FString | Refused: a comma bound to a reference beside an argument that may run first must end in a variable. That covers a call's reference parameter and an operator's reference operand (FString's `+` takes `const FString&`; the message names the operator). A temporary would not be the place the callee writes or reads, and `L[I]` located again after G would not be the one C++ binds. In a loop condition, the right of `&&` / `\|\|` / `?:` or a call on another object, a comma passed to a reference parameter must end in a variable even alone. | Refused |
| `(Bump(), M) * (G() + 0.5)` | Refused for the double math (`no Kismet conversion from float to double`), as `M * (G() + 0.5)` is. The comma runs first into a temporary, and the conversion to double stays outside it. A comma whose own value is a type no Blueprint variable holds is refused naming that type. | Refused |
| `int32 D = (1, 4);` as a class default | The right side, when the left side does nothing. A left side that does something would run when the game builds the object, which a default cannot: refused as any computed default. | Yes |

Notes:

- The editor's AND and OR nodes evaluate both inputs. AssetGen always short-circuits, so a Blueprint graph ported to
  C++ runs its right side only when it is needed. When the right side provably cannot fault or act (literals,
  locals, parameters, this object's fields, arithmetic and comparisons of those, division by a non-zero literal),
  `&&` and `||` compile to one AND or OR node, because running it anyway cannot be observed.
- The editor's Select node evaluates every option. A `?:` runs a call, an inline expansion or a pointer read in an
  arm only when that arm is picked.

### Assignment and updates

| You write | What it does | Status |
|---|---|---|
| `Total = Kills * 3;`, `Other->Score = 10;` | The Set node, on a local, a member or another object's field. A replicated property also gets the editor's FlushNetDormancy and RepNotify around the write; see [Replication](#replication). | Yes |
| `Slots[I] = Value;` | Set Array Elem. | Yes |
| `Scores[Name] = 5;` | The map Add node, which replaces an existing value. See [Containers](#containers). | Yes |
| `Spots[Name].X = 1;` | A store into a map value's member is copied out, changed and stored back, as `T E = Map[K]; E.X = 1; Map[K] = E;`, with the key evaluated once. | Yes |
| `Acc += I;`, and `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | `X = X op Y`: a math node and a Set. X is located once, so `Slots[NextSlot()] += By` calls NextSlot a single time, and Y is evaluated first. It works on everything plain `=` works on, map values and their members included. `<<=`, `>>=` and int64 `%=` follow the rules of the plain operators. | Yes |
| `int32 Z = (Y += X) * 10;` | A compound assignment used as a value is X after the store. | Yes |
| `IncRef(1, B += 1)`, `Peek(SetB(), ++B)`, where the parameter is `int32&` or `const int32&` | Passed to a reference parameter, a compound assignment or a prefix `++` / `--` is the variable itself, as a plain `=` is: the update runs first, as a statement, and the callee reads, and for `T&` writes, B after every argument. Its left side must be a plain variable (`IncRef(1, L[Idx()] += 1)` is refused). The same holds in a loop condition (`while (IncRef(0, B += 1) < 20)`), on the right of `&&` / `\|\|` or in `?:`, and in a call on another object (`P->IncRef(1, B += 1)`). There the call runs as one block each time C++ runs it: the object first, then the update, then the call on B. Behind something its statement runs first, `{G(), IncRef(0, B += 1)}`, it is refused, since the update would run before G. | Yes |
| `X++`, `++X`, `X--`, `--X` | The Increment Int and Decrement Int macros, with X located once. Postfix gives the value before the store, prefix the value after. Works on int32, int64, float and uint8 (a uint8 wraps at 255). On a raw pointer it steps by the element size; see [Pointers and memory](#pointers-and-memory). | Yes |
| `A = B = 0;`, `S1 = S2 = Name;`, `if ((X = Next()) > 3)`, `return A = N;` | A plain `=` used as a value: the assignment runs first, as a statement, and the value is the variable it wrote, read after it. Numbers, strings and structs alike. It works wherever the comma operator does, and like it, passed to a reference parameter (`const T&` included) it is that variable itself, with no copy. In a loop condition (`while ((V = Next()) > 0)`, `while (UsePair(T = S) < 100)` with a struct passed by value), on the right of `&&` / `\|\|` / `?:` or in an argument of a call on another object it runs in place, as the comma does there; a member taken of it in those places, `while ((T = S).X < 9)`, is refused ("an assignment used as a place, not a value ..."). Behind something its statement runs first, `{G(), (A = M)}`, it is refused ("an assignment used as a value after something its statement runs first ..."). The left side must be a plain variable (`A = L[0] = 1` is refused): it is read again. A comma there names one, so `int32 X = ((Bump(), N) = G());` is `(Bump(), N) = G();` then `X = N`, and `((Bump(), T).A = G())` reads T.A. | Yes |
| `(Bump(), N) += 1`, `while (++(Bump(), N) < 5)`, `(Bump(), N) = G();`, `(N = Next()) += G()` | A comma or a plain `=` assigned to or updated writes the variable it names: the comma's right side, or the `=`'s left side after the assignment. The outer assignment's right side runs first, as C++17 orders it, then the comma's left side (or the inner assignment), then the store: `(Bump(), N) = G()` gives N what G returned before Bump ran; a struct's or FString's `=` reads an element on its right through its reference, so `(Bump(), T) = L[Count]` locates L[Count] before Bump and reads it after. In a loop condition, on the right of `&&` / `\|\|` and as a statement alike. A member or an element of one is the same member or element of the variable, its index run after the comma: `while (((Bump(), T).A += 1) < 5)`, `((Bump(), L)[0] += 1)`, `(Bump(), L)[Idx()] = G();` (G, then Bump, then Idx). The inner `=`'s left side must be a plain variable, or a comma that names one (`((Bump(), N) = G()) += 1`); `(L[Idx()] = M) += 1` is refused: it is named twice. An update or a struct's `=` there is the same: `while (((N += G()) += 1) < 20)` is `N += G(); N += 1` on every trip, `++(N += 1)` steps N twice, `(N += G()) = N + 5;` reads N + 5 first, and `while (((T = S).A += 1) < 5)` copies S, then updates T.A. `(L[Idx()] += M) += 1` is refused the same way. | Yes |

### Evaluation order

| You write | What it does | Status |
|---|---|---|
| `Slots[Cursor] = NextSlot();` | The right side of `=` or `op=` runs first, then the destination (object, array, index, map key) is located, as C++17 requires. This stores at the new Cursor. | Yes |
| `GetPeer()->SetCursor(SwapPeerInline());` | The object a call runs on is evaluated before its arguments. When an argument needs statements of its own that could change the object, the object is saved into a local first. The same holds for `E1[E2]`, a container method's container and a dispatcher's Broadcast. | Yes |
| `Bump() * 100 + BumpInline()`, `Pair(Bump(), BumpInline())` | An operand or argument that needs statements (an inline call, `&&`, `\|\|`, `?:`, a `++` or `op=` value, a pointer read) runs before the whole expression or call. C++ leaves this order unspecified. | Yes |
| `FIntPoint P = {Bump(), M ? N : 0};` | A struct literal's members run left to right, each once, as C++ requires of braces: each computed member is its own statement. | Yes |

```cpp
TArray<int32> Slots;
int32 Cursor;
int32 N;
int32 NextSlot() { return ++Cursor; }
void StoreAtCursor() { Slots[Cursor] = NextSlot(); }   // stores at the new Cursor
void BumpSlot(int32 By) { Slots[NextSlot()] += By; }   // NextSlot() runs once
int32 Bump() { return ++N; }
inline int32 BumpInline() { N += 10; return N; }
int32 Operands() { return Bump() * 100 + BumpInline(); }   // BumpInline's body runs before Bump()
int32 Pair(int32 A, int32 B) { return A * 100 + B; }
int32 Args() { return Pair(Bump(), BumpInline()); }        // A is 11, B is 10
```

Notes:

- Do not rely on left-to-right side effects inside one expression. From N = 0, `Operands()` and `Args()` above both
  return 1110, because BumpInline's body runs before Bump(). Arguments run left to right only when none of them needs
  statements of its own. Split such code into statements.
- One case is not covered. An rvalue argument bound to a reference parameter is stored into a local ahead of the
  call without pinning the call's object, so if computing that argument changes what the object expression returns,
  the call goes to the new object.

## Statements and control flow

This section covers conditions, `if`, `switch`, jumps and the statements a body may hold. Every construct compiles to
plain jumps inside the function, so nothing is left on Blueprint's flow stack and a return from any depth is clean.
The rule to remember: a statement is a declaration, a call, an assignment or update, or a control statement.

### Conditions

| You write | What it does | Status |
|---|---|---|
| `if (Target)` | An object pointer is the Is Valid node, so an object that is pending kill counts as false. `Target != nullptr` can still be true for it; see [Working with other objects](#working-with-other-objects). | Yes |
| `if (Count)` | An int32 goes through Conv_IntToBool. | Yes |
| `if (F)` on a float, an int64, a byte or an enum | Compared with zero by a Not Equal node, so a NaN counts as true. | Yes |
| `(bool)F + X`, `bool IsSet(int32 V) { return V; }` | A number converts to bool the same way outside a condition: true when nonzero, as in C++. `(bool)F + X` adds 0 or 1. | Yes |
| `if (Tag)` | An FName is `Tag != None` (NotEqual_NameName). `!Tag` and `static_cast<bool>(Tag)` work too. | Yes |
| `Target && Target->CustomTimeDilation > 0` | Works as a guard: the Is Valid test decides, through the short-circuit, whether the read runs. | Yes |

An interface value is true when an object is behind it, a plain null test in which a pending-kill object still counts
([Interfaces](#interfaces)). A raw pointer is true when its address is nonzero
([Pointers and memory](#pointers-and-memory)).

### if

| You write | What it does | Status |
|---|---|---|
| `if (Health <= 0) Die(); else Heal();` | The Branch node, compiled to a conditional jump. `else if` chains work. The condition converts to bool as described under Conditions. | Yes |
| `if (int32 Twice = V * 2; Twice > 10)` | The init-statement runs once, before the test. An expression works as well: `if (N++; N > 3)`. | Yes |
| `if (APawn *P = Cast<APawn>(Obj))` | A declaring condition: P is made once and tested. | Yes |
| `if constexpr (sizeof(T) == 8)` | Only the branch clang kept is compiled, and no jump is left behind, in a UE_NO_OPTIMIZE function too. | Yes |

Notes:

- A variable declared in a condition or an init-statement stays a function-wide local afterwards; see
  [Locals](#locals). Correct C++ cannot tell.
- A member template has no function of its own. Each instantiation a call names is inlined; see
  [Inline functions and templates](#inline-functions-and-templates).

### switch

| You write | What it does | Status |
|---|---|---|
| `switch (Code)` on an int32, uint8, bool or enum | The editor's Switch on Int, Byte or Enum. The value is evaluated once, and the case bodies are compiled in source order, so fallthrough, `[[fallthrough]]`, `default` in any position, `break` and `continue` behave as in C++. | Yes |
| `switch (Id)` on an int64 | A chain of full 64-bit compares. | Yes |
| `switch (int32 K = V + 1; K)` | An init-statement or a declaring condition runs once, then the plain switch. | Yes |
| `switch (UE_NAME_SWITCH(Kind))` | The editor's Switch on Name, shown below. | Yes |
| `case 1 ... 3:` | GNU case ranges are refused ("unimplemented statement IntegerLiteral"). List the cases. | Refused |
| `switch (X) case 1: return 1;` | Refused ("`switch` needs a braced body"). | Refused |
| `case 0: { R = 1; case 1: R += 2; }` | A case label inside a nested block is refused ("unimplemented statement CaseStmt"). Every case label must sit directly in the switch body. | Refused |

```cpp
int32 Score;
void Grade(int32 Code) {
  switch (Code) {
  case 1: Score = 10; break;
  case 2:
  case 3: Score = 20;   // falls through
  case 4: Score += 5; break;
  default: Score = -1;
  }
}
int32 NameKind(FName Kind) {
  switch (UE_NAME_SWITCH(Kind)) {
  case UE_NAME_CASE("IntProperty"): return 1;
  case UE_NAME_CASE("FloatProperty"):
  case UE_NAME_CASE("DoubleProperty"): return 2;
  default: return 0;
  }
}
```

Notes:

- A dense run of three or more cases, spread over at most four times their count, dispatches through a jump table.
  Other cases cost one compare each. An int64 or FName switch never uses a table.
- On a uint8, bool or byte enum, a case outside 0..255 (-128..127 for int8) can never match, so it is left out of the
  tests. Its code is reached only by falling through into it, as in C++.
- `using enum` and `[[likely]]` inside a switch are fine.
- C++ switches only on integers, so UE_NAME_SWITCH and UE_NAME_CASE give clang a switch over hashes of the case
  texts. AssetGen stores the name once and compares it with each case (NotEqual_NameName), case-insensitively, as
  FName comparison is. Fallthrough and `default` work as in any switch.
- Every case of a name switch must be `UE_NAME_CASE("literal")`, or the switch is refused ("a case of UE_NAME_SWITCH
  needs UE_NAME_CASE"). Two identical texts are a duplicate-case error from clang. Two texts that differ only in case
  compile, but only the first can ever match.

### Jumps

| You write | What it does | Status |
|---|---|---|
| `break;`, `continue;` | Plain jumps. `break` leaves the innermost loop or switch. `continue` goes to a for loop's increment, a do-while's test, or the loop around a switch. Inside an inline function's body they belong to that body's own loops. | Yes |
| `goto done;` | A jump, backward or forward, which can leave any number of loops at once. The editor's nearest equivalent is a wire looping back. Each expansion of an inline function gets its own labels. A declaration that a goto reaches again is made again, as one in a loop is. | Yes |
| `return Found;`, `return;` | The Return Node. `return C ? A : B;` becomes two returns. In an inline function, return jumps to the end of the expanded body. Inside a TMap loop that writes values back, the values are stored first. | Yes |
| `return Log(Errors, "bad");` in a void function | A void call, then a plain return, as C++ runs it. `return (void)X;` with an X that does nothing is a plain return. | Yes |
| `goto` inside a TMap loop that writes values back | Refused; see [Loops](#loops). | Refused |

```cpp
int32 SumBelow(int32 N) {
  int32 I = 0, Sum = 0;
again:
  if (I >= N) goto done;
  Sum += I;
  I += 1;
  goto again;
done:
  return Sum;
}
```

Notes:

- A function that contains a goto, or expands an inline function that does, skips some optimizer passes. What it
  does is unchanged.
- A value-returning function that can fall off its end compiles, with clang's -Wreturn-type warning. Return a value
  on every path.

### Statements and blocks

| You write | What it does | Status |
|---|---|---|
| `{ int32 Tmp = A; A = B; B = Tmp; }` | A bare block. Its statements join the enclosing list, and nothing is destroyed at the closing brace. | Yes |
| `;`, `Done: ;`, `[[fallthrough]];` | Compile to nothing. | Yes |
| `case Angry: [[likely]] return N;` | `[[likely]]` and `[[unlikely]]` are hints for another compiler. Only the statement they mark is compiled. | Yes |
| `static_assert`, `using`, `typedef` in a body | Compile-time only. A local cannot yet use a type alias declared in the function; see [Locals](#locals). | Yes |
| `(void)Compute();` | The call. | Yes |
| `C ? Open() : Close();`, `bReady && Launch();`, `X + 1;` | Refused ("unimplemented statement ConditionalOperator", or BinaryOperator): an expression statement must be a call, an assignment or an update. Write the if out. | Refused |
| `(void)Unused;` | Refused ("unimplemented statement DeclRefExpr"). To silence an unused-parameter warning, leave the parameter unnamed. | Refused |
| `try { Risky(); } catch (...) { Recover(); }` | Refused ("unimplemented statement CXXTryStmt"): Blueprint has no exceptions. | Refused |

Note: `if (C);` on one line makes clang warn -Wempty-body. The build still succeeds, and putting the `;` on its own
line silences it.

## Loops

This section covers `while`, `do`, `for` and range-for over a TArray, TSet or TMap. A loop is a test and a jump back,
like the editor's loop macros but without the flow stack, so `break`, `continue` and `return` work from any depth. The
rule to remember: a range-for reads its container's length once, so do not add to or remove from a TArray inside a
loop over it.

### while and do

| You write | What it does | Status |
|---|---|---|
| `while (I < Limit && 100 / (Limit - I) > 1)` | The editor's WhileLoop without the flow stack: a test at the top and a jump back. The condition runs on every trip, with any statements it needs (an inline call, `&&`, `?:`, a pointer read). An inline function called in the condition gets fresh locals each trip. | Yes |
| `while (true)` | No test at all. Leave with `break` or `return`. | Yes |
| `while (int32 Left = Start - Steps)` | Left is made again and tested on every trip. | Yes |
| `do { I += 1; } while (I < Limit);` | The body runs once before the first test, and `continue` jumps to the test. | Yes |
| `do { R = 1; if (X > 0) break; R = 2; } while (false);` | Runs once with no test. `break` leaves it. | Yes |

Note: UE 4.27 compiles Blueprint's runaway-loop guard (1,000,000 iterations) out of Shipping and Test builds. In such
a build a loop that never ends hangs the game instead of being stopped.

### for

| You write | What it does | Status |
|---|---|---|
| `for (int32 I = 0; I < Count; ++I)` | `Init; while (Cond) { Body; Inc; }`: the editor's ForLoop, with any condition and increment. `continue` reaches the increment. Loops nest freely. | Yes |
| `for (;;)`, `for (; I < N; I++)` | A missing condition is `true`, and a missing init or increment is left out. | Yes |
| `for (int32 I = 0; int32 L = N - I; ++I)` | A declaring condition: L is made again and tested on every trip, and `continue` still reaches the increment. | Yes |
| `for (int32 I = 0, J = N; I < J; ++I, --J)` | Two variables, both updated in the increment. See [Operators](#operators) for the comma. | Yes |

### Range-for over a TArray

| You write | What it does | Status |
|---|---|---|
| `for (int32 X : Items)` | The editor's ForEachLoop: the array is walked by index, and X is a fresh copy on each pass, so assigning to X never changes the array. | Yes |
| `for (int32 &X : Items)` | X is another name for `Items[Index]`, so writes land in the array. `const T&` and `auto&&` work the same way. ForEachLoop's element is a copy, so it has no equivalent. | Yes |
| `for (auto [X, Y] : Points)` | A structured binding over TArray or TSet elements is refused ("a structured binding over a TArray"). Bind the element, `for (const FIntPoint &P : Points)`, and read `P.X`. | Refused |

```cpp
TArray<int32> Items;
void Double() {
  for (int32 &X : Items) {
    if (X < 0) continue;
    X *= 2;
    if (X > 100) break;
  }
}
```

Note: the length is read once, before the loop, as C++ reads `end()` once. Elements added in the body are not visited,
and after a removal the walk runs past the new end. Do not change the array's length inside the loop.

### Range-for over a TSet

| You write | What it does | Status |
|---|---|---|
| `for (int32 X : Seen)`, `for (const int32 &X : Seen)` | The set is copied into an array (To Array) and the loop walks the copy. The loop variable is a value or a const reference: `int32 &` is a clang error, because the SDK's TSet is read-only in a loop. Since the walk is over the copy, adding to or removing from the set in the body does not disturb it. | Yes |
| `for (const FName &Tag : Tags)` without the copy | A TSet loop always walks a copy today. The loop works either way; only the cost of the copy differs. | Not yet |

### Range-for over a TMap

| You write | What it does | Status |
|---|---|---|
| `for (auto &[Key, Value] : Scores)` | Walks the map where it lives. Value is the value in the map: a write through it is what `Scores[Key]` reads next, a `T&` binds it with no copy, and a container value changes in place. Key is a copy. `const auto &[Key, Value]` reads the same way. The 4.27 editor has no map ForEach; this replaces Keys, ForEachLoop and Find. | Yes |
| `for (auto [Key, Value] : Scores)` | By value: the keys are copied (Keys) and each value is fetched into a local (Find). Value is a copy and nothing is written back, as in C++. | Yes |
| `Scores.Remove(Key)` in the body | A body that adds to or removes from the map walks a copy of the keys instead, so removing the current key is safe. Each read of Value is then a Find and each store an Add. A container value is copied, and stored back at the end of each pass and before a `break` or `return`. | Yes |
| `for (auto &Pair : Scores)` | Refused ("a TMap range-for binds `auto [Key, Value]`"): a map loop binds exactly two names. | Refused |
| `goto` inside a TMap loop that writes values back | Refused ("a goto inside a TMap range-for that writes its value back would skip the write"), even when the label is inside the loop. A loop writes values back when it walks a copy of the keys and the body changes a container value through `auto &[Key, Value]`. Leave with `break`, or bind `const auto&`. | Refused |

```cpp
TMap<FName, int32> Scores;
void Update() {
  for (auto &[Key, Value] : Scores) Value += 10;    // in place
  for (auto [Key, Value] : Scores) Value += 1000;   // a copy: the map keeps its values
  for (auto &[Key, Value] : Scores) {               // removes: walks a copy of the keys
    if (Value < 0) { Scores.Remove(Key); continue; }
    Value += 1;
  }
}
```

Notes:

- The in-place walk needs a key and a value that align to 8 bytes or less. Otherwise, or when the body changes the
  map, the loop walks a copy of the keys as described above.
- A map that has lost elements has free slots. The in-place walk first compacts it by copying it out and back.
- `break`, `continue` and `return` work in a map loop, and so does a `return` in an inline function the body expands.
- A change is recognised by the map's type, not by the map itself: Add, Emplace, FindOrAdd, Remove,
  RemoveAndCopyValue, FindAndRemoveChecked, Empty, Reset, Append, Compact, CompactStable, Shrink, a sort, or assigning
  a whole map. A body that changes another map of the same type also switches to the copy walk. A change made inside
  a function the body calls is not seen, so do not change the map that way.
- Writing Value after removing its key puts the key back through Add. C++ would call that a dangling reference.
- An in-place walk cooks two helper structs into the mod's own folder, named after the map type, such as
  `FMapSlot_TMap_FName_int` and `FMapSlots_TMap_FName_int`. They ship with the mod like its other packages.

### What a range-for walks

| You write | What it does | Status |
|---|---|---|
| `for (int32 X : PickPeer()->Items)` | The range expression is evaluated once, into a local, as C++ binds its range once. PickPeer runs once, and reseating a pointer in the body does not move the walk. | Yes |
| `for (int32 X : GetItems())` | Refused when GetItems is a function that is not inline ("range-for needs a container variable, not a computed value"). Store the result in a local and walk that. An inline function's result is accepted: it is computed once into a local. | Refused |
| `for (int32 X : {1, 2, 3})` | Refused: the range is a std::initializer_list, not a Blueprint container. A `TArray<int32>{1, 2, 3}` written in the range is refused as well. Declare `TArray<int32> L = {1, 2, 3};` and walk L, or walk a `static inline const` array. | Refused |
| `for (int32 P : kPrimes)` | A `static inline const` array of constants; see [Constants](#constants). | Yes |
| `for (int32 Scale = 3; int32 X : Items)` | The C++20 init-statement runs once, before the loop. | Yes |

```cpp
TArray<int32> Items;
TArray<int32> GetItems() { return Items; }
int32 SumItems() {
  TArray<int32> Copy = GetItems();   // for (int32 X : GetItems()) is refused
  int32 Sum = 0;
  for (int32 X : Copy) Sum += X;
  return Sum;
}
```

## Locals

A local becomes a Local Variable of its function. A Blueprint local has no scope: it exists for the whole call,
whatever block declared it. The rule to remember: a local without an initializer starts at zero, empty or its struct's
defaults, and it is reset each time its declaration is reached again, so each loop pass gets a fresh one as in C++.
A function that makes a latent call is the one exception, described under Declaring locals.

### Declaring locals

| You write | What it does | Status |
|---|---|---|
| `int32 Sum = 0;` | A Local Variable. The initializer runs each time the declaration is reached. | Yes |
| `FIntPoint Info;` | With no initializer the local starts as zero, empty, None or the struct's defaults. It never holds garbage, unlike in C++. | Yes |
| `TArray<int32> Acc;` inside a loop | Reset to its starting value every time a loop, a goto or a repeated inline expansion reaches the declaration. That includes the locals of an inline function called in a loop or in a loop's condition. | Yes |
| `int32 I = 0, Hits = 0;` | Several variables in one declaration. | Yes |
| `const int32 Three = 3;` | A local holding a constant that nothing writes is no variable at all; see [The optimizer](#the-optimizer). | Yes |

```cpp
int32 CountPasses(int32 N) {
  int32 Sum = 0;
  for (int32 I = 0; I < N; ++I) {
    TArray<int32> Acc;   // empty again on every pass
    Acc.Add(I);
    Sum += Acc.Num();
  }
  return Sum;
}
```

Notes:

- In a function that makes a latent call, a local without an initializer keeps what the previous call left in it;
  see [Latent calls](#latent-calls).
- A `static` local is kept per object in a function that makes a latent call and is refused elsewhere; see
  [Latent calls](#latent-calls).

### Names and scope

| You write | What it does | Status |
|---|---|---|
| `for (int32 I = 0; I < 2; ++I)` after another loop over I | Locals of the same name and type in different blocks or loops are one variable. Correct C++ cannot observe this. | Yes |
| `int32 Total = 0;` with a member Total | The local shadows the member, and the member is left alone. | Yes |
| `if (int32 Rest = V % 3)` | A variable declared in a condition or an init-statement stays a function-wide local afterwards. | Yes |

```cpp
int32 Total;
int32 Names(int32 N) {
  int32 S = 0;
  for (int32 I = 0; I < N; ++I) S += I;
  for (int32 I = 0; I < 2; ++I) S += 100;   // the same I
  int32 Total = 0;                          // shadows the member Total: fine
  Total += S;
  return Total;
}
```

Note: give locals of different types different names. `{ int32 X; } { FString X; }` in one function compiles, but the
function then has two properties both named X.

### Reference locals

| You write | What it does | Status |
|---|---|---|
| `int32 &Alias = Count;` | Another name for a local, a parameter or a member of this (`Score` or `this->Score`), or for a member of one of their structs (`float &Z = Spawn.Z;`). Reads and writes are that variable's own Get and Set. | Yes |
| `int32 &Slot = Slots[I];` | A TArray element (of this object or another), `*P` or `P[I]`: the reference keeps the place's address in a hidden int64 local, and reads and writes go straight to that memory. | Yes |
| `const FVector &Home = K2_GetActorLocation();` | A `const T&` bound to a temporary is a copy, which lives as long as the reference. | Yes |
| `int32 &R = Other->Score;` | Another object's field has no Blueprint reference, so R keeps the field's address, found by name at run time as `&Other->Score` is. That needs the ReadProperty class you supply (see [Pointers and memory](#pointers-and-memory)); without one the build stops ("include ReadProperty.h"). The `const` and `auto &` forms, and `O->Score` through a local pointer, are the same. Without the helper, write to the field directly, or pass it to an inline function's `T&` parameter, which binds the field itself. | Yes |
| `float &X = Other->Loc.X;` | Refused ("the address of MemberExpr"): a member of another object's struct has no address the VM hands out. | Refused |

```cpp
int32 Count;
int32 Score;
FVector Spawn;
TArray<int32> Slots;
AMyActor *Other;
void Refs(int32 I) {
  int32 &Alias = Count;       // another name for Count
  Alias += 1;
  float &Z = Spawn.Z;         // a member of this object's struct: also a name
  Z += 100.f;
  int32 &Slot = Slots[I];     // keeps the element's address
  Slot += 5;
  const FVector &Home = K2_GetActorLocation();   // binds a temporary: a copy
}
void BumpOther() { Other->Score += 5; }          // int32 &R = Other->Score; needs ReadProperty
inline void Add5(int32 &V) { V += 5; }
void BumpOther2() { Add5(Other->Score); }        // V is Other->Score itself
```

Notes:

- A reference that holds an address behaves like C++'s. If the array grows or shrinks after the reference is taken,
  it may point at freed memory. Take the reference after any Add or Remove, or index the array again.
- A reference that holds the address of a struct or a TArray is usable only through its members. With
  `FVector &V = Pts[0];`, `V.X = 2.f` works, but `FVector C = V;` is refused ("a whole FVector through a pointer"), and
  so is a TArray function called on a `TArray<int32> &`.

### Other declarations

| You write | What it does | Status |
|---|---|---|
| `using T = int32; T A = X;` | A local whose type is spelled through an alias declared inside the function is refused today ("unimplemented local A: T"). The alias itself compiles to nothing. Spell the type out. | Not yet |
| `auto [A, B] = Point;` | Refused outside a TMap range-for ("a declaration statement declares nothing usable"). Read the members: `int32 A = Point.X;`. | Refused |
| `auto Inc = [](int32 V) { return V + 1; };` | A lambda is refused; see [Functions](#functions). | Refused |

## The optimizer

AssetGen optimizes each function's bytecode. It folds constants, drops work whose result nothing uses, lets locals
share variables, copies in the calls whose one body it knows and turns the bool an inline function returns into a
jump. None of this changes what a function does,
and a store to a member or to another object is never removed. The rule to remember: mark a method UE_PURE only when calling it has no side effects, because a
pure call whose result is unused is dropped.

### Constant folding

| You write | What it does | Status |
|---|---|---|
| `N * kStep + kSlots * 2 + (1 << 4 \| kStep)` | Every operator expression over constants compiles to its value; here one multiply and two additions remain. Each step is rounded to its C++ type, so `1 / 2.f` is 0.5f and `1 << 31` wraps. This covers arithmetic, bitwise operators, shifts, comparisons, `&&`, `\|\|` and `?:`. | Yes |
| `false && X`, `true \|\| X` | X is not compiled. A float constant is true when it is nonzero. | Yes |
| `N * 3 + 8 + 19` | Only constant subexpressions fold. C++ groups `+` from the left, so both additions stay; `N * 3 + (8 + 19)` has one. | Yes |
| `X + 0`, `X - 0`, `X * 1`, `X / 1`, `X \| 0`, `X ^ 0` | Compile to X with no node. A float keeps its + and -, because -0.0f + 0 is +0.0f. | Yes |
| `X / 0` | A division or modulo by a constant zero is not folded. It runs and gives 0, and clang warns -Wdivision-by-zero. | Yes |

```cpp
static constexpr int32 kStep = 3;
static constexpr int32 kSlots = 4;
int32 Budget(int32 N) { return N * kStep + kSlots * 2 + (1 << 4 | kStep); }
```

Note: a constant is a literal, an enumerator, a constexpr, const or static constexpr variable, a consteval result, a
constant argument of an inlined call, or a const local that nothing writes. [Constants](#constants) says how to
declare them.

### Constant locals and branches

| You write | What it does | Status |
|---|---|---|
| `const int32 Three = 3;` | A local initialised to a constant that nothing writes is no variable: each read is the constant itself. A local the body also writes keeps its variable. Strings and containers are never folded this way, since they would be rebuilt at every use. | Yes |
| `if (Lim > 3) break;` with a constant Lim | An if whose condition folds compiles only the branch it takes. | Yes |
| `while (false)`, `while (true)`, `do { } while (false)` | `while (false)` compiles to nothing, and the other two get no test. Code that a label or a case makes reachable is kept. | Yes |

### Negations

| You write | What it does | Status |
|---|---|---|
| `!(Ammo > 0)` | Becomes `Ammo <= 0`: an ordered comparison of integers or enums flips for free. | Yes |
| `!(A == B)` | Becomes `A != B` for the built-in comparisons (numbers, enums, bools, object pointers). FString and FName `==` is an operator call, so `!(S == T)` keeps its NOT node. | Yes |
| `!!X`, `if (!X) A(); else B();` | `!!X` is X, and an if on `!X` swaps its branches instead. | Yes |

Note: an ordered comparison of floats is never flipped, because a NaN is neither `A < B` nor `A >= B`. The result
stays what C++ says.

### What the optimizer removes

| You write | What it does | Status |
|---|---|---|
| `UKismetMathLibrary::Abs_Int(A);` | A pure call whose result is unused is dropped. Clang also warns -Wunused-value. | Yes |
| `int32 Unused = A * 3;` | A local that nothing reads is removed, with its stores. | Yes |
| `int32 Rolled = UKismetMathLibrary::RandomInteger(A);` | The unread local goes, but an impure initializer still runs as a call. | Yes |
| `return X + A * B;` after `int32 X = A * B;` | A repeated pure call is not reused: it is evaluated each time it appears. To compute it once, keep the result in a local and read the local. | Not yet |
| `if (IsReady(Item))` over an inline bool function whose returns are all constants | Each `return true` and `return false` jumps straight to the branch it picks: no bool is stored and then tested. | Yes |
| `Scale(Value, Tick())` into `inline int32 Scale(int32 V, int32 By)` | A parameter the body only reads is the caller's local variable itself, not a copy, while nothing can change that variable during the call. Another argument that writes it, or a reference parameter bound to it, keeps the copy. | Yes |
| `Bump(V)` in a `final` class or to a `final` method, `Base::Bump(V)`, or a static of a class this source cooks | The call: the body is copied in, as an inline function's is. See [Calling your own functions](#calling-your-own-functions). | Yes |

```cpp
int32 Drops(int32 A) {
  UKismetMathLibrary::Abs_Int(A);   // result unused: dropped
  int32 Unused = A * 3;             // never read: dropped
  int32 Rolled = UKismetMathLibrary::RandomInteger(A);   // unread, but the call still runs
  return A;
}
```

Notes:

- A store that the next statement overwrites on every path is dropped. A local read once is folded into the statement
  that reads it, also past stores to other locals in between when neither can see the move. Locals, the function's
  own included, share variables once their lifetimes end, and a copy from a local that ends there into one that
  starts there goes.
- "Pure" means a Kismet operator or conversion, an engine BlueprintPure function without out parameters (Random*,
  Now, Create*, Spawn* and similar are excluded), or a mod method marked UE_PURE with no non-const reference parameters. A
  UE_PURE method called only for its side effects, with its result unused, is dropped. Do not mark such a method
  UE_PURE; see [Functions](#functions).
- A function with a goto or a latent call skips some of these passes. What it does is unchanged.

### UE_NO_OPTIMIZE

| You write | What it does | Status |
|---|---|---|
| `UE_NO_OPTIMIZE int32 Raw(int32 X, int32 Y)` | `[[clang::optnone]]`: the function is compiled as written. Unused pure calls, unread locals and constant branches stay, a harmless `&&` or `\|\|` keeps its branch, and no folding or negation rewrite happens. | Yes |
| `#pragma clang optimize off` ... `#pragma clang optimize on` | The same for every function between the two lines. | Yes |

```cpp
UE_NO_OPTIMIZE int32 Raw(int32 X, int32 Y) {
  UKismetMathLibrary::Multiply_IntInt(X, Y);   // stays
  return X != 0 && Y != 0 ? 1 : 0;              // keeps its branch
}
#pragma clang optimize off
int32 RawPragma(int32 X) { UKismetMathLibrary::Abs_Int(X); return X; }
#pragma clang optimize on
```

Notes:

- `if constexpr` and consteval are still decided at build time, since clang itself evaluates them.
- Only the bytecode changes. The function's flags, as the engine sees them, are those of an optimized function.
- A call to or from such a function is never copied in (see
  [Calling your own functions](#calling-your-own-functions)).
- UeMeta.h also documents `#pragma optimize("", off)`. Only the `#pragma clang optimize off` spelling has been checked.

## Functions

A method with a body is a Blueprint function of its class: a function graph, in editor terms, that other Blueprints
can call by name and a subclass can override. The exception is `inline`, which copies the body into each call instead
(see [Inline functions and templates](#inline-functions-and-templates)). The rule to remember: unlike C++, a body
written inside the class does not make a method inline. Only the `inline` keyword does.

### Methods as Blueprint functions

| You write | What it does | Status |
|---|---|---|
| `int32 AddCharges(int32 By) { ... }` | A Blueprint function, Public and BlueprintCallable. An editor Blueprint can call it, and an editor subclass can override it. | Yes |
| `void Refill(int32 Amount);` and `void AMyActor::Refill(...) { ... }` | The same function, defined outside the class or in a `.cpp` beside the header. The definition gives the body and parameters; the declaration gives `static` and the access. | Yes |
| `int32 Pick(int32, int32 B)` | A parameter left unnamed is cooked as `P<its index>` (`P0` here; `_` is added while another parameter or a local of the body has that name, so `void Fill(int32&, int32 B) { int32 P0 = B * 2; ... }` names it `P0_`), as the editor names every pin. Callers pass it in its place. | Yes |
| `int32 GetCharges() const` | A const Blueprint function, as a UFUNCTION declared const would be. | Yes |
| `UE_PURE int32 Doubled() const` | A pure function: the editor draws it as a node without exec pins. Each C++ call runs once, where it is written. An editor pure node, by contrast, runs again for each use. | Yes |
| `UE_PURE static int32 Clamp01(int32 V)` | A pure static function. | Yes |
| `virtual int32 Priority()` | `virtual` is accepted and changes nothing. Every mod method is already called by name, and the most derived version runs. | Yes |
| `virtual int32 Step() final` | No subclass has a Step of its own: the function is cooked Final, and calls to it are direct, as in a `final` class. A subclass method named Step is refused. C++ allows `final` only on a virtual method. | Yes |
| `virtual int32 Score() = 0;` | An empty function that returns the default: 0, false, None or empty. A subclass's Score overrides it and names it as its super. A class that declares one, or inherits one with no version of its own, is cooked Abstract, which SpawnActor and CreateWidget refuse, as they refuse a class the editor marks Generate Abstract Class. `NewObject` and `AddComponentByClass` do not check the flag in a game, so a call to Score on such an object gets the default. A `SpawnActor` that names such a class warns, and a `NewObject` that names it is refused ([Objects and widgets](#objects-and-widgets)). | Yes |
| `public:` / `protected:` / `private:` | Become the function's Public, Protected or Private flag, which the editor honours; see [Classes and variables](#classes-and-variables). | Yes |
| `UE_CATEGORY("Turret\|Setup");` | The category of the members that follow, written into the editor API stub; see [Classes and variables](#classes-and-variables). | Yes |
| `UE_AUTHORITY_ONLY` / `UE_COSMETIC` | The editor's Authority Only and Cosmetic function flags; see [RPCs](#rpcs). | Yes |
| `generate_api: true` in `mods.yaml` | Writes an editor stub of the class, so a Blueprint made in the editor can place call nodes for its functions; see [Editor API stubs](GUIDE.md#editor-api-stubs). | Yes |
| `int32 Undef(int32 X);` with no body anywhere | No function is cooked, and a call to it is refused ("... declares and never defines"), as C++ would not link it. Give every method a body, or make it `virtual ... = 0` for an empty one. | Refused |
| `AMyActor() { Charges = 5; }` | A constructor body is silently ignored: it makes no function and no default. Use a member initializer or UE_DEFAULTS, and do runtime setup in ReceiveBeginPlay; see [Class defaults](#class-defaults). | Not yet |
| `int32 Sum() const { ... }` inside a UE_STRUCT | A struct holds no functions. A non-inline struct method is not refused and compiles to a call that cannot work; an inline one is refused (`called on another object`). Write a free inline function that takes the struct: `inline int32 SumOf(const FPair &P)`. | Not yet |

```cpp
class AMyActor : public AActor {
public:
  int32 Charges = 3;
  int32 AddCharges(int32 By) { Charges += By; return Charges; }   // a Blueprint function
  int32 GetCharges() const { return Charges; }                    // FUNC_Const
  UE_PURE int32 Doubled() const { return Charges * 2; }           // a pure node
  void Refill(int32 Amount);                                      // defined below
  inline int32 Capped(int32 V) { return V > 9 ? 9 : V; }          // no function: expanded at each call
};

void AMyActor::Refill(int32 Amount) { Charges = Capped(Charges + Amount); }
```

Notes:

- A `class` starts private, so methods before the first `public:` are cooked as private Blueprint functions.
- `inline` or `UE_PURE` may sit on the declaration or on the out-of-line definition.
- `UE_PURE` is your promise that the body has no side effects; nothing checks it. clang drops the attribute from a
  function returning void, with a warning, so a pure method must return a value. A call to a pure function whose
  result is unused is removed (see Results you ignore under
  [Calling engine and game functions](#calling-engine-and-game-functions)). A method with a non-const `T&` parameter
  is never removed as pure, even when it is marked.
- `UE_PURE` on an `inline` method changes nothing in the cooked class, because an inline method is not a function.

### Calling your own functions

| You write | What it does | Status |
|---|---|---|
| `Bump(By)` or `this->Bump(By)` | A call by name, as the editor's call to a function of the same Blueprint. The VM runs the most derived version on the object, so a subclass's override runs even when the parent's code makes the call. | Yes |
| `Peer->Bump(1)` | Runs on that object and reaches the most derived version for its class. | Yes |
| `Fact(V - 1)` inside `Fact` | Recursion. Each call gets its own frame, as a recursive Blueprint function does. Mutual recursion works the same way. | Yes |
| `Other->Twice(3)` where `Twice` is inline | Refused; see the inline table below. | Not yet |
| `Bump(By)` in a `final` class, or to a `final` method | Reaches that one function directly, the editor's call to a function no Blueprint can override, instead of by name. The body is copied into the caller, as an inline function's is, and the function is still cooked for every other caller: the editor, delegates, timers, other mods. A function the final class inherits, overrides, or implements for an interface it lists is not cooked Final (an override takes its parent's flags, an implementation keeps the interface function's BlueprintEvent), so a call to one whose body is not copied in (authority-only, an RPC, `noinline`) stays a call by name, as the editor's is; with no subclass, the name finds that one function. | Yes |
| `Twice(V)`, a static of a class this source cooks | Its body is copied in the same way. | Yes |
| `Peer->Bump(1)` in a `final` class | Direct, but not copied in: the body would need Peer as its `this`. | Yes |
| `[[gnu::noinline]] int32 Kept(int32 V)` | Calls to Kept stay calls, wherever they could be copied in. | Yes |

```cpp
int32 N;
AMyActor *Peer;
int32 Bump(int32 By) { N += By; return N; }
int32 Twice(int32 By) { return Bump(By) + this->Bump(By); }   // calls by name
void Sync() { Peer->Bump(1); }                                // runs on Peer
int32 Fact(int32 V) { return V <= 1 ? 1 : V * Fact(V - 1); }  // recursion
```

In a `final` class the same calls are direct, and those on `this` are copied in:

```cpp
class Counter final : public AActor {
public:
  int32 N;
  int32 Bump(int32 By) { N += By; return N; }
  int32 Twice(int32 By) { return Bump(By) + Bump(By); }         // both copied in
  int32 Fact(int32 V) { return V <= 1 ? 1 : V * Fact(V - 1); }  // stays a call
  [[gnu::noinline]] int32 Kept(int32 V) { return V + 1; }
  int32 UseKept(int32 V) { return Kept(V); }                    // stays a call
};
```

Notes:

- RPCs, authority-only and cosmetic functions, and overrides of engine functions are called through the engine's full
  routed call instead of the local one, so the engine's checks for them apply.
- The object a call runs on is evaluated before the arguments. The arguments run left to right only when none of them
  needs statements of its own (an inline call, `&&`, `||`, `?:`, a `++` value); otherwise those run first, so in
  `Pair(Bump(), BumpInline())` BumpInline's body runs before Bump(). See [Evaluation order](#evaluation-order).
- A call on a null object is skipped, not a crash, as for engine calls (see
  [Calling engine and game functions](#calling-engine-and-game-functions)).
- A Shipping build of the engine has no script recursion guard, so unbounded recursion overflows the native stack and
  crashes the game.
- Recursion through `inline` functions is refused; see
  [Inline functions and templates](#inline-functions-and-templates).
- A function is never copied into itself, so a recursive call stays a call. In mutual recursion one copy is made, and
  its call back stays a call.
- These are never copied in: a function that waits (Delay, UE_AWAIT) or contains a goto; an RPC, an authority-only or
  cosmetic function; an override of an engine function; UE_NO_OPTIMIZE on the function or on the caller; a class
  another mod cooks (UE_CLASS); a function whose body calls a parent's function that is not copied in, such as
  `Base::AuthOnly()`, itself or in an inline body it expands (see [Calling the parent](#calling-the-parent)): that
  call is bound from the function's own class, and in a subclass's copy it would be the subclass's.
- Copying makes the caller bigger. A large function called in many places may be worth `[[gnu::noinline]]`.
- A function that native code intercepts by name, such as an empty one a DLL or script mod hooks to read its
  arguments, must be `[[gnu::noinline]]`: a copied call runs the body in place and never reaches the hook.
- Inside a copied static, an engine call that leaves out its world context gets what the call passed for the static's
  own WorldContextObject parameter, as it would in the static's own function. A static without that parameter gets
  the caller's context instead of the class default object, so such a call works in the copy where the function's own
  would find no world.

### Parameters and return values

| You write | What it does | Status |
|---|---|---|
| `FVector Offset(float Up) { return ...; }` | The return type becomes the function's ReturnValue output. Any type a variable can have may be returned, and a caller can read a member of a returned struct directly: `GetTransform().Translation.Y`. | Yes |
| `auto Half(int32 V) { return V / 2; }` | A deduced return type becomes the Blueprint function's return type. | Yes |
| `int32 &Slot(int32 I)` | Refused: `a reference return is refused until what it means to a C++ caller is specified`. Return a value or a pointer. | Not yet |
| `int32 ByVal(TArray<int32> A)` | An input pin. The callee gets its own copy, containers and structs included, and its writes stay in the callee. | Yes |
| `void Around(int32 A, int32 &Lo, int32 &Hi)` | A pass-by-reference pin: writes reach the caller's variable, member or array element. This is how a function returns several values. | Yes |
| `int32 Plus1(const int32 &A)` | Passed by reference, with no copy. A literal or computed argument is first stored in a hidden local, as the editor's compiler does, because the VM needs an address. | Yes |
| `int32 Times(int32 X, int32 By = 3)` | A call that leaves the argument out passes the default expression, evaluated at that call each time, as in C++. This holds for Blueprint functions and inline ones. | Yes |
| A function that calls Delay or UE_AWAIT and returns a value or takes a non-const `T&` | Refused: `a function that resumes later returns nothing and takes no non-const reference parameters`. Store the result in a member; see [Latent calls](#latent-calls). | Refused |

```cpp
int32 N;
void Around(int32 A, int32 &Lo, int32 &Hi) { Lo = A - 1; Hi = A + 1; }   // two outputs
int32 UseAround(int32 A) {
  int32 L, H;
  Around(A, L, H);
  return L * 100 + H;
}
int32 ByVal(TArray<int32> A) { A.Add(1); return A.Num(); }   // the caller's array is unchanged
int32 Plus1(const int32 &A) { return A + 1; }
int32 Times(int32 X, int32 By = 3) { return X * By; }
int32 UseRest() { return Plus1(N * 2) + Times(5); }           // Times(5, 3)
```

Notes:

- A container or struct passed by value is copied on every call, as in the editor.
- A default argument lives only in the C++ declaration. The cooked function has no pin defaults, so an editor Blueprint
  that calls it passes every argument.
- A default can be any expression, a call included: `int32 Roll(int32 Max = UKismetMathLibrary::RandomInteger(10))`
  runs RandomInteger at each call that leaves `Max` out.
- A `T&` parameter bound to another object's member (`Add5(O->N)`) is passed through that object, which the engine
  treats as a reference. This case has not been run.

### Reference parameters and copy-back

A `T&` parameter needs a place it can refer to. Where Blueprint has none, AssetGen passes a copy and stores it back
after the call, with a warning.

| You write | What it does | Status |
|---|---|---|
| `Bump(X, 3)`, `Bump(Arr[NextIdx()], By)`, `Bump(O->Calls, By)` | The parameter names that place, fixed at the call: the object and the index are evaluated once. An inline body sees its own writes; a Blueprint function gets the place by reference. Either way the writes reach the caller. | Yes |
| `Add5(Counts[K])` with a TMap `Counts` | Blueprint has no reference to a map element (Find copies it out), so the call gets a hidden copy that is stored back into `Counts[K]` after the call. The warning says `bound to a map element`. | Warns |
| `Add5(C ? X : Y)` | The call gets a hidden copy, stored back after the call into whichever variable the condition picked. The condition is evaluated once, before the call. | Warns |
| `Add5(C ? M[K()] : X)` | Refused (`where X or Y is found by a call`): both sides would have to be located before the call, but C++ runs only the picked side's call. Pick into a local, pass the local, then store it back. | Not yet |
| `Bump(*P)` with a raw pointer `P` | Passes the memory itself, so the callee's write lands there; see [Pointers and memory](#pointers-and-memory). | Yes |

```cpp
TMap<int32, int32> Counts;
int32 X;
int32 Y;
void Add5(int32 &V) { V += 5; }
void Hit(int32 K, bool C) {
  Add5(Counts[K]);   // warning: a copy, stored back into Counts[K] after the call
  Add5(C ? X : Y);   // warning: a copy, stored back into X or Y
}
```

Notes:

- A copy is not a reference. Code that reads `Counts[K]`, `X` or `Y` while the call runs sees the old value.
- The key is evaluated once, before the call, as binding a reference would. The store goes to that key even if the
  call changes the variable the key came from.
- A member of a map element is handled the same way; the warning then says `a map element's member`.
- An inline function that only reads the parameter gets a copy, with no store-back and no warning. A `const T&` is
  never stored back.
- A local copy passed to a `T&` stays a copy, as in C++: `FStats T = Stats; SetKills(K, T);` leaves `Stats` unchanged.

### Static functions and function libraries

| You write | What it does | Status |
|---|---|---|
| `static int32 Twice(int32 V)` | A static Blueprint function (Static, Final, Public, BlueprintCallable). `Twice(X)` and `AMyActor::Twice(1)` both run it on the class default object, so inside it `this` is that default object, which has no world. | Yes |
| `static APawn *FirstPawn(UObject *WorldContextObject = nullptr)` | The first parameter whose name starts with `WorldContext` is what every engine call inside the static receives as its world context, as the editor wires the hidden pin. A caller that leaves it out passes its own `this`, or its own world context when the caller is itself such a static. | Yes |
| `class MyLib : public UBlueprintFunctionLibrary` holding statics | The editor's Blueprint Function Library. Another mod that includes its header calls the statics like an engine library's: a final call on the library's default object, with a world context left out filled with the caller's `this`. | Yes |
| A static that calls Delay or UE_AWAIT | Refused: `a static function has no object whose ubergraph frame could keep its locals`; see [Latent calls](#latent-calls). | Refused |
| `static int32 Tell(int32 V)` in a class whose mod parent has a static `Tell` too | Hides the parent's, as in C++, with a warning: "... hides Parent::Tell, a static: compiled as C++ name hiding ...". Every call to a static is bound to the one it names, so `Tell(V)` runs this one and `Parent::Tell(V)` the parent's. It is cooked with the parent's static as its super, as the editor links any function named like a parent's; the editor itself would refuse the name ("cannot be overridden"). | Warns |
| `static float Tell(float V)` over a mod parent's `static int32 Tell(int32)` | Refused: "... is static and hides Parent::Tell, a static of another signature, ...". Its super would be the parent's static, a function of other parameters, which no editor links. Rename this one. | Refused |
| A method that is not static named like a mod parent's static, or a static named like a parent's method | Refused: "... is static, and the editor takes a function of that name in a subclass for an override of it ..." or "... is static, and the Parent::Tell it hides is not ...". The editor makes a function named like a parent's an override of it, and an override must agree with it on Static. The parent may be another mod's class, from the header it shares. Rename one of them. | Refused |
| A static or a method named like one a mod parent of this source declares and never defines (no body, not `= 0`) | The parent compiles no function of that name, so this one hides nothing a Blueprint has: no warning, no refusal, no super, and its parameters may differ from the declaration's (`float Scale(float)` over a declared-only `int32 Scale(int32)`): a call to the declared-only one is refused, as C++ would not link it, so none lays out the parent's parameters. A function above the parent of that name, a native one too, is found as if the parent did not declare it. | Yes |

```cpp
static int32 Twice(int32 V) { return V * 2; }
static APawn *FirstPawn(UObject *WorldContextObject = nullptr) {
  return UGameplayStatics::GetPlayerPawn(0);   // gets WorldContextObject
}
APawn *Mine() { return FirstPawn(); }          // passes this
int32 Use(int32 X) { return Twice(X) + AMyActor::Twice(1); }
```

A library shared between mods:

```cpp
// MyLib.h
#pragma once
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

class MyLib : public UBlueprintFunctionLibrary {
public:
  UE_CLASS("/Game/_MyMods/MyLib/MyLib", "MyLib_C");   // UE_MOD_PACKAGE + "/" + class name
  UE_PURE static bool IsSelf(UObject *Obj, UObject *WorldContextObject = nullptr);
  static inline int32 AddOne(int32 V) { return V + 1; }  // expanded into the caller
};

// MyLib.cpp
#include "MyLib.h"
UE_MOD_PACKAGE("/Game/_MyMods/MyLib");

bool MyLib::IsSelf(UObject *Obj, UObject *WorldContextObject) { return Obj == WorldContextObject; }

// In another mod, members of one of its classes
AActor *Target;
bool Check() { return MyLib::IsSelf(Target); }   // WorldContextObject is this
```

Notes:

- A static without a WorldContext parameter passes its own `this`, the class default object. It has no world, so
  engine calls that need one find nothing.
- An explicit argument, `nullptr` included, is kept.
- In the editor API stub, the WorldContext parameter becomes the editor's hidden world-context pin.
- Only one source cooks the library: the one whose `UE_MOD_PACKAGE`, plus the class's namespace folders, plus the
  class name, is exactly the `UE_CLASS` path. Every other source imports it, so the library mod's pak must be loaded
  as well. The rule and its pitfalls are in [Mod sources and packages](#mod-sources-and-packages), and a full pair of
  mods is in [examples/MathLib.h](examples/MathLib.h) and [examples/LibraryUser.cpp](examples/LibraryUser.cpp).
- A `static inline` member such as `AddOne` is expanded into each caller. The cooked library does not export it, so a
  Blueprint made in the editor cannot call it.

### Overloading

A Blueprint class has one function per name. Overloads work when they are all `inline`, or when the one non-inline
overload is the one with the most parameters.

| You write | What it does | Status |
|---|---|---|
| `inline int32 Pick(int32 V)` beside `inline int32 Pick(bool B)` | Each call expands the overload C++ picked, and one overload may call another. None of them is a Blueprint function. | Yes |
| `int32 Get(int32 A, int32 B)` beside `inline int32 Get(int32 A)` | The overload with the most parameters is the Blueprint function named `Get`; the inline ones beside it expand normally. | Yes |
| `inline int32 Get(int32 A, int32 B)` beside a non-inline `int32 Get(int32 A)`, then `Get(V)` | Refused: `an overload set may not mix inline and non-inline functions`. Make the non-inline overload the one with the most parameters, or rename it. | Not yet |
| `int32 Ov(int32 A)` beside `int32 Ov(int32 A, int32 B)`, neither inline | Refused: "a second function of that name". A Blueprint class has one function per name. Give them different names, or make the extra overloads `inline`. | Refused |
| `int32 Get()` beside `int32 get()` | Refused: "differs from Get only in case". An FName ignores case, so both would be one function. The same holds for two variables, a variable and a function, and a name an ancestor already has: `int32 get()` in a class whose parent has `Get()` is refused, while `Get()` itself overrides it. | Refused |
| `int32 None;`, `int32 none();`, a `UE_STRUCT` member `int32 NONE;` | Refused: "None is UE's empty name". The name None is UE's empty name in any case, which the Blueprint editor refuses for a variable or a function. A saved value's members end at one of that name, so a `UE_STRUCT` member called None would cut the struct's value short in a class default. The same for a component and an interface's member. | Refused |

```cpp
inline int32 Pick(int32 V) { return V + 1; }
inline int32 Pick(bool B) { return B ? 100 : 200; }
int32 Get(int32 A, int32 B) { return A * B; }   // the Blueprint function named Get
inline int32 Get(int32 A) { return A + 1; }     // expanded beside it
int32 Use(int32 V) { return Pick(V) + Pick(true) + Get(V) + Get(V, 2); }
```

### Lambdas, function pointers and operators

| You write | What it does | Status |
|---|---|---|
| `auto L = [X](int32 Y) { return X + Y; };` | Refused, as are an immediately invoked lambda and a lambda passed to an inline template, each with its own `unimplemented` message. Write an inline method or a free inline function. | Refused |
| `auto *P = &Inc1; return P(X);` | Refused (`unimplemented local`): a Blueprint has no function pointers. To call a function later, bind it to a dispatcher or pass it as a delegate (`{this, &AMine::Handle}`); see [Event dispatchers](#event-dispatchers). | Refused |
| `inline FPairX operator+(FPairX L, FPairX R)` | Refused where it is used (`unimplemented operator overload`). Only the engine's operators that UeApi lists, and string `+`, are lowered. Write a named inline function. | Refused |

## Calling engine and game functions

UeApi declares the engine's and the game's reflected functions as C++ methods and statics, and you call them as in
C++. AssetGen writes the call the editor's node would make. The rule to remember: leave out the world context and a
latent call's `FLatentActionInfo`, because the compiler fills them in.

### Engine and game calls

| You write | What it does | Status |
|---|---|---|
| `K2_SetActorLocation(Home, false, Hit, false);` | Calls the engine's function on this actor, as the editor's call node does. | Yes |
| `Char->Server_SetRunning(false);` | The same on another object. The engine applies its own authority, cosmetic and RPC routing to the call. | Yes |
| `FHitResult Hit;` passed to a `T&` parameter | A `T&` parameter of an engine function is an output and needs a variable, as in C++. | Yes |
| `UKismetMathLibrary::RandomInteger(10)` | A native static is a direct library call. | Yes |
| `UGameplayStatics::ApplyDamage(...)`, `PlaySound2D(...)` | An authority-only or cosmetic static is called on the library's default object, so the engine checks where it may run, as for the editor's node. | Yes |
| `UGameplayStatics::GetPlayerPawn(0)` in a class that is no actor, component, widget, GameInstance or subsystem | Warns: "passes self as GetPlayerPawn's world context, and an object of this class finds its world only through its Outer". The editor wires self to the pin only in a class with a world. Make the object with an actor or component as its Outer, or pass a world context. | Warns |
| A static of a game Blueprint library, or of another mod's library | A final call on that class's default object. | Yes |
| `Json->GetPath(...)` on a game Blueprint, `T->Fire(2)` on another mod's class | A call by name, as the editor calls a Blueprint function that is not final: on an object of a subclass that overrides it, the override runs. A function the header marks `final`, a static, and a parent call (`Turret::Fire(N)` in an override) are bound to that class's function. | Yes |
| A call or an expression passed to an engine function's `const T&` parameter | Stored in a hidden local first, because the VM needs an address. | Yes |
| `Target->K2_DestroyActor();` with `Target` null | The call is skipped (the engine logs "Accessed None"), and a returned value reads as zero. C++ would crash here; a Blueprint does not. The same holds for a mod function called on a null object. | Yes |
| `Seen->GetPriority()` on a `TScriptInterface<ITagged>` | A call by name to the object behind the interface; see [Interfaces](#interfaces). | Yes |
| `Other->GetClass()`, `GetName()`, `GetOuter()`, `GetTypedOuter<T>(Obj)` | UObject's C++ helpers and their Objects.h relatives, which are not reflected functions. The compiler maps them; see [Working with other objects](#working-with-other-objects). | Yes |

```cpp
FVector Home;
void GoHome(APlayerCharacter *Char) {
  FHitResult Hit;                                    // an output of the engine function
  K2_SetActorLocation(Home, false, Hit, false);      // on this actor
  Char->Server_SetRunning(false);                    // on another object
  int32 R = UKismetMathLibrary::RandomInteger(10);   // a library static
  APawn *P = UGameplayStatics::GetPlayerPawn(0);     // world context left out: this
}
```

Notes:

- UeApi uses the reflected names, which are not always the C++ ones: the location getter is `K2_GetActorLocation()`,
  and there is no `GetActorLocation()`.
- UeApi declares no default arguments for engine and game functions. Pass every argument except the world context
  and a latent call's `FLatentActionInfo`.
- `GetOuter()` is the exception to the null rule: on a null object it reads memory and crashes.

### The world context

| You write | What it does | Status |
|---|---|---|
| `UGameplayStatics::GetPlayerPawn(0)` | UeApi declares every reflected function with a `UObject*` parameter named `WorldContext...` twice, with and without it. Leaving it out passes `this`, which is what the editor wires to the hidden pin. | Yes |
| The same call inside a static that has a `WorldContextObject` parameter | Passes the static's own parameter. | Yes |
| `UGameplayStatics::GetPlayerPawn(nullptr, 0)` | An explicit argument, `nullptr` included, is kept. | Yes |
| `UKismetSystemLibrary::Delay(0.5f)` | A latent function's `FLatentActionInfo` is left out as well, and the compiler fills it in. Never pass it yourself: with the world context written too it is refused (`leave the FLatentActionInfo argument out`); see [Latent calls](#latent-calls). | Yes |
| `UKismetSystemLibrary::Delay(0.5f, Info)`, the latent info passed without the world context | Not caught: it compiles with no diagnostic, and 0.5 is passed as the world context. Leave the FLatentActionInfo out. See [Latent calls](#latent-calls). | Not yet |

```cpp
APawn *Mine() { return UGameplayStatics::GetPlayerPawn(0); }            // this
APawn *None() { return UGameplayStatics::GetPlayerPawn(nullptr, 0); }   // kept: null
static APawn *InStatic(UObject *WorldContextObject) {
  return UGameplayStatics::GetPlayerPawn(0);                             // the static's own
}
void Wait() { UKismetSystemLibrary::Delay(0.5f); }                      // the latent info is filled in too
```

Notes:

- A static without a WorldContext parameter passes its own `this`, the class default object, which has no world.
- An inline helper's own WorldContext parameter follows the same rules; see
  [Inline functions and templates](#inline-functions-and-templates).

### Results you ignore

| You write | What it does | Status |
|---|---|---|
| `Bump(1);` | A call made for its effects runs even when its value is unused. | Yes |
| `Doubled();` where `Doubled` is UE_PURE, or an engine BlueprintPure function without outputs | Removed when its arguments do nothing either. AssetGen prints nothing about it; clang warns `ignoring return value of function declared with pure attribute`. | Yes |

```cpp
int32 N;
int32 Bump(int32 By) { N += By; return N; }
UE_PURE int32 Doubled() const { return N * 2; }
void Hit() { Bump(1); }                          // runs
int32 F() { Doubled(); return Doubled() + 1; }   // the first call is removed
```

Notes:

- The compiler never merges repeated pure calls: `Doubled() + Doubled()` calls it twice.
- `UE_NO_OPTIMIZE` on the function keeps even an unused pure call; see [The optimizer](#the-optimizer).

### Math and utility functions

| You write | What it does | Status |
|---|---|---|
| `UKismetMathLibrary::Clamp(V, 0, 10)` | Math is the engine's Blueprint math library, the functions behind the editor's math nodes: Clamp, FClamp, Min, FMin, FMax, Abs, Sqrt, Sin, Cos, Lerp, FInterpTo, RandomInteger, RandomFloatInRange and the rest of `UKismetMathLibrary` in `UeApi/Engine.h`. Each call is a direct library call. | Yes |
| `A + B`, `A < B`, `Dir * 2.0f` | Operators on numbers and vectors need no written call: they become the math library's operator functions; see [Operators](#operators). | Yes |
| `S == "x"` on an FString | Ignores case, as FString's `==` does in UE C++; see [Strings and text](#strings-and-text). | Yes |
| `FMath::Clamp(V, 0, 10)` | clang error `use of undeclared identifier 'FMath'`: FMath is not part of the reflected API, so UeApi does not declare it. Use UKismetMathLibrary. | Refused |
| `std::min(A, B)`, `std::sqrt(X)`, `__builtin_sqrtf(X)` | Refused, even with `#include <algorithm>` or `<cmath>` (`call to an unknown function`): a mod has no C++ standard library to call into. Use UKismetMathLibrary, or write the helper as an inline function. | Refused |
| `if constexpr (__is_same(T, int32))`, `static_assert(__is_same(int32, int))` | clang's type-trait builtins work in place of `<type_traits>` where clang decides them at build time. | Yes |
| `bool B = __is_same(A, B);`, `if (__is_same(A, B))`, `return __is_same(A, B) ? 1 : 0;` | Refused ("unimplemented argument TypeTraitExpr"), even in a `constexpr bool` local. Put the test in `if constexpr`. | Not yet |

```cpp
int32 Clamped(int32 V) { return UKismetMathLibrary::Clamp(V, 0, 10); }
float Smaller(float A, float B) { return UKismetMathLibrary::FMin(A, B); }
float Root(float X) { return UKismetMathLibrary::Sqrt(X); }
int32 Roll() { return UKismetMathLibrary::RandomInteger(6) + 1; }
```

Notes:

- UeApi spells the library's integer parameters `int`, which is `int32`.
- Most math functions are UE_PURE, so a call whose result is unused is removed, with clang's warning. RandomInteger
  and RandomFloatInRange are not pure, so every call runs.
- Other libraries sit beside it: `UKismetStringLibrary`, `UKismetTextLibrary`, `UKismetArrayLibrary`,
  `UKismetSystemLibrary` and `UGameplayStatics`. The game adds its own in `UeApi/FSD.h`, for example
  `UGameFunctionLibrary`.
- On Windows clang finds the MSVC headers, so `#include <cmath>` parses and only the call is refused. On Linux the only
  standard header a mod can include is `<initializer_list>`.
- For math on constants, a `consteval` function runs at build time and only its answer reaches the Blueprint; see
  [Constants](#constants).

### Messages on screen

| You write | What it does | Status |
|---|---|---|
| `UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg)` | Posts `Msg` as a game message, through the game state. This is what to use instead of the Print String node. | Yes |
| `UGameFunctionLibrary::GetFSDGameState()->PostGameMessage(Msg)` in a static with a `WorldContextObject` parameter | The game state of that parameter's world. | Yes |

```cpp
int32 Count = 3;
inline void Say(FString Msg) { UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg); }
static void SayFrom(FString Msg, UObject *WorldContextObject) {
  UGameFunctionLibrary::GetFSDGameState()->PostGameMessage(Msg);   // gets WorldContextObject
}
void ReceiveBeginPlay() {
  FString N = Count;   // int32 -> FString
  Say("Count is " + N);
}
```

Notes:

- `UKismetSystemLibrary::PrintString` and `PrintText` compile without a warning, to the same call as the editor's Print
  String node, but nothing appears on screen or in the log. UE 4.27 leaves their whole body out of Shipping builds,
  so the retail game runs an empty function. No compiler change can fix that.
- `Say` is inline, so no Say function exists in the cooked class; each call expands in place.
- A static without a WorldContext parameter passes the class default object, which has no world. The game state is
  then null and the call does nothing.
- PostGameMessage is not an RPC. Whether a message posted on a client reaches the other players is untested.

## Overrides and parent calls

A method with the name of an event or function that the parent class exposes overrides it. There is no macro, and
C++'s `override` keyword is refused. Copy the declaration from UeApi: an override with other parameter types than
the function it replaces is refused.

### Overriding engine and game events

| You write | What it does | Status |
|---|---|---|
| `void ReceiveBeginPlay() { ... }` | Overrides the parent's event, like adding the event node in the editor. The engine calls it. | Yes |
| `void ReceiveTick(float DeltaSeconds)` | Also turns ticking on: it sets `PrimaryActorTick.bCanEverTick` on an actor's class default object, `PrimaryComponentTick.bCanEverTick` on a component's, as the editor's compiler does. AActor and UActorComponent leave ticking off by default. | Yes |
| `void OnJumped() { ACharacter::OnJumped(); ... }` | Overrides a BlueprintNativeEvent (an event with a C++ default). The Blueprint version replaces the default, and the parent call runs it. | Yes |
| `bool CanJumpInternal() const` | An override copies its parent's access, event, net, authority, cosmetic, const and pure flags, whatever section it is written in. It drops Native, because the Blueprint version is script. | Yes |
| A class in `namespace Game::...` deriving a game Blueprint, redefining one of its functions | An override the game's code reaches by name, with the parent function's flags; see [examples/GameBlueprintChild.cpp](examples/GameBlueprintChild.cpp). | Yes |
| `class ATurret : public AActor, public ITargetable` with its functions defined | Implements an interface; see [Interfaces](#interfaces). | Yes |
| `void ReceiveTick(float DeltaSeconds) override` | clang error `only virtual member functions can be marked 'override'`: UeApi declares engine functions non-virtual. Leave it off; the name alone makes the override. | Refused |
| `UE_SERVER`, `UE_CLIENT` or `UE_MULTICAST` on an override | Refused: `an override takes its parent's replication; drop the RPC marker`; see [RPCs](#rpcs). | Refused |

```cpp
class AMyActor : public AActor {
public:
  int32 Hits;
  float Elapsed;
  EEndPlayReason LastReason;
  void ReceiveBeginPlay() { Hits = 0; }
  void ReceiveTick(float DeltaSeconds) { Elapsed += DeltaSeconds; }   // also turns ticking on
  void ReceiveEndPlay(EEndPlayReason EndPlayReason) { LastReason = EndPlayReason; }
  void ReceiveActorBeginOverlap(AActor *OtherActor) { Hits += 1; }
};

class AMyChar : public ACharacter {
public:
  int32 Jumps;
  void OnJumped() { ACharacter::OnJumped(); Jumps += 1; }   // runs the C++ default first
  bool CanJumpInternal() const { return Jumps < 2; }         // protected const, like the parent
};
```

Notes:

- The parameter list must match. `void ReceiveTick(int32 X)` is refused: "FuncTickInt::ReceiveTick is void (int32),
  and the AActor::ReceiveTick it replaces is void (float): callers pass that one's parameters; declare the same". The
  same holds for an override of a mod parent's function, of an RPC, and for an interface function's implementation.
  Names do not count, a `const T&` parameter matches a `T` one, and an enum `E` matches the `TEnum<E>` UeApi declares.
- An override keeps its parent's access: a ReceiveBeginPlay override is protected even when written under `public:`.
- AssetGen reads a parent's flags from the SDK's `Events.json`, which lists engine and game classes. A mod method named
  like an engine function that is not an event there (K2_DestroyActor, say) is refused: "AActor::K2_DestroyActor is
  native and no Blueprint event, so no function replaces it". C++ and calls bound to it keep running the engine's.
  An override of a function of another mod's class (from a `UE_CLASS` header) gets plain flags, not the parent's.
- An event override is not BlueprintCallable, so the editor API stub leaves it out.

### Overriding your own parent's methods

| You write | What it does | Status |
|---|---|---|
| `int32 Bump(int32 By)` in a subclass of a mod class that declares `Bump` | The nearest ancestor that declares the method is its super, and the override inherits that function's access, net, pure and const flags. Calls by name anywhere, the parent's code included, reach the most derived version. | Yes |
| `Twice(By)` where `Twice` is inherited, not redeclared | An ordinary call by name. A subclass's override is what runs, including one compiled later in another mod. | Yes |

Notes:

- An `inline` method of the parent cannot be overridden, because it is not a function. A call on `this` inside its
  expanded body still goes by name, so it reaches the subclass's version (see `ViaInline` below).

### Calling the parent

| You write | What it does | Status |
|---|---|---|
| `SuperBase::ReceiveBeginPlay();` inside an override | Runs the parent's own implementation once and comes back: the editor's "Add call to parent function". Write the class you derive from; C++ has no `Super`. | Yes |
| `AActor::ReceiveBeginPlay();` | Calls the engine's function. For a BlueprintImplementableEvent it does nothing, because there is no parent body. For a BlueprintNativeEvent it runs the C++ default. | Yes |
| `SuperBase::Twice(1)` in a class that does not declare Twice | SuperBase's Twice, its body copied in: C++ runs that function without dispatch, so an object of a subclass that overrides Twice does not reach its own. | Yes |
| The same call to a function whose body cannot be copied in: authority-only, cosmetic, a server or client RPC, `noinline`, one that waits, one whose body makes such a call itself, or any from a `UE_NO_OPTIMIZE` caller | SuperBase's function, without dispatch. Blueprint calls a parent's function that way only from a class that has its own function of that name, so AssetGen adds one to your class: an override of the method that only calls the nearest parent's version with the same arguments, compiled like one you write, with an override's flags and super. The call is then bound to SuperBase's function, and a subclass's override of the method overrides the added one. A call by name runs what it ran before: the added override passes it on, and the engine routes both calls the same way. | Yes |
| The same call to a multicast RPC | A call by name, with a warning: an object of a subclass that overrides the function runs its override. On a server a multicast runs locally and is also sent, so an added override would send it once, then again when it calls the parent's. An override you declare yourself does that too, as an editor override that calls its parent does. | Warns |
| `SuperTest::Thrice(1)` in SuperTest's own code | SuperTest's own Thrice, its body copied in: C++ runs that one without dispatch, also on an object of a subclass that overrides Thrice. | Yes |
| The same call to one of the class's own functions whose body cannot be copied in | A call by name, with a warning: on an object of a subclass that overrides the function, the override runs. A Blueprint calls its own class's function without dispatch only when that function is `final`, and then the call is bound to it. | Warns |
| `Other->SuperBase::Bump(1)` | Not what C++ does. The qualifier is recognised only on `this`, so this is a call by name that reaches Other's most derived Bump. | Not yet |

```cpp
class SuperBase : public AActor {
public:
  int32 Count;
  void ReceiveBeginPlay() { Count = 1; }
  int32 Bump(int32 By) { Count += By; return Count; }
  int32 Twice(int32 By) { return Bump(By) + Bump(By); }
  inline int32 TwiceInline(int32 By) { return Bump(By) + Bump(By); }
};

class SuperTest : public SuperBase {
public:
  void ReceiveBeginPlay() {
    SuperBase::ReceiveBeginPlay();   // the parent's, once
    Count = Count + 10;
  }
  int32 Bump(int32 By) { return SuperBase::Bump(By * 2); }
  int32 Thrice(int32 By) { return Twice(By) + Bump(By); }   // Twice calls this class's Bump
  int32 ViaInline(int32 By) { return TwiceInline(By); }      // so does the expanded body
};
```

Notes:

- An inline body, a member template's too, is copied into each class that calls it, and `Base::Method()` in it is
  judged from that class, as if written there: a class with its own Method makes the parent call, and one without gets
  the added override. A plain `Method()` in it stays a call by name from every such class.
- No override is added where the nearest parent's declaration of the method is inline, static or pure virtual: such a
  call whose body cannot be copied in goes by name, with the warning. A parameter the parent leaves unnamed is named
  in the added override (`P0`, `P1`, ... by position) and passed on.
- A parent this source cooks has its body copied in, as a `final` method's is (see
  [Calling your own functions](#calling-your-own-functions)). An override of an engine event, such as
  ReceiveBeginPlay above, stays a call.

## Inline functions and templates

`inline` is the editor's macro: the body is copied into each call, and no function exists at run time. A template is
always expanded this way. The rule to remember: an inline method expands only on `this`. For a helper that takes an
object, write a free inline function.

### Inline functions

| You write | What it does | Status |
|---|---|---|
| `inline int32 Clamp(int32 V, int32 Lo, int32 Hi) { ... }` | Never a Blueprint function. Each call copies the body into the caller, like an editor macro, with its own locals, and `return` jumps to the end of the copy. Loops, switch, goto, nested inline calls and early returns all work. | Yes |
| `inline` on the declaration, or only on the out-of-line definition | Either one makes the method inline. | Yes |
| `Late(Counter)` | Each argument is evaluated once, before the body runs. A by-value parameter keeps the argument's value at the call, even if the body changes what it was read from. A constant or read-only argument is used in place, without a copy. | Yes |
| `inline bool AttachTo(USceneComponent *Child, USceneComponent *Parent)` at namespace scope | A free inline function, expanded at every call. A free function can act on any object; an inline method expands only on `this`. | Yes |
| `int32 Helper(int32 X) { ... }` at namespace scope, without `inline` | Refused at the call: `call to an unknown function: Helper`. A Blueprint has no free functions. Add `inline`, or make it a static of a function library class. | Refused |
| `static inline int32 AddOne(int32 V)` in a library class | Expanded at the call, from its own class or from any class that includes the header, in any mod. The library does not export it. | Yes |
| `Bump(By)` inside an inline body or a member template that a subclass expands | A call on `this` goes by name, so the object's most derived method runs, including an override compiled later or one in a class between. | Yes |
| `Other->Twice(3)` where `Twice` is an inline method | Refused: `called on another object (only this)`, because `this` in the body stays the caller's self. Make the method non-inline, or write a free inline function that takes the object. | Not yet |
| An inline function that calls itself, directly or through another inline | Refused (`calls itself`): the expansion would never end. Make it non-inline; a Blueprint function may recurse. One overload calling another is fine. | Refused |
| `inline int32 Nope(int32 X);` with no body | Refused at the call (`has no body`). clang also warns with `-Wundefined-inline`. | Refused |
| `OnHit.Add(this, &AMine::Handle)` where `Handle` is inline | Refused: a delegate binds a function by name, and an inline function is none. Drop `inline`; see [Event dispatchers](#event-dispatchers). | Refused |
| `static int32 Count = 0;` inside an inline function | Refused: each expansion would keep its own. Use a member variable; see [Latent calls](#latent-calls). | Refused |

```cpp
int32 Counter;
inline int32 Clamp(int32 V, int32 Lo, int32 Hi) {
  if (V < Lo) return Lo;
  if (V > Hi) return Hi;
  return V;
}
inline int32 Late(int32 A) { Counter += 1; return A; }
int32 Use(int32 V) { return Clamp(V, -5, 5) + Late(Counter); }   // Late gets Counter before the body adds 1
```

Notes:

- A local declared in an inline body is fresh (zero or empty) on every expansion, also in a loop's test.
- A latent call (Delay, UE_AWAIT) inside an inline helper makes the calling function latent.
- Each call is a full copy of the body, so a large helper called in many places grows the bytecode.
- An inline function cannot be called from Blueprints made in the editor, and the editor API stub leaves it out.
- A `T&` parameter of an inline function is another name for the place it is bound to; see
  [Functions](#functions) for reference parameters and copy-back.
- `Objects.h` already defines `AttachToComponent`, so do not use that name for a helper of your own when you include it.
- AssetGen also copies in, by itself, calls bound by `final`, `Base::Method()` calls and statics of the classes the
  source cooks; see [Calling your own functions](#calling-your-own-functions).
- A refusal of a call to a free function without `inline` names the calling function, not the definition.

### World context in inline helpers

| You write | What it does | Status |
|---|---|---|
| `FirstPawn()` where `FirstPawn(UObject *WorldContextObject = nullptr)` is inline | A parameter whose name starts with `WorldContext`, left at its default, becomes the caller's `this`. Inside a static with its own WorldContext parameter it becomes that parameter. A Blueprint function or an engine function gets the same value in the same place. | Yes |
| `FirstPawn(Other)`, `FirstPawn(nullptr)` | An explicit argument, `nullptr` included, is kept. | Yes |
| An engine call inside the inline body that leaves out its own world context | Gets the caller's context, not the inline's WorldContextObject parameter. Pass the parameter on explicitly, as below. | Yes |

```cpp
inline APawn *FirstPawn(UObject *WorldContextObject = nullptr) {
  return UGameplayStatics::GetPlayerPawn(WorldContextObject, 0);
}

class AMyActor : public AActor {
public:
  APawn *Mine() { return FirstPawn(); }                                           // this
  APawn *Theirs(AActor *Other) { return FirstPawn(Other); }                       // Other
  static APawn *FromStatic(UObject *WorldContextObject) { return FirstPawn(); }   // the static's own
};
```

Notes:

- This holds for inline methods, free inline functions and static inline members of a library.
- Only the name is checked (`WorldContextObject`, `WorldContext`, `WorldContextObject_0`...), not the type. Any written
  default is replaced, not only `nullptr`.
- A static member can call only a free inline function or a static inline member. An inline method needs an object,
  and clang enforces that.
- Older AssetGen builds passed the written default (null) instead.

### Templates

| You write | What it does | Status |
|---|---|---|
| `SpawnActor<AActor>(AActor::StaticClass(), Where)` from `Objects.h` | Each instantiation clang makes is expanded at its call like any inline function. No function exists per instantiation; see [Creating objects](#creating-objects). | Yes |
| `template <class T> inline T Max2(T A, T B)` | A free function template needs `inline`. | Yes |
| `template <class T> T Max2(T A, T B)` without `inline` | Refused at the call: `call to an unknown function: Max2`. Add `inline`. | Refused |
| `template <class T> int32 WidthOf()` in a class | A member template is expanded at its call, with or without `inline`. No Blueprint function is made, and only calls on `this` expand. Its body is its own class's code, as an inline method's is: a call in it is read there, wherever it is copied. | Yes |
| `inline auto TwiceN(Number auto V)`, or `int32 AutoP(auto V)` in a class | An `auto` parameter makes the function a template, expanded at each call. A free one needs `inline`. | Yes |
| `template <class T> concept Number = requires(T A) { A + A; };` | Concepts and requires-clauses are checked by clang and cost nothing at run time. A wrong type is a "constraints not satisfied" error on the calling line. | Yes |
| `template <class... T> inline int32 Fwd(T... V) { return Sum2(V...); }` | Pack expansion into a call works. | Yes |
| `sizeof...(V)` | Refused: `unimplemented argument SizeOfPackExpr`. | Not yet |
| `if constexpr (sizeof(T) == 8)` | Only the branch clang kept is compiled; no test or jump is left in the bytecode. | Yes |

```cpp
template <class T> concept Number = requires(T A) { A + A; };
template <class T> inline T Max2(T A, T B) { return A > B ? A : B; }
inline auto TwiceN(Number auto V) { return V + V; }
inline int32 Sum2(int32 A, int32 B) { return A + B; }
template <class... T> inline int32 Fwd(T... V) { return Sum2(V...); }

class AMyActor : public AActor {
public:
  template <class T> int32 WidthOf() {
    if constexpr (sizeof(T) == 8) return 8;
    else return 4;
  }
  int32 Use(int32 A) { return Max2(A, 3) + TwiceN(A) + Fwd(A, 3) + WidthOf<int64>(); }
};
```

Notes:

- A mod has no C++ standard library, so use clang builtins such as `__is_base_of(Base, T)` and `__is_same(A, B)`
  rather than `<concepts>` or `<type_traits>`. They compile where clang decides them at build time (`if constexpr`,
  `static_assert`); used as a run-time value they are refused ("unimplemented argument TypeTraitExpr").

### constexpr and consteval functions

| You write | What it does | Status |
|---|---|---|
| `constexpr int32 SqM(int32 X) const` in a class | An ordinary Blueprint function that runs in the game; `constexpr` changes nothing. Mark it `inline` to expand it at calls, or `consteval` to run it at build time. | Yes |
| `inline constexpr int32 Sq(int32 X)` at namespace scope | Expanded like any inline function and computed in the game. | Yes |
| `constexpr int32 Sq(int32 X)` at namespace scope, without `inline` | Refused at the call: `call to an unknown function: Sq`. AssetGen does not run a plain constexpr call at build time. Add `inline`, or use `consteval`. | Refused |
| `consteval int32 Fnv(const char *S)` | clang runs the call while compiling, and only the answer reaches the Blueprint; see [Constants](#constants). | Yes |
| `int32 K = Sq(3);` as a member initializer, with a plain constexpr `Sq` | Refused (`a default is a value known when the mod is built`), and so is a consteval `Sq` called there directly. A default reaches a function call only through a constant: `static constexpr int32 kSq = Sq(3);` with `Sq` a consteval function at namespace scope, then `int32 K = kSq;`. See [Constants](#constants). | Refused |

```cpp
inline constexpr int32 Sq(int32 X) { return X * X; }   // expanded, computed in the game

class AMyActor : public AActor {
public:
  constexpr int32 SqM(int32 X) const { return X * X; }   // a Blueprint function
  int32 Use(int32 V) { return Sq(V) + SqM(V); }
};
```

## Components

An actor's components are declared in the class with `UE_COMPONENT`, which does what adding a component in the
editor's Components panel does: the actor builds the component from a template each time it spawns. A component can
also be added at run time with the `Objects.h` helpers. The rule to remember: `UE_COMPONENT` takes an engine or game
component class, and the first scene component declared is the actor's root.

### Declaring components

| You write | What it does | Status |
|---|---|---|
| `UE_COMPONENT(UStaticMeshComponent, Mesh);` | Adds a component, as Add Component in the Components panel does. `Mesh` is an ordinary object variable. A construction-script node builds the component from a template (`Mesh_GEN_VARIABLE`) each time the actor spawns and stores it in `Mesh`. | Yes |
| `Mesh->SetHiddenInGame(true, false);` in a function | By the time the construction script and `ReceiveBeginPlay` run, the variable holds this actor's own instance. Use it like any object pointer; a change affects only this actor. | Yes |
| `UE_COMPONENT(UHealthComponent, Health);` | One of DRG's own component classes works like an engine one. | Yes |
| `UE_COMPONENT(UInstancedStaticMeshComponent, Pile);` | Static mesh, instanced static mesh, hierarchical instanced static mesh, SkyAtmosphere and AtmosphericFog components write native data after their properties. AssetGen writes exactly the bytes a cooked template of that class carries, and a class derived from one of them gets the same bytes. Other component classes need nothing extra. | Yes |
| `UE_COMPONENT(UModelComponent, Bsp);` | Refused: a model component "belongs to a level's BSP" and cannot be a template. | Refused |
| `UE_COMPONENT(UCharges, Ammo);`, with a component class the mod declares | Refused: "a UE_COMPONENT names an engine component class". Add a component of your own class at run time with `AddComponentByType` or `AddComponentDeferred` (below). An abstract one (a method `= 0`) is refused as "abstract": the engine never instances an abstract class. | Refused |
| `UE_COMPONENT(UTexture2D, Icon);` | Refused: "is not a UActorComponent". | Refused |
| `UE_COMPONENT` in a class that is not an actor | Refused: "only an actor has a construction script". | Refused |
| `UE_COMPONENT` in a `UE_INTERFACE` | Refused: "an interface cannot declare a UE_COMPONENT". Declare the component on each class that implements the interface. | Refused |

```cpp
class Lantern : public AActor {
  UE_COMPONENT(UProjectileMovementComponent, Move); // not a scene component: attached to nothing
  UE_COMPONENT(USceneComponent, Root);              // the first scene component: the root
  UE_COMPONENT(UStaticMeshComponent, Mesh);         // attached to Root
  UE_COMPONENT(UPointLightComponent, Lamp);         // attached to Root

public:
  void ReceiveBeginPlay() {
    Mesh->SetHiddenInGame(true, false);             // this actor's own Mesh
    Lamp->SetIntensity(3000.0f);
  }
};

class Dummy : public AActor {
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UHealthComponent, Health);           // DRG's own component classes
  UE_COMPONENT(UOutlineComponent, Outline);
  UE_DEFAULTS { Health->canTakeDamage = false; }   // not CreationMethod, which the engine sets and which is refused

public:
  void ReceiveBeginPlay() { Health->SetHealthDirectly(50.0f); }
};
```

Notes:

- Set a component's defaults in `UE_DEFAULTS` (`Health->canTakeDamage = false;`), not with an initializer. They belong
  on the component's template, not on the actor's Class Defaults. See [Class defaults](#class-defaults).
- The native data has to be exactly right: a template of the wrong length fails to load with a fatal "Serial size
  mismatch". The static mesh and instanced static mesh bytes match DRG's own cooked templates. The hierarchical
  instanced static mesh, SkyAtmosphere and AtmosphericFog bytes come from the engine source, because the game's content
  has no such template to compare with. Of these classes, only a static mesh component has been loaded in game.
- DRG's own component classes write nothing after their engine ancestors' data: every one that appears as a template
  in the game's content ends there. 177 of the SDK's DRG component classes never appear as a template, so their layout
  is unmeasured; most of them are abstract bases, objectives and character states. If one of them wrote data of its
  own, its template would fail to load the same way, and nothing checks for that.
- An instanced mesh component's template has no instances. Add them at run time.
- A component class of your own (a `UActorComponent` or `USceneComponent` child) is described in
  [Classes and variables](#classes-and-variables).
- [examples/Beacon.cpp](examples/Beacon.cpp) is an actor with a scene root, a mesh and a light.

### The root and attachment

| You write | What it does | Status |
|---|---|---|
| `UE_COMPONENT(USceneComponent, Root);` as the first scene component | Becomes the actor's root. | Yes |
| a later scene component | Attaches directly to the root, unless `SetupAttachment` places it. | Yes |
| a component that is not a scene component, such as `UProjectileMovementComponent` | Is created with no attachment, wherever it is declared. | Yes |
| no `UE_COMPONENT` at all | The actor gets the default scene root, as in the editor: a `USceneComponent` named DefaultSceneRoot, made by the class's construction script, which a reference can name across the network, and held in the class's variable `DefaultSceneRoot`. Below a parent that gives the actor a root already, it gets none. | Yes |
| a member named `DefaultSceneRoot` in an actor class | Refused: the construction script stores the default scene root in the variable of that name, a second one or the one a subclass declares. | Refused |
| only components that are not scene components | The actor gets the engine's default scene root too, as in the editor, so a movement component has a root to move. Below a parent that gives the actor a root already, a Blueprint parent or a native one such as ACharacter (its capsule), it gets none, as in the editor. | Yes |
| `UE_DEFAULTS { Tip->SetupAttachment(Glow); }`, Glow another `UE_COMPONENT` of the class | Tip attaches to Glow, as in a C++ constructor: in the construction script it is one of Glow's child nodes. It keeps its own location, rotation and scale, relative to Glow. | Yes |
| `Tip->SetupAttachment(Glow, FName("Muzzle"));` | The same at a socket or bone of Glow (the node's AttachToName). The socket is a literal name or none; a variable or a call there is refused, since it would be dropped. | Yes |
| `Glow->SetupAttachment(Lamp);`, Lamp a component of a mod or game Blueprint parent | Glow attaches to the inherited Lamp. Its node names Lamp and the parent class whose construction script makes it (ParentComponentOrVariableName, ParentComponentOwnerClassName), as the editor saves a component dropped on an inherited one. A game Blueprint's component needs the UeApi header to mark it as a construction-script node (`Scene__UeScsNode`); one without it is refused with what to regenerate. | Yes |
| `Glow->SetupAttachment(Mesh);` in an `ACharacter` child | Glow attaches to a native default subobject. The node names the subobject by its object name, `CharacterMesh0` for `Mesh` (bIsParentComponentNative), which UeApi records. It stays `CharacterMesh0` further down, in an `APlayerCharacter` child, though `FPMesh` is a skeletal mesh too: a subclass cannot rename a subobject its parent makes. A native member that is no default subobject is refused: attach to it at run time. | Yes |
| `Glow->SetupAttachment(RootComponent);` | Glow attaches to the actor's root, whichever component that is, as a constructor's call does. Below a parent that gives the actor a root, such as `ACharacter`'s capsule or a Blueprint parent's root, its node is a root node naming no parent, which the construction script attaches to that root. With no root to inherit, Glow is never the root itself: it hangs from the first scene component `SetupAttachment` leaves alone, or, with none, from the default scene root, which stays, as the editor keeps a component added under it. It keeps its own location, rotation and scale, relative to the root. `SetupAttachment(RootComponent, FName("Sock"))` attaches at that socket of the root, the node's AttachToName. | Yes |
| `Lamp->SetupAttachment(Own);` for an inherited Lamp, `A->SetupAttachment(B); B->SetupAttachment(A);`, or one component attached twice | Refused. An inherited component stays where its own class puts it. A cycle has no node the construction script starts from, so none of its components would be made. | Refused |
| `Pivot->SetupAttachment(Lamp);` in a function | Attaches at once, keeping the relative transform: `K2_AttachToComponent` with KeepRelative for location, rotation and scale, and no welding. That is what the engine's own `SetupAttachment` leads to when the component registers; called on a component already registered, as any of a constructed actor is, the engine's own does nothing, so the compiler warns, naming the function. Call `AttachToComponent` to pick the rules yourself. | Warns |

Notes:

- If the parent class already has a root, such as `ACharacter`'s capsule or a mod parent's first scene component, this
  class's first scene component attaches under that root instead of replacing it.
- With no root to inherit, the root is the first scene component that `SetupAttachment` leaves alone, even if a
  component declared before it is attached elsewhere, to `RootComponent` too.
- The root's own location, rotation and scale are a special case: see [Class defaults](#class-defaults).

### Adding components at run time

| You write | What it does | Status |
|---|---|---|
| `AddComponentByType<USceneComponent>(this)` | The Add Component by Class node (`AActor::AddComponentByClass`), returning the component as a `T*`. It is created at the owner's origin, and a scene component attaches to the owner's root. | Yes |
| `AddComponentByType<UCharges>(this, ChargesClass, true)` | The class can be picked at run time as a `TSubclassOf<T>`; it is `T` by default. The third argument is bManualAttachment: with `true`, a scene component is not attached. A component class the mod declares itself works here. | Yes |
| `AddComponentByType<UAbstractComp>(this)`, where the class has a `= 0` method left or is a UE_FINAL_AS base | Refused: "is an abstract class ... which the engine may not construct". AddComponentByClass constructs it with NewObject, which asserts on an abstract class in a Development game. Add a subclass that defines every `= 0` method, or the UE_FINAL_AS leaf, which the message names. Only a class the call names is checked. | Refused |
| `AddComponentDeferred<UCharges>(this)`, then `FinishComponent(this, C)` | The Add Component node with exposed pins. Registration, and with it the component's BeginPlay, waits for `FinishComponent` (`AActor::FinishAddComponent`), so the values you set in between are the first ones it sees. | Yes |
| `AttachToComponent(Muzzle, Barrel)` | `K2_AttachToComponent` with one attachment rule for location, rotation and scale (SnapToTarget unless you pass another) and welding on. Returns whether it attached. | Yes |
| `AttachToComponent(Gun, Hand, FName("hand_r"), EAttachmentRule::KeepRelative, false)` | The same at a socket, with another rule and without welding. For a different rule per channel, call `Gun->K2_AttachToComponent(...)` yourself. | Yes |

```cpp
#include "../include/Objects.h" // AssetGen's include/ folder, by its path from your source

class UCharges : public UActorComponent { // a component class of the mod's own
public:
  int32 Left = 3;
};

class Turret : public AActor {
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UStaticMeshComponent, Barrel);

public:
  void ReceiveBeginPlay() {
    UCharges *C = AddComponentDeferred<UCharges>(this);
    C->Left = 5;                                                         // the first value the component sees
    FinishComponent(this, C);
    USceneComponent *Muzzle = AddComponentByType<USceneComponent>(this); // attached to Root
    AttachToComponent(Muzzle, Barrel);                                   // then moved under Barrel
  }
};
```

Notes:

- `Objects.h` is in AssetGen's `include/` folder, not in `UeApi`. Include it by its path from your source,
  `#include "../include/Objects.h"` as the examples do, or copy it beside your source; a bare `#include "Objects.h"`
  finds it only there. [Creating objects](#creating-objects) has the rest of it.
- Pass the same bManualAttachment to `AddComponentDeferred` and `FinishComponent`: the finish's decides the
  attachment. Two different constants get a warning, and so does a function that adds deferred and never calls
  `FinishComponent`, unless it hands the component on (to a member, an out parameter, its return value or a script
  function), where the finish may be.
- The usage comment at the top of `Objects.h` writes `GetRootComponent()`, which the SDK does not declare. Write
  `K2_GetRootComponent()`.

## Class defaults

`UE_DEFAULTS { ... }` sets the defaults a class does not declare itself: those of its components, and those of
properties a parent class declares. It is what editing a component or Class Defaults in the details panel writes. The
block is data: AssetGen reads its assignments when it compiles, and nothing in it ever runs. The rule to remember:
every statement is `Field = value;` or `Component->Field = value;`, and a variable the class declares takes its default
from its own initializer instead. The one call it takes is `Component->SetupAttachment(Parent);`, which places a
component: see [The root and attachment](#the-root-and-attachment).

### UE_DEFAULTS

| You write | What it does | Status |
|---|---|---|
| `UE_DEFAULTS { InitialLifeSpan = 3.0f; }` | This class's default for a property a parent declares (an engine, game or mod class), written on its default object as Class Defaults would. No `Super::` is needed. It works in any class, not only an actor. | Yes |
| `Lamp->Intensity = 1500.0f;` | A default on the template of a component this class declares, as editing the component in the details panel writes it. A zero or `false` is still written, because a template is compared with the component class's own defaults, where `bVisible` is true. | Yes |
| `Lamp->LightColor = FColor(255, 128, 0);`, `Lamp->LightColor = {255, 128, 0};` | A struct value, by constructor or by braces. Each argument goes to the member its parameter is named after, so `FColor` takes R, G, B, A as in C++, although it stores B, G, R, A. Braces on an SDK struct that has a constructor call that constructor. Only a struct with no constructor is filled by member position. | Yes |
| `Ids = {2, 3};`, `Score = {{"a", 5}, {"b", 2}};` on a TSet or TMap a parent declares | This class's whole value, as Class Defaults would set it. The default object loads a set or map over its parent's value, so the compiler writes what differs, as the editor saves it: the parent's elements (a map's keys) this value lacks, as removed, then the elements the parent lacks or maps to another value. The parent's value is known where a header gives it: an initializer, a `UE_DEFAULTS` or a `UE_STRUCT` member's default instance, in a class of this mod or of another mod's shared header (`UE_CLASS_IN`, or `UE_CLASS` where the header gives the member a value). Below an engine or game class, whose value no header says, the elements are added to that class's own, with a warning naming the member, and so below a class whose `UE_DEFAULTS` sets the member over one (its default object holds that class's elements too; the statement's own are still removed). The same holds for a TSet or TMap inside a UE_STRUCT a parent declares, when you assign the struct whole (`Held = {{2, 3}, 3};`). A key given twice holds the last value given, as in C++, and an FString key compares without case: `{{"a", 5}, {"A", 1}}` is `{"A": 1}`. | Yes |
| `Mesh->StaticMesh = &SM_Crate_B;` | Points an object property at an asset: a `UE_ASSET_AT`, a UeAssets name or an asset the mod cooks. The same as picking the asset in the details panel. | Yes |
| `Index_0 = 7;`, for a member the SDK spells `Index_0` | Written under the engine's real name, `Index`. See [Classes and variables](#classes-and-variables). | Yes |
| `Lamp->RelativeLocation.Z = 50.0f;` | Refused. Assign the whole struct: `Lamp->RelativeLocation = FVector(0.0f, 0.0f, 50.0f);`. The message is misleading: it says UeApi "does not say which default subobject RelativeLocation is", and regenerating the SDK does not help. | Refused |
| `int32 Charges;` with `UE_DEFAULTS { Charges = 3; }` | Refused: "is declared here - give it an initializer instead". Write `int32 Charges = 3;`. | Refused |
| `UE_DEFAULTS;` in the class, `void UTurret::UeDefaults__() { Charges = 3; }` outside it | Refused: "UE_DEFAULTS has no body in the class". The statements are read where the macro is written, which is also all a mod that includes the class's header sees. Write `UE_DEFAULTS { Charges = 3; }` in the class. | Refused |
| `Extra->bVisible = false;`, where `Extra` is a plain pointer member | Refused: "is not a UE_COMPONENT". Only a `UE_COMPONENT`, this class's or a parent's, has a template to hold defaults. | Refused |
| `Lamp->Intensity += 100.0f;`, an `if`, a call such as `K2_DestroyActor();` | Refused: "every statement is `Field = value;`, `Component->Field = value;` or `Component->SetupAttachment(Parent);`". The block never runs, so logic in it could do nothing. | Refused |
| `Lamp->Intensity = UKismetMathLibrary::RandomFloat();`, `InitialLifeSpan = sizeof(FVector);` | Refused: "a default is a value known when the mod is built". A value here follows the rules for a member's initializer: see [Classes and variables](#classes-and-variables). Compute anything else in `ReceiveBeginPlay` or `UserConstructionScript`. | Refused |
| `Instigator = nullptr;`, `Mesh->StaticMesh = nullptr;`, `Count = {};`, `Rule = EAttachmentRule();`, `Offset = FVector();` | The type's zero, written over the parent's value: null, 0, `false`, the zero enumerator, None, an empty string or container, and the zeros of `FVector`, `FVector2D`, `FRotator`, `FLinearColor`, `FColor` and the `FVector_NetQuantize` types, whose engine constructor sets nothing. A `UE_STRUCT`'s `{}` or `T()` is its own defaults, and so is another mod's (`UE_STRUCT_IN`): its header says each member's initializer, written. The same as `Count = 0;`. | Yes |
| `Hit = FHitResult();`, `Hit = {};`, `Floor = FFindFloorResult();`, `Xf = FTransform();`, a `UE_STRUCT`'s `{}` that holds an FHitResult | Refused: "holds what the engine's FHitResult constructor sets". Any other engine struct than those above holds what the engine's constructor sets (`FHitResult::Time` is 1, `FTransform()` is the identity), which no header says, so it cannot be written over the parent's value. Leave the statement out, or give the members in braces: `Hit = {.Time = 1.0f};`. `V4 = FVector4();` is written, (0, 0, 0, 1). | Refused |
| `Hit = {.Distance = 6.0f};` | The members given are written. One the braces leave out holds what the engine's constructor sets, which no header says: it is left unwritten where the parent's value is a fresh one (declared in a mod parent with no initializer, `{}` or `T()`, or with braces that leave it out, followed down, and set by no `UE_DEFAULTS` on the way), and refused over any other value: "Hit.FaceIndex, left out of the braces, holds what the engine's FHitResult constructor sets". Give every member, or leave the statement out. A `UE_STRUCT` value's braces, `H = {.N = 5};`, refuse a member of such a struct they leave out the same way. Braces nested for a `UE_STRUCT`'s member, `H = {.Hit = {.Distance = 5.0f}};`, are over a fresh value where that member has no initializer of its own, or one whose braces, followed down, leave it out: with `FInner In = {.K = 4};`, `O = {.In = {.Hit = {.Distance = 3.0f}}};` leaves the rest of Hit unwritten, and where the initializer gives Hit `{.Time = 0.5f}`, the Time the new braces leave out is refused. Another mod's struct (`UE_STRUCT_IN`) is no engine struct: a member its braces leave out takes the initializer its header gives it, written. | Yes |
| `Hit = {.Time = 3.0f};` then `Hit = {.Distance = 6.0f};` | The last statement on a member is its value, as the last assignment is in C++: the earlier one writes nothing, so `Hit` is Distance 6 with Time the engine's 1, not 3. | Yes |
| `Handle = FTimerHandle();` | Writes nothing, with a warning: FTimerHandle's one member is Transient, which the engine never loads from a default, so the value stays the parent's. | Warns |
| `Lantern() { InitialLifeSpan = 5.0f; }` | Not yet: a constructor is dropped with no message. It makes no function and writes no default. Use initializers and `UE_DEFAULTS`, and do run-time setup in `ReceiveBeginPlay`. | Not yet |

```cpp
UE_ASSET_AT(UStaticMesh, SM_Crate_B, "/Game/Art/Environments/SpaceRig/SM_Crate_B");

class Crate : public AActor {
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UStaticMeshComponent, Mesh);
  UE_COMPONENT(UPointLightComponent, Lamp);

  UE_DEFAULTS {
    InitialLifeSpan = 30.0f;                             // a property AActor declares
    Mesh->StaticMesh = &SM_Crate_B;                      // an asset, as picked in the details panel
    Mesh->bVisible = false;                              // written, although false
    Lamp->Intensity = 1500.0f;
    Lamp->LightColor = FColor(255, 128, 0);              // R, G, B; A defaults to 255
    Lamp->RelativeLocation = FVector(0.0f, 0.0f, 50.0f); // the whole struct, never .Z alone
  }
};
```

Notes:

- Do not redeclare an inherited property to change its default. It is refused ("AActor already has a variable
  InitialLifeSpan"): it would add a second variable of the same name that hides the parent's. Set it in UE_DEFAULTS:

  ```cpp
  class Shadowed : public AActor {
    float InitialLifeSpan = 3.0f; // a second variable that hides AActor's
  };
  ```

- A constructor call needs a value for every parameter, from an argument or a default argument (`FColor`'s A has
  one); clang rejects one with too few. Braces on a struct with no constructor fill its members in order, and a member
  left out is written as zero.
- A default is written as data, not by a node, so it can set properties that no Blueprint node can reach, such as
  EditDefaultsOnly ones.
- A class has one `UE_DEFAULTS` block.
- The path of a `UE_ASSET_AT` is not checked when the mod is built: see [Game assets](#game-assets).

### The root's transform

The engine puts an actor's root at the spawn transform and ignores the location, rotation and scale on the root's
template. This is only about a class whose parent has no root: below a mod or game Blueprint parent, or a native one
with a scene component such as `ACharacter`, the first scene component attaches under the inherited root and keeps
its own location, rotation and scale like any other.

| You write | What it does | Status |
|---|---|---|
| `Root->RelativeRotation = FRotator(0.0f, 90.0f, 0.0f);` on a plain `USceneComponent` root | AssetGen moves the root's location, rotation and scale onto every component attached to it, composed the way UE composes a child with its parent, and removes them from the root. The children end up where the editor's viewport would show them, however the actor is spawned. The root itself stays at the spawn transform. | Yes |
| `Body->RelativeScale3D = FVector(2.0f, 2.0f, 2.0f);` on a root that is a mesh, a light or any class but `USceneComponent` | Warns: "is the actor's root, which the engine puts at the spawn transform". Such a root cannot pass its transform on without losing its own, so the value stays on the template, where the engine does not apply it. Declare a `USceneComponent` first and let the mesh attach to it. | Warns |

```cpp
class Turntable : public AActor {
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UStaticMeshComponent, Mesh);
  UE_DEFAULTS {
    Root->RelativeLocation = FVector(10.0f, 0.0f, 0.0f);
    Root->RelativeRotation = FRotator(0.0f, 90.0f, 0.0f); // Pitch, Yaw, Roll
    Root->RelativeScale3D = FVector(2.0f, 2.0f, 3.0f);    // all three end up on Mesh
  }
};

class Statue : public AActor {
  UE_COMPONENT(UStaticMeshComponent, Body);                          // the first scene component: the root
  UE_DEFAULTS { Body->RelativeScale3D = FVector(2.0f, 2.0f, 2.0f); } // warns: not applied
};
```

Notes:

- Only a class that is exactly `USceneComponent` counts as plain. A subclass of it keeps its transform, with the
  warning.
- A negative scale, and a child with an absolute transform, get no special handling.

### Components a parent declares

| You write | What it does | Status |
|---|---|---|
| `Lamp->Intensity = 250.0f;`, where a mod parent declares `Lamp` | Overrides that component's defaults for this class only, as the editor does for an inherited component. No `Super::` is needed: the compiler finds the class that declares the member. A grandchild that sets `Lamp->bVisible = false;` keeps the 250 as well: defaults fold down the chain as C++ constructors do. | Yes |
| `CapsuleComponent->CapsuleRadius = 55.0f;` in an `ACharacter` child | A C++ parent's component is a default subobject. AssetGen overrides it under the subobject's real name and class, which can differ from the member's: `ACharacter`'s `CapsuleComponent` is `CollisionCylinder`. The SDK records the name as `<Member>__UeSubobject`, for a subclass too: `Mesh` in an `APlayerCharacter` child is still `CharacterMesh0`. Where two of the class's subobjects fit the member and neither has its name, it records the one the game's Blueprints of the class name on their default objects: `ABomber`'s `GooSoundComponent` is `GooAudioComponent`, not `WingSound`. | Yes |
| `CapsuleComponent->CapsuleRadius = 70.0f;` in a child of a mod class that sets the capsule too | Builds on the parent's override: the child keeps what the parent set, such as its half height, and changes only the radius. The parent is loaded first. A mod parent that leaves the capsule alone still carries one for its child to build on. | Yes |
| `temperature->TemperatureChangeScale = 2.0f;` in a child of a game Blueprint (`ENE_Spider_Grunt_Normal_C`) | The C++ ancestor's component is a default subobject of the game Blueprint's default object, which its package exports: the override is built on that export, and the class loads after every default subobject the parent exports, restated or not, and every object nested in one, such as the bonus instanced in `WPN_Pickaxe_C`'s `Damage` (the SDK lists them as `UeDefaultSubobjects`). The SDK spells a name as the game's object dump does, which can differ in case from the parent's package (`temperature`, `Temperature`); names compare without case, so both are one object. | Yes |
| `StaticMesh->RelativeScale3D = FVector(2.0f, 2.0f, 2.0f);` in a child of a game Blueprint | A game Blueprint's component is a construction-script node, as a mod parent's is. The override is keyed on that node's GUID, which the SDK records as `<Component>__UeScsNode`. The node's real name is used, even when it contains spaces. | Yes |
| `Controller->bAttachToPawn = true;` in an `APawn` child | Refused: "UeApi does not say which default subobject Controller is". Either the member is not a default subobject, and you set the value at run time, or the SDK predates the markers, and you regenerate it with genueapi. | Refused |
| a component of a game Blueprint whose header has no `__UeScsNode` marker | Refused: "its header does not say which SCS node it is". Regenerate the SDK from a dump made with the Dumper-7 fork, which writes the markers. | Refused |
| a component of a mod parent that another mod owns (pinned with `UE_CLASS`) | Not yet: refused with the same SCS node message, and its advice to re-dump the game does not apply. Set the value at run time, for example in `ReceiveBeginPlay`. | Not yet |

```cpp
#include "UeApi/Game/BP_ExplosiveBarrel_C.h"

class ShortLived : public AActor {
  UE_DEFAULTS { InitialLifeSpan = 3.0f; }                 // AActor's property, this class's default
};

class BaseProp : public AActor {
public:
  UE_COMPONENT(USceneComponent, Root);
  UE_COMPONENT(UPointLightComponent, Lamp);
  UE_DEFAULTS { Lamp->Intensity = 1000.0f; }
};

class DimProp : public BaseProp {                          // a component a mod parent declares
  UE_DEFAULTS { Lamp->Intensity = 250.0f; }
};

class Walker : public ACharacter {                         // default subobjects of an engine parent
  UE_DEFAULTS {
    CapsuleComponent->CapsuleRadius = 55.0f;               // written to the subobject CollisionCylinder
    Mesh->bVisible = false;
  }
};

class BigBarrel : public Game::GameElements::GameEvents::ExplosiveBarrelsEvent::BP_ExplosiveBarrel_C {
  UE_DEFAULTS { StaticMesh->RelativeScale3D = FVector(2.0f, 2.0f, 2.0f); } // a game Blueprint's component
};
```

Notes:

- genueapi writes both kinds of marker. The subobject names come from the object dump, and the SCS node GUIDs need a
  dump made with the Dumper-7 fork. See [The SDK](GUIDE.md#the-sdk).
- [examples/GameBlueprintChild.cpp](examples/GameBlueprintChild.cpp) extends a game Blueprint and sets inherited
  defaults.

## Creating objects

`Objects.h`, in AssetGen's `include/` folder, spells the editor's Spawn Actor from Class, Construct Object from Class
and Create Widget nodes as C++ templates. Include it by its path from your source: the examples write
`#include "../include/Objects.h"`. Adding and attaching components is in [Components](#components). The rule to
remember: each helper takes one kind of class, and the world context of the engine calls inside it is the calling
object.

### Spawning actors

| You write | What it does | Status |
|---|---|---|
| `SpawnActor<AActor>(AActor::StaticClass(), Where, this)` | The Spawn Actor from Class node: `BeginDeferredActorSpawnFromClass`, then `FinishSpawningActor`. It returns the new actor as a `T*`. The arguments are the class, the transform, the owner (none by default) and the collision handling (`Undefined` by default). The new actor's construction script and BeginPlay run inside the call. | Yes |
| `SpawnActorDeferred<Spawner>(Spawner::StaticClass(), Where)`, then `FinishSpawning(Twin, Where)` | The Spawn Actor node with Expose on Spawn pins. The actor is created but not finished. Set its variables, and `FinishSpawning` runs its construction script and BeginPlay with those values in place. Pass the same transform to both calls. | Yes |
| `AttachToActor(Plain, Twin, FName(), EAttachmentRule::KeepWorld)` | `K2_AttachToActor` with one rule for location, rotation and scale (SnapToTarget unless you pass another) and welding on. | Yes |

```cpp
#include "../include/Objects.h"

class Spawner : public AActor {
  int32 Tag = 0;

public:
  void ReceiveBeginPlay() {
    if (Tag == 7) return;                                                  // the twin runs this too
    FTransform Where = FVector(0.0f, 0.0f, 100.0f);
    AActor *Plain = SpawnActor<AActor>(AActor::StaticClass(), Where, this); // Owner = this
    Spawner *Twin = SpawnActorDeferred<Spawner>(Spawner::StaticClass(), Where);
    Twin->Tag = 7;                                                         // seen by the twin's construction and BeginPlay
    FinishSpawning(Twin, Where);
    AttachToActor(Plain, Twin, FName(), EAttachmentRule::KeepWorld);
  }
};
```

Notes:

- The world context is the caller: `this`, or a static function's own `WorldContextObject` parameter. See
  [Calling engine and game functions](#calling-engine-and-game-functions).
- Until `FinishSpawning` runs, a deferred actor has not run its construction script or BeginPlay. A function that
  spawns deferred and never calls `FinishSpawning` gets a warning, unless it hands the actor on (to a member, an out
  parameter, its return value or a script function), where the finish may be.
- [examples/Beacon.cpp](examples/Beacon.cpp) spawns an actor of the mod near the player.

### Objects and widgets

| You write | What it does | Status |
|---|---|---|
| `NewObject<UEnemyContext>(this)` | The Construct Object from Class node (`UGameplayStatics::SpawnObject`): a new object of `T`, with `this` as its Outer. `T` can be neither an actor nor a component, the same rule the editor's node applies. | Yes |
| `NewObject<UObject>(this, Kind)` | The class picked at run time, as a `TSubclassOf<T>`. | Yes |
| `CreateWidget<UUserWidget>(PlayerController, HudClass)` | The Create Widget node (`UWidgetBlueprintLibrary::Create`), with the calling object as world context. The owning player may be null. | Yes |
| `NewObject<AActor>(this)`, `SpawnActor<UObject>(UObject::StaticClass(), Where)` | Refused by clang on the calling line: "no matching function", then a note naming the constraint `T` fails. `SpawnActor` and `SpawnActorDeferred` take an actor class, `AddComponentByType` and `AddComponentDeferred` a component class, `CreateWidget` a widget class, and `NewObject` any other class. | Refused |
| `SpawnActor<AShape>(AShape::StaticClass(), Where)`, where the class has a `= 0` method left or is a UE_FINAL_AS base | Warns: "is an abstract class" (for a UE_FINAL_AS base, naming the leaf to spawn). SpawnActor makes no actor of an abstract class and returns None. Only a class the call names is checked (`X::StaticClass()`); one picked at run time is not. | Warns |
| `NewObject<USpec>(this)`, where the class has a `= 0` method left or is a UE_FINAL_AS base | Refused: "is an abstract class ... which the engine may not construct". SpawnObject makes one in a Shipping game and asserts in a Development one; the editor's Construct Object node refuses the class too. Construct a subclass that defines every `= 0` method, or the UE_FINAL_AS leaf the message names. Only a class the call names is checked (`X::StaticClass()`, or `NewObject`'s default). | Refused |
| `NewObject<UProbe>(nullptr)` | Warns: "SpawnObject with no Outer (None) makes nothing and returns None". Pass the object that owns it, such as `this`. | Warns |

```cpp
#include "../include/Objects.h"

class UEnemyContext : public UObject {
public:
  int32 Seen = 0;
};

class Hud : public AActor {
public:
  TSubclassOf<UUserWidget> HudClass; // a concrete widget class, set before the call
  TSubclassOf<UObject> Kind;
  UEnemyContext *Ctx;
  UObject *Any;
  UUserWidget *Widget;

  void ReceiveBeginPlay() {
    Ctx = NewObject<UEnemyContext>(this); // Outer = this actor
    Any = NewObject<UObject>(this, Kind); // the class picked at run time
    APlayerController *PlayerController = UGameplayStatics::GetPlayerController(this, 0);
    Widget = CreateWidget<UUserWidget>(PlayerController, HudClass);
  }
};
```

Notes:

- The Outer decides the object's world. An object of a class that waits (`Delay` and the like) finds its world only
  through its Outer, so give it an actor or a component as its Outer. The compiler warns on such a waiting function:
  "finds its world only through its Outer". See [Latent calls](#latent-calls).
- The engine's `SpawnObject` checks nothing and would hand back an actor that no world spawned or a component that
  nothing registered. The helpers' type constraints are what stop that.
- `HudClass` has to hold a concrete widget class when the call runs. A null or abstract class makes no widget. A
  widget class of your own is described in [Classes and variables](#classes-and-variables).
- The engine marks `Create` cosmetic, but AssetGen calls it directly, so unlike the editor's node it also runs on a
  dedicated server. Guard it if a server can run the code; see
  [Working with other objects](#working-with-other-objects).

### StaticClass on the mod's classes

| You write | What it does | Status |
|---|---|---|
| `Spawner::StaticClass()`, on one of the mod's own classes | Names that class (`Spawner_C`), as the editor's class picker does, although C++ finds the `StaticClass` of the engine parent. It also works written through a macro in the mod's own source. | Yes |
| `NewObject<UEnemyContext>(this)` | A helper's default `T::StaticClass()` argument names `T`. | Yes |
| `Weapons::Rifle::StaticClass()` | Not yet: a namespace-qualified name is not recognised, and the call names the class that `Rifle` inherits `StaticClass` from (`AActor` here; for a child of a game Blueprint, that Blueprint class), with no message. Write `Rifle::StaticClass()` inside the namespace, or, outside it, name the class through an alias: `using RifleT = Weapons::Rifle;`, then `RifleT::StaticClass()`, as [examples/GameBlueprintChild.cpp](examples/GameBlueprintChild.cpp) does. In a `TSubclassOf<Weapons::Rifle>` slot, such as `SpawnActor<Weapons::Rifle>`'s class argument, the result is right, because the slot's type decides. | Not yet |

```cpp
#include "../include/Objects.h"

namespace Weapons {
class Rifle : public AActor {
public:
  UClass *Own() { return Rifle::StaticClass(); } // Rifle_C
};
} // namespace Weapons

using RifleT = Weapons::Rifle;

class Armory : public AActor {
public:
  UClass *Wrong() { return Weapons::Rifle::StaticClass(); } // AActor: the qualified name is not recognised
  UClass *ViaAlias() { return RifleT::StaticClass(); }      // Rifle_C
  Weapons::Rifle *Make(FTransform Where) {
    return SpawnActor<Weapons::Rifle>(Weapons::Rifle::StaticClass(), Where); // Rifle_C: the slot's type wins
  }
};
```

Notes:

- A `StaticClass()` call written through a macro that a `UeApi` header defines has the same problem as a qualified
  name.
- `StaticClass()` on engine and game classes, and `GetClass()`, are in [Types](#types).

### Helpers of your own

| You write | What it does | Status |
|---|---|---|
| `template <Derives<AActor> T> inline T *SpawnAbove(...)` | Every `Objects.h` function is an inline template, and AssetGen splices its body into the caller. A helper costs exactly the engine calls inside it and adds no function of its own. Write more the same way in your source. | Yes |

```cpp
#include "../include/Objects.h"

template <Derives<AActor> T>
inline T *SpawnAbove(TSubclassOf<T> Class, AActor *Anchor, float Height) {
  FTransform Where = Anchor->K2_GetActorLocation() + FVector(0.0f, 0.0f, Height);
  return SpawnActor<T>(Class, Where);
}

class Helpers : public AActor {
public:
  AActor *Drop() { return SpawnAbove<AActor>(AActor::StaticClass(), this, 200.0f); }
};
```

Notes:

- Make a helper a free function, not a method: an inline method expands only when it is called on `this`. See
  [Inline functions and templates](#inline-functions-and-templates).
- The engine calls inside a helper take the caller as their world context: `this`, or a static caller's own
  `WorldContext...` parameter.

## Working with other objects

Another object's properties, containers and functions are reached through a pointer, as in C++, and each access
becomes the node the editor would use with that object on its Target pin. This section also covers testing and
comparing object pointers, UObject's C++ helpers, and finding the player, other actors and subsystems. The rule to
remember: unlike C++, a read through a null object does not crash; it gives zero and the game logs "Accessed None".
`GetOuter()` is the exception.

### Properties of other objects

| You write | What it does | Status |
|---|---|---|
| `Char->InventoryComponent->MiningItem` | The Get node with a Target pin: the property is read from that object, and a chain reads each object in turn. Properties of game classes, of the mod's other classes and of data assets read the same way. A read through a null object gives zero or empty, and the game logs "Accessed None". | Yes |
| `Other->Hits += 1;`, `Other->Cursor = Next();` | The Set node with a Target pin. As in C++17, the right side is evaluated before the object is located, so if the right side changes what the pointer names, the write goes to the new object. | Yes |
| `GetPeer()->Slots.Add(5);`, `GetPeer()->SlotMap[FName("a")] = 7;` | A container on another object is changed where it lives, as the editor's container nodes do with a Target. The object is located before the arguments are evaluated, which is C++ order. | Yes |
| `GetPeer()->OnPeerHit.Broadcast(3);` | Fires the dispatcher of another object of the same class. Another mod class's, a game Blueprint's and a BlueprintCallable native dispatcher can be broadcast too; see [Event dispatchers](#event-dispatchers). | Yes |

```cpp
class Peer : public AActor {
public:
  int32 Hits = 0;
  int32 Cursor = 0;
  TArray<int32> Slots;
  TMap<FName, int32> SlotMap;
  UE_DISPATCHER(OnPeerHit, int32 Damage);
  Peer *Other;
  Peer *GetPeer() { return Other; }
  int32 Next() { return 1; }
  void Poke() {
    Other->Hits += 1;                   // the Set node, with Other as its Target
    Other->Cursor = Next();             // Next() runs before Other is read
    GetPeer()->Slots.Add(5);            // changed where it lives
    GetPeer()->SlotMap[FName("a")] = 7;
    GetPeer()->OnPeerHit.Broadcast(3);  // another object of this class
  }
};
```

Notes:

- Guard a read with `if (Obj)` when the object can be null.
- A member Dumper-7 renamed, such as `Name_0`, is read under the engine's real name.
- A call through a null object is skipped in the same way; see
  [Calling engine and game functions](#calling-engine-and-game-functions).

### Validity and comparison

| You write | What it does | Status |
|---|---|---|
| `if (Obj)`, `!Obj`, `Obj ? A : B` | The Is Valid node: true only for an object that is not null and is not being destroyed (pending kill). This is stricter than C++'s null test. | Yes |
| `Obj != nullptr`, `Obj == nullptr` | The Equal and Not Equal (Object) nodes against None: a plain pointer comparison. An object that is being destroyed still compares `!= nullptr`. Use `if (Obj)` to treat it as gone. | Yes |
| `Hit != this`, `Other->GetClass() == GetClass()` | `EqualEqual_ObjectObject` and `NotEqual_ObjectObject`: object identity. | Yes |
| `Target && Target->InitialLifeSpan > 0.0f` | The right side is not read when `Target` is null, as in C++. Blueprint's AND node would evaluate both sides, so AssetGen keeps a branch here. | Yes |
| `A < B` on object pointers | Refused: Blueprint compares objects only with `==` and `!=`. See [Operators](#operators). | Refused |

```cpp
class Others : public AActor {
public:
  int32 Score = 0;
  bool Equipped(APlayerCharacter *Char) {
    APickaxeItem *Pickaxe = Char->InventoryComponent->MiningItem; // reads each object in turn
    return Pickaxe ? Pickaxe->IsEquipped : false;                 // IsValid, then the read
  }
  void Shorten(AActor *Target) {
    if (Target && Target->InitialLifeSpan > 0.0f)                 // not read when Target is null
      Target->InitialLifeSpan = 1.0f;
  }
  void Count(AActor *Hit, APawn *Pawn) {
    if (!Hit) return;                                             // IsValid: false while Hit is being destroyed
    if (Hit != this) Score += 1;                                  // identity
    if (Pawn != nullptr) Score += 1;                              // a plain compare
  }
};
```

Notes:

- A `TScriptInterface` compared with `nullptr` tests whether an object is behind it: see [Interfaces](#interfaces).
- To test an object's class, `Cast<T>` it: see [Types](#types).

### UObject helpers

| You write | What it does | Status |
|---|---|---|
| `Obj->GetClass()` | Get Class. The SDK forwards it to `UGameplayStatics::GetObjectClass(Obj)`. It works on `this` and on any other object. | Yes |
| `Obj->GetName()` | Get Object Name (`UKismetSystemLibrary::GetObjectName`). A class with a `GetName` function of its own, such as `UFSDSaveGame`, calls that one instead. | Yes |
| `Obj->GetOuter()` | Reads the object's outer pointer from memory, with no engine call. On a null object it crashes, as in C++. For a null-safe call, use `UKismetSystemLibrary::GetOuterObject(Obj)`. | Yes |
| `GetTypedOuter<AActor>(Obj)` | From `Objects.h`: the nearest outer of `Obj` that is a `T`, or null. `Obj` itself is never a candidate, and a null `Obj` gives null. Each step is one outer read and one Cast. | Yes |
| `GetOutermostTypedOuter<ULevel>(Obj)` | From `Objects.h`: the same walk, keeping the farthest outer that is a `T`. | Yes |
| `GetOuter()` or `GetTypedOuter<T>()` in a function that waits (`Delay`, `UE_AWAIT`) | Not yet: refused with "a pointer read in a function that makes a latent call". Do the walk in a separate function that does not wait, or loop over `UKismetSystemLibrary::GetOuterObject`, which is an engine call. | Not yet |
| `Obj->IsA(AActor::StaticClass())` | Get Class, then Class Is Child Of: `UKismetMathLibrary::ClassIsChildOf(UGameplayStatics::GetObjectClass(Obj), Class)`. False on a null object. Needs `UeApi/Engine.h`. | Yes |
| `Class->IsChildOf(AActor::StaticClass())` | Class Is Child Of (`UKismetMathLibrary::ClassIsChildOf`). False on a null class. | Yes |
| `Obj->Class` | Refused by clang: "no member named 'Class' in 'UObject'". The SDK declares none of UObject's raw fields (`Class`, `Outer`, `Name`). Use the helpers above. | Refused |

```cpp
#include "../include/Objects.h"

class Names : public AActor {
public:
  FString Who(UObject *Obj) { return Obj->GetOuter()->GetName() + "/" + Obj->GetName(); }
  bool SameKind(UObject *Other) { return Other->GetClass() == GetClass(); }
  AActor *OwningActor(UObject *Obj) { return GetTypedOuter<AActor>(Obj); }      // Objects.h
  ULevel *LevelOf(UObject *Obj) { return GetOutermostTypedOuter<ULevel>(Obj); } // Objects.h
  UObject *SafeOuter(UObject *Obj) { return UKismetSystemLibrary::GetOuterObject(Obj); }
};
```

Notes:

- `GetOuter()` needs nothing declared. The compiler adds a small `FDeref` struct for the memory read and cooks it
  into the mod; see [Pointers and memory](#pointers-and-memory). A header comment that says a mod declares `FDeref`
  itself, or that the outer walk is a `GetOuterObject` call, is out of date.
- The forwarders need the header that declares their target. Without it the compiler says "which is not declared
  here: include its UeApi header"; `UGameplayStatics` and `UKismetSystemLibrary` are in `UeApi/Engine.h`.

### Methods on structs, text and soft pointers

The editor drags a struct pin out to every library function that takes the struct first. UeApi declares each of those
as a method of the struct, so calls chain the way the nodes do. FString, FName, FText, `TSoftObjectPtr<T>` and
`TSoftClassPtr<T>` get them too.

| You write | What it does | Status |
|---|---|---|
| `AssetData.GetExportTextName()` | `UAssetRegistryHelpers::GetExportTextName(AssetData)`: the value goes first, the arguments after it. | Yes |
| `Path.ToSoftClassPtr()`, `V.ToString()` | A one-argument `Conv_XToY` is `To<Y>()`: `Conv_SoftClassPathToSoftClassRef`, `Conv_VectorToString`. | Yes |
| `Soft.LoadClassAsset_Blocking()` | The same on a soft pointer, `UKismetSystemLibrary::LoadClassAsset_Blocking(Soft)`. | Yes |
| A library function taking a world context, or a latent one, as a method | Not declared: call the static. | Not yet |

```cpp
UClass *ClassOf(const FAssetData &AssetData)
{
  return AssetData.GetExportTextName().MakeSoftClassPath().ToSoftClassPtr().LoadClassAsset_Blocking();
}
```

Notes:

- Hover a method in the editor: the comment above it names the library function and the UeApi header it is in. The
  call needs that header included; without it the compiler says "which is not declared here: include its UeApi header".
- One function per name. Where two libraries have one, `/Script/Engine`'s wins; call the other as a static.
- A function whose first parameter is a non-const reference (it changes the value) is not a method.

### The player and the game

| You write | What it does | Status |
|---|---|---|
| `UGameFunctionLibrary::GetLocalPlayerCharacter(this)` | DRG's getter for the character this machine controls, already an `APlayerCharacter`, so no cast is needed. `this` can be left out. It returns null until the character exists, so a mod that starts in `ReceiveBeginPlay` waits for it in a `Delay` loop. | Yes |
| `UGameplayStatics::GetPlayerController(0)`, `GetPlayerPawn(0)`, `GetPlayerCharacter(0)` | The Get Player Controller, Get Player Pawn and Get Player Character nodes. They return engine types, so `Cast<APlayerCharacter>` the result to reach DRG's members. The index counts the player controllers in this world. A client has only its own, so 0 is the local player. | Yes |
| `UGameFunctionLibrary::GetFSDGameState()` | One of DRG's typed getters: `GetFSDGameState`, `GetFSDGameMode`, `GetFSDGameInstance`, `GetFSDSaveGame`, `GetCSGWorld`, `GetNumPlayers` and more. The world context is filled in. | Yes |
| `for (APlayerCharacter *P : GS->ActivePlayerCharacters)` | The game state's list of every player's character, walked in place with no copy. | Yes |

```cpp
class Census : public AActor {
  APlayerCharacter *Char = nullptr;

public:
  void ReceiveBeginPlay() {
    APlayerCharacter *C = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (C == nullptr) { // null until the character exists
      UKismetSystemLibrary::Delay(0.5f);
      C = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }
    Char = C;
  }

  int32 Players() {
    AFSDGameState *GS = UGameFunctionLibrary::GetFSDGameState();
    if (!GS) return 0;
    int32 N = 0;
    for (APlayerCharacter *P : GS->ActivePlayerCharacters) // the game state's array, not a copy
      if (P) N++;
    return N;
  }
};
```

Notes:

- The character can be replaced while the mod runs. Bind its `OnDestroyed` and look for the new one, as
  [examples/WaitForPlayer.cpp](examples/WaitForPlayer.cpp) does.
- On the host, the world also holds a controller for each client. As in any UE game, the game mode exists only on the
  host.
- These getters are `UE_PURE`: a call whose result is unused is removed, and clang warns. See
  [Calling engine and game functions](#calling-engine-and-game-functions), which also shows how to post a message on
  screen through the game state.

### Finding actors and components

| You write | What it does | Status |
|---|---|---|
| `UGameplayStatics::GetAllActorsOfClass(APlayerCharacter::StaticClass(), Found)` | Get All Actors Of Class. The result comes back through the `TArray<AActor *>&` parameter: the engine empties the array, then adds every actor of the class, subclasses included, from the calling object's world. Cast each element to reach the class's own members. | Yes |
| `GetAllActorsOfClassWithTag(AActor::StaticClass(), FName("Loot"), Loot)`, `GetAllActorsWithTag(FName("Loot"), Loot)` | The With Tag nodes. They match an actor's `Tags` array. | Yes |
| `GetAllActorsWithInterface(ITargetable::StaticClass(), Found)` | Get All Actors With Interface: every actor that implements the interface. | Yes |
| `UGameplayStatics::GetActorOfClass(AMyManager::StaticClass())` | Get Actor Of Class: the first actor of the class (subclasses included) that the world's actor iterator reaches, or null. With several actors of the class, which one comes back depends on that order, so use it for a class the world holds one of. | Yes |
| `A->GetComponentByClass(UHealthComponentBase::StaticClass())` | Get Component by Class: the actor's first component of that class, or null. It is typed `UActorComponent*`, so Cast it. `K2_GetComponentsByClass(Class)` returns all of them, and `GetComponentsByTag(Class, Tag)` also filters by component tag. | Yes |
| `UActorFunctionLibrary::GetPlayersInRange(K2_GetActorLocation(), 1000.0f, true)` | DRG's getter for the player characters within a radius, as a `TArray<APlayerCharacter *>` that needs no cast. The last argument is MustBeAlive. DRG marks it authority-only: call it on the host. | Yes |
| an engine or game static marked `UE_AUTHORITY_ONLY` or `UE_COSMETIC` | Not yet. The editor's node for such a function is skipped where it should not run: an authority-only one on a client, a cosmetic one on a dedicated server. AssetGen calls every engine and game static directly, so it runs everywhere, with no warning. Guard the call with `HasAuthority()` or `UKismetSystemLibrary::IsServer(this)`. | Not yet |

```cpp
#include "../include/Objects.h"

class AMyManager : public AActor {
public:
  int32 Uses = 0;
};

class Finder : public AActor {
  AMyManager *Manager = nullptr;

public:
  int32 RunningDwarves() {
    TArray<AActor *> Found; // must be TArray<AActor *>
    UGameplayStatics::GetAllActorsOfClass(APlayerCharacter::StaticClass(), Found);
    int32 N = 0;
    for (AActor *A : Found)
      if (APlayerCharacter *P = Cast<APlayerCharacter>(A))
        if (P->IsRunning) N++;
    return N;
  }

  void ClearLoot() {
    TArray<AActor *> Loot;
    UGameplayStatics::GetAllActorsOfClassWithTag(AActor::StaticClass(), FName("Loot"), Loot);
    for (AActor *A : Loot) A->K2_DestroyActor();
  }

  float HealthOf(AActor *A) {
    UHealthComponentBase *H = Cast<UHealthComponentBase>(A->GetComponentByClass(UHealthComponentBase::StaticClass()));
    return H ? H->GetHealth() : 0.0f;
  }

  void ReceiveBeginPlay() { // find the one manager, or make it
    Manager = Cast<AMyManager>(UGameplayStatics::GetActorOfClass(AMyManager::StaticClass()));
    if (!Manager) Manager = SpawnActor<AMyManager>(AMyManager::StaticClass(), FTransform());
  }

  int32 NearHost() {
    if (!HasAuthority()) return 0; // nothing skips this host-only call on a client
    TArray<APlayerCharacter *> Near = UActorFunctionLibrary::GetPlayersInRange(K2_GetActorLocation(), 1000.0f, true);
    return Near.Num();
  }
};
```

Notes:

- The out array must be a `TArray<AActor *>`. The editor retypes the node's output pin to the class you pick; C++
  cannot, and clang rejects a `TArray<APlayerCharacter *>` with "no known conversion".
- Each of these calls walks every actor of the class in the world. Call it from `ReceiveBeginPlay` or an event and
  keep what you need, rather than calling it on every tick.
- When the SDK declares a component as a member of the actor's class, read the member instead of searching:
  `Char->InventoryComponent`.
- In multiplayer, find or spawn a shared actor on the host. A spawned actor reaches the clients only if its class
  replicates; see [Replication](#replication).
- The statics that run everywhere include `UGameplayStatics::ApplyDamage`, `ApplyPointDamage` and
  `ApplyRadialDamage`, `GetPlayersInRange`, `LockCharacters`, `DropToTarget`, the `SpawnEnemies` statics and
  `PlayCueOnAll` (authority-only), and `UWidgetBlueprintLibrary::Create` (`CreateWidget`), the `SetInputMode` statics,
  `RemoveAllWidgets`, `SpawnSound2D`, `PlaySound2D` and `SpawnDecalAtLocation` (cosmetic). A mod's own
  `UE_AUTHORITY_ONLY` and `UE_COSMETIC` functions, and the engine's member functions, are skipped as the editor's are:
  see [RPCs](#rpcs).

### Subsystems

| You write | What it does | Status |
|---|---|---|
| `UDamageSubsystem::Get()`, `UUGCSubsystem::Get()` | The editor's Get <Subsystem> node. Every subsystem class in the SDK has a static `Get`, which calls `USubsystemBlueprintLibrary`'s getter for the class's kind (engine, game instance or world) and returns the subsystem already typed. | Yes |
| `GetSubsystem<UTracerManager>()` | The same as `UTracerManager::Get()`. | Yes |
| `UTracerManager::Get(Other)` | The world or game instance subsystem of `Other`'s world or game instance. Left out, the context is `this`, or a static function's own `WorldContext...` parameter. An engine subsystem's `Get` takes no argument, because there is one engine. | Yes |
| `BP_TracerManager_C::Get()` | A subsystem the game implements as a Blueprint, for its Blueprint variables. DRG has two: `BP_TracerManager_C` (world) and `BP_FadeScreenSubSystem_C` (game instance), each in its `UeApi/Game` header. | Yes |
| `(UTracerManager *)USubsystemBlueprintLibrary::GetWorldSubsystem(this, UTracerManager::StaticClass())` | What `Get` expands to, written by hand. The C-style cast compiles to nothing. | Yes |

```cpp
#include "UeApi/SimpleUGC.h"
#include "UeApi/Game/BP_TracerManager_C.h"

class Subsystems : public AActor {
public:
  UUGCSubsystem *Ugc() { return UUGCSubsystem::Get(); }                              // an engine subsystem
  UDamageSubsystem *Damage() { return UDamageSubsystem::Get(); }                     // a game instance subsystem
  UTracerManager *Tracers() { return GetSubsystem<UTracerManager>(); }               // a world subsystem
  UTracerManager *TheirTracers(AActor *Other) { return UTracerManager::Get(Other); } // Other's world
  static UTracerManager *FromStatic(UObject *WorldContextObject) { return UTracerManager::Get(); }
  UObject *BpTracers() { return BP_TracerManager_C::Get(); }                         // a Blueprint subsystem
};
```

The same by hand, as `Get` expands it:

```cpp
#include "UeApi/SimpleUGC.h"

class SubsystemsLib : public AActor {
public:
  UTracerManager *Tracers() {
    return (UTracerManager *)USubsystemBlueprintLibrary::GetWorldSubsystem(this, UTracerManager::StaticClass());
  }
  UUGCSubsystem *Ugc() {
    return (UUGCSubsystem *)USubsystemBlueprintLibrary::GetEngineSubsystem(UUGCSubsystem::StaticClass());
  }
};
```

Notes:

- DRG has 48 subsystem classes with a `Get`: 8 engine, 20 game instance and 20 world subsystems. None is a local
  player subsystem.
- An SDK generated before subsystems got `Get` has neither `Get` nor `GetSubsystem`: clang says "no member named
  'Get'" or "use of undeclared identifier 'GetSubsystem'". Update the SDK ([The SDK](GUIDE.md#the-sdk)), or call the
  library getter by hand as above.
- Pass the context to `GetWorldSubsystem` and `GetGameInstanceSubsystem` yourself. Their parameter is named
  `contextObject`, not `WorldContext...`, so it is not filled in. `Cast<T>` in place of the C-style cast also works,
  but adds a run-time cast.
- The native parent's `Get` also finds a Blueprint subsystem: `UTracerManager::Get()` returns the instance of
  `BP_TracerManager_C`, typed as the parent.
- A static function without a `WorldContext...` parameter passes its class default object, which has no world. In a
  plain `UObject` class the context is `this`, which finds a world only through its Outer.

## Data assets

A variable at namespace scope whose type is a UE class, with a braced initializer, is cooked as an asset of that class.
This is what creating a Data Asset in the Content Browser and filling in its details does. The class can be the mod's
own `UPrimaryDataAsset` or `UDataAsset` child, or a game or engine class. The rule to remember: an asset is an object,
not a C++ value, so point at it with `&`.

### Declaring data assets

| You write | What it does | Status |
|---|---|---|
| `UMoodDef MD_Big = {.Health = -500.5f, .Title = "Big"};` | An asset of the class, cooked as `<mod package>/MD_Big`. Only the members the braces name are written, and the rest keep the class defaults. A member named with a zero value is still written. | Yes |
| `UMoodDef MD_Plain = {};` | An asset with the class defaults only. | Yes |
| `UMoodDef MD_Zero = {.Health = {}};` | `{}` for a member is its zero, written as `.Health = 0` is. A struct, container or name member's `{}` is written too: zeros, an empty container, None, or a `UE_STRUCT`'s own defaults, not the class default. | Yes |
| `UHitDef HD_Near = {.H = {.Hit = {.Distance = 5.0f}}};` | An asset starts as its class's default object, so braces for a struct member are written over the default object's value, as a `UE_DEFAULTS` statement's are over a parent's. A member they leave out that the engine's constructor sets (`FHitResult`'s FaceIndex) stays unwritten where the default object's value of it is the engine's too. Where the class's own braces or a `UE_DEFAULTS` gave it another value (`H = {.Hit = {.FaceIndex = 3}}`), or the class is a game or engine class, whose value no header says, it is refused, naming the member. | Yes |
| `UEnemyDescriptor ED_Mine = {.SpawnSpread = 250.0f, .IdealSpawnSize = 4};` | An asset of a game or engine class. | Yes |
| `namespace Moods { UMoodDef Angry = {.Health = 50}; }` | A namespace is a folder: the asset is cooked at `<mod package>/Moods/Angry`. See [Mod sources and packages](#mod-sources-and-packages). | Yes |
| `.Delay = FFloatInterval(1.0f, 5.0f)`, `.Delay = {2.0f, 6.0f}` | A struct member, by constructor or by braces, one value per member, as in any default. | Yes |
| `.Waves = {3, 5, 8}` | A container member takes a braced list. | Yes |
| `.Ids = {3}`, `.Score = {{"a", 5}}` on a TSet or TMap whose class default is `{1, 2}`, `{{"a", 1}, {"c", 3}}` | The asset's whole value, as C++ means it. The asset loads a set or map over its class default's value, so the compiler writes what differs, as the cooker does: the class default's elements (a map's keys) this value lacks, as removed, then the elements the class default lacks or maps to another value. `.Ids = {}` empties it. The class default is known where a header gives it: an initializer, a `UE_DEFAULTS` on the way or a `UE_STRUCT` member's default instance, in a class of this mod or of another mod's shared header (`UE_CLASS_IN`, or `UE_CLASS` where the header gives the member a value). A game or engine class's value no header says: there the elements are added to that class's own, with a warning naming the member, and so over a class whose `UE_DEFAULTS` sets the member over one. The same holds for a TSet or TMap inside a struct member. A key given twice holds the last value given, as in C++, and an FString key compares without case: `{{"a", 5}, {"A", 1}}` is `{"A": 1}`. A TArray is replaced whole. | Yes |
| `.Next = &MD_Calm` | One asset pointing at another. | Yes |
| `.EnemyClass = "/Game/A/BP_A.BP_A_C"` | A soft object or soft class member takes the asset's path as a string, written the way the cooker writes it. The path rules are in [Types](#types). | Yes |
| `UBigDef MD_Huge = {{.Health = 900}, false};` | Sets a member a mod base class declares: the base gets a brace list of its own as the first element. C++ cannot designate a base's member (`{.Health = 900}` is a clang error), and after a nested base list the class's own members go by position. | Yes |
| `.SpawnClass = AMyEnemy::StaticClass()`, on a `TSubclassOf` member | Not yet: a class reference has no build-time value, so this is refused with "a default is a value known when the mod is built". Use a `TSoftClassPtr` member with a path, or set the class at run time. See [Classes and variables](#classes-and-variables). | Not yet |

```cpp
class UMoodDef : public UPrimaryDataAsset {
public:
  float Health = 100;
  int32 Count = 3;
  FString Title = "Base";
  UMoodDef *Next = nullptr;
  TArray<int32> Waves;
};

UMoodDef MD_Calm = {.Count = 0};                                   // an explicit zero is written
UMoodDef MD_Big = {.Health = -500.5f, .Title = "Big", .Next = &MD_Calm, .Waves = {3, 5, 8}};
UMoodDef MD_Plain = {};                                            // the class defaults only
namespace Moods {
UMoodDef Angry = {.Health = 50};                                   // <mod package>/Moods/Angry
}

UEnemyDescriptor ED_Mine = {
    .EnemyClass = "/Game/Enemies/Spider/Grunt/ENE_Spider_Grunt_Normal.ENE_Spider_Grunt_Normal_C",
    .SpawnSpread = 250.0f,
    .IdealSpawnSize = 4,
    .CanBeUsedForConstantPressure = true};

class UProjectileDef : public UPrimaryDataAsset {
public:
  FFloatInterval Delay;
  FVector Offset;
};
UProjectileDef PD_Slow = {.Delay = {2.0f, 6.0f}, .Offset = FVector(1.0f, 2.0f, 3.0f)};

class UBigDef : public UMoodDef {
public:
  bool bHuge = true;
};
UBigDef MD_Huge = {{.Health = 900}, false};                        // the base's members first, in braces of their own
```

Notes:

- The class's members must be public. Otherwise the class is not an aggregate, and clang refuses the braces.
- Every value given by position is written, including one equal to the class default (`bHuge = false` above).
- Each asset gets its own row, with its class, in the AssetRegistry.bin the compile writes. See
  [Building mods](GUIDE.md#building-mods).
- A data-asset class that several mods use is pinned to the one mod that cooks it with `UE_CLASS`: see
  [Mod sources and packages](#mod-sources-and-packages).

### Pointing at data assets

| You write | What it does | Status |
|---|---|---|
| `UMoodDef *Picked = &MD_Big;` | `&Asset` is that asset wherever a default expects an object: a variable, or an element or value of a `TArray`, `TSet` or `TMap` default. It is what picking the asset in the details panel writes. | Yes |
| `UMoodDef *Def = &MD_Calm;` in a function, `(&MD_Big)->Health` | In a function body, `&Asset` is an object constant, like an asset picked on a node's pin. Read and write its members through the pointer, as for any object. | Yes |
| `float H = MD_Big.Health;` | Refused: "is an asset, not a value". Take its address: `(&MD_Big)->Health`. | Refused |

```cpp
UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");

class AssetUser : public AActor {
public:
  UMoodDef *Picked = &MD_Big;
  TArray<UEnemyDescriptor *> Enemies = {&ED_Spider_Grunt, &ED_Mine};
  TMap<FName, UMoodDef *> ByName = {{"big", &MD_Big}, {"calm", &MD_Calm}};

  int32 CountOf() {
    UMoodDef *Def = &MD_Calm;
    return Def->Count;
  }
  float HealthOf() { return (&MD_Big)->Health; }
};
```

Notes:

- A default that points at an asset is a hard reference, as one set in the editor is: the asset is loaded with the
  class.
- The pointer is the loaded asset, which everything that uses it shares. A write through it changes the asset for
  the rest of the session, and nothing is saved.
- [examples/EnemyInfo.cpp](examples/EnemyInfo.cpp) declares a data-asset class, its instances and soft references.

## Game assets

An asset that another package holds, the game's or another mod's, is named with `UE_ASSET_AT`, and `&Name` then points
at it in defaults and in code. The SDK's `UeAssets` headers name every game asset this way. The rule to remember: the
path is not checked when the mod is built, so a wrong path compiles.

### UE_ASSET_AT

| You write | What it does | Status |
|---|---|---|
| `UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");` | Names an asset in another package, so `&ED_Spider_Grunt` points at it in defaults and in function bodies. The object's name is the last segment of the path. Only an asset the source uses is imported. | Yes |
| `"/Game/Dir/Package.Object"` | Names an object whose name is not its package's. In the game's content the difference is mostly letter case. | Yes |
| `UE_ASSET_AT(UEnemyDescriptor, Spider, ...)` in two namespaces | Two different assets: an asset is keyed by its qualified name. | Yes |
| `(&ED_Spider_Grunt)->SpawnAmountModifier` | Reads the game asset's value when the game runs. | Yes |
| `ED_Spider_Grunt.SpawnAmountModifier * 2` | Not yet: the SDK names game assets but carries none of their values, so a value cannot be used when the mod is built. Refused with "is an asset, not a value". Read the value at run time through the pointer. | Not yet |
| `&OtherData`, another mod's asset of a class that a shared header declares without `UE_CLASS` | Refused: each mod that includes the header cooks its own copy of the class, so the reference "would load as null". The message prints the `UE_CLASS` line that pins the class to its owner; see [Mod sources and packages](#mod-sources-and-packages). | Refused |
| changing a game data asset, or a game Blueprint's defaults, in the pak | Not yet: there is no syntax for it. Change the loaded asset at run time through its pointer, or derive from the game Blueprint and set defaults in `UE_DEFAULTS` ([Class defaults](#class-defaults)). | Not yet |

```cpp
UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");
UE_ASSET_AT(USkeletalMesh, BunnyPlush,
            "/Game/Art/Environments/Holiday_GreatEggHunt/SK_greatEggHunt_bunnyPlush.SK_GreatEggHunt_BunnyPlush");

namespace Spiders {
namespace Grunt { UE_ASSET_AT(UEnemyDescriptor, Spider, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt"); }
namespace Exploder { UE_ASSET_AT(UEnemyDescriptor, Spider, "/Game/Enemies/Spider/Exploder/ED_Spider_Exploder"); }
} // namespace Spiders

class GameAssets : public AActor {
public:
  UEnemyDescriptor *Target = &ED_Spider_Grunt;
  USkeletalMesh *Plush = &BunnyPlush;
  UEnemyDescriptor *A = &Spiders::Grunt::Spider;
  UEnemyDescriptor *B = &Spiders::Exploder::Spider;

  float Modifier() { return (&ED_Spider_Grunt)->SpawnAmountModifier; } // read when the game runs
};
```

Notes:

- A wrong path compiles, and the reference can then only load as null.
- Inside a namespace that has the class's name, as in the `UeAssets` headers, spell the class `::UEnemyDescriptor`.
- The unpinned-class check looks only at a `UE_ASSET_AT` the source uses, and not at an asset the same source
  defines.
- A soft reference names an asset by its path instead and loads nothing with the class: see [Types](#types).

### The UeAssets headers

| You write | What it does | Status |
|---|---|---|
| `#include "UeAssets/UEnemyDescriptor.h"` | One header per asset class, generated from the game's AssetRegistry.bin by `tools/genueassets.py`. Each asset is a `UE_ASSET_AT` in a namespace that mirrors its folder. | Yes |
| `&UeAssets::UEnemyDescriptor::Game::Enemies::Spider::Grunt::ED_Spider_Grunt` | Points at that game asset, by its path. | Yes |
| `genueassets.py ... --pak <mod pak>` | Also names the assets of a mod pak, which has no registry, from the pak's own package headers. | Yes |

```cpp
#include "UeAssets/UEnemyDescriptor.h"
#include "UeAssets/USoundWave.h"

class UeAssetsUse : public AActor {
public:
  UEnemyDescriptor *Grunt = &UeAssets::UEnemyDescriptor::Game::Enemies::Spider::Grunt::ED_Spider_Grunt;
  int32 Sounds() { return UeAssets::USoundWave::All.Num(); } // every USoundWave in the game
};
```

Notes:

- Left out of the headers: redirectors; Blueprint, WidgetBlueprint and AnimBlueprint objects, whose classes the
  `UeApi/Game` headers name; assets of any class `UeApi` has no header for; and assets of a class whose name two
  packages share.
- A name that is not a C++ identifier gets `_` for each other character, a name that starts with a digit gets `_` in
  front, a keyword or a name that starts with `__` gets `_` at the end, and a name that clashes with a subfolder or
  with an earlier asset in the same folder gets extra `_`s.
- `--pak` reads `/Game` content only. Plugin content is left out.
- A large header costs compile time: about 2.6 s for the two largest.
- How to generate the headers: [The SDK](GUIDE.md#the-sdk).

### Lists of assets: UE_ASSET_ALL

| You write | What it does | Status |
|---|---|---|
| `UE_ASSET_ALL(UEnemyDescriptor);` in a namespace | Declares `All`, a `TArray<TSoftObjectPtr<UEnemyDescriptor>>` filled when the mod is built. It holds every `UE_ASSET_AT` in that namespace and the namespaces inside it whose class is the class given or derives from it. They are soft pointers, so nothing loads until you load one. | Yes |
| `UeAssets::USoundWave::All` | Each `UeAssets` header declares one: every game asset of that class. | Yes |

```cpp
namespace Picks {
UE_ASSET_AT(UEnemyDescriptor, Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");
namespace Boom {
UE_ASSET_AT(UEnemyDescriptor, Exploder, "/Game/Enemies/Spider/Exploder/ED_Spider_Exploder");
}
UE_ASSET_ALL(UEnemyDescriptor); // Picks::All holds Grunt and Boom::Exploder
} // namespace Picks

class AssetLists : public AActor {
public:
  int32 Listed() { return Picks::All.Num(); }
  UEnemyDescriptor *FirstLoaded() {
    return Cast<UEnemyDescriptor>(UKismetSystemLibrary::LoadAsset_Blocking(Picks::All[0]));
  }
};
```

Notes:

- Only `UE_ASSET_AT` entries count. A data asset the mod declares with braces in the same namespace is not listed.
- The entries are ordered by qualified name, not by declaration order.
- `All` is kept like any variable at namespace scope, in the default object of a class the compiler generates for it
  (`<Namespace>__All`), and every class of the source shares it. See [Global variables](#global-variables).
- A soft pointer's object is null until it is loaded, and `(T *)Soft` does not load it. Load it with
  `LoadAsset_Blocking`, as above, or with the latent `LoadAsset`: see [Types](#types) and
  [Latent calls](#latent-calls).
- A list can be big: `UeAssets::USoundWave::All` holds 15,156 paths, a 1.8 MB package that takes about 4.7 s to
  compile.

## Event dispatchers

An event dispatcher is a list of bindings that its owner calls all at once. This section covers declaring one on a
mod class, binding handlers to it and to the game's own dispatchers, broadcasting it, and passing a method as a single
delegate value. The rule to remember: a binding is an object and a function name, so a handler is a non-inline method
of the object you bind (`this` or another) whose parameters match the dispatcher's exactly. [examples/Scoreboard.cpp](examples/Scoreboard.cpp)
declares, binds and broadcasts one.

### Declaring a dispatcher

| You write | What it does | Status |
|---|---|---|
| `UE_DISPATCHER(OnScored, int32 Points, AActor *By);` | The editor's Event Dispatcher (My Blueprint > Event Dispatchers). The class gets the dispatcher OnScored and its signature function, which holds the parameter list. Game Blueprints can bind to it and call it, as with one made in the editor. | Yes |
| `UE_DISPATCHER(OnReady);` | A dispatcher with no parameters. Its handlers are `void F()`, and `Broadcast()` takes no arguments. | Yes |
| `TMulticastInlineDelegate<void(int32 Points)> OnHit;` | Refused: "a dispatcher is declared with UE_DISPATCHER". The macro also declares the signature function, which is where the parameter names survive. Use UE_DISPATCHER. | Refused |
| `TMulticastSparseDelegate<void(int32 Points)> OnHit;` | Refused with the generic "unimplemented property" message. A mod class cannot own a sparse dispatcher. Use UE_DISPATCHER. | Refused |
| `UE_DISPATCHER(...)` inside a `UE_INTERFACE` | Refused: "an interface cannot declare a UE_DISPATCHER". A dispatcher cannot move to the implementing classes the way an interface variable does. Declare it on each implementing class. | Refused |

```cpp
class Scorer : public AActor {
public:
  UE_DISPATCHER(OnScored, int32 Points, AActor *By);
  UE_DISPATCHER(OnReady);
};
```

Notes:

- The parameter list is written once, in the macro. Each parameter's type must be one AssetGen can write as a property
  (see [Types](#types)).
- Binding to the game's sparse dispatchers, such as OnDestroyed, works. Only declaring one in a mod is refused.
- The interface message appears only when no class in the mod implements the interface. When one does, classes are
  compiled first, and the implementing class fails with "unimplemented property OnHit" instead.

### Binding handlers

| You write | What it does | Status |
|---|---|---|
| `OnScored.Add(this, &Scorer::HandleScored);` | Bind Event to OnScored with a Create Event wired in. From then on, every broadcast of OnScored calls HandleScored on this object. | Yes |
| `OnDestroyed.Add(this, &Scorer::HandleDestroyed);` | Binds to a game or engine dispatcher of this object or of its component, inline or sparse: OnDestroyed, `Box->OnComponentBeginOverlap`. The handler runs whenever the game broadcasts it. | Yes |
| `C->OnFirePressed.Add(this, &Scorer::OnFire);` | Bind Event with another object as Target, also through a component: `C->InventoryComponent->OnItemEquipped`. The bound function is still this object's own. | Yes |
| `OnHit.Add(this, &PBase::BaseHandle);` | In a child of a mod class: Add, Remove and Clear work on a dispatcher the mod parent declares, and a handler the parent defines can be bound. | Yes |
| `OnHit.Add(H, &Helper::Take);` on a dispatcher a mod or game Blueprint parent declares | Binds Take on H, as on a dispatcher of this class. The delegate is typed with the parent's own `OnHit__DelegateSignature`, imported from its package, as the editor's Create Event does on an inherited dispatcher. The class makes no function of that name, which would hide the parent's. | Yes |
| The same `Add` twice | The handler is bound once. The engine skips a second binding with the same object and function. | Yes |
| `OnScored.Remove(this, &Scorer::HandleScored);` | Unbind Event from OnScored. Removes the binding that has this object and this function name. It works on every dispatcher Add works on. | Yes |
| `OnScored.Clear();` | Unbind all Events from OnScored. Empties the whole list. | Yes |
| `Proxy->OnCompleted.Add(this, &Scorer::Done);` | An async node with its output pins wired to custom events. Each Add binds one outcome, and one method can serve several. | Yes |
| `E->EventTriggeredDelegate.Add(this, &Scorer::Ping);` on a native dispatcher that is not BlueprintAssignable | Refused: "EventTriggeredDelegate is a native dispatcher that is not BlueprintAssignable (AGameEvent::EventTriggeredDelegate)". The editor's Bind, Unbind and Unbind All nodes refuse one ("Event Dispatcher is not 'BlueprintAssignable'"); only the engine's own code binds it. 17 of the game's are; Remove and Clear are refused the same. A UeApi from before genueapi marked them says nothing, and then Add is taken. | Refused |
| `OnHit.IsBound()`, `OnHit.AddUnique(...)`, `OnHit.Contains(...)` | Refused by clang: "no member named 'IsBound'". A dispatcher has Add, Remove, Clear and Broadcast, and nothing else. Add already skips a duplicate. To know whether your own dispatcher has listeners, count them where you call Add and Remove. | Refused |

```cpp
class Watcher : public AActor {
public:
  UE_DISPATCHER(OnScored, int32 Points, AActor *By);
  TArray<int32> Scores;
  int32 Fired = 0;
  void HandleScored(int32 Points, AActor *By) { Scores.Add(Points); }
  void HandleDestroyed(AActor *DestroyedActor) { Fired = Fired + 1; }
  void OnFire() {}
  void OnItemEquipped(AItem *Item) {}
  void Hook(APlayerCharacter *C) {
    OnScored.Add(this, &Watcher::HandleScored);
    OnDestroyed.Add(this, &Watcher::HandleDestroyed);
    if (C == nullptr) return;
    C->OnDestroyed.Add(this, &Watcher::HandleDestroyed);
    C->OnFirePressed.Add(this, &Watcher::OnFire);
    C->InventoryComponent->OnItemEquipped.Add(this, &Watcher::OnItemEquipped);
  }
  void Unhook(APlayerCharacter *C) {
    OnScored.Remove(this, &Watcher::HandleScored);
    if (C != nullptr) C->OnDestroyed.Remove(this, &Watcher::HandleDestroyed);
  }
};
```

An async proxy's outcome pins are dispatchers too. This is the callback style; the montage proxy needs
`#include "UeApi/AnimGraphRuntime.h"` and the image download `#include "UeApi/UMG.h"`:

```cpp
class Player : public AActor {
public:
  USkeletalMeshComponent *Mesh;
  UAnimMontage *Montage;
  UPlayMontageCallbackProxy *Proxy;
  UAsyncTaskDownloadImage *Task;
  FName Last;
  UTexture2DDynamic *Image;
  void Done(FName NotifyName) { Last = NotifyName; }
  void Got(UTexture2DDynamic *Texture) { Image = Texture; }
  void Play() {
    Proxy = UPlayMontageCallbackProxy::CreateProxyObjectForPlayMontage(Mesh, Montage, 1.0f, 0.0f, FName());
    Proxy->OnCompleted.Add(this, &Player::Done);
    Proxy->OnInterrupted.Add(this, &Player::Done);
  }
  void Fetch(FString Url) {
    Task = UAsyncTaskDownloadImage::DownloadImage(Url);
    Task->OnSuccess.Add(this, &Player::Got);
    Task->Activate();   // Add does not activate the action
  }
};
```

Notes:

- When the dispatcher is on another object and that object is null, the engine skips the Add or Remove with an
  "Accessed None" log rather than crashing. Test it with `if (C != nullptr)` when it might be missing.
- `Clear()` on a game dispatcher, such as `C->OnDestroyed.Clear()`, also removes the game's own bindings and those of
  other mods. Use Remove there.
- Remove names the exact method you added. A binding is identified by its object and its function name.
- The method that binds does not wait. Each handler runs later as a call of its own, with its own locals.
- Add never calls `Activate()`. For a UBlueprintAsyncActionBase such as UAsyncTaskDownloadImage, call `Activate()`
  yourself after binding, as the editor's node does. Once the binding method returns, nothing in your code refers to a
  proxy kept only in a local, so keep it in a member variable as above.
- The montage proxy and the widget animation proxy (`CreateProxyObjectForPlayMontage`,
  `CreatePlayAnimationProxyObject`, `CreatePlayAnimationTimeRangeProxyObject`) are kept alive by nothing but the
  variable that holds them. Put in a local of a method that does not wait, the compiler also stores it into a hidden
  transient member of the class, `<Method>_<Local>`, so it outlives the call as the editor's event graph keeps it;
  a later call of the method replaces it there.
- To continue a function when a dispatcher next fires, instead of binding a handler, use `UE_AWAIT`: see
  [Waiting on events](#waiting-on-events). [examples/AwaitEvents.cpp](examples/AwaitEvents.cpp) uses UE_AWAIT and
  contrasts it with the callback style in a comment.

### Handler functions

| You write | What it does | Status |
|---|---|---|
| `void HandleScored(int32 Points, AActor *By)` | A handler bound on `this` is a non-inline member function of the class or of a mod ancestor. It returns void and takes exactly the dispatcher's parameter types. clang checks this at the Add call. An inherited native or game Blueprint function bound on `this` follows the rules of one bound on another object below. | Yes |
| A `const` handler, or `const FHitResult &Hit` where the header says `FHitResult Hit` | clang error: "no matching member function for call to 'Add'". Copy the parameter list as the header spells it. | Refused |
| `inline void Handle(int32 Points)` as a handler | Refused: "a delegate cannot bind Handle: an inline function is expanded where it is called". An inline method never becomes a function of the class. Drop `inline`. | Refused |
| `OnHit.Add(H, &Helper::HandleHit);` | Binds HandleHit on the object H points to: the editor's Create Event with its Object pin wired. The binding is made where the Add runs, with H as it is then. HandleHit must be a function of H's class or of an ancestor of it: a mod class's method, a game Blueprint's function, or a native BlueprintCallable one (`OnRate.Add(Pawn, &AActor::SetActorTickInterval)`). Remove takes the same pair off. | Yes |
| `OnHit.Add(H, &Other::HandleHit);` where H's class has no HandleHit | Refused: "a delegate on a Helper cannot bind Other::HandleHit: the engine looks it up by name on that object, whose class has no such function". The broadcast would skip it. | Refused |
| `OnTick.Add(A, &AActor::ReceiveTick);` | Refused: "a delegate cannot bind AActor::ReceiveTick on another object: the editor binds a BlueprintCallable function that is not pure or latent, and ReceiveTick is not BlueprintCallable". An event the engine calls is not one Blueprint code can bind, on another object or on `this` ("on this object"), nor a pure or latent function. A mod method that overrides such an event is refused the same. | Refused |
| `OnUse.Add(Item, &AItem::Server_StartUsing);`, `OnPing.Add(this, &AActor::OnRep_Instigator);` | Refused the same way: "... and Server_StartUsing is not BlueprintCallable". A native function the engine does not mark BlueprintCallable, such as an RPC or a RepNotify, is not one the editor binds. UeApi's `NotCallable.json` names them. | Refused |
| `OnPing.Add(PC, &APlayerController::ClientClearCameraLensEffects);` | Binds it: an RPC the engine marks BlueprintCallable (49 are, `Server_ResetHUD` among them) is one the editor binds, on another object or on `this`. Being an RPC says nothing either way. | Yes |
| `OnHit.Add(H, &Helper::Half);` where Helper declares Half and never defines it | Refused: "a delegate cannot bind Helper::Half, which Helper declares and never defines". No function of that name exists. | Refused |

```cpp
class Pad : public AActor {
public:
  UE_COMPONENT(UBoxComponent, Box);
  int32 Hits = 0;
  // OnComponentBeginOverlap is void(UPrimitiveComponent*, AActor*, UPrimitiveComponent*, int, bool, FHitResult)
  void Overlapped(UPrimitiveComponent *Comp, AActor *Other, UPrimitiveComponent *OtherComp, int Index, bool bSweep,
                  FHitResult Sweep) {
    Hits = Hits + 1;
  }
  void ReceiveBeginPlay() { Box->OnComponentBeginOverlap.Add(this, &Pad::Overlapped); }
};
```

Another object can handle a broadcast itself: bind its method on it.

```cpp
class Helper : public AActor {
public:
  int32 N = 0;
  void HandleHit(int32 Points) { N = Points; }
};
class Relay : public AActor {
public:
  UE_DISPATCHER(OnHit, int32 Points);
  Helper *H;
  void ReceiveBeginPlay() {
    if (H != nullptr) OnHit.Add(H, &Helper::HandleHit);   // each OnHit.Broadcast runs H->HandleHit
  }
};
```

[tests/DelegateOtherBind.cpp](tests/DelegateOtherBind.cpp) binds a sibling class's, a peer's, a native and a game
Blueprint's function this way.

Notes:

- A binding stores the object, held weakly, and the function's name. When the dispatcher fires, the engine looks the
  function up by name on that object, so if a subclass overrides the handler, the override runs. A binding whose
  object is gone is skipped and dropped from the list.
- This is Blueprint dispatch, not a C++ member pointer. A broadcast silently skips a binding whose object has no
  function of that name, which is why AssetGen refuses an inline handler and one the object's class lacks.
- The refusals are raised in the function that binds, so they print with its `<Class>::<Function>: ` prefix.
- On another object the binding is the editor's: a delegate local the object's function is bound into
  (EX_BindDelegate), then added. If the object is null when the Add runs, the binding holds no object and the broadcast
  skips it.
- The same rules hold for a delegate value, `{this, &Class::Method}` or `{H, &Helper::Method}` (see Delegate values below).

### Broadcast

| You write | What it does | Status |
|---|---|---|
| `OnScored.Broadcast(N, this);` | Call OnScored. Every bound handler runs right away, one after another, before Broadcast returns. | Yes |
| `GetPeer()->OnPeerHit.Broadcast(X);` | Fires the dispatcher of another object of the same class. The object is evaluated before the arguments, so an argument that changes what GetPeer() returns does not redirect the broadcast. | Yes |
| `UE_DISPATCHER(OnList, TArray<int32> &Items);` | A non-const reference parameter. The declaration compiles, but a Broadcast is refused: "Items is a non-const reference, which a Broadcast never writes back to the caller". The engine copies each argument into a parameter block of its own. Take it by value or by `const &`. | Refused |
| `OnHit.Broadcast(1);` on a dispatcher a mod parent declares | Broadcasts through the parent's signature function, as the editor's node on a child does. | Yes |
| `T->OnHit.Broadcast(P);`, `Burrow->OnBurrowComplete.Broadcast(true);` | The Call node with another object as Target: a dispatcher another mod class declares, or a game Blueprint's (every Blueprint dispatcher is BlueprintCallable). It broadcasts through that class's signature function, imported from its package. | Yes |
| `State->OnTerrainGenerated.Broadcast();` | A native dispatcher the engine marks BlueprintCallable (41 of the game's). UeApi does not say which signature function the engine gives it, so the broadcast names one of the class's own with the same parameters and warns that it does; the handlers get the same arguments. | Warns |
| `OnDestroyed.Broadcast(this);` | Refused: "OnDestroyed is a native dispatcher (AActor::OnDestroyed) that is not BlueprintCallable". The editor's Call node refuses a native dispatcher without BlueprintCallable ("Event Dispatcher is not 'BlueprintCallable'"), and none of the engine's has it; only the engine's own code broadcasts it. With a UeApi from before genueapi marked them, every native dispatcher is refused this way, the message saying the UeApi does not tell. | Refused |

```cpp
class PBase : public AActor {
public:
  UE_DISPATCHER(OnHit, int32 Points);
  void Fire(int32 P) { OnHit.Broadcast(P); }
};
class Child : public PBase {
public:
  int32 Mine = 0;
  void Handle(int32 Points) { Mine = Points; }
  void ReceiveBeginPlay() {
    OnHit.Add(this, &Child::Handle);   // Add, Remove and Clear work on the parent's dispatcher
    OnHit.Broadcast(3);                // through PBase's OnHit__DelegateSignature, as Fire(3) does
  }
};
```

Notes:

- The engine walks a copy of the binding list. An Add or Remove inside a handler takes effect from the next Broadcast.
- A reference parameter does not behave as in C++. The engine copies the argument into the broadcast's parameter
  buffer and never copies it back, so what a handler writes through the reference does not reach the variable you
  passed. Later handlers in the same broadcast do see the write, because they share the buffer. AssetGen gives no
  warning. An argument that is not a plain variable is first stored in a local.

### Delegate values

| You write | What it does | Status |
|---|---|---|
| `{this, &Scorer::HandleTimer}` | Create Event wired into a delegate pin, written as the argument of a call that takes a TDelegate (timers, engine callbacks). The rules are those of a handler: a non-inline method, and the signature of the `TDelegate<...>` in the header. | Yes |
| `{Peer, &Scorer::HandleTimer}` | The same with the Object pin wired: HandleTimer bound on Peer, which the timer then calls. The rules are those of a handler on another object. | Yes |
| `TDelegate<void()>(this, &Scorer::HandleTimer)` | The same value, spelled out. | Yes |
| `K2_ClearTimerDelegate({})` | Refused: "a delegate value is {this, &Class::Function}". There is no empty or unbound delegate. | Refused |
| `.D = {}` in an asset's braces, `{}` as a map's value in a default (`TMap<FString, TDelegate<void()>> M = {{"A", {}}};`) | An unbound delegate, the one value a default holds (`D = {this, &C::F}` in UE_DEFAULTS is refused: a default is known when the mod is built). Written as the engine writes one, a null object and the name None; an asset's tags name no signature function. | Yes |
| `TDelegate<void()> Callback;` as a class variable, a function parameter or a local | A delegate variable (DelegateProperty). `Callback = {this, &Class::Method};` stores a value, and `Callback` passes it on, to a timer or another function. Its signature function is one the class makes per delegate type, as the editor makes one per dispatcher. One C++ type is one however it is spelled (`TDelegate<void(int32)>` and `TDelegate<void(int)>`, an enum named short or with its namespace), and two types are two, a reference and a const reference among them (`void(FVector&)`, `void(const FVector&)`). It is named `<Name>__DelegateSignature` after what needs it first: a variable or parameter of the class, the function a value binds, or the class's parameter or variable through which a class compiled with it, and generated before it - a subclass or not -, overrides a function, hands it a value or stores one into it (`Delegate__DelegateSignature` when that is a local the compiler made up). The name is numbered past a name the class, a mod or game Blueprint parent, or a mod child compiled with it already uses (not past a native parent's own delegate signatures, such as UWidget's `GetText__DelegateSignature`, which UeApi does not list). A struct or an interface makes none, so a delegate in one is refused. | Yes |
| `void Use(TDelegate<void()> D) override` of a mod parent's `Use`; `Take({Peer, &Scorer::Ping})` with Take a mod parent's or a sibling class's of this mod; `ParentVar = {Peer, &Scorer::Ping}`, `Other->Var = {Peer, &Scorer::Ping}`, `ParentArr[0] = {Peer, &Scorer::Ping}`, `Other->Map[K] = {Peer, &Scorer::Ping}` | The override's parameter names the signature function of the class that declares `Use` first, imported from its package, as the editor's override copies its parent's parameters, even where the override spells the type another way (`int` for `int32`, an enum with or without its namespace); a static hiding a parent's static does the same. A value bound on another object is typed with the signature of the parameter or variable it goes to, as a Create Event wired to that pin is, on this object or another, and one stored into an element of such a variable's TArray or a value of its TMap with the element's; a comma value `(Bump(), D)` handed to such a parameter is held in a local of its type. `ParentArr.Add({Peer, &Scorer::Ping})` and the other container functions still type the value with a signature of this class's own, which runs the same. A call whose body is copied in (`Parent::Take(...)`, a static of a class this source cooks) has no parameter to wire, and the value keeps a signature of this class's own. Only classes the same compile cooks: one another mod cooks (UE_CLASS / UE_CLASS_IN) keeps a signature of this class's own, which runs the same, since its name there is not known here. | Yes |

The timers in [Timers and input](#timers-and-input) show delegate values in use.

## Timers and input

A timer calls a method of `this` after a delay or at a fixed rate. It is an engine call that takes a delegate value.
AssetGen has no input events, so a mod polls the player controller for keys, or binds the game's own action
dispatchers. The rule to remember: the engine keeps at most one timer per object and method, so setting a timer again
restarts it. [examples/Beacon.cpp](examples/Beacon.cpp) runs a looping timer.

### Timers

| You write | What it does | Status |
|---|---|---|
| `K2_SetTimerDelegate({this, &Pulser::OnPulse}, 0.5f, true, 0.0f, 0.0f)` | Set Timer by Event. The third argument is Looping: `true` calls OnPulse every 0.5 s until the timer is cleared, `false` calls it once. The last two are the initial start delay and its variance. It returns the timer's handle. | Yes |
| `FTimerHandle Pulse;` | The return value promoted to a variable. An FTimerHandle member is an ordinary struct variable. | Yes |
| `K2_ClearAndInvalidateTimerHandle(Pulse)` | Clear and Invalidate Timer by Handle. Stops the timer and resets Pulse to an invalid handle, because it takes the handle by reference. | Yes |
| `K2_ClearTimerHandle(Pulse)` | Clear Timer by Handle. Stops the timer, but takes the handle by value, so Pulse keeps its old value. | Yes |
| `K2_ClearTimerDelegate({this, &Pulser::OnPulse})` | Clear Timer by Event. Stops the timer without a handle. | Yes |
| `K2_PauseTimerHandle(Pulse)`, `K2_UnPauseTimerHandle(Pulse)` | Pause and resume the timer. AssetGen fills the world context in as self. | Yes |
| `K2_IsTimerActiveHandle(Pulse)`, `K2_GetTimerRemainingTimeHandle(Pulse)`, `K2_IsValidTimerHandle(Pulse)` | Ask about the timer. The first two get self as world context; K2_IsValidTimerHandle takes only the handle. | Yes |
| `K2_SetTimer(this, "Poll", 1.0f, true, 0.0f, 0.0f)` | Set Timer by Function Name. AssetGen passes the name as a plain string and does not check it. The engine looks the method up by that name at run time. | Yes |
| `K2_ClearTimer(this, "Poll")` | Clear Timer by Function Name. K2_PauseTimer, K2_UnPauseTimer, K2_IsTimerActive, K2_GetTimerRemainingTime and the other by-name calls find the timer by the same name. | Yes |
| `UFSDWidgetBlueprintLibrary::SetTimerForNextTick({this, &Pulser::Later})` | DRG's own next-frame timer. It takes a delegate value, returns a handle, and gets self as world context. | Yes |

All calls above are statics of UKismetSystemLibrary except SetTimerForNextTick.

```cpp
class Pulser : public AActor {
public:
  FTimerHandle Pulse;
  int32 Beats = 0;
  void OnPulse() {
    Beats = Beats + 1;
    if (Beats >= 10) UKismetSystemLibrary::K2_ClearAndInvalidateTimerHandle(Pulse);
  }
  void ReceiveBeginPlay() {
    Pulse = UKismetSystemLibrary::K2_SetTimerDelegate({this, &Pulser::OnPulse}, 0.5f, true, 0.0f, 0.0f);
  }
  void Nudge() {   // the next OnPulse comes 2 s from now, and the timer no longer loops
    Pulse = UKismetSystemLibrary::K2_SetTimerDelegate({this, &Pulser::OnPulse}, 2.0f, false, 0.0f, 0.0f);
  }
  bool Running() { return UKismetSystemLibrary::K2_IsTimerActiveHandle(Pulse); }
};
```

Notes:

- A timer's method takes no parameters and returns nothing: the calls take a `TDelegate<void()>`.
- Setting a timer again restarts it. Before it sets a timer, K2_SetTimerDelegate looks for a live timer bound to the
  same object and method, clears it, and adds a new one with the new time and looping flag. The returned handle is a
  new one, so store it again, as Nudge does. A copy of the old handle no longer refers to a live timer, and pause,
  clear and query calls through it do nothing. For two independent timers, bind two different methods. K2_SetTimer by
  name restarts in the same way. An editor-made Blueprint behaves the same.
- A timer can be cleared from inside its own method, as OnPulse does. The engine checks after each call that the timer
  still exists before it re-arms it.
- The time must be greater than 0. A zero or negative time logs a warning containing "SetTimer passed a negative or
  zero time" and sets no timer.
- A looping timer shorter than a frame runs several times in that frame to catch up.
- The timer holds its object weakly. Once the actor is gone, a looping timer is not re-armed.
- Prefer the delegate form to the by-name form. With `{this, &Class::Method}`, clang checks that the method exists and
  takes no parameters, and AssetGen refuses an inline method. With a name, all three mistakes compile with no warning
  and fail at run time. An inline method never becomes a function of the class, and a misspelled name matches nothing,
  so the engine logs "SetTimer passed a bad function (...) or object (...)" and returns an invalid handle. A method
  that takes parameters is found, but the engine refuses it with "SetTimer passed a function (...) that expects
  parameters." In each case no timer is set.
- SetTimerForNextTick is DRG's function, and only compiling it was checked. Going by its name and by the engine's
  FTimerManager function of the same name, it calls the method once on the next frame. The engine libraries in the
  SDK have no next-tick timer of their own.

### Timer, tick or Delay

| You write | What it does | Status |
|---|---|---|
| `void ReceiveTick(float DeltaSeconds)` | Work that must run every frame. Overriding ReceiveTick turns ticking on for the actor. | Yes |
| `UKismetSystemLibrary::Delay(0.5f);` in a loop | Waits for something, then carries on in the same method with its locals intact. Waiting in ReceiveBeginPlay for the local player is the typical case. See [Latent calls](#latent-calls). | Yes |
| `K2_SetTimerDelegate(...)` | Calls a separate method at a fixed rate, or once after a delay, when the call must be paused, queried or stopped from elsewhere through its handle. | Yes |

```cpp
class Follower : public AActor {
public:
  APlayerCharacter *Char = nullptr;
  FTimerHandle Pulse;
  int32 Beats = 0;
  float Travelled = 0.0f;
  void Steer(float DeltaSeconds) { Travelled = Travelled + DeltaSeconds; }
  void EveryTwoSeconds() { Beats = Beats + 1; }
  void ReceiveBeginPlay() {   // set up once: wait for the target, then keep it
    APlayerCharacter *C = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (C == nullptr) {
      UKismetSystemLibrary::Delay(0.5f);
      C = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }
    Char = C;
    Pulse = UKismetSystemLibrary::K2_SetTimerDelegate({this, &Follower::EveryTwoSeconds}, 2.0f, true, 0.0f, 0.0f);
  }
  void ReceiveTick(float DeltaSeconds) {   // per-frame work only
    if (Char) Steer(DeltaSeconds);
  }
};
```

Notes:

- A Delay inside ReceiveTick does not hold up the frame. While a Delay is pending, the engine adds no second one at the
  same call site, so the code after it runs at most once per delay.
- Find and bind a target once, cache it, and bind again from its OnDestroyed, rather than searching for it on every
  tick. [examples/WaitForPlayer.cpp](examples/WaitForPlayer.cpp) is built this way.
- ReceiveTick runs once per frame and gets DeltaSeconds. A looping timer shorter than a frame runs several times in one
  frame instead.

### Reacting to input

| You write | What it does | Status |
|---|---|---|
| `PC->WasInputKeyJustPressed(ToggleKey)` in ReceiveTick | Polls the local player controller. True during the one input frame in which the key went down, so check it every tick. | Yes |
| `PC->IsInputKeyDown(K)`, `PC->WasInputKeyJustReleased(K)`, `PC->GetInputKeyTimeDown(K)` | True while the key is held; true during the frame in which it comes up; how long it has been held. | Yes |
| `FKey{"F5"}`, `FKey{FName("F5")}` | A key value, named by the engine's key names. It works as an argument, a local, a class variable and a `static inline const` member. | Yes |
| `FKey("F5")`, `FKey(FName("F5"))`, `FKey K{FName("F5")};`, `FKey K = {"F5"};` | Refused today: "no Kismet conversion from FString to FKey". Write `FKey{...}` out in full. | Not yet |
| `PC->WasInputKeyJustPressed("F5")` | clang error: "no viable conversion from 'const char[3]' to 'FKey'". | Refused |
| `C->OnFirePressed.Add(this, &Toggler::OnFire)` | For the player's own actions, bind the game's dispatchers on APlayerCharacter: OnFirePressed, OnFireReleased, OnJumpPressed, OnSecondaryFirePressed, OnLaserPointerPressed, OnTerrainScannerPressed and others. They fire for whatever key the player has mapped to that action. | Yes |
| Key events, input-action events, `InputComponent->BindKey(...)` | Not built. AssetGen writes none of the binding objects that the editor's key and InputAction nodes become, and has no syntax for an input event. BindKey and BindAction are native C++ only and absent from the SDK, so clang rejects them: "no member named 'BindKey' in 'UInputComponent'". Poll the player controller or bind the action dispatchers instead. | Not yet |

```cpp
class Toggler : public AActor {
public:
  FKey ToggleKey = FKey{"F5"};
  bool bOn = false;
  float HeldFor = 0.0f;
  void ReceiveTick(float DeltaSeconds) {
    APlayerController *PC = UGameplayStatics::GetPlayerController(0);
    if (PC == nullptr) return;
    if (PC->WasInputKeyJustPressed(ToggleKey)) bOn = !bOn;
    if (PC->IsInputKeyDown(FKey{"LeftShift"})) HeldFor = PC->GetInputKeyTimeDown(FKey{"LeftShift"});
  }
};
```

Notes:

- Name keys by the engine's names: "F5", "LeftShift", "SpaceBar", "LeftMouseButton", "A". The digit keys are "Zero",
  "One", "Two" and so on, not "1". AssetGen does not check the name: one the engine does not know compiles.
- A class variable such as ToggleKey is written into the class defaults. `FKey ToggleKey = {"F5"};` is refused with
  the default-value message; write `FKey{"F5"}`.
- Other one-argument struct constructors in parentheses are refused the same way: `FFrameNumber(5)` and
  `FBoneReference(FName("hand_r"))`. The braced forms `FFrameNumber{5}` and `FBoneReference{"hand_r"}` compile. See
  [Structs](#structs).
- The key state read is the player controller's. A key that an open menu or text box takes may never reach it.
- `EnableInput(PC)` on its own compiles, as a call to AActor::EnableInput. It gives the actor an input component, but
  AssetGen binds nothing to it.

## Interfaces

An interface is a set of functions that unrelated classes implement. This section covers holding an object through an
interface (TScriptInterface), implementing the game's interfaces, and declaring the mod's own with UE_INTERFACE. The
rule to remember: an implementation is matched to the interface function by name only, so copy the interface's
signature exactly and never mark an implementation `inline`. [examples/Scoreboard.cpp](examples/Scoreboard.cpp)
declares and implements one.

### Interface values

| You write | What it does | Status |
|---|---|---|
| `TScriptInterface<IAimable> Current;` | An interface-typed variable, parameter or return value (InterfaceProperty: the object plus the interface pointer, 16 bytes). The interface can be the game's (a /Script interface such as IHealth), a game Blueprint Interface (through its `UeApi/Game/` header), or one the mod declares. | Yes |
| `TScriptInterface<IAimable> Seen = Other;` | Cast To IAimable. If the object's class does not implement the interface, or the object is null, the result is empty, so testing it is the cast's success check. | Yes |
| `TScriptInterface<IAimable> Target = Mark;` | Converts one interface value to another. The result is empty if the object behind it does not implement the target interface. Converting to the same interface is a plain copy. | Yes |
| `Health.GetObject()` | The object behind the interface. GetObject() is the only method a TScriptInterface has. | Yes |
| `Current->GetPriority()` | Calls the interface function on the object behind it, with the interface value as Target. The implementation is whatever that object has under that name. | Yes |
| `if (Seen)`, `!Seen`, `Seen == nullptr`, `Seen != nullptr`, `Seen ? A : B` | Whether an object is behind the interface. | Yes |
| `Health = nullptr;`, `TakeIface(nullptr)` | The null interface. It clears both halves of the value. | Yes |
| `Cast<IAimable>(Other)` | Refused for a mod interface: "Cast<> to an unknown class". Write `TScriptInterface<IAimable> I = Other;` instead. | Refused |

```cpp
class IAimable {
public:
  UE_INTERFACE;
  void OnAimed(AActor *By);
  int32 GetPriority();
};
class Sight : public AActor {
public:
  TScriptInterface<IAimable> Current;
  TScriptInterface<IHealth> Health;
  int32 Rate(AActor *Other) {
    TScriptInterface<IAimable> Seen = Other;   // Cast To IAimable
    if (!Seen) return -1;                      // Other does not implement it, or is null
    if (Current == nullptr) Current = Seen;
    return Seen->GetPriority();
  }
  float HealthOf(APawn *P) {
    Health = P;
    if (Health == nullptr) return -1;
    return Health->GetHealth();
  }
  AActor *CurrentActor() { return Cast<AActor>(Current.GetObject()); }
  void Forget() {
    Health = nullptr;
    Current = nullptr;
  }
};
```

Notes:

- A call through an empty interface is skipped rather than crashing as it would in C++. Test with `if (I)` first.
- The call goes by name, so it reaches the implementation even when the implementing class comes from a different mod
  or from the game.
- `if (Seen)` only checks for null. `if (Obj)` on an object pointer compiles to IsValid (see
  [Working with other objects](#working-with-other-objects)), but an object that is pending kill still counts as true
  behind an interface. [Statements and control flow](#statements-and-control-flow) lists every condition form.
- Write nullptr on the right: `nullptr == Seen` is a clang error. Two interface values have no `==` either; compare
  `A.GetObject() == B.GetObject()`.
- The headers declare no conversion from an interface to an object pointer, so `AActor *A = Seen;` is not accepted.
  Write `Cast<AActor>(Seen.GetObject())`.
- nullptr needs no full declaration of the interface, so a forward-declared one is enough there. It is also the only
  default a TScriptInterface class variable takes.
- For a Blueprint interface (a name ending in `_C`) that a header only forward-declares, the variable is refused with
  the include to add: "is only forward-declared here - #include ...".
- For a game interface, `Cast<IHealth>(Other)` is Other if it implements IHealth, else nullptr, as in C++. The
  engine's cast makes a 16-byte interface value, and AssetGen takes the object out of it (EX_InterfaceToObjCast). To
  call the interface's functions, hold it in `TScriptInterface<IHealth>`.

### Implementing an interface

| You write | What it does | Status |
|---|---|---|
| `class Singer : public AActor, public ICurveSourceInterface` | Class Settings > Implemented Interfaces. The first base is the UE parent, and every base after it is an implemented interface. A class can implement several, game and mod interfaces alike. | Yes |
| `float GetCurveValue(FName CurveName) const { return 0.5f; }` | An implementation is an ordinary method, matched to the interface function by name. Only functions the interface marks BlueprintNativeEvent or BlueprintImplementableEvent can be implemented; AssetGen checks this against the SDK's Events.json. | Yes |
| A function left out | Gets an empty stub, as the editor compiles one. The stub returns the zero value (0, false, None, null) and leaves out-parameters unchanged. | Yes |
| A function left out that a parent this source cooks already has, without listing the interface | The parent's function implements it, as in C++: AssetGen adds an override of it that only calls the parent's (or expands it, if inline), the editor's override with a parent call, so calls through the interface and by name run the parent's body. An empty stub would replace it for every caller, and with no function at all a call would find the interface's own empty one first. | Yes |
| The same, where the parent's function has a parameter with no name | The added override names it (`P0`, `P1`, ... by position) and passes it on, as the editor's override names every pin. | Yes |
| The same, where the parent's function is a multicast | Refused: "... no override can call it for ITrigger, and an empty one would replace it; declare Pistol::Pull". An override would send a multicast twice on a server. | Refused |
| The same, where the parent's function is `final`, or has another signature than the interface's | Refused: "Pistol implements ITrigger, whose Pull needs a function of that name in Pistol, and the Rifle::Pull it inherits is final ..." or "... whose Pull is int32 (int32), and the Rifle::Pull it inherits is float (float): a Blueprint class has one function of a name ...". No function of a final one's name may follow it, and callers through the interface pass the interface's parameters. Rename one of them. | Refused |
| `int32 Pull(int32 N)` implementing ITrigger, where a parent has `float Pull(float)` | Refused: "Pistol::Pull implements ITrigger::Pull, int32 (int32), and replaces the Rifle::Pull it inherits, float (float) ...". A Blueprint function overrides the parent's of its name too, so it would have two signatures. The parent may be another mod's class, from the header it shares; the same goes for ITrigger's stub when the class leaves `Pull` out. Rename one of them. | Refused |
| The same, where the parent has no `Pull` but implements another interface, IAim, whose `Pull` is `float Pull(float)`, and leaves it out; or the class leaves ITrigger's `Pull` out too | Refused: "Pistol::Pull implements ITrigger::Pull, int32 (int32), and replaces the Pull of IAim, float (float), that Rifle implements ..." or "Pistol implements ITrigger, whose Pull is int32 (int32), and replaces the Pull of IAim ...". The parent's stub for IAim is a function of that name, and the editor refuses an override of another signature ("Cannot override ... declared in a parent with a different signature"). Rename one of the interface functions. | Refused |
| The same, where the parent's function is static, or the class declares its own `Pull` beside the parent's static | Refused: "... is static: the editor takes such a function for an override of the static and refuses it ...; rename Rifle::Pull". A static implements no interface function, but the editor makes any function of its name in a subclass an override of it, and an override of a static must be static. | Refused |
| An implementation that returns a value, is `const`, or has out-parameters | Compiled as a function, where the editor would make a function graph rather than an event. It takes the interface function's flags, so callers through the interface find it. A void event such as ShowDamageEffects is compiled as a function too. | Yes |
| `class Pistol : public Weapons::Rifle` | A subclass implements the interface through its parent without listing it. Redefining an interface function is an ordinary override, and `Weapons::Rifle::Pull(Times)` calls the parent's. | Yes |
| `class NativeOnly : public AActor, public IHealth` | Refused: "IHealth::GetHealth is native only ..., so a Blueprint cannot implement IHealth". The editor refuses the same. Calling IHealth through `TScriptInterface<IHealth>` on the game's objects still works. | Refused |
| Listing an interface a mod parent already implements | Refused: "... which its parent Beacon already implements". The child's stubs would override the parent's implementations. Override single functions in the child instead. | Refused |
| `class TwoShare : public AActor, public IAimable, public IMarkable` (IMarkable extends IAimable) | Refused: "... which both extend IAimable". List only the most derived interface, which already implements its parents. | Refused |
| A base after the parent that is not an interface | Refused: "... which is not an interface". A struct there gives "implements an undeclared interface". | Refused |
| An interface whose implementable function uses a type AssetGen cannot write | Refused: "takes a type AssetGen cannot write yet". No interface in the current SDK triggers it. | Refused |

```cpp
class Singer : public AActor, public ICurveSourceInterface {
public:
  FName GetBindingName() const { return FName("Singer"); }
  float GetCurveValue(FName CurveName) const { return 0.5f; }
  void GetCurves(TArray<FNamedCurveValue> &OutValues) const {}
};
class Hummer : public AActor, public ICurveSourceInterface {
public:
  FName GetBindingName() const { return FName("Hummer"); }
  // GetCurveValue and GetCurves get empty stubs
};
```

A subclass overrides an implementation like any other method:

```cpp
namespace Weapons {
class ITrigger {
public:
  UE_INTERFACE;
  int32 Pull(int32 Times);
};
class Rifle : public AActor, public ITrigger {
public:
  int32 Pull(int32 Times) { return Times * 2; }
};
}
class Pistol : public Weapons::Rifle {
public:
  int32 Pull(int32 Times) { return Weapons::Rifle::Pull(Times) + 100; }
};
```

Notes:

- Matching is by name only. An implementation whose parameters differ from the interface function's, such as
  `int32 GetPriority(int32 Extra)` for `int32 GetPriority()`, compiles with no diagnostic, and callers through the
  interface pass it none. Copy the header's signature.
- Do not mark an implementation `inline`. It compiles with no diagnostic, but an inline method is not a function of the
  class, so the class gets the empty stub under that name, and a call through the interface returns 0.
- List the UE class first. If an interface comes first, it is taken as the parent.
- An empty stub stands in for every function left out that no parent has, including those along the chain of an
  interface that extends another. Without it, a call through the interface would reach the interface's own function.
- The native-only message names the first such function in alphabetical order.
- The already-implemented check sees mod ancestors, and native ones only for the interfaces UeApi lists on them
  (`UeNativeInterfaces`): the dump lists no class's interfaces, so genueapi takes, with `--game`, those a game
  Blueprint shows by overriding one's function. A method named like a function of such an interface is an override of
  it, as `OnMessageAI` (ITriggerAI) is in an `AWoodLouse` child: its parameters must match.
- For some of these base lists clang also prints a harmless warning, "direct base 'IAimable' is inaccessible due to
  ambiguity".

### Declaring an interface

| You write | What it does | Status |
|---|---|---|
| `class IAimable { public: UE_INTERFACE; void OnAimed(AActor *By); int32 GetPriority(); };` | A Blueprint Interface asset, cooked at `<UE_MOD_PACKAGE>/IAimable` as class IAimable_C, with one empty function per method. The asset registry lists `<UE_MOD_PACKAGE>/IAimable.IAimable_C`, as for a class. Functions can return values and take reference (out) parameters. | Yes |
| `class Turret : public AActor, public IAimable` | Implements it as for a game interface: matched by name, with stubs for the functions left out. There is no Events.json check, because every function of a mod interface can be implemented. | Yes |
| `virtual int32 GetPriority() = 0;` on the interface | The same as a declaration without a body. A class that leaves it out gets the empty stub and is not cooked Abstract. | Yes |
| `int32 GetPriority() { return 1; }` on the interface | A default implementation. A class that leaves the function out gets this body, with `this` being that class, instead of an empty stub. Blueprint Interface functions have no bodies in the editor. | Yes |
| `class IMarkable : public IAimable { public: UE_INTERFACE; ... };` | An interface that extends one other. A class that lists IMarkable alone implements both: a cast to either succeeds, and it gets stubs for the parent's functions it leaves out. | Yes |
| `class IPriorityTarget : public ITargetable { public: UE_INTERFACE; ... };` | A mod interface that extends a game interface (FSD's Targetable). An implementer gets the game interface's functions too, checked against Events.json. | Yes |
| `int32 Marks = 3;` on the interface | A variable on an interface, AssetGen's own feature: the engine's interfaces hold no state. It becomes a property of every class that directly implements the interface, not declared again in their subclasses. The initializer is its default, and the implementer's or a subclass's UE_DEFAULTS can set another; a subclass's TSet or TMap is written over the implementer's value, as any inherited one is. Its name is held to the implementer's as its own variables are: one its parents already have (`Tags` on an actor), or that a subclass declares again, is refused. | Yes |
| `UTurretDef TD_Big = {{}, {.Marks = 5}, 100};` | A data asset of a class that implements IMarkable gives the interface's variables in the interface's own braces, after the base's (here UPrimaryDataAsset's `{}`), then the class's own members. | Yes |
| `UE_REPLICATED_USING(int32, Score, OnRep_Score);` on the interface | A replicated variable. Each implementing class gets the replicated property and an OnRep_Score of its own: its own definition, else the interface's body, else an empty stub. See [Replication](#replication). | Yes |
| `TScriptInterface<IMarkable> M = Other; return M->Marks;` | Refused: "Marks is a variable of the interface IMarkable, which holds no state itself". Read it through an object of an implementing class, or declare a getter function on the interface. | Refused |
| `int32 Marks;` in a class that implements IMarkable | Refused: "the variable Marks of the interface IMarkable is declared twice". The interface's variable already is this class's property; set its default in UE_DEFAULTS. | Refused |
| `class IBoth : public IAimable, public IMarkable` | Refused: "an interface extends one interface at most". A UClass has one super. | Refused |
| `class IActorish : public AActor { public: UE_INTERFACE; };` | Refused: "IActorish extends AActor, which is not an interface". An interface's only base is another interface, the mod's or the game's. | Refused |
| `UE_COMPONENT(UStaticMeshComponent, Hull);` on an interface | Refused: "an interface cannot declare a UE_COMPONENT". Declare it on the implementing actor. | Refused |
| Implementing a mod interface from an editor Blueprint | Not built. The editor API stubs cover a mod's classes, structs and enums, not its interfaces. See [Editor API stubs](GUIDE.md#editor-api-stubs). | Not yet |

```cpp
class IAimable {
public:
  UE_INTERFACE;
  void OnAimed(AActor *By);
  int32 GetPriority();
};
class IMarkable : public IAimable {
public:
  UE_INTERFACE;
  int32 Marks = 3;
  AActor *MarkedBy;
  void Mark(int32 Count);
  int32 Weight() { return 1; }   // default body for classes that leave it out
};
class Beacon : public AActor, public IMarkable {
  UE_DEFAULTS { Marks = 12; }
public:
  void OnAimed(AActor *By) { MarkedBy = By; }
  int32 GetPriority() { return 7; }
  void Mark(int32 Count) { Marks = Marks + Count; }
};
class LoudBeacon : public Beacon {
  UE_DEFAULTS { Marks = 40; }
public:
  void Mark(int32 Count) { Beacon::Mark(Count * 2); }
  int32 Total(Beacon *Other) { return Marks + Other->Marks; }
};
```

Notes:

- Do not reuse a game interface's C++ name. FSD.h already defines `class ITargetable` (the game's Targetable), so a
  mod `class ITargetable` collides with it in any mod that includes FSD.h.
- The asset keeps the C++ name, `I` included (IAimable_C), whereas the game's native interfaces are reflected without
  the `I` (Targetable).
- In a namespace, the interface cooks in that folder: `namespace Weapons { class ITrigger ... }` becomes
  `<UE_MOD_PACKAGE>/Weapons/ITrigger`.
- A default body is copied into each implementing class. The interface asset itself holds an empty function, so a class
  from outside this mod does not get the default.
- Two unrelated implementers each have their own Marks. It is read and written on `this`, or through a pointer typed as
  the implementer or a subclass (`Other->Marks`). To read it for any implementer, declare a getter on the interface.
- The "not an interface" message for IActorish appears only when no class in the mod implements it. With an
  implementer, classes are compiled first and the error is a misleading one: "AActor::ActorHasTag is native only ...,
  so a Blueprint cannot implement AActor".
- UE_DISPATCHER on an interface is refused as well (see [Event dispatchers](#event-dispatchers)).

## Replication

This section covers variables the server sends to clients, what an assignment to one of them does, and how code tells
the host from a client. Calls between machines are in [RPCs](#rpcs). The rule to remember: an assignment to a
RepNotify variable calls its OnRep function right away, on whichever machine makes it, the server included, as the
editor's Set w/ Notify node does. [examples/NetworkedSwitch.cpp](examples/NetworkedSwitch.cpp) puts the pieces
together.

### Replicated variables

| You write | What it does | Status |
|---|---|---|
| `UE_REPLICATED(int32, Score) = 5;` | A replicated variable, the editor's Replication: Replicated. The server sends its value to clients. A write on a client changes only that client's copy. The initializer after the macro is the class default, as for any member. | Yes |
| `UE_REPLICATED_USING(bool, bOpen, OnRep_Open);` | Replication: RepNotify. When a new value arrives on a client, the engine calls `OnRep_Open()` there. Your own assignment calls it too, on the machine that writes (see the next table). | Yes |
| `UE_REPLICATED_IF(float, Aim, SkipOwner);` | The editor's Replication Condition. The third argument is an `ELifetimeCondition` name, with or without the `COND_` prefix. | Yes |
| `UE_REPLICATED_USING_IF(TArray<int32>, Slots, OnRep_Slots, OwnerOnly);` | RepNotify and a condition on one variable. An array replicates, and its OnRep fires for the array as a whole. | Yes |
| `UE_DEFAULTS { bReplicates = true; }` | A class that declares a replicated variable or an RPC of its own gets `bReplicates = true` in its Class Defaults, and a subclass inherits it. A class with neither keeps its parent's setting: set it by hand in `UE_DEFAULTS`. | Yes |
| `UE_REPLICATED_IF(float, Aim, OwnerAndSimulated);` | Refused: "unknown replication condition". The name is case-sensitive, so `ownerOnly` is refused too. | Refused |
| `UE_REPLICATED(TScriptInterface<IMark>, Target);`, or a replicated `UE_STRUCT` holding a TMap or an interface at any depth | Refused: "an interface does not replicate", "a TMap or TSet in FBag does not replicate". The engine sends a struct member by member, and sends nothing for a TMap, a TSet or an interface. | Refused |
| `UE_REPLICATED(TSet<int32>, Ids);` | Refused: "a TMap or TSet does not replicate". The engine replicates neither, so the variable would never leave the server. A `TMap` or `TSet` inside a replicated array is refused too. Replicate two arrays, keys and values, and rebuild the map in the OnRep. | Refused |
| `UE_REPLICATED(TMap<FName, int32>, Scores);` | Never reaches AssetGen. The comma in `TMap<FName, int32>` splits the macro argument, and clang stops with "too many arguments provided to function-like macro invocation". A type alias for the map is no way around it: through an alias declared in the class, the variable is refused as above ("a TMap or TSet does not replicate"), and an alias at namespace scope of a container or value type is refused as a variable's type ("unimplemented property"). | Refused |
| `void OnRep_Ammo(int32 OldAmmo)` | Refused: the RepNotify "must be a method of the class taking no parameters". A Blueprint RepNotify takes none, so C++'s previous-value form has no equivalent. The same message appears when no method of that name exists. Keep the previous value in a member and compare it in the OnRep. | Refused |
| `int32 OnRep_Ammo()` | Refused: the RepNotify "must return void". The engine calls it with no room for a result. | Refused |
| `inline void OnRep_Ammo()` | Refused: the RepNotify "is inline, so no function of the class". An inline method is no Blueprint function, so a client would never find the OnRep. Drop `inline`. | Refused |
| `UE_REPLICATED(int32, A);` in a `UE_STRUCT` | Refused: "UE_REPLICATED on a struct member has no effect". UE has no per-member replication for a struct. Replicate the class variable that holds the struct, which replicates it as a whole. | Refused |

```cpp
class Door : public AActor {
  UE_REPLICATED(int32, Score) = 5;                  // the initializer is the class default
  UE_REPLICATED_USING(bool, bOpen, OnRep_Open);
  UE_REPLICATED_IF(FVector, AimPoint, InitialOnly); // COND_InitialOnly works too
  UE_REPLICATED_USING_IF(TArray<int32>, Slots, OnRep_Slots, OwnerOnly);
  int32 Opened = 0;

public:
  void OnRep_Open() { Opened += 1; }
  void OnRep_Slots() {}
};

class Plain : public AActor {
  UE_DEFAULTS { bReplicates = true; }   // no replicated variable or RPC of its own
};
```

Notes:

- The condition names follow UE 4.27's `ELifetimeCondition`: `None`, `InitialOnly`, `OwnerOnly`, `SkipOwner`,
  `SimulatedOnly`, `AutonomousOnly`, `SimulatedOrPhysics`, `InitialOrOwner`, `Custom`, `ReplayOrOwner`, `ReplayOnly`,
  `SimulatedOnlyNoReplay`, `SimulatedOrPhysicsNoReplay`, `SkipReplay` and `Never`.
- The OnRep is an ordinary method of the class with no parameters: in the class body without `inline`, or defined
  outside the class.
- Only actors, and components of a replicating actor, replicate. On any other class, `bReplicates` does nothing, and
  a replicated variable or an RPC is refused ("does nothing, since only an actor or an actor component replicates").
- `SetReplicates(true)`, called on the server, turns replication on at run time.

### Writing replicated variables

| You write | What it does | Status |
|---|---|---|
| `bOpen = true;` | The editor's Set w/ Notify node. On an actor, AssetGen calls `FlushNetDormancy()` first, so a dormant actor still sends the change. Then it writes. Then, for a RepNotify variable, it calls `OnRep_Open()` on whichever machine runs the code, the server included. Native UE C++ never calls an OnRep on the server by itself, so this differs from C++. | Yes |
| `Other->bOpen = false;` | The flush, the write and the OnRep all go to `Other`, not to self. | Yes |
| `Slots[0] = 4;` | An element write counts as a set of the whole array: flush, write, `OnRep_Slots()`. | Yes |
| `Score += 1;` or `++Score;` | Rewritten as `Score = Score + 1`, with the target found once, so it gets the same flush and OnRep. | Yes |
| `Me()->bOpen = true;` | The object expression runs once per statement, as C++ runs it, and the flush, the write and the OnRep all use that one result. | Yes |
| `Slots.Add(3);` | A container call changes the array in place and adds neither the flush nor the OnRep. The editor does the same: only its Set node adds them. The change still replicates on the next update of an actor that is not dormant. Call `FlushNetDormancy()` and the OnRep yourself, or assign the whole array. | Yes |
| `Target.Z = 100.0f;` | A write to one member of a replicated struct gets no flush and no OnRep. To get both, copy the value, change the copy and assign it back. | Yes |
| `bReplicateMovement = true;` | An engine or game C++ replicated property: the flush and the write, and no OnRep call, because a native RepNotify is meant for clients. The editor does the same. Where the engine has a setter, such as `SetReplicateMovement`, the setter may do work that a raw write skips. | Yes |
| `Glass->Thrown = true;` | A replicated variable that one of the game's Blueprints declares works like one of yours: flush, write, then its OnRep (`OnRep_Thrown`), called on that object. Here `Glass` is a `Game::GameElements::Bar::Bar_Glass_Physics_C *` from `UeApi/Game/Bar_Glass_Physics_C.h`. | Yes |

```cpp
class Door : public AActor {
  UE_REPLICATED_USING(FVector, Target, OnRep_Target);
  UE_REPLICATED_USING(TArray<int32>, Slots, OnRep_Slots);

public:
  void OnRep_Target() {}
  void OnRep_Slots() {}

  void Raise() {
    Target.Z = 100.0f;    // no flush, no OnRep_Target
    FVector T = Target;
    T.Z = 100.0f;
    Target = T;           // FlushNetDormancy(); the write; OnRep_Target();
  }
  void AddSlot(int32 Id) {
    Slots.Add(Id);        // no flush, no OnRep_Slots
    FlushNetDormancy();   // add them yourself when you need them
    OnRep_Slots();
  }
};
```

Notes:

- `FlushNetDormancy` is authority-only, so on a client the engine skips it and only the write and the OnRep run.
- Assigning a RepNotify variable inside its own OnRep calls that OnRep again.

### Replicated variables on an interface

| You write | What it does | Status |
|---|---|---|
| `UE_REPLICATED_USING(int32, Points, OnRep_Points) = 0;` in a `UE_INTERFACE` | Each class that implements the interface gets `Points` as a replicated variable of its own, with the replication settings written on the interface. A write calls the RepNotify as usual. | Yes |
| `void OnRep_Points() { Seen += 1; }` in the implementing class | The RepNotify belongs to the implementing class. AssetGen looks for it on the class first, then on the interface. A class that leaves it out gets the interface's out-of-line body when there is one (`void IScored::OnRep_Points() { Mirror = Points; }`), otherwise an empty function, and every write still calls it. | Yes |
| an OnRep declared on neither | Refused: the RepNotify "must be a method of the class taking no parameters". If the interface leaves the declaration out, every implementing class has to define it. | Refused |

```cpp
class IScored {
public:
  UE_INTERFACE;
  UE_REPLICATED_USING(int32, Points, OnRep_Points) = 0;
  void OnRep_Points();                      // optional here
};

class Target : public AActor, public IScored {
  int32 Seen = 0;

public:
  void OnRep_Points() { Seen += 1; }        // this class's RepNotify
  void Bump() { Points = Points + 1; }      // flush, write, OnRep_Points()
};
```

Notes:

- A variable on an interface is AssetGen's own feature: the editor has nothing like it. The interface asset itself
  holds neither the variable nor the OnRep. The rest of mod interfaces is in [Interfaces](#interfaces).

### Host or client at run time

| You write | What it does | Status |
|---|---|---|
| `UKismetSystemLibrary::IsServer(this)` | The editor's Is Server node. True on a listen-server host, on a dedicated server and in a solo (standalone) game. False on a client. `IsServer()` without the argument passes self. `IsDedicatedServer()` and `IsStandalone()` work the same way. | Yes |
| `HasAuthority()`, `Other->HasAuthority()` | The editor's Has Authority node. True when this machine holds the authoritative copy of the actor: on the server for an actor that replicates, and on any machine for an actor that machine spawned itself. | Yes |
| `GetLocalRole() == ENetRole::ROLE_AutonomousProxy` | Get Local Role. Tells a client's own copy (`ROLE_AutonomousProxy`) from other players' copies (`ROLE_SimulatedProxy`). | Yes |
| `Watched->IsLocallyControlled()` | Is Locally Controlled, on a pawn. True only on the machine whose own controller possesses it: on the host, the host's pawn; on a client, that client's own. A pawn with no controller gives false. DRG's `UGameFunctionLibrary::GetLocalPlayerCharacter(this)` returns this machine's player directly. | Yes |
| `GetOwner()` in a component | A component has no role of its own, so it asks its owner: `AActor *O = GetOwner(); if (O) bAuth = O->HasAuthority();`. | Yes |

```cpp
class Tally : public AActor {
  int32 Total;
  int32 Charges;

public:
  APawn *Watched;
  bool bMine;

  void ReceiveBeginPlay() {
    if (UKismetSystemLibrary::IsServer(this))   // IsServer() is the same call
      Total = 0;                                // the host, or a solo game
    if (!HasAuthority())
      return;                                   // only the machine with authority over this actor goes on
    Charges = 3;
  }
  bool IsMyClientsCopy() { return GetLocalRole() == ENetRole::ROLE_AutonomousProxy; }
  void ReceiveTick(float DeltaSeconds) {
    if (Watched == nullptr)
      return;
    bMine = Watched->IsLocallyControlled();
  }
};
```

Notes:

- Having authority is not the same as being the host. Every actor starts with authority, and only a copy received
  from the server gives it up. A mod actor that each machine spawns for itself has authority on every machine. To ask
  whether this machine is the host, use `IsServer`.
- `IsServer` reads the world of its argument. An actor or a component has one. Any other object finds it only through
  its Outer, and with no world `IsServer` returns false, even on the host. Nothing warns about this. Make such an
  object with `NewObject<T>(this)` from an actor, which makes the actor its Outer. With no world,
  `IsDedicatedServer` falls back to whether the process is a dedicated server. A plain `UObject` class has no
  `HasAuthority`: use `IsServer` there.
- In a static function, `IsServer()` without the argument passes that function's own `WorldContextObject` parameter.
- To skip a whole function on machines without authority, mark it `UE_AUTHORITY_ONLY` instead of branching (see
  [RPCs](#rpcs)).

## RPCs

A method marked `UE_SERVER`, `UE_CLIENT` or `UE_MULTICAST` is an RPC: a call on one machine runs it on another.
`UE_AUTHORITY_ONLY` and `UE_COSMETIC` mark a method the engine skips where it should not run. The rule to remember:
put these markers only on an ordinary method with a body, neither `inline` nor `static`. A marker on an `inline` or
`static` method, or on a method that is never defined, compiles without a diagnostic and does not do what it says.
[examples/NetworkedSwitch.cpp](examples/NetworkedSwitch.cpp) has a Server RPC and a reliable multicast.

### Declaring RPCs

| You write | What it does | Status |
|---|---|---|
| `UE_SERVER UE_RELIABLE void ServerOpen(bool bValue);` | The editor's custom event with Replicates: Run on Server. Called on the owning client, it runs on the server. Called on the server, it runs right there. The marker may sit on the declaration in the class or on the definition. | Yes |
| `UE_CLIENT void ClientPing(int32 Seq) { Seen = Seq; }` | Replicates: Run on owning Client. Called on the server, it runs on the client that owns the actor. | Yes |
| `UE_MULTICAST void PlayCreak() { Seen = 2; }` | Replicates: Multicast. Called on the server, it runs on the server and on every client. Called on a client, it runs only there. | Yes |
| `UE_MULTICAST UE_RELIABLE void PlayHit() { Local = 5; }` | The Reliable checkbox. An RPC without `UE_RELIABLE` is unreliable. | Yes |
| `UE_RELIABLE void Ping() { N = 1; }` | Refused: "UE_RELIABLE needs UE_SERVER, UE_CLIENT or UE_MULTICAST". | Refused |
| `UE_SERVER int32 ServerScore() { return N; }` | Refused: "an RPC returns void". An RPC runs on another machine and cannot hand a value back. Answer with a Client RPC or a replicated variable. | Refused |
| `UE_SERVER void ServerBump(int32 &Count) { Count += 1; }` | Warns once per parameter it writes: "reaches the caller only when the call runs locally". The receiving machine works on a copy, so the caller sees the write only when the call ran locally, as when the server calls its own Server RPC. Take the parameter by value or `const &`. | Warns |
| `UE_SERVER void ServerSync(TMap<FName, int32> Scores) { Kept = Scores; }` | Refused: "an RPC parameter cannot be a TMap or TSet". The engine sends nothing for either, so the argument would arrive empty. Pass two arrays, keys and values. | Refused |
| `UE_SERVER void Send(FBag Sack)`, where `FBag` holds a `TMap`; `UE_SERVER void S(TScriptInterface<IMark> T)` | Refused: "an RPC parameter cannot hold what does not replicate". A struct is sent member by member, and neither a TMap nor an interface sends anything. Pass the object as a `UObject*` or an actor pointer instead of an interface. | Refused |
| `UE_SERVER inline void ServerPing() { Pings += 1; }` | Refused: "an RPC, authority-only or cosmetic marker on an inline method does nothing". An inline method is expanded at each call and is no Blueprint function, so no call to it is routed. The same holds for `UE_AUTHORITY_ONLY` and `UE_COSMETIC`. Drop `inline`. | Refused |
| `UE_SERVER static void S(int32 X) { }` | Refused: "a static function cannot be an RPC". The engine routes a static function without looking at its net flags, so it would run locally and never be sent. Use a member function. | Refused |
| `UE_MULTICAST UE_SERVER void Also() { }` | Refused: "an RPC goes one way". The sender picks one direction and the receiver checks its own, so a second marker would drop the call on one side. `UE_MULTICAST UE_CLIENT` is refused the same way. | Refused |

```cpp
class Door : public AActor {
  UE_REPLICATED_USING(bool, bOpen, OnRep_Open);
  int32 Credits;

public:
  int32 Seen = 0;
  void OnRep_Open() { Seen = 1; }
  UE_SERVER UE_RELIABLE void ServerOpen(bool bValue);
  UE_CLIENT void ClientPing(int32 Seq) { Seen = Seq; }
  UE_MULTICAST void PlayCreak() { Seen = 2; }
  UE_AUTHORITY_ONLY void GrantReward() { Credits += 10; }
  UE_COSMETIC void PlaySparks() { Seen = 3; }

  void Use() { ServerOpen(!bOpen); }   // on the owning client: runs on the server
};

void Door::ServerOpen(bool bValue) {
  bOpen = bValue;
  PlayCreak();
  GrantReward();
}
```

Notes:

- Engine rule: a Server RPC called on a client runs only when that client's connection owns the actor. Otherwise the
  call is dropped.
- In a class that is neither an actor nor an actor component, an RPC always runs locally.
- A class with an RPC of its own gets `bReplicates` (see [Replication](#replication)).
- Give every RPC a body. A method that is declared but never defined is not compiled, so no marker check runs on it,
  and a call to it is refused, as C++ would not link it. See [Functions](#functions).

### Authority-only and cosmetic functions

| You write | What it does | Status |
|---|---|---|
| `UE_AUTHORITY_ONLY void GrantReward() { Credits += 10; }` | The editor's Authority Only function flag (BlueprintAuthorityOnly). A call on a machine without authority over the actor is skipped instead of failing. | Yes |
| `UE_COSMETIC void PlaySparks() { Seen = 3; }` | The Cosmetic function flag (BlueprintCosmetic). The engine skips the call on a dedicated server. A listen-server host and clients run it. | Yes |

Notes:

- The engine skips a call only on an actor or a component of one. On any other object the function always runs.
- UeApi marks the engine's and the game's functions the same way, and a call to one of their member functions is
  skipped the same way. An engine or game static marked authority-only or cosmetic is called directly and is not
  skipped; the list and the guard are in [Working with other objects](#working-with-other-objects).

### Calling RPCs

| You write | What it does | Status |
|---|---|---|
| `ServerOpen(false);`, `Other->PlayCreak();` | A call to an RPC, or to an authority-only or cosmetic function, goes through the engine's call routing, which runs it here, sends it over the network or drops it. A plain method is called directly. | Yes |
| `Char->Server_SetRunning(false);` | The game's and the engine's RPCs carry the same markers in UeApi. A call to one is an ordinary call, and the engine routes it. | Yes |

### RPCs in subclasses

| You write | What it does | Status |
|---|---|---|
| `void ServerOpen(bool bValue) { Seen = bValue ? 7 : 8; }` in a subclass | An override without a marker keeps the parent's net flags (Server, Reliable) and is still that RPC. | Yes |
| `void OnRep_Open() { Seen = 10; }` in a subclass | When the parent's code assigns the variable, the subclass's OnRep runs in the parent's place, because the call goes by name. | Yes |
| `UE_SERVER void ServerOpen(bool bValue) { Seen = 2; }` in a subclass | Refused: "an override takes its parent's replication; drop the RPC marker". The engine asserts on a mismatch. Any RPC marker on any override is refused, one on an engine event such as `UE_MULTICAST void ReceiveBeginPlay()` included. | Refused |

```cpp
class DoorKid : public Door {
public:
  void ServerOpen(bool bValue) { Seen = bValue ? 7 : 8; }   // still Run on Server, Reliable
  void OnRep_Open() { Seen = 10; }                          // runs when Door's code sets bOpen
};
```

Notes:

- A member the subclass uses, such as `Seen`, must be public or protected in the parent. Overrides in general are in
  [Overrides and parent calls](#overrides-and-parent-calls).

## Latent calls

A latent call finishes later: `Delay`, `RetriggerableDelay`, the async loads, and every engine or game function with
an `FLatentActionInfo` parameter. A method that makes one moves into the class's event graph, the ubergraph named
`ExecuteUbergraph_<Class>`, as an editor event with a Delay node does. Its locals and parameters live in that graph's
frame, one per object. The rule to remember: a method that waits returns `void`, takes no non-const reference parameters
and is not `static`, and its caller carries on as soon as it reaches its first wait.
[examples/WaitForPlayer.cpp](examples/WaitForPlayer.cpp) waits in a loop until the player exists.

### Waiting with Delay

| You write | What it does | Status |
|---|---|---|
| `UKismetSystemLibrary::Delay(0.5f);` | The editor's Delay node. The code after it runs when the delay ends, with the method's locals intact. Leave out the world context (`Delay(this, 0.5f)` is the same call) and the `FLatentActionInfo`: AssetGen supplies both. | Yes |
| `UKismetSystemLibrary::RetriggerableDelay(HoldTime);` | Retriggerable Delay. A call while one is pending restarts the countdown instead of being ignored, so the code after it runs once, `HoldTime` after the last call. All callers of the method share its one call site, so a tick and any other caller restart the same countdown. | Yes |
| `UPendingLatentActionLibrary::WaitOneFrame();` | Any latent function, static or a method, has a UeApi overload without the `FLatentActionInfo` and without the world context, and that is the one you call. AssetGen fills in the resume point, an ID for this call site and self. | Yes |
| `Mover->FindNearestPathfinderPoint_Async(Pos, 500.0f, Ok, At);` | A latent function that answers through reference parameters. `Ok` and `At` are locals in the frame, so the action can fill them in, and the code after the call reads them. This is how the editor wires a latent node's output pins. | Yes |
| `UKismetSystemLibrary::Delay(this, 1.0f, FLatentActionInfo(0, 1, "ExecuteUbergraph_Door", this));` | Refused: "leave the FLatentActionInfo argument out". The resume point exists only once AssetGen has laid out the event graph. | Refused |
| `UKismetSystemLibrary::Delay(1.0f, FLatentActionInfo(0, 1, "ExecuteUbergraph_Door", this));` | Refused the same way, without the world context too. | Refused |

```cpp
UDeepPathfinderMovement *Mover;
USceneComponent *Mesh;
FVector Destination;
float HoldTime = 0.25f;
bool bHolding;
bool bUsing;

void FindSpot(FVector Pos) {
  bool Ok = false;
  FVector At;
  Mover->FindNearestPathfinderPoint_Async(Pos, 500.0f, Ok, At);
  if (Ok)
    Destination = At;   // read after the resume
}
void Lift(EMoveComponentAction Action) {   // Move, Stop and Return share this one call site
  UKismetSystemLibrary::MoveComponentTo(Mesh, FVector(0, 0, 200), FRotator(), false, false, 1.0f, false, Action);
}
void Hold() {
  bHolding = true;
  UKismetSystemLibrary::RetriggerableDelay(HoldTime);
  bHolding = false;     // HoldTime after the last call
}
void ReceiveTick(float DeltaSeconds) {
  if (bUsing)
    Hold();
}
```

Notes:

- Each call site has its own ID. `MoveComponentTo`'s Stop and Return act only on a move started from the same call
  site, so send Move, Stop and Return through one non-inline method that takes the `EMoveComponentAction`, as `Lift`
  does.

### When the code after a wait runs

| You write | What it does | Status |
|---|---|---|
| `FindPlayer(); Ready = true;` | A call to a method that waits returns when that method reaches its first wait, and the rest of it runs later. The caller never waits for it to finish, as with an editor custom event that contains a Delay. The code before the first wait runs inside the caller's call. | Yes |
| `Log.Add(Tag); UKismetSystemLibrary::Delay(Seconds); Log.Add(Tag + 1);` | Parameters and locals keep their values across the wait: parameters are copied into the frame, and locals live there. Member variables are read fresh after the resume. | Yes |
| `while (C == nullptr)` with a Delay inside | Each round waits before the next one starts, and the loop variables survive each wait. A `for` loop works the same way. This differs from a Delay in the body of the editor's For Loop node, which does not pause the loop. | Yes |
| `Pause(2.0f);`, an inline helper that calls Delay | The helper is expanded into the caller, so the caller becomes the method that waits, and it resumes after the helper's Delay. Each expansion is a call site of its own. The caller has to follow the latent rules. A non-inline helper that waits returns to its caller at its first wait instead. | Yes |
| `void ReceiveBeginPlay()` with a Delay inside | An override of an engine event can wait. The engine still calls it as that event. | Yes |
| `Twice(1); Twice(2);` before `Twice` resumes | An object has one frame for a method that waits, not one per call. The second call starts at the top and overwrites the pending call's parameters and locals, and its Delay is ignored because one is already pending at that call site. The method resumes once, with the second call's values. An editor event graph behaves the same; a C++ coroutine would not. | Yes |
| `UUserWidget *W = CreateWidget<UUserWidget>(P); UKismetSystemLibrary::Delay(1.0f); Kept = W;` | The frame holds an object local weakly: unless the object is flagged RF_StrongRefOnFrame, as SpawnObject's (`NewObject`) and the callback proxies' are, a garbage collection during the wait can take it, and the local then reads None. The editor's event graph is the same. The compiler warns where a local may hold an object the method made or loaded (not one read out of a property, nor an actor, component or async action) across a wait and reads it after: keep such an object in a member. | Warns |
| `TArray<int32> Nums;` with no initializer | Empty on the object's first call. On a later call it holds whatever the previous call left in it, where C++ would give a new empty array. Initialize the local, or `Clear()` it at the top. Inside a loop it is reset every round, as usual. | Yes |

```cpp
class Door : public AActor {
public:
  int32 Stage;
  int32 Ticks;
  int32 Got;
  APlayerCharacter *Player;

  void ReceiveBeginPlay() {
    int32 Before = Stage + 7;
    Stage = 1;
    UKismetSystemLibrary::Delay(0.5f);   // ReceiveBeginPlay returns to the engine here
    Stage = Before + 1;                  // half a second later; Before kept its value
    FindPlayer();
  }
  void FindPlayer() {
    APlayerCharacter *C = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (C == nullptr) {
      UKismetSystemLibrary::Delay(0.5f);
      C = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }
    Player = C;
  }
  void ReceiveTick(float DeltaSeconds) {
    Ticks += 1;                          // every frame
    UKismetSystemLibrary::Delay(1.0f);   // ignored while the last one is pending
    SlowWork();                          // about once a second
  }
  void SlowWork() {}
  void Collect() {
    TArray<int32> Nums;                  // keeps what the previous call left in it
    Nums.Clear();                        // so empty it first
    for (int32 I = 1; I <= 5; ++I)
      Nums.Add(I * I);
    UKismetSystemLibrary::Delay(2.0f);
    Got = Nums.Num();
  }
};
```

Notes:

- To learn when a method that waits has finished, set a member at its end, broadcast a dispatcher of your own, or
  `UE_AWAIT` that dispatcher (see [Waiting on events](#waiting-on-events)).
- In `ReceiveTick`, a Delay makes everything after it run at most once per delay, as the example shows.
- Different objects have separate frames and separate pending waits: `InitSpacerig` and `InitCave`, or two spawned
  copies of one class, each run their own `ReceiveBeginPlay` loop with their own locals. Only a second call on the same
  object, before it resumes, shares the frame as in the `Twice` row. Different methods of one object keep separate
  locals.
- Locals outside a method that waits are in [Locals](#locals); inline helpers in
  [Inline functions and templates](#inline-functions-and-templates).

### Where a method can wait

| You write | What it does | Status |
|---|---|---|
| `static void Later(UObject *WorldContextObject)` with a Delay inside | Refused: "a static function has no object whose ubergraph frame could keep its locals". | Refused |
| `int32 CountLater()` with a Delay inside | Refused: "a function that resumes later returns nothing and takes no non-const reference parameters". The caller gets control back at the first wait, before there is a value. Store the result in a member, or broadcast a dispatcher when done. | Refused |
| `void SayLater(const FString &Msg)` with a Delay inside | Msg is copied into the event graph's frame when the method is called, through EX_LocalOutVariable, as the editor's event stubs copy one (a ReceiveHit's `const FHitResult &Hit`); the code after the wait reads that copy. | Yes |
| `void FillLater(int32 &Out)` with a Delay inside | Refused with the CountLater message: a write after the wait would land in the frame's copy, the caller having gone on. Store the result in a member. | Refused |
| a Delay in a `UObject` child class | Warns: "finds its world only through its Outer". Actor, ActorComponent, UserWidget, GameInstance and Subsystem classes have a world of their own; any other object uses its Outer's. Make the object with an actor or component as its Outer, or the wait does nothing and the method never resumes. The editor offers Delay only where the class has a world. | Warns |
| `V = __Read64__(Addr);` in a method that waits | Refused: "a pointer read in a function that makes a latent call". Move the read into a separate non-inline method that does not wait, and call that. See [Pointers and memory](#pointers-and-memory). `GetOuter()` and `GetTypedOuter` read memory too and are refused the same way (see [Working with other objects](#working-with-other-objects)). | Not yet |
| `void ExecuteUbergraph_Door(int32 EntryPoint);` | Refused: "ExecuteUbergraph_<Class> is the name of a class's ubergraph". The engine resumes a class's latent calls through that function, found by name on the object, so a method of that name in the class or a subclass would catch them. | Refused |

```cpp
#include "../include/Objects.h"   // AssetGen's include/ folder, by its path from your source

class LatentJob : public UObject {
  int32 Done;

public:
  void Run(float Seconds) {   // warns: this class finds its world only through its Outer
    UKismetSystemLibrary::Delay(Seconds);
    Done = 1;
  }
};

class Door : public AActor {
public:
  void ReceiveBeginPlay() {
    LatentJob *Job = NewObject<LatentJob>(this);   // this actor is its Outer
    Job->Run(0.75f);
  }
};
```

### Async loads

| You write | What it does | Status |
|---|---|---|
| `UObject *Obj = UKismetSystemLibrary::LoadAsset(Icon);` | The editor's Async Load Asset node, written as a call that returns. The method waits for the load, and the value is the loaded object, or null if the load failed. | Yes |
| `auto Cls = UKismetSystemLibrary::LoadAssetClass(EnemyClass);` | Async Load Class Asset. The value is the loaded class, or null. | Yes |
| `UKismetSystemLibrary::LoadAsset_Blocking(Icon)` | An ordinary call that does not wait, for a caller that must not become latent. | Yes |

```cpp
TSoftObjectPtr<UTexture2D> Icon;
TSoftClassPtr<AActor> EnemyClass;
UTexture2D *LoadedIcon;
TSubclassOf<UObject> LoadedClass;

void LoadAll() {
  UObject *Obj = UKismetSystemLibrary::LoadAsset(Icon);   // waits for the load
  LoadedIcon = Cast<UTexture2D>(Obj);                     // null if the load failed
  auto Cls = UKismetSystemLibrary::LoadAssetClass(EnemyClass);
  LoadedClass = Cls;
}
void LoadNow() {
  LoadedIcon = Cast<UTexture2D>(UKismetSystemLibrary::LoadAsset_Blocking(Icon));   // no wait
}
```

Notes:

- The method that loads follows every rule above: it returns `void`, takes no non-const reference parameters and
  is not `static`.
- UeApi's overload leaves out the completion delegate. AssetGen binds a generated event to it, named
  `<Method>_<DelegateParam>_<N>`, which stores the value before the method resumes. Here they are `LoadAll_OnLoaded_0`
  and `LoadAll_OnLoaded_1`, and they show in the class's function list. Any latent function with exactly one completion
  delegate of one parameter gets the same value-returning overload.
- Soft references themselves are in [Types](#types). [examples/EnemyInfo.cpp](examples/EnemyInfo.cpp) loads one.

### Static locals

| You write | What it does | Status |
|---|---|---|
| `static int32 Count = Stage + 10;` in a method that waits | The static lives in the object's frame. Its initializer runs on that object's first call only, and the value is kept after that: once per object, not once per program as in C++. A plain local of the same name elsewhere in the method is a separate variable and does not reset it. | Yes |
| `static int32 Seen;` | Starts at zero on the object's first call and keeps its value across calls and loop rounds. A loop around it does not reset it, as it would a plain local. | Yes |
| `static int32 Count = N;` in a method that never waits | Refused: "static Count lives in the ubergraph's frame". Only the frame of a method that waits outlives a call. Make `Count` a member. | Refused |
| `static int32 Count = 0;` in an inline helper | Refused: "each expansion would keep its own", unlike C++'s single static. | Refused |
| `static constexpr int32 kFive = 5;` | A static that holds a constant nothing writes is replaced by that constant. It needs no frame, so it works in any method, waiting or not. | Yes |

```cpp
int32 Stage;
int32 Calls;
int32 Got;

void Counted() {
  static int32 Count = Stage + 10;   // runs on this object's first call only
  ++Count;
  for (int32 I = 0; I < 2; ++I) {
    static int32 Seen;               // zero on the first call, never reset by the loop
    ++Seen;
    Got = Seen;
  }
  UKismetSystemLibrary::Delay(0.1f);
  Calls = Count;
}
void Plain() {
  static constexpr int32 kFive = 5;  // a constant: fine in any method
  Stage = kFive;
}
```

## Waiting on events

`UE_AWAIT(Obj->Dispatcher)` waits in the middle of a method until a dispatcher next fires, then continues there with
the value it carries, as the editor's async action node continues from an output pin. The method becomes latent, so
everything in [Latent calls](#latent-calls) applies to it. The rule to remember: the binding stays, and every later
broadcast runs the code after the await again. [examples/AwaitEvents.cpp](examples/AwaitEvents.cpp) waits this way on
the game's dispatchers and one of its own, and its comments show the async-action form.

### UE_AWAIT

| You write | What it does | Status |
|---|---|---|
| `FName Notify = UE_AWAIT(Proxy->OnCompleted);` | Binds a generated event to the dispatcher and returns to the caller. When the dispatcher fires, the event stores its one parameter, and the method resumes after the await with that value. | Yes |
| `Image = UE_AWAIT(Task->OnSuccess);` on an async action | For a `UBlueprintAsyncActionBase`, such as `UAsyncTaskDownloadImage`, AssetGen calls `Activate()` right after the bind, as the editor's async node does, at the first await of that variable on each path the method runs: in whichever branch of an `if` runs, and in a loop on the first round only. A later await on the same variable does not activate it again, until the variable is assigned a new action. An object that is not an async action, such as the montage proxy, is never activated: its factory already started the work. | Yes |
| `if (Stop) UE_AWAIT(T->OnSuccess);` then `UE_AWAIT(T->OnFail);` | Refused: `some paths reach it with T's async action already activated and some without`. Keeping the second await's `Activate()` would start the action twice on one path, dropping it would never start it on the other. The same for a loop that can skip its await, such as with `continue` before it. | Refused |
| `UE_AWAIT(Target->OnDestroyed);` | As a statement: waits for the dispatcher and drops its value. | Yes |
| `UE_AWAIT(Task->OnSuccess);` with `Task` None | The bind, and `Activate()` for an async action, run only when the object passes IsValid, as the editor's async node tests its proxy. A None or pending-kill object binds nothing and logs no "Accessed None"; the method stays parked at the await. | Yes |
| `int32 Code = UE_AWAIT(OnReady);` | The object in front of the dispatcher may be self, a member or a local. | Yes |
| the code after the await, on a later broadcast | The generated event stays bound, so each later broadcast runs the code after the await again: it acts as a handler, not a one-shot wait. The event has no name you can write, so only `Clear()` on the dispatcher stops it, and that drops every binding. | Yes |
| `UE_AWAIT(OnScored);` on a dispatcher of two or more parameters | Compiles as a statement and resumes, but the values are dropped: `UE_AWAIT` has a value only for a dispatcher of exactly one parameter. Bind a handler to read them. | Not yet |
| `bool WaitDone() { UE_AWAIT(Proxy->OnCompleted); return true; }` | Refused with the latent messages ("a function that resumes later returns nothing and takes no non-const reference parameters"; for a static method, "a static function has no object whose ubergraph frame could keep its locals"). A method that awaits returns `void`, takes no non-const reference parameters and is not `static`. | Refused |
| `UE_AWAIT(UAsyncTaskDownloadImage::DownloadImage(Url)->OnSuccess);` | Refused: "keep the object in a variable, it is used twice", once for the bind and once for `Activate()`. | Refused |
| `UE_AWAIT(Temp);` on a local delegate | Refused: `UE_AWAIT` takes a dispatcher property of an object. A multicast delegate as a local or a parameter is refused before `UE_AWAIT` sees it ("unimplemented local"). A single-cast `TDelegate` cannot be awaited at all. | Refused |
| `Task->OnSuccess.Add(this, &Door::Done);` | Callback style: a handler per outcome, and the method does not wait. `Add` never calls `Activate()`, so call `Task->Activate()` yourself after binding. See [Event dispatchers](#event-dispatchers). | Yes |

```cpp
#include "UeApi/AnimGraphRuntime.h"
#include "UeApi/UMG.h"

class Door : public AActor {
public:
  USkeletalMeshComponent *Mesh;
  UAnimMontage *Montage;
  AActor *Target;
  UTexture2DDynamic *Image;
  FName Last;
  int32 Kills;
  int32 Result;
  UE_DISPATCHER(OnReady, int32 Code);

  void PlayThen() {
    UPlayMontageCallbackProxy *Proxy =
        UPlayMontageCallbackProxy::CreateProxyObjectForPlayMontage(Mesh, Montage, 1.0f, 0.0f, FName());
    FName Notify = UE_AWAIT(Proxy->OnCompleted);   // PlayThen returns to its caller here
    Last = Notify;                                 // runs when the montage completes
  }
  void Download(FString Url) {
    UAsyncTaskDownloadImage *Task = UAsyncTaskDownloadImage::DownloadImage(Url);
    Image = UE_AWAIT(Task->OnSuccess);             // binds OnSuccess, then calls Task->Activate()
  }
  void WaitReady() {
    int32 Code = UE_AWAIT(OnReady);                // this object's own dispatcher
    Result = Code;                                 // runs again on every later OnReady.Broadcast
  }
  void CountKill() {
    UE_AWAIT(Target->OnDestroyed);                 // the value is dropped
    Kills += 1;
  }
};
```

Notes:

- Only the awaited dispatcher is bound. If the action fires another outcome instead, such as the montage proxy's
  `OnInterrupted`, the method never resumes. Use the callback style when either outcome can happen.
- The generated event is named `<Method>_<Dispatcher>_<N>` (`PlayThen_OnCompleted_0` above) and shows in the class's
  function list.
- In a `UObject` child class, a method that only awaits still prints the latent "finds its world only through its
  Outer" warning, although a dispatcher needs no world.

## Pointers and memory

A pointer to anything that is not a UObject is a raw memory address. AssetGen keeps it as an int64 in the Blueprint
and turns `*P`, `P[I]`, `P->X`, pointer arithmetic and `&` into reads and writes of the game's memory. None of this
needs an include or a declaration. The rule to remember: nothing checks an address. A wrong one reads garbage or
crashes the game, as it would in native C++. [examples/MemoryRead.cpp](examples/MemoryRead.cpp) is a worked example.

### Raw pointers and object pointers

| You write | What it does | Status |
|---|---|---|
| `int32 *P`, `void *Q`, `FVector *V`, `UObject **PP` | A pointer to anything but a UObject is an int64 address. Pointer parameters, return values and locals are Int64 variables in the Blueprint. The compiler keeps the pointee type, so `*P` knows what it reads. There is no editor node for any of this. | Yes |
| `FString *S`, `FName *N`, `TArray<int32> *A` | FString, FName and the containers are plain records, not objects, so pointers to them are raw addresses too. | Yes |
| `AActor *A`, `UObject *O` | A pointer to a UObject class stays an object reference that the garbage collector sees. | Yes |
| `AActor &A = *Other;`, `Others[1]` | An object pointer is not an address, so `*` and `[]` on one are refused ("which is not a raw pointer", "indexing that is not through a raw pointer"). Use the pointer as it is, `Other->Field`. For the object's memory, convert it: `(uint8 *)Other`. | Refused |
| `const char *S`, `wchar_t *W` | Never an address. A `char*` parameter or local is an FString, and a string literal is a string constant. Walk raw bytes with `int8*` or `uint8*`. | Yes |
| `if (P)`, `P != nullptr` | Compares the address with 0 (NotEqual_Int64Int64). `nullptr` is the int64 0. | Yes |
| `if (Obj)` | On an object pointer this is an IsValid test, not a null check: see [Working with other objects](#working-with-other-objects). | Yes |

Notes:

- A class is a UObject when it has UE_CLASS or a base class. A class that is only forward-declared counts as one when
  its name is `U` or `A` followed by a capital letter, or ends in `_C`. Any other forward-declared class is raw memory,
  so include the SDK header that defines it.

### Reading and writing through a pointer

| You write | What it does | Status |
|---|---|---|
| `int32 V = *P;` | Reads the value at the address. | Yes |
| `P[I]`, `2[P]` | Reads the element at P + I × sizeof(pointee); a literal I folds to a constant offset. This is pointer indexing, not `Items[I]` on a TArray. | Yes |
| `*P = 42;`, `P[2] = 7;` | Writes the value straight into the memory. | Yes |
| `*(int8 *)Addr`, `*(signed char *)Addr` | Reads one byte, sign-extended: 0x80 to 0xFF read as negative ints, as in C++. | Yes |
| `*(uint8 *)Addr`, `*(bool *)Addr` | Reads one byte as 0 to 255. A bool read is `byte != 0`. | Yes |
| `FString S = *(FString *)Addr;` | Copies the string out. The function owns its copy, which is freed when it returns. | Yes |
| `FText T = *(FText *)Addr;`, `*(FText *)Addr = T;` | Copies the text with the engine's FText copy, which shares the text data by reference count. Needs the FDerefTextView helper struct (see Helper code you supply, below). | Yes |
| `*(UObject **)Addr`, `*(UClass **)Addr` | Reads 8 bytes as an object or class reference. | Yes |
| `(int64)*(void **)Addr` | A pointer to a raw pointer reads a plain int64 address. | Yes |
| `int64 V = *(uint32 *)Addr;` | Not built: Kismet has no unsigned 32-bit integer, and an int32 read would make 0xFFFFFFFF compare as -1. Refused with "reading a uint32 through a pointer". Read an int32 and mask it, as below. | Not yet |
| `*(uint32 *)Addr = 5;` | Refused with "no Kismet conversion from int to uint32". Write through an int32 pointer: `*(int32 *)Addr = 5;`. | Refused |
| `FVector V = *Pv;`, `*Pv = FVector(1.0f, 2.0f, 3.0f);` | A whole struct has no read or write through a pointer, and neither do a double and a uint16. Refused with "a whole FVector through a pointer" ("a whole double", ...). Read and write the members with `Pv->X`. | Not yet |

```cpp
TArray<int32> Items;

int32 Walk() {
  Items.Add(10);
  Items.Add(20);
  Items.Add(30);
  int32 *P = &Items[0];               // the address of the first element
  int32 Second = P[1];                // 20
  *P = Second + 1;                    // Items[0] is now 21
  int32 *Last = P + 2;                // two elements on: 8 bytes
  return *Last + (int32)(Last - P);   // 30 + 2
}

int64 Unsigned(uint32 *P) {
  int32 Raw = *(int32 *)P;            // 4 bytes, signed
  return (int64)Raw & 0xFFFFFFFFLL;   // 0 to 4294967295
}
```

Notes:

- A read takes the pointee's size in the game: 8 bytes for int64, uint64, FName and any pointer; 4 for int32 and
  float; 1 for int8, uint8 and bool; an enum's underlying size (1, 4 or 8); 16 for FString; 24 for FText.
- The uint32 message also suggests an int64 read. An int64 read takes 8 bytes, so it includes the 4 bytes after the
  value. Use it only when the 8 bytes really are an int64.

### Struct members through a pointer

| You write | What it does | Status |
|---|---|---|
| `V->Z = 5.0f;`, `float Y = V->Y;` | Reads or writes one member in place, at the address plus the member's offset. The struct is never copied, so a pointer to a struct of any size works. | Yes |
| `(*V).X` | The same as `V->X`. | Yes |

```cpp
TArray<FVector> Points;

float Lift() {
  Points.Add(FVector(1.0f, 2.0f, 3.0f));
  FVector *V = &Points[0];
  V->Z = V->Z + 100.0f;                 // writes Points[0].Z in place
  float *Y = (float *)((int64)V + 4);   // FVector::Y is 4 bytes in
  return *Y + (*V).X;
}
```

Notes:

- On a struct the mod defines, `P->X` gives the function a hidden, unused local named `__Keep<Struct>__`. It keeps
  the struct's package loaded when only the bytecode names the struct. You see it in a disassembly, never in behaviour.

### Pointer arithmetic

| You write | What it does | Status |
|---|---|---|
| `P + N`, `N + P`, `P - N` | Moves by N elements, as in C++: the byte offset is N × sizeof(pointee). The result is another address. | Yes |
| `P - Q` | The number of elements between two pointers, as an int64. | Yes |
| `P += N`, `P -= N`, `++P`, `--P`, `P++` | Steps a pointer variable in place, by elements. `P++` yields the value from before the step. | Yes |
| `(int64)P + 4`, `(uint8 *)P + 4` | Byte arithmetic: an int64 or a byte pointer moves by bytes. | Yes |

### Taking an address

| You write | What it does | Status |
|---|---|---|
| `&Items[I]` | The address of a TArray element: the array's data pointer plus I × the element size. The array must be a variable, a member or a local. | Yes |
| `&MakeArr()[0]` | Refused with "the address of an element needs an array variable". Store the array in a local first. | Refused |
| `&*P`, `&P[I]` | P itself, and P + I × sizeof(pointee). Nothing is read. | Yes |
| `&Obj->Member`, `&Member` | The address of an object's property (`&Member` is one of the class's own), looked up by name at run time. Needs a class named ReadProperty that you supply (see Helper code you supply, below). The result is 0 when the object's class has no such property. | Yes |
| `&V.X`, `&Pv->X` | The address of a struct member is not built. Refused with "the address of MemberExpr" for a struct variable, and "the address of a member of int64" through a pointer. Add the offset yourself: `(int64)Pv + 4` is the address of `Pv->Y` for an FVector. | Not yet |
| `&Local`, `&Param` | A local or a parameter is a Blueprint variable, which has no address the VM hands out. Refused with "a Blueprint variable has none the VM hands out". Point into an object, a TArray element or memory through a pointer instead. | Refused |
| `&this->K` with `static inline int32 K = 3;` | An inline class variable has no storage: each use is its initializer. Refused with "a static variable has no address". A bare `&K` gets the Blueprint-variable message instead. | Refused |

```cpp
#include "ReadProperty.h"   // your helper's header

float Dilation(AActor *Obj) {
  float *P = &Obj->CustomTimeDilation;   // 0 when Obj's class has no such property
  return P ? *P : 0.0f;
}

int64 WhereSelf() { return (int64)&CustomTimeDilation; }
```

Notes:

- An element address stays valid only until the array reallocates, for example on Add or Remove, as in C++.
- A cooked property carries no offset, so `&Obj->Member` compiles to a call to
  `ReadProperty::GetPropertyAddress(Obj, FName("Member"), <world context>)`. The world context is the calling
  function's world-context parameter, or self when it has none. `&Member` on the class's own object passes self as Obj.
- Without a class named ReadProperty in scope, the build fails with "`&Obj->Member` finds the member at run time
  through ReadProperty::GetPropertyAddress: include ReadProperty.h". The build does not check the function it calls.
- Only an object's members have a run-time lookup. A struct member's address is the "Not yet" row above.
- `float &R = Other->CustomTimeDilation;` makes the same GetPropertyAddress call and keeps the address it returns.
  Without a ReadProperty class it is refused with the same message, after `reference R: `. Reference locals are in
  [Locals](#locals).

### References to memory

| You write | What it does | Status |
|---|---|---|
| `int32 &R = *P;`, `int32 &R = P[2];` | Keeps the address in a hidden int64 local. Reads and writes through R go to that memory. | Yes |
| `int32 &Alias = Local;` | Another name for the variable, with no storage of its own. Writes reach the original. | Yes |
| `const FString &R = MakeName();` | A `const T&` bound to a temporary is an ordinary local holding a copy. | Yes |
| `Bump(*P);` with `void Bump(int32 &V)` | Passes the memory itself, so the callee's writes land at the address. A T& parameter of a mod function is an out-parameter in the Blueprint. | Yes |
| `int32 &Get(int32 *P) { return *P; }` | A reference return is not built. Refused with "a reference return is refused"; return a value or a pointer. | Not yet |

```cpp
void Bump(int32 &V) { V = V + 1; }

void Refs(int32 *P) {
  int32 &Third = P[2];   // keeps the address P + 2 in a hidden int64 local
  Third = Third + 5;     // writes P[2]
  Bump(*P);              // Bump writes P[0] itself
}
```

Notes:

- Reference locals in general are in [Locals](#locals), and reference parameters in [Functions](#functions).

### Objects and addresses

| You write | What it does | Status |
|---|---|---|
| `(int64)Obj`, `(uint8 *)Obj` | The object's address, as an int64 or a byte pointer: the reference's 8 bytes, reinterpreted. `__AddrOf__(Obj)` is the explicit form. | Yes |
| `(AActor *)Addr` | An int64 reinterpreted as an object reference. Nothing checks that an object lives there. | Yes |
| `(AActor *)0`, `(AActor *)5` | The literal 0 is null. Any other integer literal is widened to int64 first. | Yes |
| `(AActor *)N` with an int32 N | Refused with "an int becomes a pointer through int64". Store the value in an int64 variable first: `int64 Addr = N;` then `(AActor *)Addr`. `(AActor *)(int64)N` is refused too. | Refused |

```cpp
int64 Where(AActor *Obj) { return (int64)Obj; }
AActor *Back(int64 Addr) { return (AActor *)Addr; }

UClass *ClassOfObject(UObject *Obj) {
  return *(UClass **)((int64)Obj + 16);   // UObject::ClassPrivate, at 0x10 in UE 4.27
}
```

Notes:

- The reinterpretation reads the storage its operand leaves behind, and a call, `this` or a literal leaves none. The
  compiler stores such an operand in a hidden `__PtrTmpN__` local first. This explains an extra local in a
  disassembly.

### sizeof and alignof

| You write | What it does | Status |
|---|---|---|
| `sizeof(FVector)`, `sizeof(*P)` | The type's size in the game, folded to a constant: 12 for FVector. This is not the size clang gives the SDK's stand-in type. Pointer arithmetic uses the same sizes. | Yes |
| `alignof(FTransform)` | The type's alignment in the game, folded to a constant: 16 here. | Yes |
| `sizeof(AActor)` | An object class by value has no layout the compiler knows. Refused with "unimplemented struct member type: AActor". `sizeof(AActor *)` is 8. | Refused |

Notes:

- The known layouts: the integer types, float, double, bool, FName (8), FString (16), FText (24), any pointer (8),
  enums, TArray and TScriptInterface (16), TSet and TMap (80), soft pointers (40), TSubclassOf (8), engine structs,
  and mod structs, laid out with each member at its natural alignment.
- sizeof and alignof work in code. A member default does not accept them: see
  [Classes and variables](#classes-and-variables).

### Where pointer code runs

| You write | What it does | Status |
|---|---|---|
| `while (*Q != 0) { ++Q; }` | A condition that reads memory is read again before every iteration, as in C++. The compiler writes the loop as `while (true) { if (!Cond) break; ... }`. | Yes |
| `Value = *P;` in a function that also calls `Delay(1.0f)` | A function that makes a latent call runs in a generated event graph, where a memory read is not built. Refused with "a pointer read in a function that makes a latent call". Do the read in a function that does not wait and is not `inline`, and call it. An inline function is expanded into the caller, so its read is refused the same way. | Not yet |

```cpp
int64 Addr = 0;
int64 Value = 0;

int64 ReadNow() { return *(int64 *)Addr; }

void Poll() {
  Value = ReadNow();   // the read runs in a function that does not wait
  UKismetSystemLibrary::Delay(1.0f);
}

int32 CountNonZero(int32 *Q) {
  int32 N = 0;
  while (*Q != 0) {    // read again before every iteration
    ++Q;
    ++N;
  }
  return N;
}
```

Notes:

- The refusal covers a read such as `*P`, the `__Read*__` intrinsics, and `Obj->GetOuter()`, which reads the outer
  from memory (see [Working with other objects](#working-with-other-objects)). A write through a pointer and a
  `P->X` read are not refused in such a function, but nothing has checked how they behave there. Latent functions
  are in [Latent calls](#latent-calls).
- Reads and writes are not checked. The VM does no bounds or validity check, so a wrong address reads garbage or
  raises an access violation in the game process. There is no soft failure. Check what you can first: that an object
  is valid, and that a property exists and has the type you expect.

### The FDeref scratch struct

| You write | What it does | Status |
|---|---|---|
| `*P`, `P[I]`, `P->X` | Nothing to declare. Every read or write through a pointer goes through a 16-byte scratch struct, FDeref (`int64 Data; int32 Num; int32 Max;`). The compiler creates it on first use and cooks it as FDeref.uasset in the mod's package, beside the class. | Yes |
| `struct FDeref { UE_STRUCT; int64 Data; int32 Num; int32 Max; };` | A struct the mod declares under this name is used instead, and none is created. | Yes |

Notes:

- The class imports FDeref.uasset, so it is part of the mod's output: ship it with the class.
- The layout of a declared FDeref is not checked. Another shape compiles without a warning, and every pointer access
  in the mod then names members the struct does not have. Do not declare one unless you have a reason.
- The FText view is separate: its path is fixed (see the next section), and an FDerefTextView declared in an ordinary
  mod is cooked under that mod's package and never used.

### Helper code you supply

AssetGen ships two headers, `include/Intrin.h` and `include/Objects.h`, and no helper mods. Three features call code
that the compiler does not generate, so you supply it:

| You write | What it does | Status |
|---|---|---|
| `&Obj->Member` | Calls the Blueprint function `GetPropertyAddress` on the class named ReadProperty that the source can see. | Yes |
| `*(FText *)Addr`, `__ReadText__(Addr)` | Imports the struct `/Game/_ElytrasMods/ReadProperty/FDerefTextView`, a path the compiler hard-codes. | Yes |
| `__ClassOf__(x)` | Calls GetParmClassName on the class that uses it: see [Intrinsics](#intrinsics). | Yes |

One small helper mod covers the first two. It cooks FDerefTextView at the fixed path, and a ReadProperty class that
exports a real GetPropertyAddress. The function walks the object's class and its super classes for a property with
that name, and returns the object's address plus the property's offset, or 0 when the object is null or has no such
property.

```cpp
// ReadProperty.h: included by every mod that uses &Obj->Member
#pragma once
#include "UeApi/Types.h"
#include "UeApi/FSD.h"
#include "../include/Intrin.h"   // AssetGen's include/Intrin.h, by its path from here

class ReadProperty : public UBlueprintFunctionLibrary {
public:
  UE_CLASS("/Game/_ElytrasMods/ReadProperty/ReadProperty", "ReadProperty_C");
  static int64 GetPropertyAddress(class UObject *Target, FName PropName, class UObject *WorldContextObject = nullptr);
};
```

```cpp
// ReadProperty.cpp: the helper mod's only source
#include "ReadProperty.h"
UE_MOD_PACKAGE("/Game/_ElytrasMods/ReadProperty");

struct FDerefTextView { UE_STRUCT; TArray<FText> Data; };   // the view every FText pointer access uses

int64 ReadProperty::GetPropertyAddress(class UObject *Target, FName PropName, class UObject *WorldContextObject) {
  if (Target == nullptr) return 0;
  int32 Want = __NameIndex__(PropName);
  int64 Struct = __Read64__(__AddrOf__(Target) + 16);        // UObject::ClassPrivate
  while (Struct != 0) {
    int64 Field = __Read64__(Struct + 80);                    // UStruct::ChildProperties
    while (Field != 0) {
      if (__Read32__(Field + 40) == Want)                     // FField::NamePrivate
        return __AddrOf__(Target) + __Read32__(Field + 76);   // FProperty::Offset_Internal
      Field = __Read64__(Field + 32);                         // FField::Next
    }
    Struct = __Read64__(Struct + 64);                         // UStruct::SuperStruct
  }
  return 0;
}
```

```yaml
mods:
  - name: ReadProperty
    sources: [ReadProperty.cpp]
  - name: MyMod
    sources: [MyMod.cpp]
    needs: [ReadProperty]   # build the helper first
    embed: true             # also pack the helper's assets into MyMod_P.pak
```

Notes:

- The package must be exactly `/Game/_ElytrasMods/ReadProperty` for the FText view, because the compiler hard-codes
  that path. A helper that only serves `&Obj->Member` can live in any package, as long as the class is named
  ReadProperty and its UE_CLASS path is `<its UE_MOD_PACKAGE>/ReadProperty`.
- Keep FDerefTextView in the .cpp, not in the header. A UE_STRUCT is cooked under the package of the mod that
  declares it, and FText access only ever uses the copy at the fixed path.
- GetPropertyAddress must not be `inline`, and must keep its three parameters. An inline method gets no function of
  its own, and the call always passes three arguments. Neither mistake is caught at build time.
- The name match compares only the FName's comparison index, not its number suffix.
- The offsets are the game's UE 4.27 layout, as the game's SDK dump states it. The helper is checked at build level
  (its imports and exports); it has not been run in the game.
- Without `embed`, ship the helper's own pak beside yours. [Building mods](GUIDE.md#building-mods) covers mods.yaml.
- None of the three features is checked at build time, except that `&Obj->Member` with no ReadProperty class in scope
  fails. A mod that uses FText through a pointer, or `__ClassOf__`, without the helper compiles with no warning.
  bpbuild reports a missing import only when the imported package lies under a mod in the same build, and it checks
  packages, not the functions in them.

## Intrinsics

An intrinsic is a function spelled `__Name__` that the compiler turns into a fixed piece of bytecode. None of them is a
real function. They are declared in AssetGen's `include/Intrin.h`, which a source includes to use them; raw pointers
need none of them. The rule to remember: an intrinsic whose value goes nowhere emits nothing. A `__Read32__(P);` on
its own line, or a local that nothing reads, does nothing, though a real function call among its arguments still runs.
The spelling `__Name__` is reserved: a call to one the compiler does not know is refused with "unimplemented
intrinsic".

### Reading memory

| You write | What it does | Status |
|---|---|---|
| `__Read64__(A)` | Reads 8 bytes at the int64 address A, as an int64. | Yes |
| `__Read32__(A)` | Reads 4 bytes as an int32. | Yes |
| `__ReadFloat__(A)` | Reads 4 bytes as a float. | Yes |
| `__ReadByte__(A)` | Reads 1 byte as a uint8, 0 to 255. | Yes |
| `__ReadObject__(A)`, `__ReadClass__(A)` | Reads 8 bytes as a UObject* or a UClass*. `__ReadClass__` assigns to a UClass* variable without a cast. | Yes |
| `__ReadName__(A)` | Reads 8 bytes as an FName. | Yes |
| `__ReadString__(A)` | Copies the 16-byte FString at A. | Yes |
| `__ReadText__(A)` | Copies the 24-byte FText at A. Needs the FDerefTextView helper struct: see [Pointers and memory](#pointers-and-memory). | Yes |
| `__Read32__(P)` with `int32 *P` | Every read also takes a pointer, and reads at the address it holds. | Yes |
| `__AddrOf__(Obj)` | The object's address as an int64, the same as `(int64)Obj`. | Yes |
| `__NameIndex__(Name)` | The FName's comparison index, its first 4 bytes, as an int32. A cheap identity to compare with a name read from memory. It ignores the name's number suffix. | Yes |

```cpp
#include "../include/Intrin.h"   // AssetGen's include/Intrin.h

UClass *ClassOfObject(UObject *Obj) {
  int64 Addr = __AddrOf__(Obj);      // the object's address, as (int64)Obj gives
  return __ReadClass__(Addr + 16);   // UObject::ClassPrivate, at 0x10 in UE 4.27
}
```

Notes:

- The reads work exactly like `*P` on the matching pointer type, with the same rules: nothing checks the address,
  and a function that makes a latent call cannot contain one.

### __PtrCast__

| You write | What it does | Status |
|---|---|---|
| `__PtrCast__<int64>(Obj)` | The object's address. | Yes |
| `__PtrCast__<AActor *>(Addr)` | The object at an address. | Yes |
| `__PtrCast__<int32 *>(Addr)` | The address as a pointer, which is still an int64. | Yes |
| `__PtrCast__<int32 &>(Addr)` | The int32 at Addr, as a value you can read and assign to. | Yes |
| `__PtrCast__<int64>(R)` with `int32 &R = *P` | The address R keeps, not the value there. | Yes |
| `__PtrCast__<int64>(R)` with `int32 &R = Local` | A reference to a Blueprint variable has no address. Refused with "a reference to a Blueprint variable, which has no address". | Refused |

```cpp
#include "../include/Intrin.h"   // AssetGen's include/Intrin.h

bool Casts(int32 *P, AActor *Obj) {
  int64 Addr = __PtrCast__<int64>(P);            // the address P holds
  __PtrCast__<int32 &>(Addr) = 60;               // writes the int32 at Addr
  int32 &Third = P[2];
  int64 ThirdAddr = __PtrCast__<int64>(Third);   // Addr + 8, the address Third keeps
  int32 *Q = __PtrCast__<int32 *>(ThirdAddr);    // the same address as a pointer
  AActor *Same = __PtrCast__<AActor *>(__PtrCast__<int64>(Obj));
  return Same == Obj && *Q == P[2];
}
```

Notes:

- `__PtrCast__` is resolved at compile time. The only run-time work is reinterpreting an object as an int64, or the
  reverse, where one is needed.

### __RefAt__

| You write | What it does | Status |
|---|---|---|
| `__RefAt__(Addr)` | Passes the memory at Addr by reference to an engine function whose native code steps its wildcard argument itself (a CustomThunk). The function sees Addr, not the address of a temporary copy. Read as a plain value, it is 8 bytes of int64. | Yes |

```cpp
#include "../include/Wildcard.h"   // AssetGen's include/Wildcard.h: the engine's wildcard setters, int64 in place
                                   // of the wildcard parameter (and Intrin.h, for __RefAt__)

// In your class:
void Copy(AActor *To, FName ToField, int64 Addr) {
  WildcardSystemSetters::SetStructurePropertyByName(To, ToField, __RefAt__(Addr));
}
```

Notes:

- To call such a function, redeclare it with an int64 parameter in place of the wildcard, as above.
- Each use gets its own hidden FDeref local, so a later read in the same statement cannot overwrite it before the
  call.
- SetStructurePropertyByName copies with the destination property's type and trusts the source address. Check that
  the property exists and that the two types match before you call it.

### __ClassOf__

| You write | What it does | Status |
|---|---|---|
| `__ClassOf__(Out)` | The property class name (IntProperty, FloatProperty, ...) of a parameter or local of the current function, found at run time. It compiles to `GetParmClassName(<this function>, FName("Out"))` on the class that uses it; Out itself is not evaluated. Slower than a string literal, but it follows a rename or a retype. | Yes |
| `__ClassOf__(Obj->Field)` | Refused with "argument must be a bare parameter or local reference". | Refused |

```cpp
#include "../include/Intrin.h"   // AssetGen's include/Intrin.h

/* __ClassOf__(x) calls this: the property class name of Fn's parameter or local ParmName, or None. */
UE_PURE static FName GetParmClassName(class UObject *Fn, FName ParmName) {
  int32 Want = __NameIndex__(ParmName);
  int64 Field = __Read64__(__AddrOf__(Fn) + 80);   // UStruct::ChildProperties
  while (Field != 0) {
    if (__Read32__(Field + 40) == Want)             // FField::NamePrivate
      return __ReadName__(__Read64__(Field + 8));   // FField::ClassPrivate, then FFieldClass::Name
    Field = __Read64__(Field + 32);                 // FField::Next
  }
  return FName();
}

bool IsInt(int32 &Out) { return __ClassOf__(Out) == FName("IntProperty"); }
```

Notes:

- Each class that uses `__ClassOf__` needs its own GetParmClassName. AssetGen ships none; the body above works with
  the game's UE 4.27 layout. Intrin.h's comment names a reference version in ReadProperty, which is not part of
  AssetGen.
- Declare it `static`, not `inline`, with exactly two parameters. An inline method has no function of its own, and a
  third, defaulted parameter is never filled, because the call passes two arguments.
- The build does not check that the method exists. Without it, the mod compiles with no warning, and the class calls
  a function it does not export.

### __Asm__

| You write | What it does | Status |
|---|---|---|
| `__Asm__("0B");` | Appends the hex bytes to the function's bytecode unchanged (0x0B is EX_Nothing). Whitespace, commas and underscores are skipped, and case does not matter. A statement only. | Yes |
| `__Asm__("0B", 1);` | The same, with the VM memory size the bytes stand for. Pass it for a token that carries an FName, an object reference or a jump target. | Yes |

Notes:

- The one-argument form advances the VM memory offset, which jump targets count, by the byte length. That is right
  only for tokens whose on-disk and in-memory forms are the same size. A wrong size shifts every later jump target.
- The bytes are not checked. Invalid bytes still cook, and the VM runs them.
- A malformed string, such as an odd digit count or the `x` of `0x0B`, compiles with no error or warning, and the
  statement emits nothing. Write bare hex digits, two per byte, and check the result with `tools/walkscript.py`.

### __EmbedFile__

| You write | What it does | Status |
|---|---|---|
| `TArray<uint8> Payload = __EmbedFile__("data/blob.bin");` | Bakes a file's bytes into a `TArray<uint8>` member's default at build time: see [Classes and variables](#classes-and-variables). In a function body it is refused as an "unimplemented intrinsic". | Yes |

## Macro and intrinsic index

Every macro in the SDK's `UeApi/UeMeta.h`, every intrinsic in AssetGen's `include/Intrin.h`, every helper in
`include/Objects.h`, and the plain C++ spellings that AssetGen gives a meaning of their own, in alphabetical order.
Leading underscores and symbols don't count for the order. The last column links the section that documents each one.
A name that is not here is ordinary C++, or an engine or game function from the SDK: see
[Calling engine and game functions](#calling-engine-and-game-functions). A call to a `__Name__` function that is not
listed here is refused with "unimplemented intrinsic".

| Name | What it does | Section |
|---|---|---|
| `AddComponentByType<T>(Owner)` | Add Component by Class: adds a component to an actor at run time. It also takes a component class the mod declares, which UE_COMPONENT refuses. | [Components](#components) |
| `AddComponentDeferred<T>(Owner)` | Add Component with exposed pins: registration waits for FinishComponent, so the values set in between are the component's first. | [Components](#components) |
| `__AddrOf__(Obj)` | An object's address as an int64, the same as `(int64)Obj`. | [Intrinsics](#intrinsics) |
| `__Asm__(Bytes)`, `__Asm__(Bytes, MemBytes)` | Appends raw Kismet bytecode written in hex. Nothing checks the bytes, and a malformed hex string is dropped with no message. | [Intrinsics](#intrinsics) |
| `&Asset` | A pointer to an asset, one the mod cooks or one named by UE_ASSET_AT, in a default or in code. | [Data assets](#data-assets), [Game assets](#game-assets) |
| `AttachToActor(Child, Parent)` | Attaches an actor to another, with one rule for location, rotation and scale. | [Creating objects](#creating-objects) |
| `AttachToComponent(Child, Parent)` | Attaches a scene component to another, at a socket if you name one. Returns whether it attached. | [Components](#components) |
| `__Await__` | What UE_AWAIT expands to. Write UE_AWAIT. | [Waiting on events](#waiting-on-events) |
| `Base::Method(...)` | Inside an override, the parent's implementation, run on this object: Add call to parent function. | [Overrides and parent calls](#overrides-and-parent-calls) |
| Braced list, `{2, 3, 5}` | A container's elements: the class default when it is a member's initializer, a Make Array / Set / Map in a function. | [Containers](#containers) |
| Braced object at namespace scope, `UMyDef Big = {.Health = 500};` | An asset the mod cooks, like a Data Asset made in the Content Browser. The braces set the members they name. | [Data assets](#data-assets) |
| `Cast<T>(Obj)` | Cast To: the object if it is a T, otherwise null. To reach an interface, use TScriptInterface. | [Types](#types), [Interfaces](#interfaces) |
| `__ClassOf__(X)` | The property class name of a parameter or local, such as IntProperty, found at run time by the class's GetParmClassName. | [Intrinsics](#intrinsics) |
| `const` member, `const int32 Limit = 3;` | A read-only variable (BlueprintReadOnly): a Get node and no Set. Its initializer is the default. | [Classes and variables](#classes-and-variables) |
| `UE_READONLY int32 Cap = 5;` | BlueprintReadOnly like `const`, but a subclass's `UE_DEFAULTS` may set it, and a write from code compiles with a warning. | [Access, read-only and categories](#access-read-only-and-categories) |
| `const` method, `int32 Get() const` | A const Blueprint function. | [Functions](#functions) |
| `consteval` function | Runs while the mod compiles, and only the result is cooked. Its arguments must be constants. | [Constants](#constants) |
| `constexpr` and `const` variables at namespace scope | Constants: no storage, each use is the value, worked out at build time. | [Constants](#constants) |
| `constexpr` function | Not run at build time. A method is an ordinary Blueprint function, and a free one needs `inline`. For build time, write `consteval`. | [Inline functions and templates](#inline-functions-and-templates) |
| Constructor, `MyMod() { ... }` | Not compiled, and nothing says so. Use initializers, UE_DEFAULTS and ReceiveBeginPlay. | [Class defaults](#class-defaults) |
| Container methods, `Items.Add(X)`, `Map.Find(Key, Out)` | The Array, Set and Map nodes, called as methods. `Num()` is Length, and a TArray's `Remove(I)` removes the element at index I. | [Containers](#containers) |
| `CreateWidget<T>(OwningPlayer)` | Create Widget: a widget of T, or of a class you pass, owned by a player controller. | [Creating objects](#creating-objects) |
| Delay, `UKismetSystemLibrary::Delay(Seconds)` | A latent call: the function stops there and goes on when the delay ends. Every function with an FLatentActionInfo parameter works this way. | [Latent calls](#latent-calls) |
| `Derives<T, Base>` | The concept that Objects.h constrains its helpers with: T derives from Base. A wrong T is a clang error on the calling line. | [Creating objects](#creating-objects) |
| Dispatcher methods, `OnHit.Add(this, &C::F)`, `Remove`, `Clear()`, `Broadcast(...)` | Bind Event, Unbind Event, Unbind all Events and Call. A dispatcher has no other methods. | [Event dispatchers](#event-dispatchers) |
| Double literal beside a float, `X * 0.5` | Refused: in C++ it is double math, and Blueprint 4.27 has no double. Write `0.5f`. | [Literals and conversions](#literals-and-conversions) |
| `__EmbedFile__("Path")` | A file's bytes, read at build time, as the default of a `TArray<uint8>` member. | [Classes and variables](#classes-and-variables) |
| `__EnumMap__`, `__EnumMapInit__`, `__EnumMapSide__`, `__EnumMapCheck__`, `__EnumMapPair__`, `__EnumMapText__`, `__EnumMapNone__`, `__EnumMapSame__`, `__EnumMapEnum__`, `__EnumMapTake__`, `__EnumMapMember__` | What UE_ENUM_MAP expands to. Write UE_ENUM_MAP. | [Enums](#enums) |
| Event override, `void ReceiveBeginPlay()` | Overrides that event, as adding its node in the editor does. Copy the SDK's parameter list: nothing checks it. | [Overrides and parent calls](#overrides-and-parent-calls) |
| `ExecuteUbergraph_<Class>` | The name of the event graph that holds the methods that wait. A method named so is refused. | [Latent calls](#latent-calls) |
| `FDeref` | The scratch struct that every read and write through a pointer goes through. AssetGen creates it when the mod declares none. | [Pointers and memory](#pointers-and-memory) |
| `FDerefTextView` | The struct that an FText read or write through a pointer imports, from a path the compiler fixes. A helper mod you supply cooks it. | [Pointers and memory](#pointers-and-memory) |
| `final` | On a class or a virtual method: no subclass has a version of its own. The functions are cooked Final, and calls to them are direct and, on `this`, copied in. | [Calling your own functions](#calling-your-own-functions) |
| `FinishComponent(Owner, Component)` | Registers a component that AddComponentDeferred held back. | [Components](#components) |
| `FinishSpawning(Actor, Transform)` | Finishes an actor that SpawnActorDeferred began: its construction script and BeginPlay run. | [Creating objects](#creating-objects) |
| `FKey{"F5"}` | A key for the player controller's key queries, such as IsInputKeyDown. Write it with braces: `FKey("F5")` is refused today. | [Timers and input](#timers-and-input) |
| `FLatentActionInfo` | Leave it out of a latent call. The compiler supplies it. | [Latent calls](#latent-calls) |
| `for (auto &X : Items)`, `for (auto &[Key, Value] : Map)` | Range-for: a TArray by index, a TSet through a copy, a TMap by key and value. | [Loops](#loops) |
| FString methods, `S.Len()` | Not yet: `S.Len()` compiles into a call that cannot work. String functions are UKismetStringLibrary statics, `UKismetStringLibrary::Len(S)`. | [Strings and text](#strings-and-text) |
| `GetClass()`, `GetName()` | Get Class and Get Object Name, on this or any object. | [Working with other objects](#working-with-other-objects) |
| `GetOuter()` | The object's outer, read from memory. Not on a null object, and not in a function that waits. | [Working with other objects](#working-with-other-objects) |
| `GetOutermostTypedOuter<T>(Obj)` | The farthest outer of Obj that is a T, or null. | [Working with other objects](#working-with-other-objects) |
| `GetParmClassName` | The method that `__ClassOf__` calls on the class that uses it. You define it, with exactly two parameters. | [Intrinsics](#intrinsics) |
| `GetSubsystem<T>()` | The same as `T::Get()`. | [Working with other objects](#working-with-other-objects) |
| `GetTypedOuter<T>(Obj)` | The nearest outer of Obj that is a T, or null. | [Working with other objects](#working-with-other-objects) |
| `if (Obj)`, `!Obj` | Is Valid: false for null and for an object that is being destroyed. | [Working with other objects](#working-with-other-objects) |
| `if (X)` on a value that is not a bool | An int32 goes through ToBool (integer), a float, int64 or byte is compared with zero, and an FName is tested against None. | [Statements and control flow](#statements-and-control-flow) |
| `InitCave`, `InitSpacerig` | The class names the game spawns from a mod: InitSpacerig in the Space Rig, InitCave in a mission. | [Running in the game](GUIDE.md#running-in-the-game) |
| `inline` functions and templates | Expanded at each call like an editor macro, with no Blueprint function of its own. A free function or template must be `inline`. | [Inline functions and templates](#inline-functions-and-templates) |
| `(int64)Obj`, `(uint8 *)Obj` | The object's address, as an int64 or as a pointer. | [Pointers and memory](#pointers-and-memory) |
| Lambdas, function pointers, operator overloads | Refused, all three: a Blueprint has no lambda, no function pointer and no operator of a mod's own. Write an inline function, or pass `{this, &C::F}` as a delegate. | [Functions](#functions) |
| LoadAsset, `UKismetSystemLibrary::LoadAsset(Soft)`, `LoadAssetClass(Soft)` | Async Load Asset and Async Load Class Asset: the function waits, and the call's value is what loaded, or null. | [Latent calls](#latent-calls) |
| `Map[Key]` | Read: Find, the value type's default for a missing key. Write: Add. | [Containers](#containers) |
| `__NameCase__` | What UE_NAME_CASE expands to: a hash of the text, for clang. Write UE_NAME_CASE. | [Statements and control flow](#statements-and-control-flow) |
| `__NameIndex__(Name)` | An FName's comparison index, its first 4 bytes, as an int32. | [Intrinsics](#intrinsics) |
| `namespace A::B` | A folder under the mod's package. | [Mod sources and packages](#mod-sources-and-packages) |
| `namespace Game::A::B` | The path /Game/A/B, outside the mod's package. The SDK puts each game Blueprint in the namespace of its folder. | [Mod sources and packages](#mod-sources-and-packages) |
| Namespace-scope variable, `int32 Total = 0;` | One value shared by every class of the source, kept in the default object of a class the compiler generates. | [Global variables](#global-variables) |
| `__NameSwitch__` | What UE_NAME_SWITCH expands to. Write UE_NAME_SWITCH. | [Statements and control flow](#statements-and-control-flow) |
| `NewObject<T>(Outer)` | Construct Object from Class: an object that is neither an actor nor a component. The Outer gives it its world. | [Creating objects](#creating-objects) |
| `[[gnu::noinline]]`, `__attribute__((noinline))` | Calls to the function stay calls: AssetGen never copies its body in. | [Calling your own functions](#calling-your-own-functions) |
| Number conversions, `float F = Count;`, `int32(F)` | Convert as C++ converts, through Blueprint's conversion nodes. | [Literals and conversions](#literals-and-conversions) |
| `Obj == nullptr` | A plain compare with None: an object that is being destroyed is still not null. | [Working with other objects](#working-with-other-objects) |
| `&Obj->Member` | The member's address, found at run time by `ReadProperty::GetPropertyAddress`, which you supply. | [Pointers and memory](#pointers-and-memory) |
| `Obj->Member`, `Obj->Method()` on a null object | No crash: a read gives zero or empty, a call is skipped, and the game logs Accessed None. `GetOuter()` is the exception. | [Working with other objects](#working-with-other-objects) |
| `override` | Refused by clang: the SDK declares engine events non-virtual. Leave it off; the name makes the override. | [Overrides and parent calls](#overrides-and-parent-calls) |
| Pointer to a type that is not a UObject, `int32 *P` | An int64 address. `*P`, `P[I]` and `P->Member` read and write the game's memory, unchecked. | [Pointers and memory](#pointers-and-memory) |
| `#pragma clang optimize off` | The functions up to `#pragma clang optimize on` compile as written, as with UE_NO_OPTIMIZE. | [The optimizer](#the-optimizer) |
| `__PtrCast__<To>(Value)` | Reinterprets between objects, int64 addresses, pointers and references, at compile time. | [Intrinsics](#intrinsics) |
| `public:`, `protected:`, `private:` | The function access in the cooked class. A private variable is left out of the editor API stub. A `class` starts private. | [Classes and variables](#classes-and-variables) |
| pure virtual, `virtual T F() = 0` | An empty function returning the default. A class left with one it has no version of is cooked Abstract, which SpawnActor and CreateWidget refuse. | [Functions](#functions) |
| `__Read32__(Addr)` | Reads an int32 at an int64 address, or at the address a pointer holds. | [Intrinsics](#intrinsics) |
| `__Read64__(Addr)` | Reads an int64 at an address or pointer. | [Intrinsics](#intrinsics) |
| `__ReadByte__(Addr)` | Reads a uint8 at an address or pointer. | [Intrinsics](#intrinsics) |
| `__ReadClass__(Addr)` | Reads 8 bytes at an address or pointer as a `UClass *`. | [Intrinsics](#intrinsics) |
| `__ReadFloat__(Addr)` | Reads a float at an address or pointer. | [Intrinsics](#intrinsics) |
| `__ReadName__(Addr)` | Reads an FName at an address or pointer. | [Intrinsics](#intrinsics) |
| `__ReadObject__(Addr)` | Reads 8 bytes at an address or pointer as a `UObject *`. | [Intrinsics](#intrinsics) |
| `ReadProperty::GetPropertyAddress` | The function that `&Obj->Member` calls. A helper mod you supply exports it. | [Pointers and memory](#pointers-and-memory) |
| `__ReadString__(Addr)` | Copies the FString at an address or pointer. | [Intrinsics](#intrinsics) |
| `__ReadText__(Addr)` | Copies the FText at an address or pointer. Needs the FDerefTextView helper struct. | [Intrinsics](#intrinsics) |
| `__RefAt__(Addr)` | Passes the memory at Addr by reference, for an engine function's wildcard parameter. | [Intrinsics](#intrinsics) |
| Respelled SDK member, `Name_0`, `Index_0` | Write the SDK spelling. Everything AssetGen cooks uses the real name. | [Classes and variables](#classes-and-variables) |
| `Comp->SetupAttachment(Parent)` | In UE_DEFAULTS, places a component under another, its own class's or an inherited one, as a constructor does. In a function, attaches at once, keeping the relative transform. | [Components](#components) |
| `sizeof`, `alignof` | The game's layout, not clang's view of the SDK's stand-in types. | [Pointers and memory](#pointers-and-memory) |
| `SpawnActor<T>(Class, Transform)` | Spawn Actor from Class. The new actor's construction script and BeginPlay run inside the call. | [Creating objects](#creating-objects) |
| `SpawnActorDeferred<T>(Class, Transform)` | Spawn Actor with Expose on Spawn pins: set its variables, then call FinishSpawning. | [Creating objects](#creating-objects) |
| `static inline const` and `static constexpr` members | An inline class variable: no property and no default, each use is its initializer. A static that is not `const` is refused. | [Constants](#constants) |
| `static` local | In a function that waits, kept in the object's frame and initialized on that object's first call. Refused in a function that never waits and in an inline helper; a `static constexpr` local works anywhere. | [Latent calls](#latent-calls) |
| `static` member function | Runs on the class default object. The statics of a function library can be called from other mods. | [Functions](#functions) |
| `static_cast<T *>(Obj)` | No node: the reference passes through unchecked. Use `Cast<T>` when the object may not be a T. | [Types](#types) |
| String `+`, `FString("Kills: ") + Count` | Append: each operand becomes a string, and the result converts to the destination type. | [Strings and text](#strings-and-text) |
| String conversions, `FString S = Count;`, `int(Label)` | Numbers, names, texts and objects convert to strings implicitly. Text parses to a number only with an explicit cast. | [Strings and text](#strings-and-text) |
| `<Subsystem>::Get()` | A subsystem, as the editor's Get node gives it. `Get(Other)` asks Other's world. | [Working with other objects](#working-with-other-objects) |
| `Super::Method()` | Not a name C++ knows. Write the parent class: `Base::Method()`. | [Overrides and parent calls](#overrides-and-parent-calls) |
| `T &` parameter | A pass-by-reference pin: the callee's writes reach the caller. Bound to a map element or to `C ? X : Y`, it gets a copy stored back after the call, with a warning. | [Functions](#functions) |
| `TDelegate<...>`, `{this, &C::F}` | A method of `this` passed as a delegate argument (Create Event), such as to a timer, or kept in a delegate variable. | [Event dispatchers](#event-dispatchers), [Timers and input](#timers-and-input) |
| `TScriptInterface<I>` | An interface value. Assigning an object casts it, and `GetObject()` gives the object back. | [Interfaces](#interfaces) |
| `(T *)Soft` | Resolve Soft Reference: the object or class, null unless it is loaded. | [Types](#types) |
| `TSoftObjectPtr<T>`, `TSoftClassPtr<T>` | Soft Object and Soft Class References. A default is the asset's path. | [Types](#types) |
| `T::StaticClass()` | The class as a constant. On a mod class, that class. Write it unqualified: `Ns::T::StaticClass()` names the class it inherits StaticClass from, with no message. | [Types](#types), [Creating objects](#creating-objects) |
| `TSubclassOf<T>`, `UClass *` | Class References. `TSubclassOf<T>` takes T and its subclasses. | [Types](#types) |
| `UE_ASSET_ALL(Class)` | Declares `All`: soft pointers to every UE_ASSET_AT of Class or a subclass in this namespace and the ones inside it, filled in at build time. | [Game assets](#game-assets) |
| `UE_ASSET_AT(Class, Name, Path)` | Names an asset that another package holds, the game's or another mod's, so that `&Name` points at it. | [Game assets](#game-assets) |
| `UE_AUTHORITY_ONLY` | Authority Only (BlueprintAuthorityOnly): on an actor or its component, a call on a machine without authority is skipped. An engine or game static with this mark is not skipped. | [RPCs](#rpcs) |
| `UE_AWAIT(Dispatcher)` | The rest of the function runs when the dispatcher next fires. Its value is the dispatcher's parameter when it has exactly one. | [Waiting on events](#waiting-on-events) |
| `UE_CATEGORY("Setup\|Doors")` | The editor category of the functions and variables that follow it, `\|` starting a subcategory. It is written into the editor API stub only. | [Classes and variables](#classes-and-variables) |
| `UE_CATEGORY__JOIN`, `UE_CATEGORY__JOIN2` | Internal to UE_CATEGORY. Not written in a mod. | [Classes and variables](#classes-and-variables) |
| `UE_CLASS(Package, UeName)` | The package and UE name of a class. In a shared header it names the one source that cooks the class, the one whose UE_MOD_PACKAGE, namespaces and class name give exactly that path; every other source imports it. The SDK declares each engine and game class with it. | [Mod sources and packages](#mod-sources-and-packages) |
| `UE_CLIENT` | Run on owning Client RPC: called on the server, it runs on the client that owns the actor. | [RPCs](#rpcs) |
| `UE_COMPONENT(Type, Name)` | A component of a mod actor, of an engine or game component class, as added in the Components panel. The first scene component is the root. | [Components](#components) |
| `UE_COSMETIC` | Cosmetic (BlueprintCosmetic): on an actor or its component, a call on a dedicated server is skipped. | [RPCs](#rpcs) |
| `UE_DEFAULTS { ... }` | Defaults of components and of inherited properties, as the Details panel and Class Defaults set them. Read at build time, never run. | [Class defaults](#class-defaults) |
| `UE_DISPATCHER(Name, Params...)` | An event dispatcher on a mod class, with its parameter list. | [Event dispatchers](#event-dispatchers) |
| `UE_ENUM(Enum)` | Cooks an `enum class` based on `uint8`, `int32` or `int64` as a UserDefinedEnum (an Enumeration asset). | [Enums](#enums) |
| `UE_ENUM_IN(Enum, Package)` | UE_ENUM for an enum in a shared header: only the source whose UE_MOD_PACKAGE is exactly Package cooks it. | [Enums](#enums) |
| `UE_ENUM_MAP(Key, Value, Name)`, `UE_ENUM_MAP(Enum)` | A TMap member from each enumerator to its name, or back, filled in at build time: declared by the three-argument form, the default alone by the one-argument form. | [Enums](#enums) |
| `UE_FINAL_AS(Base, Leaf)` | Declares `class Leaf final : public Base {}`, Base's one subclass: Base is compiled as final and cooked Abstract. | [Classes and variables](#classes-and-variables) |
| `UE_INTERFACE` | Declares a mod interface, cooked as a Blueprint Interface asset. Its variables go to the classes that implement it. | [Interfaces](#interfaces) |
| `UE_MOD_PACKAGE(Path)` | The /Game folder that a source's classes, structs, enums, interfaces and assets are cooked into. A namespace is a subfolder. | [Mod sources and packages](#mod-sources-and-packages) |
| `UE_MULTICAST` | Multicast RPC: called on the server, it runs on the server and on every client. | [RPCs](#rpcs) |
| `UE_NAME_CASE("Text")` | A case of UE_NAME_SWITCH. Every case of one must be written this way. | [Statements and control flow](#statements-and-control-flow) |
| `UE_NAME_SWITCH(Name)` | `switch` on an FName, the editor's Switch on Name. | [Statements and control flow](#statements-and-control-flow) |
| `UE_NO_OPTIMIZE` | Compiles the function as written: unused pure calls, unread locals and constant branches stay. | [The optimizer](#the-optimizer) |
| `UE_PURE` | A pure function (BlueprintPure), drawn without exec pins. It must return a value. | [Functions](#functions) |
| `UE_RELIABLE` | Makes a UE_SERVER, UE_CLIENT or UE_MULTICAST RPC reliable. Refused on a method that is not an RPC. | [RPCs](#rpcs) |
| `UE_REPLICATED(Type, Name)` | A replicated variable. The initializer after the macro is its default, and the class gets bReplicates. | [Replication](#replication) |
| `UE_REPLICATED_IF(Type, Name, Cond)` | A replicated variable with a replication condition, an ELifetimeCondition name with or without `COND_`. | [Replication](#replication) |
| `UE_REPLICATED_USING(Type, Name, OnRep)` | A RepNotify variable. OnRep runs when a new value arrives, and right after the mod's own writes, as the Set w/ Notify node does. | [Replication](#replication) |
| `UE_REPLICATED_USING_IF(Type, Name, OnRep, Cond)` | RepNotify and a replication condition on one variable. | [Replication](#replication) |
| `UE_SERVER` | Run on Server RPC: called on the owning client, it runs on the server. | [RPCs](#rpcs) |
| `UE_STRUCT` | Cooks a struct as a UserDefinedStruct (a Structure asset). | [Structs](#structs) |
| `UE_CLASS_IN(ModPackage)` | UE_CLASS for a mod's own class, named by its owner's UE_MOD_PACKAGE: the package is ModPackage plus namespaces plus the class name, the UE name the class name plus `_C`. | [Mod sources and packages](#mod-sources-and-packages) |
| `UE_STRUCT_IN(Package)` | UE_STRUCT for a struct in a shared header: only the source whose UE_MOD_PACKAGE is exactly Package cooks it. | [Structs](#structs) |
| `UeAssets::<Class>::All` | Every game asset of that class, as soft pointers. | [Game assets](#game-assets) |
| `UeAssets::<Class>::Game::...::<Name>` | A game asset by its content path, for `&` to point at. | [Game assets](#game-assets) |
| `TEnum<E>` | An enum that is E to the compiler, with `.Name()` and `.String()`. | [TEnum](#tenum) |
| `using FTarget = AActor;` | An alias of a class. A variable of it is still an object reference. | [Classes and variables](#classes-and-variables) |
| `virtual` | Accepted and ignored: every mod method is called by name, so the most derived one runs. With `final`, see `final`; with `= 0`, see pure virtual. | [Functions](#functions) |
| `WorldContextObject` argument left out | Filled with self, as the editor's hidden pin is; in a static function, with its own world context parameter. An inline helper's own WorldContext parameter left at its default gets the same. | [Calling engine and game functions](#calling-engine-and-game-functions) |

## Diagnostics

When AssetGen refuses a source, it prints one line, and `assetgen compile` exits with code 1:

```text
  FAILED: <message>
```

A refusal raised inside a function starts with `<Class>::<Function>: `. One raised inside an inline function's body
adds `inline <Class>::<Method>: ` after that, or `inline <Function>: ` for a free inline function, so the prefix names
the function the problem is in. A refusal about a class's variables, or about the source as a whole, has no prefix.
The compile stops at the first refusal: fix it and compile again to see the next one.

A warning does not stop the compile:

```text
  warning: <Class>::<Function>: <message>
```

A warning means the code compiled to its nearest Blueprint equivalent, which behaves differently from C++ in the way
the message says. Read it and decide whether the difference matters to your mod.

clang reads the source before AssetGen does. Its own errors and warnings come first, in clang's format, and a clang
error ends the compile with `FAILED: clang rejected <Source> (diagnostics above)`. The lists below include the clang
errors that mod code meets most often.

A message that starts with `internal:` or `internal error:` is a bug in AssetGen, not in your mod. Report it with the
bug form. [Internal errors](#internal-errors) says what to include, and lists the compiler checks that fail without
that prefix.

Each entry gives the message, with the parts that change written as `<Name>`, what causes it, what to write instead,
and where the feature is described. In each group, the messages you are most likely to meet come first.

### Mod sources, classes and global variables

- `the source declares no UE_MOD_PACKAGE, so its classes have no /Game path`: the source and the headers it includes
  have no `UE_MOD_PACKAGE("/Game/...");` line. Fix: add `UE_MOD_PACKAGE("/Game/_MyMods/<ModName>");` after the
  includes. Namespaces then become folders under that path. See [Mod sources and packages](#mod-sources-and-packages).
- `the source declares no UE_STRUCT, UE_ENUM or class deriving from a UE class`: nothing in the source would be cooked,
  for example a file that holds only inline helper functions. Fix: declare a class that derives from a UE class, a
  UE_STRUCT or a UE_ENUM in it. A file of shared helpers belongs in a header that the mod's sources include, not in the
  mod's `sources` list in `mods.yaml`. See [Mod sources and packages](#mod-sources-and-packages).
- `` <Member>: a default is a value known when the mod is built - a literal, a constant expression over literals, enum
  constants and constexpr variables, a braced struct, or an &Asset. A function call counts only through
  `constexpr T k = F();` with F consteval, which clang runs itself. Anything computed when the game runs belongs in
  ReceiveBeginPlay or UserConstructionScript ``: a member's initializer that AssetGen cannot work out when it builds the
  mod: a call such as `int32 Roll = UKismetMathLibrary::RandomInteger(3);`, a call to a plain `constexpr` function, or
  `sizeof`. A value in UE_DEFAULTS gets the same message. Not yet: a class as the default of a `TSubclassOf` or
  `UClass*` member, `TSubclassOf<AActor> Kind = AActor::StaticClass();`, is refused with this message too. Fix: use a
  literal or a constant expression, compute a constant with a `consteval` function through a `constexpr` variable, or
  set the value in ReceiveBeginPlay or UserConstructionScript. A class reference member starts null: set it at run
  time, or use a `TSoftClassPtr` member, which takes a path as its default. See
  [Classes and variables](#classes-and-variables).
- `` <Member>: a container default is a braced list, `= { 1, 2 }`, of values a lone member could take ``: a TArray,
  TSet or TMap member initialized with something other than braces, such as a call, `TArray<int32> Ids = MakeIds();`.
  Fix: list the elements in braces, `TArray<int32> Ids = { 1, 2 };`, each a value that a single variable of the element
  type could take as its default, or fill the container in ReceiveBeginPlay. See
  [Classes and variables](#classes-and-variables).
- `<Name> is an asset, not a value: point at it with &<Name>`: a function uses a namespace-scope variable of a UObject
  class by value. That is a game asset named with UE_ASSET_AT, a braced data asset of the mod, or a variable such as
  `UMoodDef GlobalDef;`. For example, with
  `UE_ASSET_AT(UEnemyDescriptor, ED_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");`, the read
  `ED_Grunt.SpawnSpread` is refused. Reading a game asset's values when the mod is built is Not yet, and is refused
  with this message. Fix: take the address, `(&ED_Grunt)->SpawnSpread`, or keep `&ED_Grunt` in a
  pointer member. Instead of a variable of your own, declare a braced data asset or name an existing asset with
  UE_ASSET_AT. See [Global variables](#global-variables), [Data assets](#data-assets) and [Game assets](#game-assets).
- `<Class> and <Other> would both be cooked as <Package>`: two classes land on one package. Package names are compared
  without regard to case, so `Lamp` and `lamp` collide. So do a class `Lamp` and `Game::_MyMods::Tools::Lamp` in a mod
  whose package is `/Game/_MyMods/Tools`: both are `/Game/_MyMods/Tools/Lamp`. Fix: rename one, or move it to another
  namespace. A namespace is the asset's folder. See [Mod sources and packages](#mod-sources-and-packages).
- `UE_CLASS on <Class> names the package "<Package>", which does not end in the asset that declares <UeName>. Did you
  mean "<Package>/<Asset>"?`: a UE_CLASS names the class's folder instead of its asset, as in
  `UE_CLASS("/Game/_MyMods/Core", "CoreLib_C");`. That package does not exist, so in game a call into the class would
  find no function. The check runs on every class, those in included headers too, and compares only the last part of
  the path with the name. A folder that has the class's own name, `/Game/_MyMods/CoreLib` for `CoreLib_C`, is not
  caught. Fix: use the path the message suggests, `UE_CLASS("/Game/_MyMods/Core/CoreLib", "CoreLib_C");`: the package
  is `<folder>/<Asset>` and the name is `<Asset>_C`. See [Mod sources and packages](#mod-sources-and-packages).
- `UE_CLASS on <Class> says "<UeName>", but cooking it here requires "<Leaf>_C"`: this mod cooks the class, because
  its UE_CLASS package is the path this mod gives it, and the name in UE_CLASS is not the class's name plus `_C`. For
  example `UE_CLASS("/Game/_MyMods/Core/CoreLib", "CoreLib");` compiled by the mod whose package is
  `/Game/_MyMods/Core`. Fix: `UE_CLASS("/Game/_MyMods/Core/CoreLib", "CoreLib_C");`. See
  [Mod sources and packages](#mod-sources-and-packages).
- `<Asset> at <Path> is a <Class>, which this mod cooks its own copy of, so it would load as null: name the class's
  owner where it is declared, e.g. UE_CLASS("<Folder>/<Class>", "<Class>_C")`: the source uses another mod's asset
  through UE_ASSET_AT, and the asset's class comes from a shared header without UE_CLASS. This mod would then cook its
  own copy of the class, and the other mod's asset is not an instance of that copy. Only an asset the source uses is
  checked. Fix: add the UE_CLASS the message prints to the class's declaration in the shared header. Every mod but the
  owner then imports the class instead of cooking it. See [Mod sources and packages](#mod-sources-and-packages) and
  [Game assets](#game-assets).
- `<Name> is declared extern but not defined in this source; a mod's global is defined where it is used`: a function
  uses a namespace-scope variable that this source only declares, `extern int32 Shared;`, with the definition in
  another source. Each source is compiled on its own and cooks the holder of every global it uses. Fix: define the
  variable, with the same initializer, in each source that uses it. Sources with the same UE_MOD_PACKAGE then write the
  same holder and share one variable. A `const` or `constexpr` global needs no holder. See
  [Global variables](#global-variables).
- `<Name>: UE_ASSET_ALL's All is a TArray<TSoftObjectPtr<a class>>`: the element class of a UE_ASSET_ALL `All` is not
  a class AssetGen has a declaration for, for example one the included headers only forward-declare. Fix: include the
  header that declares the class, and let `UE_ASSET_ALL(<Class>)` declare `All`. See [Game assets](#game-assets).

### Types and conversions

- `TODO: unimplemented <Where>: <Type>`: a variable, parameter, return value or container element of a type that a
  Blueprint variable cannot have. `<Where>` says which, such as `property Mask`, `local D`, `parameter X on F`,
  `return type on F` or `member A`. The usual types are `double`, `int8`, `int16`, `uint16`, `uint32` and `uint64`; a
  mod enum without UE_ENUM; a class held by value in a UE_STRUCT; a lambda or a function pointer in a local; the
  `std::initializer_list` that `auto L = { 1, 2, 3 };` makes; and a local whose type is a `using` alias declared
  inside the function. Fix: use float, int32, int64, bool, uint8, FString, FName, FText, a uint8, int32 or int64 enum,
  an engine struct or UE_STRUCT, an object pointer, a TSubclassOf, TSoftObjectPtr, TSoftClassPtr or TScriptInterface
  of a declared class, or a TArray, TSet or TMap of these. Add UE_ENUM to a mod enum, hold objects through pointers,
  name the container type of a braced list, and spell out an alias's type. See [Types](#types).
- `no Kismet conversion from <From> to <To>`: C++ converts between two types and no Blueprint conversion node does.
  The common cases:
  - `from float to double`: an unsuffixed literal in arithmetic or a comparison, `X * 0.5` or `X > 0.5`, which C++
    makes a double operation. Write `0.5f`.
  - `from float to uint8`: a float cast straight to a byte, `(uint8)X`. Truncate to int32 first, `(uint8)(int32)X`.
  - `from int to uint32`: a cast to `uint32`, as in `(uint32)X >> 1`, or a write through a `uint32 *`, `*P = 5;`.
    Blueprint has no unsigned 32-bit integer, so there is no unsigned shift. Stay in int32 or int64, and write through
    `*(int32 *)P`.
  - `from int to <Struct>`: a struct built with a one-argument constructor in parentheses, which C++ reads as a
    conversion. A UE_STRUCT needs no constructor: drop it and write designated braces, `FPair P = { .A = 3 };`.

  See [Literals and conversions](#literals-and-conversions).
- `<Where>: <Class> is only forward-declared here - #include "UeApi/Game/<Class>.h"`: a variable, parameter or return
  value names a game Blueprint class, a name that ends in `_C`, that the included headers only forward-declare, as in
  `class BP_Foo_C *Target;`. Fix: add the `#include` the message names. See [Types](#types).
- `TODO: unimplemented <Where>: <Type> (a uint8, int32 or int64 enum only)`: a variable of a game enum wider than a
  byte, uint16 or uint32, such as ECreatureSize. A Blueprint enum variable holds a uint8, an int32 or an int64. Fix: a
  mod cannot hold such an enum in a variable, so leave the member or parameter out. See [Enums](#enums).
- `warning: <Class>::<Function>: int64 -> float goes through int32 (UE 4.27 has no int64 -> float), so a value outside
  int32's range wraps`: an int64 converted to float, `(float)X`. UE 4.27 Blueprint has no such conversion, so the value
  goes through int32 and only its low 32 bits survive: 2^32 + 5 becomes 5.0, where C++ would round the whole value.
  Fix: nothing, when the value fits in an int32. Otherwise bring it into that range before converting. See
  [Literals and conversions](#literals-and-conversions).
- `no template named 'TWeakObjectPtr'; did you mean 'TSoftObjectPtr'?`: clang's message. The SDK declares no weak or
  lazy object pointer. Fix: use an object pointer, tested with `if (Obj)`, or a soft reference. See [Types](#types).
- `no conversion from <From> to <To>`: a conversion to `TScriptInterface<I>` where AssetGen has no declaration of `I`,
  for example an interface the headers only forward-declare, or from a value that is neither an object nor another
  interface. Fix: include the header that defines the interface, and convert from an object pointer or another
  `TScriptInterface`. `nullptr` makes a null interface. See [Interfaces](#interfaces).
- `a branch's value: <Reason>`: rare. The value of `C ? A : B`, `&&` or `||` goes into a hidden variable, and its type
  cannot be a Blueprint variable. `<Reason>` is the type refusal. Fix: follow `<Reason>`, or use an if/else that
  assigns each branch. See [Operators](#operators).

### Constants, enums, structs and containers

- `<Where>: a TSet element of type <T> cannot hash, and the engine hashes each one` (or `a TMap key`): a bool, an
  FText, a delegate, or an engine struct without GetTypeHash (FRotator, FHitResult, FTransform) as a set element or
  map key. Fix: use a type that hashes, such as an int, an FName or FVector, or a `UE_STRUCT` holding the value.
- `a container operation needs a variable, not a computed value: <Method>`,
  `` `[]` on a map needs a map variable, not a computed value `` and
  `indexing needs an array variable, not a computed value`: a container method, a map's `[]` or an array index on a
  container that a function call returns, as in `GetItems().Num()` or `GetItems()[0]`. `<Method>` is the Blueprint
  node's name, so `Num` shows as `Length`. A braced list, an inline class variable and an inline function's result
  already count as variables. Fix: store the container in a local first, `TArray<int32> L = GetItems(); L.Num();`.
  After changing a copy of a map, store it back where it came from. See [Containers](#containers).
- `` static <Name>: a Blueprint class has no static storage, so a static variable is inline, each use its initializer;
  declare it `static inline const` (or `static constexpr`), or make it a plain member to keep a value ``: a class's
  static variable that is not const is read or written, as with `static inline float Loose = 0.25f;`. The static is
  refused where it is used, not where it is declared. Fix: `static inline const float Loose = 0.25f;`, or
  `static constexpr`, when nothing changes it. Make it a plain member when it must keep a value. See
  [Constants](#constants).
- `<Name> is inline: each use is its initializer, and there is no variable to write`: a write to an inline class
  constant whose type starts with `const` but still lets clang assign it, such as a pointer to const:
  `static inline const AActor *Keep = nullptr;` and then `Keep = this;`. Fix: make it a plain member if it has to
  change. See [Constants](#constants).
- `UE_ENUM(<Enum>) names no enum with enumerators`: UE_ENUM names an enum that AssetGen did not find at namespace
  scope in the same namespace, or one with no enumerators. clang accepts any name there, so a typo,
  `UE_ENUM(EMod);` for `EMood`, reaches this message. Fix: write `UE_ENUM(EMood);` after the enum, at namespace scope
  in the enum's namespace, and give the enum at least one enumerator. See [Enums](#enums).
- `` UE_ENUM(<Enum>): declare it `: uint8` (a Blueprint enum), `: int32` or `: int64` ``: the enum has another
  underlying type, as in `enum class EBad : int16`, or none. The check runs even when no variable uses the enum. Fix:
  `: uint8` for an ordinary Blueprint enum, or `: int32` or `: int64`. See [Enums](#enums).
- `enum constant with no value: <Name>`: an enumerator of an enum declared inside a class, such as
  `enum class EKind : uint8 { A, B };` as a class member. AssetGen records enums declared at namespace scope. Fix:
  declare the enum at namespace scope. See [Enums](#enums).
- `UE_ENUM(<Enum>): <Enumerator> is out of range (the largest value is _MAX's)`: the cooked enum adds `<Enum>_MAX`
  one past the largest value, and that would not fit, as with `A = 255` in a uint8 enum. Fix: keep every value at most
  254 in a uint8 enum, and one below the type's maximum in an int32 or int64 enum. See [Enums](#enums).
- `UE_ENUM(<Enum>): <Package> already has an enum <Name>, whose enumerator names (<Name>::...) the engine keeps in one
  global table; rename it`: a mod enum named like a game or engine enum. Fix: rename it. See [Enums](#enums).
- `UE_ENUM(<Enum>): <Enum>_MAX is the sentinel the engine adds, one past the largest value; leave it out or give it
  that value`: the enum declares its own `_MAX` with another value, as in `{ A, B, EGear_MAX = 7 }`. Fix: drop the
  explicit value, or the enumerator. See [Enums](#enums).
- `<Class>::<Function>: TODO: unimplemented intrinsic __EnumMap__`: Not yet. UE_ENUM_MAP in a function body is
  refused. It works only as a member default. Fix: keep the table in a member,
  `UE_ENUM_MAP(EMood, FName, Names);`, and read the member. See [Enums](#enums).
- `static assertion failed due to requirement '__EnumMapPair__<<Key>, <Value>>': UE_ENUM_MAP(Key, Value, Name): one of
  Key and Value is an enum and the other FName or FString; for a TEnum<E>, write E` (clang, pointing into UeMeta.h,
  with a note at your line): the three-argument form over two types that are not an enum and FName or FString, such
  as `UE_ENUM_MAP(int32, FName, M);` or `UE_ENUM_MAP(TEnum<EMood>, FName, M);`. Fix: name the enum itself on one side
  and FName or FString on the other. See [Enums](#enums).
- `static assertion failed: UE_ENUM_MAP takes (Enum), or (Key, Value, Name) to declare the member` (clang): UE_ENUM_MAP
  with two arguments, or none, or more than three, as a member, `UE_ENUM_MAP(FName, EMood);`. As an initializer,
  `= UE_ENUM_MAP(FName, EMood)`, clang's own "expected expression" comes instead. Fix: `UE_ENUM_MAP(FName, EMood,
  Moods);` declares the member; `TMap<FName, EMood> Moods = UE_ENUM_MAP(EMood);` gives one you declare its default.
  See [Enums](#enums).
- `no viable conversion from '__EnumMapInit__<<Enum>>' to 'TMap<...>'` (clang): the one-argument form as the default
  of a map that is not between that enum and FName or FString. Fix: declare the map over the enum, or use the
  three-argument form. See [Enums](#enums).
- `<Name> is static: name it without the object in front, which would never be evaluated`: an inline class constant
  read through an object that a call computes, `Me()->kHold`. The constant is its initializer, so the call in front
  would never run. `this->kHold` and a variable in front are accepted. Fix: `kHold` or `<Class>::kHold`. See
  [Constants](#constants).
- `invalid operands to binary expression`, `no viable overloaded '+='` and
  `invalid argument type 'FVector' to unary expression`: clang's messages, for an operator the SDK does not declare
  because Blueprint has no node for it: `<` or `>` on strings, `==` on a UE_STRUCT or on containers, `+=` on a struct,
  unary `-` on a struct. Fix: compare strings for equality only; compare the members of a UE_STRUCT that matter; use
  `A.Identical(B)` for containers; write `Home = Home + W;`; call `UKismetMathLibrary::NegateVector(V)`. See
  [Strings and text](#strings-and-text), [Structs](#structs) and [Containers](#containers).
- `` <Struct> has fields AssetGen cannot write, so it takes no whole-struct literal:
  `<Struct> V = { .Field = value };` ``: a constructor call with arguments on an engine struct that has members a
  whole-struct literal cannot hold, such as weak pointers, delegates or bitfields. The SDK gives such structs no
  constructor that takes every member, so clang usually refuses the call first. Fix: designated braces, as the message
  shows, `FHitResult H = { .Time = 0.5f };`. Members you leave out keep the struct's defaults. See [Structs](#structs).
- `<Struct> literal must give every field (<N>)`, `<Struct> takes one value per member (<N>), in declaration order:
  <Member>` and `<Type> has no member for value <N>`: rare. A struct value is built with a constructor call or braces
  that give a different number of values than the struct has members. Fix: designated braces that name the members you
  set, `FPair P = { .A = 3 };`. The members you leave out keep their defaults. See [Structs](#structs).
- `` a map's element is a braced `{ key, value }` pair `` and `a TMap default is a list of { key, value } pairs:
  <Member>`: an entry of a TMap braced list, in a function or as a member default, is not itself a `{ key, value }`
  pair. Fix: write each entry as a nested pair, `TMap<FName, int32> Cost = { { "Gold", 5 }, { "Iron", 2 } };`. See
  [Containers](#containers).
- `a braced list makes a TArray, TSet or TMap here, not a <Type>`, `unknown struct type in an initializer: <Type>`,
  `<Name> is inline, so it needs its initializer where it is declared`
  and `UE_ENUM_MAP: <Member> is not a map over an enum`: checks behind clang's and AssetGen's earlier ones, which a
  source rarely reaches. Fix: name the container type of a braced list, `TArray<int32> L = { 1, 2, 3 };`; include the
  header that declares a struct; give an inline constant its value where it is declared; use
  UE_ENUM_MAP only on a `TMap<E, FName>`, `TMap<FName, E>`, `TMap<E, FString>` or `TMap<FString, E>`. See
  [Containers](#containers), [Structs](#structs), [Constants](#constants) and [Enums](#enums).

### Operators, statements and loops

- `bit shift with a non-constant amount (Kismet has no shift op; use explicit multiply/divide)`: Blueprint has no shift
  node. AssetGen turns a shift into a multiply or divide only when the amount is an integer literal written in place.
  A variable, `X <<= N`, a `constexpr` or `const` constant, an enumerator, a cast, `sizeof` and a parenthesized `(2)`
  all get this message, though C++ treats several of them as constants. A macro that expands to a literal works. A
  shift whose two sides are both constants is worked out first, so `1 << kShift` works. Fix: write the literal,
  `X << 3`, or multiply or divide by a power of two. See [Operators](#operators).
- `bit shift amount out of range: <N>`: a literal amount at or past the width of the left side: more than 31 on an
  int32, more than 63 on an int64, as in `X << 32` with an int32 `X`. The width comes from the left side only. Fix: keep
  the amount in 0..31 for an int32 and 0..63 for an int64, and widen the left side first for a larger shift,
  `(int64)X << 40`. See [Operators](#operators).
- `TODO: unimplemented binary operator <Op> on <Flavour>`: an operator with no Kismet node for its operand types.
  `<Flavour>` names them: IntInt, Int64Int64, FloatFloat, ByteByte, BoolBool or ObjectObject. The usual cases:
  - `% on Int64Int64`: `%` or `%=` on int64, which UE 4.27 lacks. Write `A - A / B * B`.
  - `< on ObjectObject` (or `>`, `<=`, `>=`): ordering object pointers. Blueprint has only `==` and `!=` on objects.

  See [Operators](#operators).
- `TODO: unimplemented operator overload <Operator> yielding <Type>`, and `TODO: unimplemented operator overload
  <Operator>` for a statement: an overloaded operator with no Kismet node. As a value, this is an operator a mod
  declares for its own struct, `A + B` with `inline FPair operator+(FPair L, FPair R)`, or a lambda called in place
  (`operator()`). As a statement, it is a `[]` whose value is dropped, `Items[0];`. Fix: call a named inline function
  instead of your operator, and use the value of `[]` or drop the statement. See [Operators](#operators).
- `TODO: unimplemented statement <Kind>`: a statement with no Blueprint form. `<Kind>` is clang's name for it:
  - `CXXTryStmt`: try and catch. Blueprint has no exceptions.
  - `ConditionalOperator`: `C ? Open() : Close();`. Write the if/else out.
  - `BinaryOperator`: `bReady && Launch();` or another unused expression. Write the if out.
  - `DeclRefExpr`: `(void)Unused;`. Leave an unused parameter unnamed instead. `(void)SomeCall();` compiles as the call.
  - `IntegerLiteral`: a GNU case range, `case 1 ... 3:`. Write one `case` per value; labels in a row share a body.
  - `CaseStmt`: a case label inside a nested block of the switch. Put every label directly in the switch body.

  See [Statements and control flow](#statements-and-control-flow).
- `TODO: assignment to <Kind>, not a property or local`: the left side of `=` is not a variable, member or element,
  such as `(C ? Left : Right) = 5;` (`ConditionalOperator`) or a call that returns a reference, `Ref() = 1;`
  (`CXXMemberCallExpr`). Fix: assign to the place itself, `if (C) Left = 5; else Right = 5;`. See
  [Operators](#operators).
- `a declaration statement declares nothing usable`: a declaration inside a function that declares no variable and no
  type alias, such as a local struct, `struct FLocal { int32 A; };`, or a structured binding, `auto [A, B] = Point;`.
  Fix: define types at namespace scope, and read a struct's members one by one, `int32 A = Point.X;`. A structured
  binding works only as a TMap range-for's `auto [Key, Value]`. See [Locals](#locals).
- `` `switch` needs a braced body ``: a switch whose body is one statement, `switch (N) case 1: Fire();`. Fix:
  `switch (N) { case 1: Fire(); }`. See [Statements and control flow](#statements-and-control-flow).
- `a case of UE_NAME_SWITCH needs UE_NAME_CASE("Text")`: a case in `switch (UE_NAME_SWITCH(N))` written without
  UE_NAME_CASE and a string literal, such as `case 3:`. Fix: `case UE_NAME_CASE("IntProperty"):`. See
  [Statements and control flow](#statements-and-control-flow).
- `TODO: range-for over <Type> (TArray, TSet and TMap only)`: a range-for over something other than a TArray, TSet or
  TMap, such as a braced list, `for (int32 X : { 1, 2, 3 })` (`std::initializer_list<int>`). Fix: loop over a TArray,
  `TArray<int32> L = { 1, 2, 3 };`, or over a `static inline const` TArray. See [Loops](#loops).
- `range-for needs a container variable, not a computed value`: the range is a container that a function returns,
  `for (int32 X : GetItems())`. An inline function's result, and a property reached through a call,
  `PickPeer()->Items`, work. Fix: `TArray<int32> Items = GetItems();` and then `for (int32 X : Items)`. See
  [Loops](#loops).
- `` a TMap range-for binds `auto [Key, Value]` ``: a TMap loop with a plain loop variable, `for (auto &Pair : Scores)`.
  Fix: bind `auto [Key, Value]` for copies, `auto& [Key, Value]` to walk the map's own slots with Value writable, or
  `const auto& [Key, Value]`. See [Loops](#loops).
- `TODO: a structured binding over a <Type>`: a structured binding over a TArray or TSet,
  `for (auto [X, Y] : Points)`. Fix: bind the element and read its members, `for (const FIntPoint &P : Points)` and
  `P.X`. See [Loops](#loops).
- `` a goto inside a TMap range-for that writes its value back would skip the write: bind `const auto& [Key, Value]`,
  or leave with break ``: a goto inside `for (auto& [Key, Value] : Map)` in the form that copies Value out and writes
  it back after each pass. AssetGen uses that form when Value is a container the body changes, and the body also adds
  to, removes from or sorts a map of that type. Fix: bind `const auto&` when the body only reads Value, or leave with
  `break`, which runs the write-back, and jump from after the loop. See [Loops](#loops).
- `` `~` on a float ``, `TODO: unimplemented unary operator <Op>`, `` a `case` needs one constant value `` and
  `TODO: assignment to an operator call that is not an array element`: checks behind clang's own, which a source
  rarely reaches: `~` on a float, a unary operator outside the standard set (`&`, `*`, `+`, `-`, `~`, `!`, `++`,
  `--`), a case whose value is not one constant, and `=` onto an overloaded operator other than `[]`. Fix: use `~` on
  integers, the standard operators, one constant per case, and assign to a variable, member or element. See
  [Operators](#operators).

### Functions and inline functions

- `<Class>::<Function> is <Type>, and the <Parent>::<Function> it replaces is <Type>: callers pass that one's
  parameters; declare the same`: an override, or an interface function's implementation, with other parameter
  types than the function it replaces. Names do not count, and `const T&` matches `T`. Fix: copy the declaration. See
  [Overrides and parent calls](#overrides-and-parent-calls).
- `<Owner>::<Method>, which <Owner> declares and never defines, is no function of the class, and C++ would not link a
  call to it`: a call to a method of a class this source cooks that has no body and is not `= 0`. By name the call
  would run a subclass's function of that name, whatever its parameters, and on an object of the class find none,
  which is fatal. Fix: give the method a body, or `= 0` for an empty one subclasses override.
- `<Class>::<Function>: <Parent>::<Function> is native and no Blueprint event, so no function replaces it`: a method
  named like an engine function that is not an event, such as `K2_DestroyActor`. C++ and calls bound to it keep
  running the engine's. Fix: rename the method.
- `<Class>::<Name>: a second function of that name; a Blueprint class has one member per name, so rename one`: two
  non-inline overloads, or two members of one name. Fix: rename one, or make the extra overloads `inline`. See
  [Overloading](#overloading).
- `<Class>::<Name>: differs from <Other> only in case, and an FName ignores case; rename one`: `Get` and `get` in one
  class. Fix: rename one. See [Overloading](#overloading).
- `<Class>::<Name>: None is UE's empty name, in any case: the Blueprint editor refuses it, and the members of a saved
  value end at one of that name; rename it`: a variable, function, component or `UE_STRUCT` member named None, none or
  NONE. Fix: rename it. See [Overloading](#overloading).
- `<Class>::<Name>: <Ancestor> already has a variable <Name>, and an FName ignores case; rename it` (or `a function`):
  a member reusing a name the parent chain has, in any case. Overriding a function under its exact name is fine. To
  change an inherited variable's default, assign it in `UE_DEFAULTS`. A mod interface's variables count as the
  implementing class's own: one named `Tags` on an actor is refused the same way (`<Class>` is the implementer). See
  [Class defaults](#class-defaults).
- `<Class>::<Function>: <N> parameters, the return value included; a function takes at most 255`: the engine counts a
  function's parameters in one byte. Fix: pass a struct instead of the long list.
- `<Class>::<Function>: its parameters take <N> bytes; a function's parameter block holds at most 65535`: the engine
  sizes a function's parameters in two bytes, and a `const&` parameter is a copy there too. Fix: keep the big struct
  in a member variable and let the function read it there.

- `call to an unknown function: <Name>`: a call to a function AssetGen has no body for. It compiles methods of classes
  and free functions marked `inline`, so the usual cause is a free function or function template without `inline`, a
  plain `constexpr` free function, or a standard library function such as `std::min`, `sqrtf` or `__builtin_sqrtf`.
  The message names the caller, not the definition. Fix: mark a free function `inline`, or make it a static method of a
  UBlueprintFunctionLibrary class. For math, call the UKismetMathLibrary statics. To compute a value when the mod is
  built, use `consteval`. See [Inline functions and templates](#inline-functions-and-templates) and
  [Calling engine and game functions](#calling-engine-and-game-functions).
- `use of undeclared identifier 'FMath'`: clang's message. UE's native FMath is not part of the reflected API, and a
  mod has no C++ standard library to call. Fix: call UKismetMathLibrary, as in
  `UKismetMathLibrary::Clamp(V, 0, 10)`. See [Calling engine and game functions](#calling-engine-and-game-functions).
- `only virtual member functions can be marked 'override'`: clang's message, for `override` on an event override,
  `void ReceiveTick(float DeltaSeconds) override`. UeApi declares engine functions non-virtual. Fix: leave `override`
  off. The name alone makes the override. See [Overrides and parent calls](#overrides-and-parent-calls).
- `warning: <Class>::<Function>: <Base>::<Method>() is a call by name, which on an object of a subclass that
  overrides <Method> runs that override; to run <Base>'s alone, call it from an override of <Method> in <Class>`:
  a qualified call to a method its class does not declare and whose body cannot be copied in (authority-only,
  cosmetic, an RPC, `noinline`, one that waits), where AssetGen cannot add the override itself: the call is in an
  inline method, or the nearest parent's declaration of the method is inline, static or pure virtual. Elsewhere
  AssetGen adds the override and prints nothing. Fix: move the call into a method that is not inline, or declare the
  method in the class, calling
  `<Base>::<Method>()`; the qualified call then runs Base's function alone. See
  [Calling the parent](#calling-the-parent).
- `warning: <Class>::<Function>: <Base>::<Method>() is a call by name, which on an object of a subclass that
  overrides <Method> runs that override: a multicast is called without dispatch only from an override of it, which on
  a server sends it a second time`: the same qualified call, to a multicast RPC. An override that forwards would send
  the multicast twice on a server, so none is added. Fix: none that keeps one send; call it by name, or move what
  must run alone into a function that is not an RPC. See [Calling the parent](#calling-the-parent).
- `warning: <Class>::<Function>: <Callee>'s reference parameter <Parm> is bound to <What>: Blueprint has no reference
  to it, so <Parm> gets a copy, stored back after the call`: a `T&` parameter is bound to a place Blueprint cannot pass
  by reference. `<What>` is `a map element` (`Add5(M[K])`), `a map element's member`, `` `C ? X : Y` `` or
  `that expression`. The call gets a hidden copy, which is stored back into the same place after the call. The key or
  the condition is evaluated once, before the call. Fix: usually nothing. Code that reads the original place while the
  call runs sees the old value; if that matters, copy to a local, call, and store the local back yourself. See
  [Functions](#functions).
- `` <Callee>: TODO: its reference parameter <Parm> is bound to `C ? X : Y` where X or Y is found by a call ``: as
  above, but a side of the conditional is found by a call, `Add5(C ? M[K()] : X)`. Both sides would be located before
  the call, while C++ runs only the picked side's call. Fix: branch with if/else,
  `if (C) Add5(M[K()]); else Add5(X);`. See [Functions](#functions).
- `TODO: inline function <Class>::<Method> called on another object (only this)`: an inline method called on another
  object, `Other->Twice(3)`, or an inline method of a UE_STRUCT. An inline body runs with the caller's `this`. Fix: drop
  `inline`, so that the method is a Blueprint function any object can run, or write a free inline function that takes
  the object as a parameter. See [Inline functions and templates](#inline-functions-and-templates).
- `<Class>::<Function> returns <Type>: a reference return is refused until what it means to a C++ caller is specified;
  return a value or a pointer`: Not yet. A method that returns `T&`, such as `int32 &Slot(int32 I)` or
  `int32 &Get(int32 *P) { return *P; }`. Fix: return a value, or a pointer. See [Functions](#functions).
- `<Class>::<Method>: TODO: an overload set may not mix inline and non-inline functions`: a call to a non-inline
  overload of a name whose overload with the most parameters is inline. A name keeps one Blueprint function, the
  overload with the most parameters. When that overload is inline, the class makes no Blueprint function for the name,
  so the call finds no function. Calling an inline overload beside a non-inline one with more parameters works. Fix:
  give the inline and non-inline functions different names, or make every overload of the name inline. See
  [Functions](#functions).
- `<Class> derives from <Base>, which is UE_FINAL_AS <Leaf>: that is its one subclass`: a second class derives from
  a UE_FINAL_AS base, here or in another mod that includes its header. Fix: derive from the leaf's base's own parent,
  or drop UE_FINAL_AS and declare the base's subclasses yourself.
- `UE_FINAL_AS(<Base>, <Leaf>): the base must be a Blueprint class, a mod's`: the base is an engine (`/Script/`)
  class, whose code is not compiled here. Fix: derive the leaf from it with plain `class Leaf final : public Base`.
- `UE_FINAL_AS(<Base>, <Leaf>): <Base> is the game's Blueprint (<path>), which no mod cooks: it stays as it is cooked
  there, ...; derive <Leaf> from it as a plain class`: the base is a game Blueprint (or a class pinned to a path its
  name does not give, "is cooked at <path>, not by this mod"). Its functions stay non-final and its other subclasses
  stay, so this mod's calls bound to them as final would skip their overrides. Fix: `class Leaf : public Base`.
- `UE_FINAL_AS(<Base>, <Leaf>): <Base> is another mod's class (UE_CLASS "<path>"), and only the UE_FINAL_AS in the
  header that declares it, which a source of that mod beside it includes too, makes the leaf that mod cooks; ...`:
  the macro is written in this mod's own source, its `.cpp` or a header of its own that re-declares the base, not in
  the shared header beside the base, the one a source of the owner's (its `UE_MOD_PACKAGE`, or the only one in its
  folder) includes. The leaf is pinned beside the base and imported, and its owner, which never sees this macro, never
  cooks it. Fix: move the macro into that header, or derive the leaf plainly. AssetGen finds where the macro is written by reading the source and the files it includes by a quoted
  path; a macro written through another macro is not found and is accepted as before.
- `<Class>::<Method>: <Base>::<Method> is final, so no subclass may have a function of that name; rename this one`: a
  subclass declares a method with the name of an ancestor's `final` one and other parameters, `int32 Step(int32 By)`
  under `virtual int32 Step() final`. C++ lets it hide the parent's, but a Blueprint finds functions by name, and the
  same parameters are already refused by clang. Fix: rename the subclass's method, or drop `final`. See
  [Functions](#functions).
- `<Class>::<Method>: <Base>::<Method> is static, and the editor takes a function of that name in a subclass for an
  override of it, which only a static can be ("Check flags: Exec, Final, Static"); rename this one`: a method that is
  not static has the name of an ancestor's static. C++ lets it hide the static, but the editor links a function named
  like a parent's as an override of it and refuses one that does not agree on Static. Fix: rename one of them, or make
  this one static too. See [Static functions and function libraries](#static-functions-and-function-libraries).
- `<Class>::<Method> is static, and the <Base>::<Method> it hides is not: ...; rename this one`: the same the other way
  round, a static named like an ancestor's method. Fix: rename one of them.
- `<Class>::<Method> is static and hides <Base>::<Method>, a static of another signature, ...; rename this one`: a
  static named like an ancestor's static that takes other parameters. Its super would be that static, a function of
  other parameters, which the editor never links. Fix: rename one of them, or give both the same signature (that
  compiles, with a warning).
- `warning: <Class>::<Method> hides <Base>::<Method>, a static: compiled as C++ name hiding, ...`: a static named like
  an ancestor's static of the same signature. Each call runs the one it names, as in C++; the editor would refuse the
  name. Fix: rename one of them to make no Blueprint the editor could not.
- `inline function <Class>::<Method> calls itself`: recursion through inline functions, direct or through another
  inline function. Each call copies the body in, so the copying never ends. One overload calling another is fine. Fix:
  drop `inline`, because a Blueprint function can call itself, or write a loop. See
  [Inline functions and templates](#inline-functions-and-templates).
- `inline function <Class>::<Method> has no body`: an inline function declared and never defined,
  `inline int32 Nope(int32 X);`. clang warns about it too. Fix: define it in the source or in a header the source
  includes. See [Inline functions and templates](#inline-functions-and-templates).
- `static <Name> in an inline function: each expansion would keep its own`: a static local in an inline function,
  `static int32 Count = 0;`. The message follows the prefix `<Class>::<Function>: inline <Function>: `
  (`inline <Class>::<Method>: ` for a method). A `static constexpr` constant works here. Fix: make it a member of the
  class. See [Latent calls](#latent-calls).
- `the comma operator after something its statement runs first (...), which its left side would run before: write its
  left side as a statement of its own`: a comma behind an operand C++ evaluates before it, `{G(), (A, B)}` (braces
  run their members in order) or `L[(A, I)] = G()` (an assignment's right side runs first). Fix: as the message says.
  See [Operators](#operators).
- `the comma operator used as a place, not a value (...), where no statement before this one can hold its left side
  (...)`: `while ((A, S).X < 3)`, a member taken of a comma or a comma bound to an operator's or a constructor's
  reference, in a loop condition, on the right of `&&` / `||` / `?:` or in a call on another object. `an assignment
  used as a place, not a value (...)` is the same for a `=`, `while ((T = S).X < 9)`. Fix: put the left side, or the
  assignment, in a statement of its own. See [Operators](#operators).
- `an assignment assigned to or updated, whose left side is no plain variable, which would be evaluated again to write
  it: ...`: `(L[Idx()] = M) += 1`, as a statement or in a loop condition. Fix: assign in a statement of its own, then
  update what it assigned. See [Assignment and updates](#assignment-and-updates).
- ``an update (`+=`, `++`, ...) assigned to or updated, whose left side is no plain variable, which would be evaluated
  again to write it: ...``: the same for an update, `(L[Idx()] += M) += 1`. Fix: update in a statement of its own,
  then write what it updated.
- `the comma operator's value here is a <Type>, which no Blueprint variable can hold: ...`: a comma that runs in place
  (a loop condition, ...) worth a double or another type no Blueprint variable has, `while ((A, 0.5) < X)`. Fix: put
  the left side in a statement of its own, and use a float. See [Types](#types).
- `the comma operator here would run first, whole, into a temporary, and its value is a <Type>, which no Blueprint
  variable can hold: ...`: the same for a comma that moves into a temporary beside an argument that may run first.
  Fix: as above.
- ``the comma operator here is written to or bound to a reference (a `T&` or `const T&` parameter), beside something
  that may run before it, and its right side is no variable: ...``: `F(G(), (A, L[0]))` where F takes `int32&` or
  `const int32&`: the argument cannot run first into a temporary, which F would then write, or read with what
  `L[0]` held before G ran. An operator's reference operand gets the same message naming the operator instead:
  ``... bound to a reference, operator `+`'s `const FString &` operand, ...`` for `Get() + (A, L[0])` over FString.
  Fix: put the left side in a statement of its own. See [Operators](#operators).
- `<what> passed to a reference parameter, whose right side is no plain variable, where no statement before this one
  can hold it (...)` (`left side` for an update or an assignment): `while (IncRef(0, (A, L[0])) < 20)` or
  `while (IncRef(0, L[0] += 1) < 20)`, in a loop condition, on the right of `&&` / `||` / `?:` or in a call on
  another object. The parameter must be a variable, and nothing before the statement can hold the rest. Fix: as the
  message says. See [Operators](#operators).
- `<what> passed to a reference parameter after something its statement runs first (...), which it would run before:
  ...`: `{G(), IncRef(0, B += 1)}`, the update would run before G. Fix: as the message says.
- `an assignment used as a value after something its statement runs first (...), which its left side would run
  before: assign in a statement of its own, then use what it assigned`: a plain `=` used as a value where the comma
  operator is refused for that reason too, `{G(), (A = N)}`. Fix: as the message says. See [Operators](#operators).
- `an assignment used as a value, whose left side is no plain variable, which would be evaluated again to read it:
  ...`: `A = L[0] = N`. A left side that is a comma, or a member of one, names its variable and works:
  `int32 X = ((Bump(), N) = G());`, `((Bump(), T).A = G())`. An element of one, `int32 X = ((Bump(), L)[1] = G());`
  or `while (((Bump(), L)[I] = M) < 0)`, is refused like `A = L[0] = N`: the element would be located again to read
  it. Fix: assign in a statement of its own, then use what it assigned. See [Operators](#operators).
- `an update (`+=`, `++`, ...) passed to a reference parameter, whose left side is no plain variable, ...`:
  `IncRef(1, L[Idx()] += 1)` where IncRef takes `int32&` or `const int32&`. Fix: update in a statement of its own,
  then pass the element. See [Operators](#operators).
- `inline call to <Class>::<Method> with <N> arguments`: a C-style variadic inline function,
  `inline int32 First(int32 N, ...)`, called with extra arguments. Fix: give it a fixed parameter list, or overloads.
  See [Inline functions and templates](#inline-functions-and-templates).
- `TODO: unimplemented local <Name>: (lambda at <File>:<Line>:<Column>)`,
  `TODO: unimplemented operator overload operator() yielding <Type>` and `TODO: unimplemented argument LambdaExpr`: a
  lambda in a local, called in place, or passed as an argument. A function pointer in a local gives
  `TODO: unimplemented local <Name>: <Type> (*)(<Parms>)`. A Blueprint has neither. Fix: write an inline method or a
  free inline function. To call something later, bind a method to a dispatcher or a delegate parameter,
  `{ this, &AMine::Handle }`. See [Functions](#functions).
- `TODO: unimplemented argument <Kind>`: an expression with no Blueprint form, named by clang's kind, such as
  `CXXNewExpr` for `new`. Fix: create objects with the helpers in `Objects.h`, such as `NewObject<T>` and
  `SpawnActor<T>`. See [Creating objects](#creating-objects).
- `TODO: DeclRefExpr to <Kind>`: a name used as a value that is not a variable, parameter, enumerator or constant, such
  as a function named without being called, `Inc1 != nullptr` (`FunctionDecl`). Fix: call functions, and bind a
  delegate with `{ this, &Class::Function }`. See [Functions](#functions).
- `TODO: unimplemented callee <Kind>`: a call written through an expression instead of a function's name. `<Kind>` is
  clang's name for that expression. Fix: call functions by name, and choose between them with a switch or a
  dispatcher. See [Functions](#functions).
- `ignoring return value of function declared with pure attribute [-Wunused-value]`: clang's warning, for a UE_PURE
  call whose value is unused. AssetGen removes such a call when its arguments do nothing. Fix: use the value, or drop
  the call. UE_NO_OPTIMIZE on the calling function keeps it. See
  [Calling engine and game functions](#calling-engine-and-game-functions).

These compile with no message and do not work as C++ would: the body of a C++ constructor, which is dropped; the
shorter of two non-inline methods with one name; a non-inline method of a UE_STRUCT; and `S.Len()`, the one FString method the SDK declares. `UKismetSystemLibrary::PrintString` compiles too, but UE 4.27 keeps
its body only outside shipping builds, so the retail game prints nothing. See [Functions](#functions),
[Strings and text](#strings-and-text) and [Calling engine and game functions](#calling-engine-and-game-functions).

### Components, defaults, assets and other objects

- `<Class>::UE_DEFAULTS: <Component>->CreationMethod is set by the engine when it makes the component; drop it`:
  how a component was made decides how its actor keeps it, and a template marked Instance or UserConstructionScript
  is never registered with its actor. Fix: drop the line.
- `UserConstructionScript: <Function> spawns an actor, which the engine refuses while a construction script runs (it
  returns None); spawn in ReceiveBeginPlay`: `SpawnActor<T>` or a deferred spawn written in UserConstructionScript.
  Fix: spawn in ReceiveBeginPlay. A helper of the class the construction script calls warns instead: `warning:
  <Class>::<Helper> spawns an actor and UserConstructionScript calls it`, since it may run elsewhere too.
- `warning: <Class>::<Function>: <Target> is an abstract class (a method of it is `= 0`), and the engine spawns no
  actor of one`: `SpawnActor<T>` or `SpawnActorDeferred<T>` of a class with a pure virtual left, named by the call
  (`X::StaticClass()`), or of a UE_FINAL_AS base (`(UE_FINAL_AS <Leaf>'s base)`, then `spawn <Leaf>`). SpawnActor
  makes no actor of it and returns None. The build goes on. Fix: spawn a subclass that defines every `= 0` method, or
  the leaf. See [Objects and widgets](#objects-and-widgets).
- `<Function>: <Target> is an abstract class (a method of it is `= 0`), which the engine may not construct`:
  `NewObject<T>` or `AddComponentByType<T>` of such a class, or of a UE_FINAL_AS base (`(UE_FINAL_AS <Leaf>'s base)
  ... construct <Leaf>`). SpawnObject and AddComponentByClass make one with NewObject, in a Shipping game, and assert
  in a Development one. Fix: construct a subclass that defines every `= 0` method, or the leaf. See
  [Objects and widgets](#objects-and-widgets).
- `warning: <Class>::<Function>: SpawnObject with no Outer (None) makes nothing and returns None`: `NewObject<T>(nullptr)`.
  The build goes on. Fix: pass the object that owns it, such as `this`. See [Objects and widgets](#objects-and-widgets).
- `warning: <Function>: a deferred spawn (SpawnActorDeferred, BeginDeferredActorSpawnFromClass) is not finished in
  this function` and `warning: <Function>: a deferred component add (AddComponentDeferred, AddComponentByClass with
  bDeferredFinish) is not finished in this function`: the function starts more than it finishes and keeps the object
  to itself. Until the finish, the actor runs no construction script and no BeginPlay, and the component is neither
  attached nor registered. Fix: call `FinishSpawning` / `FinishComponent` on it, or keep it in a member for the
  function that does. See [Creating objects](#creating-objects).
- `warning: <Function>: a deferred component add is given one bManualAttachment and finished with another`: the
  finish's decides whether the component attaches to the root; the add's is not read. Fix: pass the same to both.
- `<Function>: AddComponent looks up a component template by name, and a mod class has no component templates, so it
  returns None; add one by class with AddComponentByClass`: `AddComponent(FName("X"), ...)`. Fix: use
  `AddComponentByClass`, or `AddComponentByType<T>(Owner)`. See [Components](#components).
- `<Class>::DefaultSceneRoot: DefaultSceneRoot is the root the construction script adds; rename the component`: the
  name is taken by the root the engine adds to a class with no scene component of its own. Fix: rename it.
- `<Class>::DefaultSceneRoot: DefaultSceneRoot is the variable of the root an actor's construction script adds, and
  this class has no scene component of its own left to be that root; rename it`: a member of that name in an actor
  class that gets the DefaultSceneRoot node (no scene component of its own takes the root, none inherited): the class
  already has a variable of that name, which the construction script stores the root in. Fix: rename it. Where a scene
  component of the class's own is the root, no such variable exists and the member is a member like any other.
- `<Class>::DefaultSceneRoot: DefaultSceneRoot is the variable of the root <Ancestor>'s construction script adds, which
  a variable of that name here would hide; rename it`: a member of that name below a mod class that gets the
  DefaultSceneRoot node. An object variable here would be the one the root is stored in, the ancestor's left empty;
  any other would be a variable named like its parent's, which the editor never builds. Fix: rename it.
- `<Class>::<Component>: <Ancestor> already has a default subobject <Name> (its <Member>); rename the component`: a
  component named like a native parent's own component, such as `CharacterMesh0` or `CollisionCylinder` under an
  ACharacter. The engine finds the objects under an actor by name. Fix: rename it; to change the native one, set
  its properties in `UE_DEFAULTS`. See [Components](#components).
- `the name '<start>...' is too long: <N> characters, where an FName holds at most 1023 (NAME_SIZE)`: a name in the
  package, usually an `FName("...")` literal, is 1024 characters or longer. The engine stops reading the package's
  names at such an entry and misreads every later one. Fix: shorten it; keep long text in an FString.
- `<Class>::UE_DEFAULTS: <Member> is declared here - give it an initializer instead`: UE_DEFAULTS assigns a variable
  that the same class declares, as in `int32 Health; UE_DEFAULTS { Health = 100; }`. Fix: give the variable its value
  where it is declared, `int32 Health = 100;`. UE_DEFAULTS is for inherited variables and for components. See
  [Class defaults](#class-defaults).
- ``<Class>: UE_DEFAULTS has no body in the class; AssetGen reads its statements there alone, so write them in it, `UE_DEFAULTS { ... }`: a definition out of the class, `void <Class>::UeDefaults__()`, is never read, nor seen by a mod that includes the class's header``:
  `UE_DEFAULTS;` declares the block without its statements, which may sit in an out-of-line definition. Those would
  be dropped, and a mod that includes the class's header could not see them either. Fix: move the statements into
  the class, `UE_DEFAULTS { Health = 100; }`. See [Class defaults](#class-defaults).
- `` <Class>::UE_DEFAULTS: every statement is `Field = value;`, `Component->Field = value;` or `Component->SetupAttachment(Parent);` ``:
  a statement in UE_DEFAULTS is not a plain `=` onto a member, nor a SetupAttachment: a call such as
  `K2_DestroyActor();`, `Health += 5;`, a local or an `if`. The block never runs; AssetGen only reads its assignments
  and attachments. A value computed at run time is refused with the
  member-default message that starts `<Member>: a default is a value known when the mod is built`. Fix: keep only
  assignments of build-time values in UE_DEFAULTS, and move the rest to ReceiveBeginPlay or UserConstructionScript.
  See [Class defaults](#class-defaults).
- `<Class>::UE_DEFAULTS: <Member> needs a literal value`: the value writes nothing: `{}` or `T()` of a type AssetGen
  writes no zero for. `nullptr`, `{}` and `T()` of a number, an enum, a name, a string, an object, a container or a
  struct are written as that type's zero. Fix: write an explicit value, or leave the statement out to
  keep the parent's default. See [Class defaults](#class-defaults).
- `` <Member>: `<Struct>()` or `{}` holds what the engine's <Struct> constructor sets, which its header does not
  say, so AssetGen cannot write it over the value already there ``: `Hit = FHitResult();` or `Hit = {};` in
  UE_DEFAULTS or an asset edit, or a `UE_STRUCT`'s `{}` with such a member (`H.Hit: ...`), or `T()` in braces for a
  `UE_STRUCT` member with an initializer of its own, for any engine struct but `FVector`, `FVector2D`, `FRotator`,
  `FLinearColor`, `FColor` and the `FVector_NetQuantize` types (another mod's `UE_STRUCT_IN` struct is no engine
  struct: its `T()` is its members' initializers). Its values are what the engine's constructor sets
  (`FHitResult::Time` is 1, `FTransform()` is the identity). Fix: leave the statement out to keep the parent's value,
  or give the members in braces,
  `Hit = {.Time = 1.0f};`. See [Class defaults](#class-defaults).
- `` <Member>.<Left>, left out of the braces, holds what the engine's <Struct> constructor sets, which its header does
  not say, so AssetGen cannot write it over the value already there ``: braces in UE_DEFAULTS give some members and
  leave `<Left>` out, over a parent's value that is not a fresh one: a member of an engine struct whose header has no
  constructor (`Hit2 = {.Distance = 5.0f};` leaves `Hit2.FaceIndex` out over a native parent's value, and
  `Hit2.Time` out over a mod parent's `FHitResult Hit2 = {.Time = 0.5f};`, whose other members are the engine's
  and stay unwritten), or a `UE_STRUCT` value's member of such a
  struct, with no initializer or `{}` (`H = {.N = 5};` leaves `H.Hit` out), or braces nested for a `UE_STRUCT`
  member with an initializer of its own (`H = {.Hit = {.Distance = 5.0f}};` where the member is `FHitResult Hit =
  {.Time = 0.5f};` leaves `H.Hit.Time` out: the parent's `H.Hit` holds that initializer's Time; FaceIndex, which the
  initializer leaves out too, is still fresh and stays unwritten). A class's own default, a struct's member
  initializer and a container's element say the same where new braces leave out a member that such an initializer
  gives, of a `UE_STRUCT` or another mod's struct (`UE_STRUCT_IN`): `H = {.Hit = {.Distance = 5.0f}}` names
  `H.Hit.Time`. A mod's own asset's braces say it over the class's default object, which the asset starts as:
  `{.H = {.Hit = {.Distance = 5.0f}}}` where the class's `H = {.Hit = {.FaceIndex = 3}}` names `H.Hit.FaceIndex`.
  Fix: give that member in the braces, or leave the statement out to keep the parent's value. See
  [Class defaults](#class-defaults) and [Data assets](#data-assets).
- `warning: <Member>: every member of <Struct> is Transient, which the engine never loads from a default: nothing is
  written, and it keeps the parent's value`: `Handle = FTimerHandle();` in UE_DEFAULTS. No tag can set a Transient
  member (the loader skips it), so the default holds the parent's handle. Fix: none needed; drop the statement to
  silence it.
- `warning: <Asset>.<Member>: <Class>'s default object holds a value that no header says, so none of its elements is
  removed: any it has load as well`, or `<Class>::UE_DEFAULTS: <Member>: ...`: a data asset's braces, or a
  `UE_DEFAULTS` statement, give a TSet or TMap whose class default belongs to an engine class or a game Blueprint,
  named as the source writes it (`BoolSave_C`, not its `Game::` path), or lies over one: a class whose `UE_DEFAULTS`
  sets the member is written over that class's value, which its default object then holds too. The loader starts the value from that default
  and applies what AssetGen writes, so elements the braces leave out stay.
  A class of a shared header declared with `UE_CLASS` whose member has no initializer gets it too, ending "if a mod
  cooks <Class>, declare it with UE_CLASS_IN or give the member an initializer": a game Blueprint's header looks the
  same. Fix: for a game or engine class, none (its value is the game's); for another mod's class, what the message
  says. See [Data assets](#data-assets).
- `warning: <Class>::UE_DEFAULTS: <Root> is the actor's root, which the engine puts at the spawn transform, so its
  <Properties> is not applied. A USceneComponent root passes its transform on to the components attached to it.`:
  UE_DEFAULTS sets RelativeLocation, RelativeRotation or RelativeScale3D on the actor's root, and the root is not a
  plain USceneComponent, for example a UStaticMeshComponent declared first. The build goes on. Fix: declare
  `UE_COMPONENT(USceneComponent, Root);` first, so that later components attach to it; AssetGen then moves the root's
  transform onto each attached component. See [Components](#components).
- `<Class>::UE_DEFAULTS: <Component> is not a UE_COMPONENT`: `Target->Field = value;` where `Target` is a pointer
  member of a mod class that was not declared with UE_COMPONENT. Fix: declare it with `UE_COMPONENT(<Type>, Target);`,
  or set the value at run time. See [Class defaults](#class-defaults).
- `<Class>::UE_DEFAULTS: UeApi does not say which default subobject <Component> is on <EngineClass> - regenerate it
  with genueapi, which reads that off the object dump`: a default set through a native parent's component, such as
  `CapsuleComponent->CapsuleRadius = 55.0f;` on an ACharacter child, when the UeApi headers record no default subobject
  of that name. The headers come from an older genueapi, or the member is a plain pointer and not a default subobject,
  or two of the class's subobjects fit it and neither its name, nor the class that declares it, nor a game Blueprint
  deriving from the class (which genueapi reads with `--game`) tells which.
  The same message appears for a path one level too deep, `Lamp->RelativeLocation.Z = 50.0f;`, which it misreads as a
  component called RelativeLocation; regenerating does not help there. Fix: regenerate UeApi with genueapi from a dump
  that has `GObjects-Dump-WithProperties.txt`; assign whole values,
  `Lamp->RelativeLocation = FVector(0.0f, 0.0f, 50.0f);`; set a member that is not a default subobject at run time.
  See [Class defaults](#class-defaults). The same message, ending "a member that is no default subobject is attached
  to at run time, with AttachToComponent", is `SetupAttachment` onto such a member, which no subobject of that name
  backs. `AActor`'s `RootComponent` is not one: `SetupAttachment(RootComponent)` attaches to the actor's root.
- `<Class>::UE_DEFAULTS: <Component> is a component of the Blueprint <BlueprintClass>, and its header does not say
  which SCS node it is - re-dump the game with the Dumper-7 fork (ScsNode=) and regenerate UeApi`: a default on a
  component of a game Blueprint parent whose header has no `<Component>__UeScsNode` marker. The same message appears
  for a component of a mod parent that another mod cooks (a class pinned with UE_CLASS); the advice to re-dump does
  not apply there. Fix: for a game Blueprint, regenerate UeApi from a dump made with the Dumper-7 fork; for another
  mod's class, set the component's value at run time, for example in ReceiveBeginPlay. See
  [Class defaults](#class-defaults).
- `<Class>::UE_DEFAULTS: <Parent> is a variable of the Blueprint <BlueprintClass>, and its header does not say it is a
  node of its construction script - re-dump the game with the Dumper-7 fork (ScsNode=) and regenerate UeApi`:
  `SetupAttachment(Parent)` onto a game Blueprint parent's member whose header has no `<Parent>__UeScsNode` marker,
  so nothing says the member is a component that Blueprint's construction script makes. Fix: as for the message above;
  or attach at run time, by calling `SetupAttachment` or `AttachToComponent` in ReceiveBeginPlay. The same message
  without the Blueprint, `<Parent> is not a UE_COMPONENT`, is a mod class's plain pointer member. See
  [The root and attachment](#the-root-and-attachment).
- `<Class>::UE_DEFAULTS: SetupAttachment places a component this class declares with UE_COMPONENT; an inherited one
  stays where its class put it`: `Lamp->SetupAttachment(...)` where Lamp is a parent's component. The node that makes
  Lamp is the parent's, which a subclass does not move, in the editor either. Fix: attach it at run time, or move the
  `SetupAttachment` into the parent's UE_DEFAULTS. See [The root and attachment](#the-root-and-attachment).
- `<Class>::UE_DEFAULTS: SetupAttachment attaches <A> -> <B> -> <A>, a cycle no component of which is ever made`, and
  `<Component> is attached to itself`: the construction script starts from the components attached to nothing of the
  class's own, and none in a cycle is. Fix: break the cycle. See [The root and attachment](#the-root-and-attachment).
- `<Class>::UE_DEFAULTS: <Component> is attached twice`: two `SetupAttachment` calls for one component. A node has one
  parent. Fix: keep one. See [The root and attachment](#the-root-and-attachment).
- `<Class>::UE_DEFAULTS: <Component>->SetupAttachment takes a component of this class or of a class above it, by its
  member name`: the parent is written as something other than a member, such as `GetRootComponent()`, a local or
  `nullptr`. The node names its parent, so only a component the class has can be one. Fix: name the component's
  member (`Lamp`, `Mesh`); attach to anything else at run time. See [The root and attachment](#the-root-and-attachment).
- `<Class>::UE_DEFAULTS: <Component>->SetupAttachment's socket is a literal name (FName("hand_r")) or none`: the
  socket is a variable or a call, which a node cannot hold. Fix: write the name, or attach at run time. See
  [The root and attachment](#the-root-and-attachment).
- `warning: <Class>::<Function>: SetupAttachment attaches at once here, as AttachToComponent with KeepRelative
  location, rotation and scale and no welding, ...`: `SetupAttachment` in a function. It attaches as the engine's own
  call would once the component registers, but on a component already registered, as an actor's are once it is
  constructed, the engine's own does nothing. The build goes on. Fix: call `AttachToComponent` with the rules you
  want, or place a component of the class's own with `SetupAttachment` in UE_DEFAULTS. See
  [The root and attachment](#the-root-and-attachment).
- `<Class>::<Member>: only an actor has a construction script`: UE_COMPONENT in a class that does not derive from
  AActor. Fix: declare components only on an actor class. See [Components](#components).
- `<Class>::<Member>: a UE_COMPONENT names an engine component class`: UE_COMPONENT of a component class the mod
  declares itself. Fix: use an engine or game component class, and add a component class of your own at run time with
  `AddComponentDeferred<T>` or `AddComponentByType<T>` from `Objects.h`. See [Components](#components).
- `<Class>::<Member>: <ComponentClass> is not a UActorComponent`: UE_COMPONENT of a class that is not a component,
  such as `UE_COMPONENT(UTexture2D, Icon);`. Fix: use a UActorComponent subclass, or make it a plain pointer member.
  See [Components](#components).
- `<Class>::<Member>: a UModelComponent belongs to a level's BSP and cannot be a component template`:
  `UE_COMPONENT(UModelComponent, Bsp);`. Fix: use a mesh component. See [Components](#components).
- `no matching function for call to 'NewObject'`, with a note such as
  `because '!Derives<AActor, AActor>' evaluated to false`: clang's message, followed by AssetGen's
  `clang rejected <Source> (diagnostics above)`. A helper from `Objects.h` was called with the wrong kind of class, as
  in `NewObject<AActor>(this)`. SpawnActor and SpawnActorDeferred take an actor, AddComponentByType and
  AddComponentDeferred a component, CreateWidget a widget, and NewObject any other object. Fix: call the helper made
  for that kind of class, such as `SpawnActor<T>` for an actor. See [Creating objects](#creating-objects).
- `Cast<> to an unknown class: <Type>`: `Cast<T>(X)` where T is not a UObject class that AssetGen has a declaration
  for. `Cast<IFoo>(Obj)` to a mod interface reports the type as `int64`. Fix: include the header that defines T, and
  cast only to classes. For an interface, write `TScriptInterface<IFoo> I = Obj;`. See [Types](#types) and
  [Interfaces](#interfaces).
- `<Method>() is <Target>, which is not declared here: include its UeApi header`: `Obj->GetOuter()`, `GetClass()`,
  `GetName()`, `IsA()`, or a struct, text or soft pointer method (`AssetData.GetExportTextName()`), in a source that
  does not include the header declaring the function the SDK forwards the call to.
  Fix: include the header that declares `<Target>`; UGameplayStatics and UKismetSystemLibrary are in `UeApi/Engine.h`.
  See [Working with other objects](#working-with-other-objects).
- `access to an unknown property: <Member>`: a data member of a type AssetGen keeps no record of. It records named
  classes and structs that have a body, so a member of an anonymous struct or union, or of a class template of your
  own such as `template <class T> struct FBox { T V; };`, is unknown. Fix: keep the data in a named UE_STRUCT or on
  the mod class. See [Structs](#structs).
- `StaticClass() on an unknown class`: `X::StaticClass()` where AssetGen cannot resolve `X` to a class it knows.
  Fix: qualify the call with a class the source declares or includes, `AMyActor::StaticClass()`. See
  [Working with other objects](#working-with-other-objects).
- `<Class> derives from an undeclared class: <Base>`: the parent class has no declaration in the parsed headers.
  clang rejects an incomplete base first, so this is rare. Fix: derive from a class whose header is included. See
  [Classes and variables](#classes-and-variables).
- `<Class>::UE_DEFAULTS: <Component>__UeScsNode is not 32 hex digits`: the game Blueprint parent's header carries a
  damaged SCS node marker, for example after hand editing. Fix: regenerate the UeApi headers. See
  [Class defaults](#class-defaults).
- `<Class>::UE_DEFAULTS: cannot tell which class declares <Member>`, `<Class>::UE_DEFAULTS: <Component> is not declared
  with UE_COMPONENT` and `<Class>::UE_DEFAULTS: cannot resolve the class of <Component>`: backstops behind the checks
  above, which normally fire first. Fix: assign a member declared on the class's ancestry or on one of its components,
  and check the UE_COMPONENT declarations of the class and its mod parents. See [Class defaults](#class-defaults).
- ``<Method>(): TODO: only a call on an object (`Obj-><Method>()`)``: GetOuter, GetClass or GetName written as
  something other than a member call. C++ allows only member calls, so ordinary code does not reach it. Fix: call it
  on an object, `Obj->GetOuter()`, or as `GetName()` on this. See
  [Working with other objects](#working-with-other-objects).

### Event dispatchers, timers and input

- `<Dispatcher>.Broadcast: <Parameter> is a non-const reference, which a Broadcast never writes back to the caller`:
  the engine copies each argument into a parameter block of its own, so what the handlers write stays there. Fix:
  take the parameter by value or by `const &`, and hand results back through a member variable.
- `a delegate on \`this\` cannot bind <Class>::<Function>: the engine looks it up by name on this object, whose class
  has no such function`: `{this, &Other::F}` where this class is no `Other`. Fix: bind it on an `Other` object,
  `{O, &Other::F}`, or bind a method of this class.
- `a delegate on a <Type> cannot bind <Class>::<Function>: the engine looks it up by name on that object, whose class
  has no such function`: `OnHit.Add(H, &Other::F)` where H's type is no `Other`; the broadcast would skip it. Fix:
  bind a function of H's class, or pass an object that has F. See [Event dispatchers](#event-dispatchers).
- `a delegate cannot bind <Class>::<Function> on another object: the editor binds a BlueprintCallable function that is
  not pure or latent, and <Function> is not BlueprintCallable` (or `is pure`, `is latent`; `on this object` for a
  binding on `this`): the editor's Create Event node lists only such functions of its Object pin's class. An event the
  engine calls (`AActor::ReceiveTick`) or a mod method that overrides one is none, and neither is a native function
  the engine does not mark BlueprintCallable: an RPC (`AItem::Server_StartUsing`), a RepNotify
  (`AActor::OnRep_Instigator`). Fix: bind a method of your own class that calls it. See
  [Event dispatchers](#event-dispatchers).
- `a delegate cannot bind <Class>::<Function>, which <Class> declares and never defines`: a binding on another object
  names a mod method with no body, so no function of that name exists. Fix: define it.
- `a delegate binds <Function> on an object, through a pointer to its class, not <Type>`: the object half of a binding
  is no class pointer (an interface, a struct). Fix: pass the object, `I.GetObject()` cast to its class.
- `<Function>: <Dispatcher>__DelegateSignature is the dispatcher's signature, which does nothing when called; call
  <Dispatcher>.Broadcast(...)`. Fix: broadcast the dispatcher.
- `<Function>: K2_SetTimer by name <Name> names no function of the class` (or `names an inline method`, or `names a
  function that takes parameters`)`, so the engine sets no timer`: the engine finds the function by name when the
  timer is set, and only calls one without parameters. Fix: name a method of the class that takes none, or pass a
  delegate with `K2_SetTimerDelegate`.
- `` a delegate cannot bind <Function>: an inline function is expanded where it is called, no UFunction (drop `inline`) ``:
  Add, Remove or a delegate value names an inline method. An inline method is copied into each caller and never
  becomes a function of the class. Fix: drop `inline` from the handler. See [Event dispatchers](#event-dispatchers).
- `<Dispatcher>.Broadcast: <Dispatcher> is a native dispatcher (<Class>::<Dispatcher>) that is not BlueprintCallable,
  and the editor's Call node refuses one ("Event Dispatcher is not 'BlueprintCallable'"): only the engine's own code
  broadcasts it`: `OnDestroyed.Broadcast(this)` and the like; none of the engine's dispatchers is BlueprintCallable, 41
  of the game's are. Fix: none; call what the engine calls to make the event happen (`K2_DestroyActor()`).
- `<Dispatcher>.Broadcast: <Dispatcher> is a native dispatcher (<Class>::<Dispatcher>) and the editor's Call node takes
  one only when it is BlueprintCallable, which this UeApi does not say (regenerate it with genueapi)`: the UeApi
  headers have no `<Dispatcher>__UeDispatcher` mark for it, which genueapi writes for every native dispatcher (since
  UeApi version 3; an older folder is refused before this), so the header was edited by hand. Fix: regenerate them.
- `<Dispatcher>.<Add|Remove|Clear>: <Dispatcher> is a native dispatcher that is not BlueprintAssignable
  (<Class>::<Dispatcher>), and the editor's dispatcher nodes refuse one ("Event Dispatcher is not 'BlueprintAssignable'"):
  only the engine's own code binds it`: 17 of the game's dispatchers are not BlueprintAssignable. Fix: none from
  Blueprint code.
- `a delegate value is {this, &Class::Function}`: a delegate argument built any other way, such as an empty `{}`
  passed to `UKismetSystemLibrary::K2_ClearTimerDelegate`. There is no empty delegate value. Fix: pass
  `{ this, &AMine::Handle }` or `TDelegate<void()>(this, &AMine::Handle)`. See
  [Event dispatchers](#event-dispatchers).
- `<Where>: TDelegate<...> needs a signature function, which only a class holds, not a struct or an interface`
  (`member D: ...`): a `UE_STRUCT` member of a delegate type. The editor makes a delegate's signature function in a
  class; a struct has none to name. Fix: keep the delegate in a class variable. A mod interface's TDelegate variable
  is the implementing class's own property, which makes it. See [Delegate values](#delegate-values).
- `` a delegate binds `&Class::Function` ``: the function half of a bind is not written as `&Class::Function`, for
  example a member-pointer variable. Fix: write `&Class::Function` in the Add or Remove call itself. See
  [Event dispatchers](#event-dispatchers).
- `<Class>::<Member>: a dispatcher is declared with UE_DISPATCHER`: a `TMulticastInlineDelegate<void(...)>` member
  written by hand. Fix: `UE_DISPATCHER(OnHit, int32 Points);`, which also declares the signature that carries the
  parameter names. See [Event dispatchers](#event-dispatchers).
- `TODO: unimplemented property <Name>: TMulticastSparseDelegate<...>`: a mod class declares a sparse dispatcher
  member. A mod class cannot own one; binding to the game's sparse dispatchers, such as OnDestroyed, works. Fix: use
  UE_DISPATCHER. See [Event dispatchers](#event-dispatchers).
- `no member named 'IsBound' in 'TMulticastInlineDelegate<void (int)>'`: clang's message. A dispatcher has four
  methods, Add, Remove, Clear and Broadcast, and clang rejects any other name, such as IsBound, AddUnique or Contains.
  AssetGen's own check behind it, `TODO: unimplemented dispatcher method <Method>`, is not reached with the SDK's
  headers. Fix: Add already skips a second binding of the same object and function. To know whether your own
  dispatcher has listeners, count them where you call Add and Remove. See [Event dispatchers](#event-dispatchers).
- `a dispatcher must be a property: <Method>`: Add, Remove, Clear or Broadcast on a dispatcher that is not reached as
  a property of an object. Fix: declare the dispatcher on a class with `UE_DISPATCHER(Name, Params...)` and call it
  through the object. See [Event dispatchers](#event-dispatchers).
- `no Kismet conversion from FString to FKey`: a key written with parentheses, `FKey("F5")` or
  `FKey(FName("F5"))`. A one-argument constructor in parentheses is read as a conversion, and no Kismet conversion
  makes an FKey. `FKey K = {"F5"};` is refused the same way, and so are other one-argument struct constructors, such as
  `FFrameNumber(5)`. Fix: write the braces out in full, `FKey{"F5"}`, `FFrameNumber{5}`. See
  [Timers and input](#timers-and-input).
- `no member named 'BindKey' in 'UInputComponent'`: clang's message. Not yet: AssetGen has no key or input-action
  events, and BindKey and BindAction are native C++ only and absent from the SDK. Fix: poll the player controller in
  ReceiveTick, `PC->WasInputKeyJustPressed(FKey{"F5"})`, or bind the game's action dispatchers on the player
  character, such as OnFirePressed. See [Timers and input](#timers-and-input).

### Interfaces

- `<Interface>::<Function> is native only (not a BlueprintNativeEvent or BlueprintImplementableEvent), so a Blueprint
  cannot implement <Interface>`: the class lists a game or engine interface with a function a Blueprint cannot
  implement, such as IHealth. The message names the first such function in alphabetical order. Fix: do not implement
  that interface. Calling it on game objects through `TScriptInterface<IHealth>` still works. See
  [Interfaces](#interfaces).
- `<Member> is a variable of the interface <Interface>, which holds no state itself: read it through an object of a
  class that implements <Interface>, not through the interface`: an interface variable read or written through
  `TScriptInterface<IMarkable>`. Fix: reach it on `this` in an implementing class, or through a pointer typed as that
  class, `Cast<Beacon>(Obj)->Marks`. See [Interfaces](#interfaces).
- `<Class> implements <Interface>, which its parent <Ancestor> already implements (through <Other>)`: a class lists an
  interface that a parent already implements (a mod one, or a native one UeApi lists it on), directly or through an
  interface that extends it. The
  ` (through <Other>)` part appears only in the second case. Fix: drop the interface from the child and override its
  functions as ordinary methods. See [Interfaces](#interfaces).
- `<Class> implements <Other> and <Interface>, which both extend <Common>`: two listed interfaces share a link in their
  chains: one extends the other, or both extend a third. Fix: implement one of them, or change the interfaces so their
  chains share no link. See [Interfaces](#interfaces).
- `<Class>: the variable <Member> of the interface <Interface> is declared twice`: the class declares a member with
  the name of a variable of an interface it implements, or two of its interfaces declare that name. Fix: remove the
  class's own declaration. The interface's variable is already this class's property; give it this class's default in
  UE_DEFAULTS. See [Interfaces](#interfaces).
- `<Class> implements <Interface>, which is not an interface`: a mod class without UE_INTERFACE is listed after the
  parent. Every base after the first is taken as an interface. Fix: add UE_INTERFACE to that class, or remove it from
  the base list. See [Interfaces](#interfaces).
- `<Class> implements an undeclared interface: <Interface>`: a base after the parent is a struct or has no
  declaration, as in `class A : public AActor, public FSomeStruct`. Fix: list only interfaces after the parent, and
  hold a struct as a member. See [Interfaces](#interfaces).
- `<Interface>: an interface extends one interface at most, as a UClass has one super`:
  `class IChild : public IA, public IB { UE_INTERFACE; };`. Fix: chain them (IB extends IA, IChild extends IB), or
  have the implementing class list both. See [Interfaces](#interfaces).
- `<Interface> extends <Base>, which is not an interface`: a UE_INTERFACE derives from a class that is not an
  interface, as in `class IActorish : public AActor { public: UE_INTERFACE; };`. This message appears only while no
  class implements the interface. With an implementer, the class fails first with a misleading
  `AActor::<Function> is native only ...` message. Fix: give a root interface no base, or derive it from another
  UE_INTERFACE or a game interface. See [Interfaces](#interfaces).
- `<Interface>::<Member>: an interface cannot declare a UE_DISPATCHER`: UE_DISPATCHER inside a UE_INTERFACE. When a
  class in the mod implements the interface, that class fails first with
  `TODO: unimplemented property <Name>: TMulticastInlineDelegate<...>`. Fix: declare the dispatcher on the implementing
  class. See [Interfaces](#interfaces).
- `<Interface>::<Member>: an interface cannot declare a UE_COMPONENT`: UE_COMPONENT inside a UE_INTERFACE. Fix:
  declare the component on the implementing actor. Plain variables on an interface are allowed. See
  [Interfaces](#interfaces).
- `<Interface>::<Function> takes a type AssetGen cannot write yet, so <Class> cannot implement <Interface>`: Not yet. A
  game interface has a Blueprint event with a parameter type the SDK generator could not express, so the header leaves
  the function out and the interface cannot be implemented. No interface in the current SDK reaches this. Fix: none
  from the mod side. See [Interfaces](#interfaces).

### Replication, RPCs and latent calls

- `latent call <Callee>: a function that resumes later returns nothing and takes no non-const reference parameters`: a
  latent call such as `UKismetSystemLibrary::Delay` in a function that returns a value or takes a non-const
  reference parameter. The code after the wait resumes in the class's event graph, which has no return value or
  out parameter: a `const&` is copied into it, a write through any other would be lost. After UE_AWAIT the message
  starts `UE_AWAIT: ` instead of `latent call <Callee>: `. Fix: return void, take parameters by value or `const&`,
  and keep results in member variables. See [Latent calls](#latent-calls).
- `latent call <Callee>: a static function has no object whose ubergraph frame could keep its locals`: a latent call,
  or UE_AWAIT (`UE_AWAIT: a static function ...`), in a static function. Fix: make it a non-static method. See
  [Latent calls](#latent-calls).
- `<Callee>: leave the FLatentActionInfo argument out, the compiler supplies it`: a latent call given its
  FLatentActionInfo, as in `UKismetSystemLibrary::Delay(this, 1.0f, Info);`. Fix: call the overload without it,
  `UKismetSystemLibrary::Delay(1.0f);`. See [Latent calls](#latent-calls).
- `warning: <Class>::<Function> waits, and an object of this class finds its world only through its Outer: make it with
  an actor or component as Outer, or the call does nothing and the function never resumes`: a latent call or
  UE_AWAIT in a class that is not an actor, component, widget, GameInstance or subsystem, such as a UObject child. The
  build goes on. Fix: create the object with an actor or component as its Outer, as `NewObject<T>(this)` does from an
  actor. See [Latent calls](#latent-calls).
- `warning: <Class>::<Function> keeps <Local> (a <Type>) across a wait only in its ubergraph frame, which holds an object
  weakly`: a method that waits (Delay, LoadAsset, UE_AWAIT) holds an object it made or loaded only in a local across a
  wait and reads the local after it. The frame keeps an object only weakly, so a garbage collection during the wait
  can take it and the local reads None. The build goes on. Fix: keep the object in a member variable. See
  [When the code after a wait runs](#when-the-code-after-a-wait-runs).
- `UE_AWAIT: keep the object in a variable, it is used twice`: the dispatcher's object is a call result, as in
  `UE_AWAIT(UAsyncTaskDownloadImage::DownloadImage(Url)->OnSuccess)`. The object is used by the bind and again to
  start the action. Fix: `UAsyncTaskDownloadImage *Task = UAsyncTaskDownloadImage::DownloadImage(Url);` and then
  `Image = UE_AWAIT(Task->OnSuccess);`. See [Waiting on events](#waiting-on-events).
- `UE_AWAIT on <Var>: some paths reach it with <Var>'s async action already activated and some without, so it would
  start twice or never`: an await on an async action that one path reaches after an earlier await on the same
  variable and another path reaches without one, as in `if (Stop) UE_AWAIT(T->OnSuccess); UE_AWAIT(T->OnFail);`, or
  in a loop that can skip its await (`continue` before it). Fix: await the variable on every path before this await
  or on none, or assign it again on each path. See [Waiting on events](#waiting-on-events).
- `<Class>::<Function>: static <Local> lives in the ubergraph's frame, which only a function that makes a latent call
  runs in; make <Local> a member`: a `static` local that the function changes, as in
  `static int32 Count = 0; ++Count;`, in a method that makes no latent call. Fix: make it a member of the class. A
  `static constexpr` constant works anywhere, and a static local is allowed in a function that makes a latent call,
  where each object keeps its own. See [Latent calls](#latent-calls).
- `warning: <Class>::<Function>: modifying reference parameter <Parm> of an RPC reaches the caller only when the call
  runs locally; take it by value or const&`: an RPC writes through a `T&` parameter, as in
  `UE_SERVER void ServerBump(int32 &Count) { Count += 1; }`. The remote side gets a copy. The build goes on. Fix: take
  the parameter by value or by `const&`, and send a result back another way, such as a Client RPC or a replicated
  variable. See [RPCs](#rpcs).
- `<Class>::<Function>: an RPC goes one way: UE_MULTICAST with UE_SERVER or UE_CLIENT`: two direction markers on one
  function. The sender picks one direction and the receiver checks its own. Fix: keep one marker. See [RPCs](#rpcs).
- `<Class>::<Function>: a static function cannot be an RPC, it is never sent`: `UE_SERVER static void S();`. The
  engine routes a static function locally. Fix: make it a member function. See [RPCs](#rpcs).
- `<Class>::<Function>: an RPC returns void`: `UE_SERVER int32 Fire();` with a body. Fix: return void, and send a
  result back with another RPC or a replicated variable. See [RPCs](#rpcs).
- `<Class>::<Member>: its RepNotify <Notify> must be a method of the class taking no parameters`:
  `UE_REPLICATED_USING(int32, Health, OnRep_Health);` with no `void OnRep_Health()`, or with one that takes
  parameters. Fix: declare `void OnRep_Health();` on the class. For a variable declared on a mod interface, a
  declaration on the interface also satisfies the check; each implementing class still gets the RepNotify as its own
  function. See [Replication](#replication).
- `<Class>::<Member>: its RepNotify <Notify> must return void`: `int32 OnRep_Health()`. The engine calls the OnRep
  with no room for a result. Fix: return void. See [Replication](#replication).
- `<Class>::<Member>: its RepNotify <Notify> is inline, so no function of the class: drop inline`:
  `inline void OnRep_Health()`. An inline method is expanded where it is called and is no Blueprint function, so a
  client would never find it. Fix: declare it without `inline`. See [Replication](#replication).
- `<Class>::<Member>: unknown replication condition <Condition>`: a condition name that is not an ELifetimeCondition,
  as in `UE_REPLICATED_IF(FVector, Aim, OwnersOnly);`. Fix: use one of None, InitialOnly, OwnerOnly, SkipOwner,
  SimulatedOnly, AutonomousOnly, SimulatedOrPhysics, InitialOrOwner, Custom, ReplayOrOwner, ReplayOnly,
  SimulatedOnlyNoReplay, SimulatedOrPhysicsNoReplay, SkipReplay or Never. A `COND_` prefix is optional. See
  [Replication](#replication).
- `<Class>::<Member>: a TMap or TSet does not replicate`: a replicated TSet or TMap, or a replicated container that
  holds one. The engine replicates neither. A TMap cannot even be written in the macro: the comma in `TMap<K, V>`
  splits the macro argument, and clang stops with `too many arguments provided to function-like macro invocation`.
  Fix: replicate TArrays, for example one of keys and one of values, and rebuild the map in the OnRep function. See
  [Replication](#replication).
- `<Class>::<Member>: a TMap or TSet in <Struct> does not replicate`, `an interface does not replicate`: a replicated
  variable that is or holds, at any depth of a `UE_STRUCT`, a TMap, a TSet or a `TScriptInterface`. The engine sends a
  struct member by member and sends nothing for those. For an RPC: `an RPC parameter cannot hold what does not
  replicate: <Parameter> is or holds ...`. Fix: send a `UObject*` or actor pointer instead of an interface, and
  arrays instead of a map.
- `<Class>::<Function>: an RPC, authority-only or cosmetic marker on an inline method does nothing, since no call to
  it is routed; drop inline`: the engine routes a call by the called function's flags, and an inline method is no
  function. Fix: drop `inline`.
- `<Class>::<Function>: UE_RELIABLE needs UE_SERVER, UE_CLIENT or UE_MULTICAST`: `UE_RELIABLE void Ping() {}` with no
  RPC kind. Fix: add UE_SERVER, UE_CLIENT or UE_MULTICAST, or drop UE_RELIABLE. See [RPCs](#rpcs).
- `<Class>::<Function>: an override takes its parent's replication; drop the RPC marker`: an RPC marker on a method
  that overrides a parent's function or event, such as `UE_SERVER void ReceiveBeginPlay()`. Fix: remove the marker.
  The override keeps its parent's net flags. See [RPCs](#rpcs).
- `<Class>::<Function>: an RPC parameter cannot be a TMap or TSet, which do not replicate`:
  `UE_SERVER void Send(TSet<int32> Ids);`, or a parameter that holds a TMap or TSet. Fix: pass TArrays instead. See
  [RPCs](#rpcs).
- `UE_AWAIT takes a dispatcher property`: UE_AWAIT on something other than a dispatcher property of an object. A
  dispatcher held in a local is refused earlier, at its declaration, with
  `TODO: unimplemented local <Name>: TMulticastInlineDelegate<...>`. A single-cast TDelegate cannot be awaited. Fix:
  await a dispatcher property, a UE_DISPATCHER or an engine one, as in `UE_AWAIT(Proxy->OnCompleted)`. See
  [Waiting on events](#waiting-on-events).
- `UE_AWAIT: cannot read the signature of <Type>`: the dispatcher's C++ type does not spell out
  `TMulticastInlineDelegate<void(...)>`, so AssetGen cannot read its parameters. Fix: declare the dispatcher with
  `UE_DISPATCHER(Name, Params...)`. See [Waiting on events](#waiting-on-events).
- `<Class>::ExecuteUbergraph_<Name>: ExecuteUbergraph_<Class> is the name of a class's ubergraph, whose latent calls
  resume through it; rename it`: the engine resumes a class's waits through that function, found by name on the
  object, so a method of that name would catch them. `<Class> declares ExecuteUbergraph_<Class>, the ubergraph's own
  name` is the same for a parent from another mod. Fix: rename the method. See [Latent calls](#latent-calls).

### Pointers, memory and intrinsics

- `TODO: a whole <Type> through a pointer; P->Member reaches its members`: a whole struct or container read, written
  or passed through a raw pointer, as in `FVector V = *Pv;` or `*Pv = FVector(1.0f, 2.0f, 3.0f);`. Only integers,
  float, bool, enums, FName, FString, FText and pointers are read whole. Fix: go member by member, `Pv->X = 1.0f;`.
  See [Pointers and memory](#pointers-and-memory).
- ``TODO: the address of <ExprKind>: a Blueprint variable has none the VM hands out; point into an object
  (`(uint8*)Obj`), a TArray element (`&Items[I]`) or memory through a pointer``: the address of a local, a parameter
  or a member of a struct value, as in `&Count` or `&V.X`. `<ExprKind>` is clang's name for the expression, such as
  DeclRefExpr or MemberExpr. The address of a struct member is Not yet: it is refused with this message. Fix: point
  into an object, an array element or memory through a pointer, as the message says. For a struct reached through a
  pointer, add the member's offset yourself: `(int64)Pv + 4` is the address of `Pv->Y`. See
  [Pointers and memory](#pointers-and-memory).
- `TODO: the address of a member of <Type>, which is not an object class`: Not yet. `&Pv->X` through a struct
  pointer. The message names `int64`, because AssetGen holds the pointer as an address. Fix: add the member's offset
  to the pointer, `(int64)Pv + 4` for FVector's Y, or read and write `Pv->X` directly. See
  [Pointers and memory](#pointers-and-memory).
- `` `&Obj->Member` finds the member at run time through ReadProperty::GetPropertyAddress: include ReadProperty.h ``:
  `&Obj->Member`, or `&Member` on the object itself, with no class named ReadProperty in scope. A cooked property
  carries no offset, so the address is looked up at run time by a function of that class, which AssetGen does not
  ship. A reference local bound to another object's member, `float &R = Other->CustomTimeDilation;`, gives the same
  message after `reference R: `. Fix: include a header that declares `class ReadProperty` with a static, non-inline
  `int64 GetPropertyAddress(UObject *Target, FName PropName, UObject *WorldContextObject = nullptr)`, and build the mod
  that defines it. For a reference, read the member into a local instead. See
  [Pointers and memory](#pointers-and-memory).
- `<Class>::<Function>: TODO: a pointer read in a function that makes a latent call`: Not yet. A read through a
  pointer (`*P`, a `__Read*__` intrinsic, or `GetOuter()`, which reads memory) in a method that also makes a latent
  call or uses UE_AWAIT is refused. Fix: do the read in a separate method with no latent call and call it, or store
  the value in a member before the wait. See [Pointers and memory](#pointers-and-memory).
- `TODO: reading a uint32 through a pointer (Kismet has no unsigned 32-bit int); read it as int32 or int64`: `*P` on a
  `uint32 *`. Reading it as an int64 reads 8 bytes, which is only right when the memory really holds an int64. Fix:
  read it as int32 and accept the sign, or widen and mask it:
  `int32 Raw = *(int32 *)P; return (int64)Raw & 0xFFFFFFFFLL;`. See [Pointers and memory](#pointers-and-memory).
- `an int becomes a pointer through int64: <PointerType>`: an int32 value that is not a constant converted to a
  pointer, as in `(AActor *)N` with `int32 N`. A literal is fine: 0 is null. A cast in between, `(AActor *)(int64)N`,
  is refused the same way. Fix: hold the address in an int64 variable, `int64 A = N; AActor *P = (AActor *)A;`, or
  keep addresses in int64 from the start. See [Pointers and memory](#pointers-and-memory).
- ``TODO: `*` on <Type>, which is not a raw pointer``: `*` on an object pointer, as in `AActor &A = *Other;`. An
  object pointer is a reference to the object, not an address. Fix: use the pointer as it is, `Other->Field`; for the
  object's memory, convert it with `__PtrCast__<int64>(Other)` or `(uint8 *)Other`. See
  [Pointers and memory](#pointers-and-memory).
- `TODO: indexing that is not through a raw pointer: <Type>`: `A[I]` where neither side is a raw pointer, such as an
  object pointer. Fix: index raw pointers (`P[I]`) or TArrays (`Items[I]`). See
  [Pointers and memory](#pointers-and-memory).
- `the address of an element needs an array variable`: `&Arr[I]` on an array a call returns, as in
  `&GetItems()[0]`. Fix: keep the array in a variable and take the element's address there, `&Items[I]`. See
  [Pointers and memory](#pointers-and-memory).
- `a static variable has no address: <Name> is inline, each use its initializer`: `&this->K` or `&Obj->K` on an
  inline or static class variable. A bare `&K` gives the address-of message above instead. Fix: use the value; a
  constant is its initializer at each use. See [Pointers and memory](#pointers-and-memory).
- `reference <Name>: <Reason>`: a reference local, `T &R = X;`, whose initializer is neither a variable nor a member
  reached with `.` from this or from a variable, so AssetGen needs an address it cannot take. `int32 &R = Scores[Name];`
  gives the address-of message above as the reason. Fix: follow `<Reason>`, use the place directly, or copy it into a
  local and store it back. `T &R = LocalVar` and `T &R = MemberVar` work. See [Locals](#locals).
- `__PtrCast__ of <Name>, a reference to a Blueprint variable, which has no address`: `__PtrCast__` of a reference
  local bound to a Blueprint variable, as in `int32 &R = Local; __PtrCast__<int64>(R)`. Fix: only a reference bound
  to memory through a pointer, `int32 &R = *P;`, has an address; otherwise work with the pointer itself. See
  [Pointers and memory](#pointers-and-memory).
- `<Operator>: <Reason>`: `sizeof` or `alignof` of a type whose game layout AssetGen cannot compute. `sizeof(AActor)`
  gives `sizeof: TODO: unimplemented struct member type: AActor`. Fix: measure integers, float, bool, FName, FString,
  FText, pointers, enums, containers, soft pointers, TSubclassOf or structs; an object class is measured through a
  pointer. See [Pointers and memory](#pointers-and-memory).
- `__ClassOf__ argument must be a bare parameter or local reference`: `__ClassOf__(X)` where X is not a plain
  parameter or local name, as in `__ClassOf__(Obj->Field)`. Fix: pass the name, `__ClassOf__(Target)`. The call
  becomes a call to GetParmClassName on the enclosing class, which that class must define; the build does not check
  it. See [Intrinsics](#intrinsics).
- `TODO: unimplemented intrinsic <Name>`: a call to a function spelled with two leading and two trailing underscores
  that is not one of AssetGen's intrinsics, such as a helper of your own named `__Helper__`, or `__EmbedFile__` used
  inside a function body. Fix: do not give your own functions that spelling, and use `__EmbedFile__` only as the
  initializer of a member. See [Intrinsics](#intrinsics).
- `__EmbedFile__: cannot read (or empty): <Path>`: the file is missing or empty. The path is taken relative to the
  folder of the `.cpp` being compiled, not the working directory. Fix: put a non-empty file at that path. See
  [Classes and variables](#classes-and-variables).
- `__EmbedFile__: the path must be a string literal: <Member>`: the path is a named constant or other expression.
  Fix: write it as a string literal, `__EmbedFile__("data/blob.bin")`. See
  [Classes and variables](#classes-and-variables).
- `__EmbedFile__ initialises a TArray<uint8>: <Member>`: a backstop for `__EmbedFile__` as the default of an array
  that is not a `TArray<uint8>`. `__EmbedFile__` returns a `TArray<uint8>`, so clang refuses any other array type
  first, as in `no viable conversion from 'TArray<uint8>' to 'TArray<EMood>'`. Fix: declare the member as
  `TArray<uint8>`. See [Classes and variables](#classes-and-variables).
- `__PtrCast__ takes one argument` and `__ClassOf__ requires an argument`: backstops; clang checks the argument
  count first. Fix: pass exactly one argument. See [Intrinsics](#intrinsics).
- `__Asm__: first argument must be a string literal of hex bytes`, `__Asm__: not a hex character: '<Char>'`,
  `__Asm__: odd number of hex digits` and `__Asm__: MemBytes must be an integer literal`: these checks run while the
  bytecode is written, and today their messages are not printed. A malformed `__Asm__` call compiles with no message
  and adds no bytes. Fix: pass a string literal of hex digit pairs, with no `0x`; spaces, tabs, line breaks, commas
  and underscores between them are skipped. Pass MemBytes as an integer literal. See [Intrinsics](#intrinsics).

### The command line, bpbuild and genueapi

#### assetgen

- `FAILED: <message>`: how `assetgen compile` and `assetgen registry` print a refusal, with exit code 1. A message
  raised in a function body starts with `<Class>::<Function>: `, and one raised inside an inline function's expansion
  also carries `inline <Class>::<Method>: `, or `inline <Function>: ` for a free inline function. Fix: look up `<message>` in the lists of this section.
- `clang rejected <Source> (diagnostics above)`: clang found a C++ error; its own messages are printed above. Fix:
  fix what clang reports, and check that `clang++` is on `PATH`. See
  [Mod sources and packages](#mod-sources-and-packages).
- `missing or invalid <IncludeDir>/<File> (run genueapi.py)`: the include-dir argument is not a generated UeApi
  folder: `Conv.json`, `Ops.json`, `Types.json` or `Events.json` is missing or unreadable. A folder with no
  `Version.json` and neither `Types.json` nor `Conv.json` (a mod folder, a path that does not exist) is reported so,
  naming `Conv.json`, before clang runs. Fix: pass the UeApi folder that genueapi wrote, or regenerate it. See
  [The SDK](GUIDE.md#the-sdk).
- `<IncludeDir> was written by an older genueapi (no Version.json, this assetgen needs version <N>) - regenerate it
  with AssetGen/tools/genueapi.py` (or `version <M>` for an older stamp): the UeApi folder comes from a genueapi older
  than this compiler, which would compile against it without a word wrong where it relies on what that one did not
  write (a game Blueprint's child would load before its parent's subobjects). genueapi writes `Version.json` last, so
  a run that stopped halfway leaves none either (a folder with no `Types.json` or `Conv.json` either is no UeApi, the
  message above). Fix: regenerate UeApi with the genueapi of this AssetGen, or use the
  SDK release made for it. See [The SDK](GUIDE.md#the-sdk).
- `<Class> derives from the game Blueprint <Parent>, but <IncludeDir> was generated without --game, so it does not list
  the default subobjects that Blueprint's default object exports, which this class must load after - regenerate it
  with AssetGen/tools/genueapi.py <SDK dir> <UeApi dir> --game <extracted Content dir>`: the UeApi's `Version.json`
  says genueapi ran without `--game`, so no game Blueprint header lists its default subobjects or its tail, which
  reads the same as a Blueprint that has none. The class would compile and then load before its parent's subobjects.
  A class with a native parent still compiles against such a UeApi. Fix: regenerate UeApi with `--game` and the
  game's extracted `Content` folder, or use the SDK release. See [The SDK](GUIDE.md#generating-your-own).
- `usage: assetgen verify <out-dir> <reference-dir>` (and the lines after it): an unknown subcommand or too few
  arguments, with exit code 2. Fix: `assetgen compile <source.cpp> <UeApi dir> <out dir> [--api <api dir>]`. The
  `--api` folder is where the editor stubs go, not the UeApi folder. See [Building mods](GUIDE.md#building-mods).
- `<Class> -> no API asset: <Reason>`: printed with `--api` (`generate_api` in `mods.yaml`); the build goes on. The
  class gets no editor stub, most often because `<Class> exposes no callable function or variable`: every function is
  inline or overrides an engine event, and every variable is private. Fix: nothing for the cooked mod. If Blueprints in
  the editor should see the class, give it a function that is not inline or a variable that is not private. See
  [Editor API stubs](GUIDE.md#editor-api-stubs).
- `<Asset> TODO: <Function> may set variables from calls; function-based defaults are not in the API asset until it
  emits real graphs`: printed with `--api` (`generate_api` in `mods.yaml`) for a class that has a public variable and a
  ReceiveBeginPlay or UserConstructionScript. The build goes on. The editor stub shows only the variables' defaults,
  not what that code sets. Fix: nothing. The cooked class runs the code as written. See
  [Editor API stubs](GUIDE.md#editor-api-stubs).
- `<Function> skipped: parameter <Parm> has no editor-side type yet` and
  `<Variable> skipped: variable has no editor-side type yet`: printed with `--api` when a parameter or a public
  variable has a type that the editor stub cannot show as a pin. The build goes on. The stub leaves out that function
  or variable, and the cooked class keeps it. Fix: take the value as a plain type the editor has a pin for, or accept
  that Blueprints made in the editor do not see that member. See [Editor API stubs](GUIDE.md#editor-api-stubs).
- `<Struct> skipped member <Member>: type has no editor-side asset yet` and
  `<Struct>: no member has an editor-side type yet`: the same for a UE_STRUCT's member under `--api`. The first only
  leaves the member out of the editor's copy of the struct. The second fails the compile, when no member is left. Fix:
  give the struct at least one member of a plain type, or build the mod without `generate_api`. See
  [Editor API stubs](GUIDE.md#editor-api-stubs).
- `cannot write .uasset` and `cannot write .uexp`: a package file could not be opened for writing: it is read-only or
  locked, or a folder has its name. The message names no path. For a class's editor stub it is only reported, as
  `<Class> -> no API asset: cannot write .uasset`. Fix: make the out dir or api dir writable and close whatever holds
  the file, then build again.
- `cannot write AssetRegistry.bin` and `short write on AssetRegistry.bin`: the registry could not be opened for
  writing (a missing folder or a read-only file), or the disk filled while it was written. compile writes it to
  `<root>/AssetRegistry.bin` when the out dir is `<root>/Content/<package path>`, and into the out dir otherwise.
  Fix: create the folder, make the file writable, or free space. See [Building mods](GUIDE.md#building-mods).
- `clang produced no AST for <Source>` and `could not parse clang's AST dump`: clang exited without an error but wrote
  no syntax tree, or one that is cut short or is not JSON. The tree streams from clang through a pipe and is never
  written to disk. Fix: check that the `clang++` on `PATH` runs, and compile again. Report it if it repeats.
- `assetgen: the filtered AST dump did not parse; reading it again unfiltered (clang runs again, so its warnings above
  print again)`, on stderr: before parsing clang's syntax tree, AssetGen drops the parts it never reads, and the
  parser refused what that step left. The compile runs clang again and reads the whole tree, so it takes longer and
  clang's warnings print twice; what it writes is the same. Fix: nothing in the mod. It is a bug in AssetGen: report
  it with the source.
- `<Path>: cannot read`: `assetgen registry` was given an input that does not exist or cannot be opened. Fix: pass a
  registry that assetgen wrote, such as a mod's `build/<mod>/FSD/AssetRegistry.bin`.
- `<Path>: not an AssetRegistry.bin`, `<Path>: not a UE 4.27 AssetRegistry.bin`, `<Path>: a malformed tag store`,
  `<Path>: asset tags - not a registry assetgen wrote, so not one it can merge`,
  `<Path>: asset tags, bundles or chunks, which assetgen never writes` and `<Path>: truncated`: AssetGen merges its
  rows into an AssetRegistry.bin that is already there, and that file is not one AssetGen wrote: the game's own
  registry, an editor-cooked one, one from another engine version, or a damaged file. `assetgen compile` merges into
  `<root>/AssetRegistry.bin` when the out dir is `<root>/Content/<package path>`, so compiling into an extracted game
  folder reaches the game's registry. `assetgen registry` checks its inputs the same way. Fix: compile into a clean
  staging folder, such as the one bpbuild uses, delete a damaged registry and build again, and merge only registries
  that AssetGen wrote. See [Building mods](GUIDE.md#building-mods).
- `no zero literal for <Type>`: `T()` or `T{}` of an SDK struct where the SDK tables and the compiler disagree about
  the struct's fields. It is not a mistake in the mod. Fix: regenerate UeApi with the genueapi of the same AssetGen,
  and report it if it persists. Meanwhile build the value with designated braces, `T V = { .Field = value };`. See
  [Structs](#structs).
- `reference missing: <Path>`, `FAILED <out-dir>/InitCave: <message>`, `<uasset|uexp> DIFFERS at 0x<Offset>: ...` and
  `<uasset|uexp> length differs: ...`: output of `assetgen verify`, a self-test of the package writer that rebuilds
  one known class and compares it byte for byte with an Epic-cooked original, which is not distributed. It is not part
  of building a mod. Fix: for `reference missing`, point it at the folder holding the original; for `FAILED`, create
  a writable out dir. A `DIFFERS` or `length differs` line is a writer regression: report it.

#### bpbuild.py

- `<Mod> FAILED`: `assetgen compile` failed on one of the mod's sources; its `FAILED: <message>` line is printed
  directly above. The mod's remaining sources are not compiled, the other mods still build, and the exit code is 1.
  Fix: fix what assetgen's message names. See [Building mods](GUIDE.md#building-mods).
- `<Sources> declares no UE_MOD_PACKAGE`: no `.cpp` in the mod's `sources` has a `UE_MOD_PACKAGE("...")` line.
  bpbuild reads the package with a text search, so it does not see the macro when a header holds it or another macro
  produces it. The same check runs on each `needs` dependency when the mod sets `embed: true`. It stops the whole
  build. Fix: write the `UE_MOD_PACKAGE` line in one of the mod's `.cpp` files. See
  [Building mods](GUIDE.md#building-mods).
- `<Mod> SKIP - no such source: <Paths>`: a listed source does not exist, or the mod lists no `.cpp` (`(no .cpp listed)`).
  Paths are relative to the folder that holds `mods.yaml`. The mod counts as failed; the others still build. Fix: fix
  the `sources` paths. See [Building mods](GUIDE.md#building-mods).
- ``mods.yaml: `needs` names an unknown mod: <Name>``: a `needs` entry names no mod in `mods.yaml`. It stops the
  build. A `needs` cycle is not an error. Fix: correct the name, or add the mod. See
  [Building mods](GUIDE.md#building-mods).
- `<Asset>.uasset imports <Package>, which no mod in this build produces`: after every mod built and packed, a cooked
  asset imports a package under one of this build's mod packages that no mod produced. Such an import loads without
  an error and resolves to null. bpbuild counts it as a failure (`cross-mod imports`) and exits with 1. The usual
  cause is a UE_CLASS that names the mod's folder instead of the class's own package; another is a mod that uses
  another mod's class while that mod is missing from `mods.yaml`. Fix: point UE_CLASS at `<mod package>/<Class>`, as
  in `UE_CLASS("/Game/_MyMods/MathLib/MathLib", "MathLib_C");`, and list the mod that cooks it. See
  [Mod sources and packages](#mod-sources-and-packages).
- `UnrealPak not found at <Path> - skipping the pak.`: no UnrealPak at `UNREALPAK`, at the default UE 4.27 path on
  Windows, or on `PATH` elsewhere. Every mod is still compiled and staged under `build/<mod>/FSD`, but each mod that
  needed a pak counts as failed and the exit code is 1. Fix: install UE 4.27, set `UNREALPAK=<path>`, or pass
  `--no-pak` and pack the staged files yourself. See [Building mods](GUIDE.md#building-mods).
- `UnrealPak failed (<Code>):`: UnrealPak exited with an error; the last six lines of its output follow. Fix: read
  those lines. See [Building mods](GUIDE.md#building-mods).
- ``mods.yaml: an `api_dir` entry has no `path` ``: an `api_dir` entry written as a mapping without `path:`, as in
  `api_dir: [{ ue: 4.27 }]`. Fix: `api_dir: { path: <project>/Content, ue: 4.27 }`, or a plain path. See
  [Editor API stubs](GUIDE.md#editor-api-stubs).
- `mods.yaml: api_dir <Path> wants UE <Version>; assetgen writes 4.27 only`: an `api_dir` entry with `ue:` other than
  4.27. Fix: point `api_dir` at a UE 4.27 project, or leave `ue:` out. See [Editor API stubs](GUIDE.md#editor-api-stubs).
- `no manifest at <Path>`: bpbuild found no `mods.yaml` in the folder it was given; `<Path>` is the last place it
  looked. Fix: pass the folder that holds `mods.yaml`. See [Building mods](GUIDE.md#building-mods).
- `mods.yaml lists no mods.`: the `mods:` list is empty or missing. Exit code 0. Fix: add an entry with `name` and
  `sources`. See [Building mods](GUIDE.md#building-mods).
- `usage: bpbuild.py ...`: fewer than three arguments. Fix:
  `python tools/bpbuild.py <mods dir> <UeApi dir> <assetgen> [--force] [--no-pak]`. See
  [Building mods](GUIDE.md#building-mods).

#### genueapi.py and genueassets.py

- `no <Dir>/GObjects-Dump-WithProperties.txt: run genueapi on the SDK inside its Dumper-7 dump, not a copy of it`: the
  SDK folder is not the `SDK/SDK` folder of a dump with the object dump two levels up, for example a copied SDK.
  Without the object dump, members that Dumper-7 renamed would compile under names the game does not know. Fix:
  `python tools/genueapi.py <dump>/SDK/SDK <UeApi dir>`, run inside the dump folder. See [The SDK](GUIDE.md#the-sdk).
- `... has no object dump beside it.`: genueapi run with fewer than two arguments prints only the last sentence of its
  help text, not a usage line. Fix: `python tools/genueapi.py <SDK dir> <UeApi dir>`. See [The SDK](GUIDE.md#the-sdk).
- `NOT named back, the respelling rules do not explain them (is the object dump of the same run?): <Count>, e.g.
  <Names>`: for these members, the object dump's name does not match Dumper-7's spelling, so they keep the SDK's
  spelling. Their reads, writes and defaults can miss in game with no error. The SDK and the object dump almost always
  come from different Dumper-7 runs. Fix: dump once and run genueapi on that dump. See [The SDK](GUIDE.md#the-sdk).
- `blueprint classes dropped for want of a /Game path: <Count> (re-dump with Dumper-7 FullAssetPaths=1)`: the dump was
  taken without `FullAssetPaths=1`, so these game Blueprint classes are left out of UeApi. Fix: set `FullAssetPaths=1`
  in `Dumper-7.ini`, dump again with the fork, and rerun genueapi. See [The SDK](GUIDE.md#the-sdk).
- `container method held back: <Function>`: a Kismet `Array_`, `Set_` or `Map_` function has a type genueapi cannot
  map, so it is missing from TArray, TSet or TMap. Fix: nothing; only that method is unavailable. See
  [The SDK](GUIDE.md#the-sdk).
- `SDK helpers left out, no UFunction behind them: <Count> (...)`: SDK functions with no engine function behind them,
  Dumper-7's own C++ helpers. No Blueprint can call them. Fix: nothing. See [The SDK](GUIDE.md#the-sdk).
- `out of reach: <Kind> <Count>, ...`: class functions and properties held back because one of their types has no
  mapping yet, counted by kind. Fix: nothing a mod can do; a member that is not in the headers cannot be named. See
  [The SDK](GUIDE.md#the-sdk).
- `base-class cycle between packages: <Package> -> <Package> -> ...`: the package headers would have to include each
  other in a loop, so genueapi stops with exit code 1. Fix: none from a mod; report it with the dump, as Internal
  errors below describes.
- `usage: genueassets.py <AssetRegistry.bin> <UeApi dir> <UeAssets dir> [--pak <mod pak or its extracted folder>]...`:
  not exactly three positional arguments. Fix:
  `python tools/genueassets.py <dir>/FSD/AssetRegistry.bin <UeApi dir> <UeAssets dir>`. See
  [The SDK](GUIDE.md#the-sdk).
- `UnrealPak could not extract <Pak> (UNREALPAK=<Path>)`: `--pak` named a `.pak` file and UnrealPak failed or is
  missing. Fix: set `UNREALPAK` to a working UnrealPak, or pass the extracted folder instead of the `.pak`. See
  [The SDK](GUIDE.md#the-sdk).

#### Other tools

- `usage: dumpar.py <AssetRegistry.bin>`: `dumpar.py` was run without a file. Fix: pass the registry to read, such as
  a mod's `build/<mod>/FSD/AssetRegistry.bin`. See [Testing and inspecting](GUIDE.md#testing-and-inspecting).
- `truncated at 0x<Offset>: wanted <N> bytes`, `name batch: <N> string bytes unread`, `tag store: bad begin magic`,
  `tag store: bad end magic`, `not an asset registry: guid <Guid>` and `<N> trailing byte(s) after the body`:
  `dumpar.py` could not parse the whole file as an AssetRegistry.bin. Fix: check that the file is a registry. If
  assetgen wrote it, report it. See [Testing and inspecting](GUIDE.md#testing-and-inspecting).
- `<Function>: no such export`: `runscript.py` was asked to run a function the package does not export. Fix: check
  the function name and the package's base path, which is the path without `.uasset`. The interpreter's other
  messages are listed under Internal errors below. See [Testing and inspecting](GUIDE.md#testing-and-inspecting).
- `usage: genenums.py <path to UE_4.27/Engine/Source/Runtime>` and `enum not found: <Enum>`: `genenums.py`, which
  regenerates the compiler's own `UeEnums.h` from the engine source, was run without its argument, or the
  `Script.h` or `ObjectMacros.h` it reads under that folder has no enum of that name. It is for compiler development
  only; building a mod never runs it. Fix: pass the `Engine/Source/Runtime` folder of a UE 4.27 source tree.

### Internal errors

An internal error is a check that valid mod code should never reach. It points at a bug in AssetGen or in one of its
tools, not in your mod. Some of these messages start with `internal:` or `internal error:`. Many compiler guards have
no prefix and read like ordinary refusals, so they are listed here by what they concern.

**Reporting one.** Use the **Compiler bug** issue form that the README describes. Give the AssetGen commit, the SDK
version, the smallest source that still fails, the full output and, for anything that compiled, the generated
`.uasset` and `.uexp`. The `<Class>::<Function>: ` prefix of the message names the function to cut the source down to.
Until it is fixed, write the statement it points at in another form.

- `internal error: <Exception>`, printed as `FAILED: internal error: <Exception>`: a C++ exception escaped the
  compiler, for example on a shape of clang's syntax tree it did not expect. The text after `internal error:` is the
  exception's own message.
- **Parts of clang's syntax tree that are missing.** clang always writes these parts for code it accepts:
  `assignment with a missing side`, `` `if` with a missing condition or then-branch ``,
  `` `do` with a missing body or condition ``, `` `while` with a missing condition or body ``, `` `for` with no body ``,
  `a range-for with a missing part`, `member call with no object`, `call with no callee`,
  `property access with no object: <Member>`, `struct member access with no object: <Member>`,
  `conversion operator with no object`, `empty argument expression`, ``unary `<Op>` with a missing operand``,
  `binary operator with a missing side`, `` `<Op>` with no destination `` and
  `__ClassOf__: argument DeclRefExpr has no name`.
- **Argument counts clang checks first.** `UE_NAME_SWITCH needs a name`,
  `` a TMap range-for binds exactly `auto [Key, Value]` ``, `<Intrinsic> takes exactly one argument (the address)`
  (the `__Read*__` intrinsics) and `__RefAt__ takes exactly one argument (the address)`.
- **The compiler's own bookkeeping.** `` `break` outside a loop or switch `` and `` `continue` outside a loop `` (clang
  rejects a stray one, so these mean AssetGen lost track of a loop), `TODO: assignment to a DeclRefExpr of kind
  <DeclKind>`, `call to a function on an unknown class: <Class>`, `access to a property of an unknown class: <Member>`,
  `__ClassOf__: no enclosing class in scope`, `hoisting <Intrinsic>: <Reason>`,
  `internal: <Method>'s <Which> is not in the call`,
  `internal: a TMap walked in place with an element aligned past 8 bytes`, `internal: no exact == for <Type>`,
  `internal: <Type> of <Holder> outside a class` and `internal: a signature function <Class> was to make for <Type>,
  <Name>, is imported but never made` (a delegate's signature function another class of the mod names, which the
  class declaring the delegate did not make: nothing is cooked).
- **Code lowered outside a function body.** `internal: a call outside a function body`,
  `internal: an inline call outside a function body`, `internal: a struct value outside a function body`,
  `internal: a container value outside a function body`, `internal: Contains outside a function body` and
  `internal: an update expression outside a function body`.
- **The SDK and the compiler disagree.** `internal: <Function> returns a value but has no completion delegate`: a
  latent function's value-returning overload in UeApi has no one-parameter completion delegate to take the value
  from. Report it with the UeApi header as well. `no zero literal for <Type>`, listed with assetgen above, is the
  other case.
- **The package writer's self-checks.** `import entry size drifted`, `export entry size drifted from 104 bytes` and
  `header size mismatch`: the writer checks the size of what it wrote. `<Struct>: member <Member> has no GUID suffix`
  is the same kind of check on an editor stub with `--api`. Each fails the compile.
- **Checks that are not printed today.** The bytecode emitter checks its own input while it writes the bytecode:
  `internal: Index arg is incomplete`, `internal: DynCast arg has no operand`,
  `internal: InterfaceCtx arg has no interface`, `internal: StructLit arg has no members`,
  `internal: Member arg has no base`, `internal: Call arg has no sub-call`, `internal: unknown argument kind`,
  `<Intrinsic> takes exactly one argument`, and the `__Asm__` checks listed above. Their messages are dropped and the
  compile still succeeds. A function that misbehaves in game after a clean build can be one of these; report
  it with the source.
- **walkscript.py.** `unknown op <Op> at <Offset>`: an opcode that its UE 4.27 grammar does not know. Report it with
  the package.
- **runscript.py and runvm.py**, the offline interpreters the test suite uses. runvm reports some of the same checks
  with a `vm: ` prefix and slightly different wording. Their messages are of three kinds:
  - Gaps in the interpreter, not necessarily a compiler bug: `unsupported op <Op> at mem <Offset>`,
    `unsupported expression op <Op> at mem <Offset>`, `unsupported statement op <Op> at mem <Offset>`,
    `unsupported destination op <Op>`, `unsupported context expression op <Op>`, `unsupported call <Function>` and
    `unsupported text literal type <Type> at mem <Offset>`. The run stopped at something the interpreter does not
    model; the mod may still be right. Report it so the interpreter can learn it.
  - Code that would misbehave in the real VM, which is a miscompile:
    `<Function>: reference parameter <Parm> gets a non-variable (op <Op>), which crashes the VM`,
    `<Callee> reads argument <Index> by address, and op <Op> at mem <Offset> leaves the address of what it read`,
    `op <Op> at mem <Offset> reads through op <Inner>, which leaves no address`,
    `an int32 evaluated into the 1-byte <Variable> at mem <Offset>`,
    `an int64 evaluated into the <PropertyType> <Variable> at mem <Offset>`, `Array_Append of an array onto itself`,
    `a read of map slot member <Field>`, `a store to map slot member <Field>`, `a read of a free map slot`,
    `a store to a free map slot`, `SwitchValue at mem <Offset>: its index (op <Op>) leaves no address`,
    `SwitchValue at mem <Offset>: no case is <Value>, a script exception in game`,
    `SwitchValue at mem <Offset>: a case skips to <Target>, not <Expected>`,
    `SwitchValue at mem <Offset> skips to <Target>, not <Expected>`,
    `op <Op> at mem <Offset> says <N> elements and has <M>`, `pop from an empty flow stack`,
    `ran off the end of the script` and `<Function>: no script found`. Report it with the mod.
  - `runaway loop`: more than 1,000,000 statements ran in one call. Check the loop's exit condition first; if the C++
    loop ends, report it.
