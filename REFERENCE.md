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
| Objects | [Components](#components), [Widget layouts](#widget-layouts), [Class defaults](#class-defaults), [Creating objects](#creating-objects), [Working with other objects](#working-with-other-objects), [Data assets](#data-assets), [Game assets](#game-assets), [Editing game packages](#editing-game-packages) |
| Events and waiting | [Event dispatchers](#event-dispatchers), [Latent calls](#latent-calls), [Waiting on events](#waiting-on-events), [Timers and input](#timers-and-input), [Timelines](#timelines), [Interfaces](#interfaces) |
| Network | [Replication](#replication), [RPCs](#rpcs) |
| Low level | [Pointers and memory](#pointers-and-memory), [Intrinsics](#intrinsics) |
| Lookup | [Macro and intrinsic index](#macro-and-intrinsic-index), [Diagnostics](#diagnostics) |

## Reading this reference

Most sections open with what they cover and the one rule most worth remembering. Tables follow, one row per construct:
what you write, what it does, and its status. Snippets and notes come after the tables.

Every row has an id, such as `switch.switch-code`, which is also its link anchor (`REFERENCE.md#switch.switch-code`).
`python tools/docq.py <words>` finds the rows that hold all the words and names, for each, the test in
`tools/test_bytecode.py` that proves it: that test's `proves('<id>')` line. The suite fails when a row has no proving
test and is not listed in `tools/unproved.txt`, a list that only shrinks.

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
| <a id="the-mod-package.ue-mod-package-game-mymods"></a>`UE_MOD_PACKAGE("/Game/_MyMods/Hello");` | Names the `/Game` folder for everything the source cooks. Write it once, at file scope. It is the only place the path is written: the bpbuild manifest does not repeat it. | Yes |
| <a id="the-mod-package.ue-mod-package"></a>a source with no `UE_MOD_PACKAGE` | Refused: "the source declares no UE_MOD_PACKAGE". | Refused |
| <a id="the-mod-package.ue-mod-package-2"></a>a second `UE_MOD_PACKAGE` in one source | Refused by clang: "redefinition of 'UeModPackage'". The macro declares a variable. | Refused |
| <a id="the-mod-package.a-source-with-nothing-to"></a>a source with nothing to cook | Refused: "the source declares no UE_STRUCT, UE_ENUM, class deriving from a UE class, asset or edit". A class with no base does not count. | Refused |
| <a id="the-mod-package.class-hello-public-aactor"></a>`class Hello : public AActor {};` | One package per class, named after it: `/Game/_MyMods/Hello/Hello`. The class inside is `Hello_C`, as the editor names a Blueprint's generated class, and the asset registry lists `/Game/_MyMods/Hello/Hello.Hello_C`. The name is the C++ name as written, prefix included: `UTurretDef` becomes `UTurretDef_C`. | Yes |
| <a id="the-mod-package.several-classes-in-one-source"></a>several classes in one source | Each class is its own package in the mod folder. The classes can name each other's types and share [global variables](#global-variables). | Yes |
| <a id="the-mod-package.two-declarations-that-land-on"></a>two declarations that land on one package | Refused, naming both: "would both be cooked as". Package names are compared ignoring case. | Refused |

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
| <a id="namespaces-and-folders.namespace-weapons-class-rifle"></a>`namespace Weapons { class Rifle : public AActor {}; }` | A namespace is a subfolder. `Rifle` is cooked at `/Game/_MyMods/Hello/Weapons/Rifle`, and child classes, calls, variable types and the registry all use that path. The same holds for a `UE_STRUCT`, an interface, a `UE_ENUM` and a braced asset. | Yes |
| <a id="namespaces-and-folders.rifle"></a>`Rifle` inside `namespace Weapons`, `Weapons::Rifle` outside it | Both name the same class, as in C++. | Yes |
| <a id="namespaces-and-folders.weapons-rifle"></a>`Weapons::Rifle *` inside `namespace Game::_MyMods`, where the class is `Game::_MyMods::Weapons::Rifle` | The same class, qualified only as far as the namespace needs. clang also spells a type this way, for example a `?:` over two such pointers where a parameter named after the class hides it. When another namespace also declares a `Rifle`, write the full name. | Yes |
| <a id="namespaces-and-folders.namespace-game-weaponsntools"></a>`namespace Game::WeaponsNTools::GrapplingGun { ... }` | A namespace that starts at `Game` is an absolute `/Game` path. The class leaves the mod folder and is cooked at `/Game/WeaponsNTools/GrapplingGun/<Class>`. The SDK declares each game Blueprint in the namespace of its folder, so a class that extends one can sit beside it. | Yes |
| <a id="namespaces-and-folders.namespace-tally-int32-hits"></a>`namespace Tally { int32 Hits; }` | A global variable in a namespace gets no subfolder. See [Global variables](#global-variables). | Yes |
| <a id="namespaces-and-folders.namespace-game-mymods-hello"></a>`namespace Game::_MyMods::Hello { class Hello : public AActor {}; }` beside a plain `class Hello` | Both land on `/Game/_MyMods/Hello/Hello`. Refused: "would both be cooked as". | Refused |
| <a id="namespaces-and-folders.class-pistol-public-rifle"></a>`class Pistol : public Rifle` in `namespace Weapons`, when another namespace also declares a `Rifle` | Refused: "derives from an undeclared class: Rifle". Write `Weapons::Rifle`. | Refused |

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
| <a id="sharing-a-class.ue-class-game-mymods-turrets"></a>`UE_CLASS("/Game/_MyMods/Turrets/UTurretDef", "UTurretDef_C");` in a class of a shared header | Records the class's package and its UE name. A source cooks the class only when that package is exactly the path the source would give it: its `UE_MOD_PACKAGE`, then any namespace, then the class name. Every other source that includes the header imports the class and cooks no copy. | Yes |
| <a id="sharing-a-class.ue-class-in-game-mymods"></a>`UE_CLASS_IN("/Game/_MyMods/Turrets");` in UTurretDef | The same as the row above, the class written once: it names the owning mod's `UE_MOD_PACKAGE`, and the compiler appends the namespaces and class name for the package and `_C` for the UE name. Use it for a mod's own classes; `UE_CLASS` stays for an engine class or a name that differs from the C++ one. | Yes |
| <a id="sharing-a-class.ue-class-game-mymods-turrets-2"></a>`UE_CLASS("/Game/_MyMods/Turrets", "UTurretDef_C");` | The package names the folder, not the class's own asset. Refused: "does not end in the asset that declares UTurretDef_C", with the path it should be. | Refused |
| <a id="sharing-a-class.ue-class-game-mymods-turrets-3"></a>`UE_CLASS("/Game/_MyMods/Turrets/UTurretDef", "UTurretDef");` in the mod that cooks it | A cooked Blueprint class is named `<Class>_C`. Refused: "cooking it here requires", with the right name. | Refused |
| <a id="sharing-a-class.ue-class-script-engine"></a>`UE_CLASS("/Script/Engine", "KismetSystemLibrary");` | Names an engine class: a `/Script` package and the native name, with no `_C`. The class is imported, never cooked, and needs no base. The SDK headers declare every class this way. Write one by hand for an engine class or function the SDK leaves out. | Yes |
| <a id="sharing-a-class.ue-struct-in-game-mymods"></a>`UE_STRUCT_IN("/Game/_MyMods/Turrets");`, `UE_ENUM_IN(EMood, "/Game/_MyMods/Turrets");` | The same one-owner rule for a struct and an enum. These name the owning mod's `UE_MOD_PACKAGE`, not the type's own package. See [Structs](#structs) and [Enums](#enums). | Yes |
| <a id="sharing-a-class.include-turrets-turrets-h"></a>A class, struct or enum with no marker in a header of another mod's folder: `#include "../Turrets/Turrets.h"` | That mod's, as `UE_CLASS_IN` / `UE_STRUCT_IN` / `UE_ENUM_IN` with its package would say: imported from `<its UE_MOD_PACKAGE>/<namespaces>/<Name>`, never cooked here. A file's mod is the one whose `UE_MOD_PACKAGE` the file holds, else the one of the nearest folder above it whose sources hold one, when they all name the same package. The owner cooks a struct or enum only where its own sources use it (see [Mod structs](#mod-structs-ue)); `UE_STRUCT_IN` cooks one they do not. | Yes |
| <a id="sharing-a-class.a-header-in-a-folder"></a>A header in a folder of several mods' sources (BpMods/ itself, the tests' folder), or in no mod's folder | Nobody's by location: an unmarked class in it is cooked by every mod that includes it, as before, and a struct or enum needs `UE_STRUCT_IN` / `UE_ENUM_IN` to be shared, or a plain `UE_STRUCT` / `UE_ENUM` to be cooked by each. | Yes |
| <a id="sharing-a-class.t-fire-2"></a>`T->Fire(2);`, `T->OnFired.Add(this, &Gunner::OnHeard);` on a `Turret *T`, where a shared header declares `class Turret : public AActor { public: UE_CLASS("/Game/_MyMods/Turrets/Turret", "Turret_C"); int32 Shots = 0; int32 Fire(int32 N); UE_DISPATCHER(OnFired, int32 Total); };` | Calls and binds another mod's actor class. The header declares the methods, the owner's source defines them (`int32 Turret::Fire(int32 N) { ... }`), and a user finds an instance as it would a game actor, for example with `UGameplayStatics::GetAllActorsOfClass(Turret::StaticClass(), Found)`, then `Cast<Turret>`. Compiled, not run in game. | Yes |
| <a id="sharing-a-class.otherdata"></a>`&OtherData`, where `OtherData` is another mod's asset of a class that a shared header declares without `UE_CLASS` | Refused: the reference would load as null. The message names the `UE_CLASS` line that fixes it. See [Game assets](#game-assets). | Refused |

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
| <a id="blueprint-classes.class-hello-public-aactor"></a>`class Hello : public AActor { ... };` | A Blueprint class (a BlueprintGeneratedClass) whose parent is `AActor`, as if made in the editor with that parent. | Yes |
| <a id="blueprint-classes.class-helper"></a>`class Helper { ... };` | A class with no base is plain C++. Nothing is cooked for it. | Yes |
| <a id="blueprint-classes.class-x-public-anotincluded"></a>`class X : public ANotIncluded {};` | Refused by clang, "expected class name", when no included header declares the base. Include the SDK header that declares it. | Refused |
| <a id="blueprint-classes.class-initcave-public-hello"></a>`class InitCave : public Hello {};` | A child of another class of the mod. A parent in the same source is used from there. A parent pinned with `UE_CLASS` to another mod is imported from that mod. | Yes |
| <a id="blueprint-classes.class-turret-final-public-aactor"></a>`class Turret final : public AActor { ... };` | A class with no subclass. Its functions, overrides and interface implementations aside, are cooked Final, which the editor does not let a Blueprint override, and a call to one of them reaches that function directly instead of by name; on `this` the body is usually copied in. See [Calling your own functions](#calling-your-own-functions). | Yes |
| <a id="blueprint-classes.ue-final-as-uturretbase-turret"></a>`UE_FINAL_AS(UTurretBase, Turret);` at namespace scope, after UTurretBase | Final through one leaf: declares `class Turret final : public UTurretBase {}`, the class that is made, in the namespace it is written in (its package path, as for any class). UTurretBase's own code is then compiled as if it were final: its own functions, overrides and interface implementations aside, are cooked Final, and its calls on `this` to them are direct and usually copied in. A call to one of its overrides, or to a function it inherits, goes by name, as the editor calls a function that is not Final; with Turret the only class made, it reaches the same function. UTurretBase is cooked Abstract, and any other class deriving from it is refused, so put the macro in the header beside UTurretBase: a mod that includes the header is refused a subclass too. When UTurretBase has a `UE_CLASS` (a header mods share), Turret is pinned beside it, so only the mod that owns UTurretBase cooks Turret and every other mod imports it, and a second UE_FINAL_AS on the same base is refused ("has two UE_FINAL_AS leaves"). Such a base is final only through the UE_FINAL_AS in the header that declares it, which its owner sees too: one written in another mod's own source, its `.cpp` or a header of its own that re-declares the base, is refused ("only the UE_FINAL_AS in the header that declares it ..."), since its owner would never cook that leaf. The header counts as the owner's when a source beside it includes it whose `UE_MOD_PACKAGE` is the owner's, or that has none in a folder whose only `UE_MOD_PACKAGE` is the owner's (a mod of several sources). A game Blueprint base is refused ("is the game's Blueprint ..."): no mod cooks it, so it stays as the game has it. A base left with a `= 0` method is refused ("would be abstract"): Turret brings no method of its own, so it would be abstract too and nothing could be made. | Yes |
| <a id="blueprint-classes.class-wpn-grapplinggun-long"></a>`class WPN_GrapplingGun_Long : public WPN_GrapplingGun_C` | A child of one of the game's Blueprint classes. Include the parent's `UeApi/Game/` header and derive from it; the parent is imported from the game. Put the child in the parent's `Game::` namespace to cook it beside the parent (see [Mod sources and packages](#mod-sources-and-packages)). The child loads after every default subobject the parent's default object exports, which UeApi lists only when genueapi had `--game`: against a UeApi made without it, the class is refused, saying to regenerate with `--game`. | Yes |

Notes:

- The compiler cooks any class that has a base, not only one with a UE base. A class that derives from a plain C++
  class compiles with no warning, but its parent is a Blueprint that is never written. Derive from a UE class, or from
  a mod class that has one.
- An unqualified call reaches the most-derived override, and `Base::Method()` runs the parent's body. A method
  whose every path calls itself, such as `void PreConstruct(bool D) { PreConstruct(D); ... }` meant as the parent
  call, is refused ("all paths through this function will call itself"): in the game it overflows the stack. See
  [Overrides and parent calls](#overrides-and-parent-calls) and
  [examples/GameBlueprintChild.cpp](examples/GameBlueprintChild.cpp).
- A child changes an inherited default, including one of a component that a game Blueprint parent adds, in
  `UE_DEFAULTS`. See [Class defaults](#class-defaults).
- The game starts a mod through its classes named `InitCave` and `InitSpacerig`. See
  [Running in the game](GUIDE.md#running-in-the-game) and [examples/HelloWorld.cpp](examples/HelloWorld.cpp).

### Parent classes

| You write | What it does | Status |
|---|---|---|
| <a id="parent-classes.public-aactor"></a>`: public AActor`, or any actor class | An actor Blueprint. Only a class whose parents reach `AActor` has a construction script, and so components. | Yes |
| <a id="parent-classes.public-uobject"></a>`: public UObject` | A plain object Blueprint. Create one at run time; see [Creating objects](#creating-objects). | Yes |
| <a id="parent-classes.public-ublueprintfunctionlibrary"></a>`: public UBlueprintFunctionLibrary` | A Blueprint Function Library, a class of static functions. See [Functions](#functions). | Yes |
| <a id="parent-classes.public-uprimarydataasset"></a>`: public UPrimaryDataAsset`, `: public UDataAsset` | A data asset class. Its instances are braced variables at namespace scope; see [Data assets](#data-assets). | Yes |
| <a id="parent-classes.public-usavegame"></a>`: public USaveGame`, `: public UFSDSaveGame` | A save game class. | Yes |
| <a id="parent-classes.public-uactorcomponent"></a>`: public UActorComponent`, `: public USceneComponent` | A component Blueprint, with the component class flags. Add one to an actor at run time with `AddComponentByType<T>(Owner)`, the editor's Add Component by Class node. Its own events to override are `ReceiveBeginPlay`, `ReceiveEndPlay` and `ReceiveTick`. A mod actor can also declare it with `UE_COMPONENT`; see [Declaring components](#declaring-components.ue-component-ucharges-ammo). | Yes |
| <a id="parent-classes.void-receivetick-float"></a>`void ReceiveTick(float DeltaSeconds)` in a component class | Turns the component's ticking on: it sets `PrimaryComponentTick.bCanEverTick` on the class default object, as the editor's compiler does. UActorComponent leaves it off. | Yes |
| <a id="parent-classes.public-uuserwidget"></a>`: public UUserWidget` | A widget with logic and no layout. `CreateWidget<T>` makes one. Its events, such as `Construct`, are overrides. | Yes |
| <a id="parent-classes.void-tick-fgeometry-mygeometry"></a>`void Tick(FGeometry MyGeometry, float InDeltaTime)` or `void OnPaint(FPaintContext &Context) const` in a widget class | Turns that event on for the class. The engine calls a widget's `Tick` and `OnPaint` only while `bHasScriptImplementedTick` and `bHasScriptImplementedPaint` are set on its class default object, and AssetGen sets each where the class overrides the event itself, as the editor's widget compiler does. A parent's override does not count: in a subclass without one of its own the event never runs, the parent's version included. AssetGen warns, "the Tick it inherits from ... never runs": override it in the subclass and call the parent's. | Yes |
| <a id="parent-classes.a-designer-layout-a-widget"></a>a designer layout (a widget tree) for a mod widget | Declared with `UE_WIDGET` and laid out in the constructor: see [Widget layouts](#widget-layouts). A widget class with none starts with an empty tree, under a game widget Blueprint parent too, and shows nothing of its own: AssetGen warns, "has no widget tree", unless the class or a mod class above it sets `WidgetTree->RootWidget` (in `OnInitialized`, before the widget is first shown), or the class itself overrides `OnPaint`. | Yes |

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

A widget class with logic only, and the actor that adds it to the viewport:

```cpp
class UScoreWidget : public UUserWidget {             // no tree, so it shows nothing: AssetGen warns
public:
  int32 Shown = 0;
  float Elapsed = 0;
  void Construct() { Shown += 1; }
  void Tick(FGeometry MyGeometry, float InDeltaTime) { Elapsed += InDeltaTime; }   // overriding it turns Tick on
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
- The widget events have run in game: in DRG, a widget's own `Tick` and `OnPaint` run, an inherited one does not,
  and a `Tick` below a game widget Blueprint without one runs. The component behaviour above follows from the
  engine's source; no mod component has run yet.

### Member variables

| You write | What it does | Status |
|---|---|---|
| <a id="member-variables.int32-health-100"></a>`int32 Health = 100;` | A Blueprint variable of the class. Blueprints get and set it, and it is editable in Class Defaults but not on a placed instance. | Yes |
| <a id="member-variables.bool"></a>`bool`, `uint8`, `int32`, `int64`, `float`, `FName`, `FString`, `FText` | Blueprint's Boolean, Byte, Integer, Integer64, Float, Name, String and Text variables. | Yes |
| <a id="member-variables.emood-mood"></a>`EMood Mood;` | An enum variable. A `uint8` enum is cooked as a Byte of that enum, except a game `enum class`, which is an enum property over a byte as in the editor; an `int32` or `int64` enum is an enum property over that integer. See [Enums](#enums). | Yes |
| <a id="member-variables.aactor-target"></a>`AActor *Target;`, `UClass *Cls;`, `TSubclassOf<AActor> Kind;` | Object and class references. See [Types](#types). | Yes |
| <a id="member-variables.uscenecomponent-spare"></a>`USceneComponent *Spare;`, `TArray<UStaticMeshComponent *> Pieces;`, `FHitResult LastHit;` | A reference to a component or widget, or a container or struct that holds one, is flagged instanced, as the editor flags it, so an actor spawned from the class gets its own copy of what the class default points at, not the default's. | Yes |
| <a id="member-variables.tsoftobjectptr-t"></a>`TSoftObjectPtr<T>`, `TSoftClassPtr<T>`, `TScriptInterface<I>` | Soft object and soft class references, and an interface reference. | Yes |
| <a id="member-variables.tenum-emood"></a>`TEnum<EMood>` | The enum itself, plus `.Name()` and `.String()`. See [TEnum](#tenum). | Yes |
| <a id="member-variables.tenummask-emood"></a>`TEnumMask<EMood>` | An int32 bitmask over the enum, with `.Has()`, `.Add()`, `.Remove()` and `.Clear()`. See [TEnumMask](#tenummask). | Yes |
| <a id="member-variables.fvector-home"></a>`FVector Home;`, `FAmmo Ammo;` | An engine or game struct, or a mod `UE_STRUCT`. See [Structs](#structs). | Yes |
| <a id="member-variables.tarray-fvector"></a>`TArray<FVector>`, `TSet<int32>`, `TMap<FName, int32>` | Array, Set and Map variables. A container inside a container goes through a generated wrapper struct; see [Containers](#containers). | Yes |
| <a id="member-variables.tset-bool"></a>`TSet<bool>`, `TMap<FText, int32>`, `TSet<FRotator>` | Refused: "cannot hash, and the engine hashes each one". A set element or map key must hash: not a bool, an FText or a delegate, and an engine struct only if it has a GetTypeHash (FVector, FGuid, FGameplayTag and others do; FRotator, FHitResult, FTransform do not). A `UE_STRUCT` always hashes. | Refused |
| <a id="member-variables.ue-dispatcher-onscored-int32"></a>`UE_DISPATCHER(OnScored, int32 Score);` | An event dispatcher. See [Event dispatchers](#event-dispatchers). | Yes |
| <a id="member-variables.int32-raw"></a>`int32 *Raw;` | A pointer to anything but a UObject is an address, held in an Integer64 variable. See [Pointers and memory](#pointers-and-memory). | Yes |
| <a id="member-variables.uint16-w"></a>`uint16 W;`, `double D;` | Refused: "unimplemented property W: uint16". UE 4.27 Blueprint has no such type; see [Types](#types). | Refused |
| <a id="member-variables.a-member-whose-type-is"></a>a member whose type is a game class the source only forward-declares | Refused, with the `#include "UeApi/Game/<Class>.h"` line to add. | Refused |
| <a id="member-variables.static-constexpr-int32-kslots-12"></a>`static constexpr int32 kSlots = 12;`, `static inline const TArray<int32> kPrimes = {2, 3, 5};` | No variable is cooked. Each use is the value, and a braced list is built where it is used. See [Constants](#constants). | Yes |
| <a id="member-variables.static-inline-float-loose-0"></a>`static inline float Loose = 0.25f;` | A static that is not const is refused where it is used: a Blueprint class has no static storage. See [Constants](#constants). | Refused |
| <a id="member-variables.const-int32-limit-3"></a>`const int32 Limit = 3;` | A read-only variable. See Access, read-only and categories below. | Yes |

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
| <a id="defaults.int32-budget-kslots-2-1"></a>`int32 Budget = kSlots * 2 + 1;` | The compiler works the initializer out when the mod is built and writes it into the class default object: the variable's default value in Class Defaults. Nothing runs in game to set it. | Yes |
| <a id="defaults.int32-hits-0"></a>`int32 Hits = 0;`, `int32 Hits;` | A zero default writes nothing, as the engine cooks it, and the variable starts at zero. The same holds for the zero enumerator, `nullptr` and an empty string. | Yes |
| <a id="defaults.int32-hits"></a>`int32 Hits{};`, `EMood Mood = EMood();`, `int32 Budget{25};` | `{}` and `T()` are the zero, and write nothing; braces around one value are that value. | Yes |
| <a id="defaults.int32-x-ukismetmathlibrary"></a>`int32 X = UKismetMathLibrary::RandomInteger(5);` | Refused: "a default is a value known when the mod is built". Set such a value in `ReceiveBeginPlay` or `UserConstructionScript`. | Refused |
| <a id="defaults.static-constexpr-int32-kseed-fnv"></a>`static constexpr int32 kSeed = Fnv("types");` then `int32 Seed = kSeed;` | A call counts as a default only through a `consteval` function, which clang runs itself. A call to a plain `constexpr` function is refused like any call. See [Constants](#constants). | Yes |
| <a id="defaults.tsubclassof-aactor-kind-aactor"></a>`TSubclassOf<AActor> Kind = AActor::StaticClass();`, `UClass *Any = AMyEnemy::StaticClass();` | The class is the default, as picking it under Class Defaults makes it: an engine or game class, a game Blueprint, or one of the mod's own classes, this one included. The same holds in a container's braces, a struct value, a `UE_DEFAULTS` statement and an asset's braces. `= nullptr` is the zero. | Yes |
| <a id="defaults.tsubclassof-apawn-kind-aactor"></a>`TSubclassOf<APawn> Kind = AActor::StaticClass();` | Refused: "a TSubclassOf<APawn> holds APawn or a subclass of it, and AActor is neither". C++ converts any class to a `TSubclassOf`, but the editor's class picker offers only those. | Refused |
| <a id="defaults.tsoftclassptr-aactor-soft-game-a"></a>`TSoftClassPtr<AActor> Soft = "/Game/A/BP_A.BP_A_C";` | A soft reference's default is its path. `"/Game/Dir/Pkg"` means `Pkg.Pkg`, so write the full path for a Blueprint class. See [Types](#types). | Yes |
| <a id="defaults.tarray-uint8-payload-embedfile"></a>`TArray<uint8> Payload = __EmbedFile__("data/blob.bin");` | Reads the file when the mod is built, from a path relative to the source's folder, and writes its bytes into the default, one element per byte. The compile prints `embed          -> Payload  (3 bytes)`. The file is not needed at run time. `__EmbedFile__` comes from `Intrin.h` in AssetGen's `include/` folder, included by its path as `Objects.h` is. | Yes |
| <a id="defaults.embedfile-missing-bin"></a>`__EmbedFile__("missing.bin")`, `__EmbedFile__(kPath)` | Refused: a missing or empty file ("cannot read (or empty)"), and a path that is not a string literal ("the path must be a string literal"). It works only as the initializer of a `TArray<uint8>` member; clang refuses it for any other array type. | Refused |
| <a id="defaults.ue-enum-map-emood-fname"></a>`UE_ENUM_MAP(EMood, FName, Names);` | Declares a `TMap<EMood, FName>` member holding a name table that the compiler fills in when the mod is built, as a member default only. See [Enums](#enums). | Yes |
| <a id="defaults.amyactor-charges-5"></a>`AMyActor() { Charges = 5; }`, `AMyActor() : Charges(5) {}` | The same as `int32 Charges = 5;`: a default constructor is read as the class's defaults. See [Constructors](#constructors). | Yes |

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
- A member with the name of an inherited variable is refused ("... already has a variable X; rename it"): it would be a
  second variable that hides the parent's. To change the inherited default, use `UE_DEFAULTS`; see
  [Class defaults](#class-defaults).

### Access, read-only and categories

| You write | What it does | Status |
|---|---|---|
| <a id="access-read-only.public"></a>`public:`, `protected:`, `private:` before a function | The function is cooked as public, protected or private, and the editor allows or refuses a call node to it the same way. An override keeps its parent's access. Nothing is checked at run time. | Yes |
| <a id="access-read-only.private"></a>`private:` before a variable | The variable is still cooked on the class, but the editor API stub leaves it out. A Blueprint variable has no other access: a protected one and a public one look the same. | Yes |
| <a id="access-read-only.const-int32-limit-3"></a>`const int32 Limit = 3;` | A read-only variable (BlueprintReadOnly): the editor offers a Get node and no Set node. The initializer is the default. clang refuses a write in C++, and the Blueprint VM does not check it. | Yes |
| <a id="access-read-only.ue-readonly-int32-cap-5"></a>`UE_READONLY int32 Cap = 5;` | Read-only the same way (BlueprintReadOnly), but it can still be written: a subclass's `UE_DEFAULTS` may give it a default, and a write from code compiles with `warning: <Class>::<Function>: Cap is BlueprintReadOnly; the editor would not set it`, because the VM allows the write (for example, setting a deferred spawn's members before FinishSpawning). UeApi declares the engine's and game's BlueprintReadOnly properties this way. | Yes |
| <a id="access-read-only.ue-category-turret-setup"></a>`UE_CATEGORY("Turret\|Setup");` | Gives the functions and variables declared after it that category. `\|` starts a subcategory, and `UE_CATEGORY("")` ends the category. It is positional, like an access specifier. | Yes |
| <a id="access-read-only.ue-tooltip-adds-charges-ue"></a>`UE_TOOLTIP("Adds charges.") UE_KEYWORDS("refill reload") void Recharge(int32 By);` | The function's tooltip, shown over its call node and in the action menu, and extra words the action menu's search finds it by: the Description and Keywords of the editor's function details. Like a category, they reach the editor API stub only. The cooked game keeps no editor metadata, so the pak carries neither, and nothing changes in game. | Yes |
| <a id="access-read-only.ue-tooltip-count-int32-n"></a>`UE_TOOLTIP("Count") int32 N;` | Refused: "marks no method". They describe the method that follows them. | Refused |

```cpp
class Turret : public AActor {
public:
  UE_CATEGORY("Turret|Setup");
  void Configure() {}
  int32 Charges;
  UE_CATEGORY("");                  // back to none
  int32 Plain;
  UE_TOOLTIP("Fires once.") UE_KEYWORDS("shoot shot")
  void Fire() {}
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
  editor can call the mod. `UE_CATEGORY` is written only there, as the category of each variable and function, and so
  are `UE_TOOLTIP` and `UE_KEYWORDS`: the cooked class has no categories, and without `generate_api` the macros do
  nothing. The stub keeps a private function, flagged private. A stub's tooltip and keywords have been checked in UE
  4.27.2: they are the compiled function's ToolTip and Keywords. Whether the editor lists the members under their
  category has not been checked. See [Editor API stubs](GUIDE.md#editor-api-stubs).

### Names

| You write | What it does | Status |
|---|---|---|
| <a id="names.index-0-7"></a>`Index_0 = 7;`, for a member the SDK spells `Index_0` | Write the SDK's spelling. Dumper-7 respells names that C++ cannot use: a clash gets a suffix (`Name` becomes `Name_0`), an illegal character becomes `_` (`Audio Flying` becomes `Audio_Flying`), and a leading digit becomes a word (`3P` becomes `ThreeP`). The SDK keeps the real name beside the member, and everything the compiler cooks uses the real name. | Yes |
| <a id="names.class-gr-e-public-aactor"></a>`class Größe : public AActor { FName Umlaut = "Größe"; };` | Non-ASCII class, member and asset names, and non-ASCII name and string defaults, are cooked and registered correctly. | Yes |
| <a id="names.using-ftarget-aactor"></a>`using FTarget = AActor;`, then `FTarget *Aimed;` | An alias of a UObject class is still an object reference, not an address. It works at namespace scope and at class scope. | Yes |
| <a id="names.using-fmoods-tarray-emood"></a>`using FMoods = TArray<EMood>;`, `using Factory = TScriptInterface<IFoo>;` | A global alias of a template type is that type wherever it is used, in out-of-line method definitions too. A global alias of a class is that class the same way, so an override may spell a parameter through it. | Yes |
| <a id="names.using-grapple-game-weaponsntools"></a>`using Grapple = Game::WeaponsNTools::GrapplingGun::WPN_GrapplingGun_C;` | An alias to a game class's full `Game::` path. The SDK gives a game class a short name at global scope only when no other game package has a class of that name. For one that does, write such an alias. | Yes |

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
| <a id="namespace-scope.int32-counter-5"></a>`int32 Counter = 5;` | A variable that every class of the source shares. Its holder is a generated `UObject` class at `<UE_MOD_PACKAGE>/Counter`, with one member of the same name and type. The initializer is its default. | Yes |
| <a id="namespace-scope.namespace-tally-int32-hits"></a>`namespace Tally { int32 Hits; }` | A namespace gives no subfolder here: the holder is `<UE_MOD_PACKAGE>/Tally__Hits`. A global with no initializer starts at zero, as in C++. | Yes |
| <a id="namespace-scope.counter-by"></a>`Counter += By;`, `++Tally::Hits;`, `return Counter++;` | Assignment, compound assignment, `++` and a postfix read all work on a global. | Yes |
| <a id="namespace-scope.constexpr-int32-kstep-3"></a>`constexpr int32 kStep = 3;`, `const int32 kMask = 1 << 4;` | A `const` or `constexpr` global is not stored: each use is its value. See [Constants](#constants). | Yes |
| <a id="namespace-scope.a-global-that-no-function"></a>a global that no function uses | Gets no holder class. Nothing is written for it. | Yes |
| <a id="namespace-scope.umooddef-globaldef"></a>`UMoodDef GlobalDef;`, a variable of a UObject class by value | Refused where it is used: "is an asset, not a value". Declare a braced asset of the mod or name an existing one with `UE_ASSET_AT`, and point at it with `&`. See [Data assets](#data-assets). | Refused |
| <a id="namespace-scope.extern-int32-shared"></a>`extern int32 Shared;`, used but not defined in this source | Refused: "declared extern but not defined in this source". | Refused |

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
| <a id="globals-across-sources.int32-counter-5"></a>`int32 Counter = 5;` in two sources with different `UE_MOD_PACKAGE`s | Two separate variables. Each holder sits under its own source's package, so they never collide. | Yes |
| <a id="globals-across-sources.int32-counter-5-2"></a>`int32 Counter = 5;` in two sources with the same `UE_MOD_PACKAGE` | Both sources write the same holder package, byte for byte, so both name one variable. | Yes |
| <a id="globals-across-sources.extern-int32-counter"></a>`extern int32 Counter;` to reach another source's global | Refused, as above. Each source that uses a global cooks its holder, so the global is defined where it is used. | Refused |

## Types

This section covers the types a member, parameter, local or return value can have, and the Blueprint variable type each
becomes: numbers, strings, object and class references, and soft references. Enums, structs and containers have
sections of their own. The rule to remember: a variable can only have a type that UE 4.27 Blueprint has, so there is no
`double`, no `uint32` and no weak pointer.

### Variable types

| You write | What it does | Status |
|---|---|---|
| <a id="variable-types.bool-barmed-true"></a>`bool bArmed = true;` | A Boolean variable. | Yes |
| <a id="variable-types.uint8-charges-3"></a>`uint8 Charges = 3;` | A Byte variable. `unsigned char` is the same type. | Yes |
| <a id="variable-types.int32-kills"></a>`int32 Kills;` | An Integer variable. `int` is the same type. | Yes |
| <a id="variable-types.int64-stamp"></a>`int64 Stamp;` | An Integer64 variable. `long long` is the same type. | Yes |
| <a id="variable-types.float-ratio-0-5f"></a>`float Ratio = 0.5f;` | A Float variable. | Yes |
| <a id="variable-types.fstring-label"></a>`FString Label;` | A String variable. A `const char *` member or parameter is a String too. | Yes |
| <a id="variable-types.fname-key-alpha"></a>`FName Key = "alpha";` | A Name variable. | Yes |
| <a id="variable-types.ftext-caption"></a>`FText Caption;` | A Text variable. | Yes |
| <a id="variable-types.aactor-target"></a>`AActor *Target;` | An Object Reference variable. See the next table. | Yes |
| <a id="variable-types.emood-mood"></a>`EMood Mood;` | An enum variable: [Enums](#enums). | Yes |
| <a id="variable-types.fvector-home"></a>`FVector Home;` | A struct variable: [Structs](#structs). | Yes |
| <a id="variable-types.tarray-int32-scores"></a>`TArray<int32> Scores;` | An Array, Set or Map variable: [Containers](#containers). | Yes |
| <a id="variable-types.uint32-mask"></a>`uint32 Mask;` | Refused (`unimplemented property Mask: uint32`): Blueprint 4.27 has no such type. The same holds for `int8`, `int16`, `uint16` and `uint64`, as a member, parameter, local or return value. Use `int32` or `int64`. | Refused |
| <a id="variable-types.double-d"></a>`double D;` | Refused (`unimplemented property D: double`): Blueprint reals in 4.27 are `float`. Use `float`. | Refused |

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
| <a id="object-and-class.apawn-spotted"></a>`APawn *Spotted;` | An Object Reference variable of that class. Its default can only be `nullptr` or `&Asset`. | Yes |
| <a id="object-and-class.using-ftarget-aactor"></a>`using FTarget = AActor;` then `FTarget *Aimed;` | An alias of a class is still an Object Reference: [Classes and variables](#classes-and-variables). | Yes |
| <a id="object-and-class.int32-p"></a>`int32 *P;` | A pointer to a type that is not a UObject is an int64 address: [Pointers and memory](#pointers-and-memory). | Yes |
| <a id="object-and-class.tsubclassof-aactor-kind"></a>`TSubclassOf<AActor> Kind;` | A Class Reference limited to `AActor` and its subclasses. | Yes |
| <a id="object-and-class.uclass-raw"></a>`UClass *Raw;` | A Class Reference that takes any class. | Yes |
| <a id="object-and-class.raw-kind-kind-raw"></a>`Raw = Kind; Kind = Raw;` | Each converts to the other. Nothing checks at run time that the class in `Raw` derives from `AActor`. | Yes |
| <a id="object-and-class.tsubclassof-aactor-kind-aactor"></a>`TSubclassOf<AActor> Kind = AActor::StaticClass();` | The class as the default. `TSubclassOf<APawn> Kind = AActor::StaticClass();` is refused: a class the `TSubclassOf` does not take. See [Classes and variables](#classes-and-variables). | Yes |
| <a id="object-and-class.tscriptinterface-ihealth-health"></a>`TScriptInterface<IHealth> Health;` | An Interface variable: [Interfaces](#interfaces). | Yes |
| <a id="object-and-class.data-instigator-k2-getpawn"></a>`Data.Instigator->K2_GetPawn()`, `AActor *A = Data.DamageCauser;`, `if (Data.Component)`, `Cast<AProjectileBase>(Data.DamageCauser)` on a game struct's `TWeakObjectPtr` member (FDamageData's) | Reads it like an object reference, as the editor's Break node does: EX_StructMemberContext of the member, whose read hands out the object, None once it is gone. Assigning one is refused ("an assignment to a TWeakObjectPtr member"): the game's are read-only, and EX_Let would store an object into the weak pointer's bytes. | Yes |
| <a id="object-and-class.tweakobjectptr-aactor-last"></a>`TWeakObjectPtr<AActor> Last;` | Refused: "TODO: unimplemented property Last" (a member or local of the mod's own). Keep an object pointer and test it with `if (Obj)`, or keep a soft reference. | Refused |

Notes:
- A class that the headers only forward-declare cannot be a variable's type. The message says which `UeApi/Game/...`
  header to `#include`.

### Casts and class values

| You write | What it does | Status |
|---|---|---|
| <a id="casts-and-class.cast-apawn-obj"></a>`Cast<APawn>(Obj)` | The Cast To node: the object if it is an `APawn`, otherwise null. The class must be one the headers declare, or the cast is refused with "Cast<> to an unknown class". | Yes |
| <a id="casts-and-class.static-cast-apawn-a"></a>`static_cast<APawn *>(A)` | Compiles to nothing. The reference passes through unchecked, as in C++. Use `Cast<T>` when the object may not be a `T`. | Yes |
| <a id="casts-and-class.aactor-u-p"></a>`AActor *U = P;` | An upcast. It compiles to nothing. | Yes |
| <a id="casts-and-class.aactor-staticclass"></a>`AActor::StaticClass()` | The class as a constant, like a class picked on a pin. On one of the mod's own classes it names that class: [Creating objects](#creating-objects). | Yes |
| <a id="casts-and-class.other-getclass"></a>`Other->GetClass()` | The Get Class node. The other object helpers: [Working with other objects](#working-with-other-objects). | Yes |
| <a id="casts-and-class.if-target"></a>`if (Target)` | Is Valid, so an object that is being destroyed tests false. `Target != nullptr` is a plain compare. See [Working with other objects](#working-with-other-objects). | Yes |
| <a id="casts-and-class.a-b"></a>`A < B` on object pointers | Refused: Blueprint compares object references only with `==` and `!=`. See [Operators](#operators). | Refused |

### Soft references

| You write | What it does | Status |
|---|---|---|
| <a id="soft-references.tsoftobjectptr-utexture2d-icon"></a>`TSoftObjectPtr<UTexture2D> Icon = "/Game/UI/Icons/T_Icon.T_Icon";` | A Soft Object Reference. Its default is the asset's path. Nothing is loaded with the class. | Yes |
| <a id="soft-references.tsoftclassptr-aactor-kind-game-a"></a>`TSoftClassPtr<AActor> Kind = "/Game/A/BP_A.BP_A_C";` | A Soft Class Reference. For a Blueprint class the path names the generated class, `<Name>_C`. | Yes |
| <a id="soft-references.tarray-tsoftclassptr-aactor"></a>`TArray<TSoftClassPtr<AActor>> Kinds = {"/Game/A/BP_A.BP_A_C"};` | An array of soft references, with paths as its default. | Yes |
| <a id="soft-references.game-dir-pkg"></a>`= "/Game/Dir/Pkg"` in a default | A path with no object name means `Pkg.Pkg`. | Yes |
| <a id="soft-references.icon-game-ui-icons-t"></a>`Icon = "/Game/UI/Icons/T_Icon.T_Icon";` in a function | A soft path constant, written exactly as given. | Yes |
| <a id="soft-references.seen-a"></a>`Seen = A;` | An object converts to a soft reference (To Soft Object Reference); a class converts to a soft class reference. | Yes |
| <a id="soft-references.kinds-contains-a-getclass"></a>`Kinds.Contains(A->GetClass())` | The class converts to a soft class reference where one is wanted, as the editor does. | Yes |
| <a id="soft-references.aactor-seen"></a>`(AActor *)Seen`, `(UClass *)Kind` | Resolve Soft Reference: the object or class, or null if it is not loaded. The cast is explicit so that the null is visible in the source. | Yes |
| <a id="soft-references.fstring-path-seen"></a>`FString Path = Seen;` | The path as a string, and through it an FName or FText. | Yes |
| <a id="soft-references.fstring-tsoftclassptr-aactor-a"></a>`FString(TSoftClassPtr<AActor>(A->GetClass()))` | A class's path. `FString(A->GetClass())` would give its name. | Yes |
| <a id="soft-references.kind-other"></a>`Kind == Other` | Compares the two paths (Equal (Soft Class Reference)), whatever class each one is of. Nothing is loaded. | Yes |
| <a id="soft-references.tsoftobjectptr-uobject-any-icon"></a>`TSoftObjectPtr<UObject> Any = Icon;` | A soft reference to a subclass passes where one to its parent is wanted, with no conversion node. | Yes |
| <a id="soft-references.ukismetsystemlibrary-loadasset"></a>`UKismetSystemLibrary::LoadAsset_Blocking(Icon)` | Loads the asset now and returns it. `LoadClassAsset_Blocking(Kind)` does the same for a class. | Yes |
| <a id="soft-references.ukismetsystemlibrary-loadasset-2"></a>`UKismetSystemLibrary::LoadAsset(Icon)` | The latent Async Load Asset node: the function waits, and the call's value is the object. `LoadAssetClass` loads a class. See [Latent calls](#latent-calls). | Yes |
| <a id="soft-references.ukismetsystemlibrary"></a>`UKismetSystemLibrary::IsValidSoftObjectReference(Icon)` | Whether a path is set. It loads nothing. | Yes |

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
| <a id="number-literals.int32-mask-0xff"></a>`int32 Mask = 0xFF;` | An int32 constant. | Yes |
| <a id="number-literals.int64-big-1234567890123"></a>`int64 Big = 1234567890123;` | An int64 constant when the value does not fit in int32. A literal is never truncated. | Yes |
| <a id="number-literals.int64-18446744073709551615ull"></a>`(int64)18446744073709551615ULL` | A uint64 literal past int64's range keeps its bits, as the C++ cast does: this is -1. | Yes |
| <a id="number-literals.float-reach-300-f"></a>`float Reach = 300.f;` | A Float constant. A literal beyond float's range, `(float)1e39`, is infinity. | Yes |
| <a id="number-literals.x-0-5"></a>`X * 0.5`, `X > 0.5` | Refused (`no Kismet conversion from float to double`): C++ makes this double math. Write `0.5f`. | Refused |
| <a id="number-literals.float-y-0-5"></a>`float Y = 0.5;`, `return 0.25;`, `FVector(1.0, 2.0, X)` | A double literal on its own converts to float where a float is wanted. So does a default, `float Delay = 0.75;`. | Yes |
| <a id="number-literals.a"></a>`'A'`, `('a' - 'A')` | The int constant of its code unit. A plain `char` is signed, so `'\xff'` is -1. | Yes |
| <a id="number-literals.nullptr"></a>`nullptr` | None for an object or class. For a `TScriptInterface` it is the null interface. Where an int64 address is wanted it is 0. | Yes |
| <a id="number-literals.int32-n-7"></a>`int32 N{7};`, `EMood M{EMood::Angry};` | Braces around one value of a type that is not a struct are that value. | Yes |
| <a id="number-literals.emood-m"></a>`EMood M{};`, `int32 N = int32();`, `AActor* A{};`, `Mood = {};`, `return {};` | C++'s value-initialisation: the type's zero, the zero enumerator or None. In a function and as a default, where a zero writes nothing. In `UE_DEFAULTS` the zero is written, as `= 0` is (see [Class defaults](#class-defaults)). | Yes |
| <a id="number-literals.text"></a>`"text"`, `L"wide"` | A String, Name or Text constant: [Strings and text](#strings-and-text). | Yes |

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
| <a id="number-conversions.ratio-count"></a>`Ratio = Count;` | int32 to float (Conv_IntToFloat). Every widening is the matching Kismet conversion node: int32 to int64, uint8 to int32, bool to int32, uint8 to float. A literal converts with no node. | Yes |
| <a id="number-conversions.big-count"></a>`Big = Count;` | int32 to int64. A uint8 or bool reaches int64 through int32. | Yes |
| <a id="number-conversions.int32-n-x"></a>`int32 N = X;`, `(int32)X`, `(int64)X` | float to integer, truncated toward zero as in C++ (the Truncate node): -2.75 becomes -2. | Yes |
| <a id="number-conversions.uint8-x"></a>`(uint8)X` with `float X` | Refused (`no Kismet conversion from float to uint8`). Write `(uint8)(int32)X`, which then wraps the int as C++ does. | Refused |
| <a id="number-conversions.uint8-v"></a>`(uint8)V`, `(int32)Big` | A narrowing cast wraps: 511 becomes 255, and 2^32+5 becomes 5. It happens even in the middle of a chain of casts, and an implicit int64 to int32 assignment wraps the same way. | Yes |
| <a id="number-conversions.float-big"></a>`(float)Big` with `int64 Big` | Warns: UE 4.27 has no int64 to float node, so the value goes through int32. A value outside int32's range wraps (2^32+5 becomes 5.0). | Warns |
| <a id="number-conversions.charges-200"></a>`Charges += 200;` with `uint8 Charges` | The byte is promoted to int32 for the math, and the store back wraps modulo 256, as in C++. | Yes |
| <a id="number-conversions.sum-n"></a>`Sum / N` with two int32 | Integer division, as in C++. Each operation takes the type C++ promotes to: float if either side is float, otherwise int64 if either side is int64, otherwise int32. | Yes |
| <a id="number-conversions.a-b"></a>`A / B` with `int32 A`, `float B` | `A` converts to float, then a float divide. | Yes |
| <a id="number-conversions.bool-f"></a>`(bool)F`, `bool B = Count;` | True when nonzero, as in C++. An int32 goes through ToBool (integer) (Conv_IntToBool); a float, int64, byte or enum is compared with zero, so a NaN is true. Conditions of every type: [Statements and control flow](#statements-and-control-flow). | Yes |
| <a id="number-conversions.row"></a>`+`, `%`, `<<`, `~` and the other operators | The Kismet math nodes: [Operators](#operators). A shift amount must be a constant, and `%` on int64 is refused. | Yes |
| <a id="number-conversions.take-hit-location"></a>`Take(Hit.Location)` with `Take(const FTransform&)`, `FVector V = Hit.ImpactPoint;` | A struct derived from another (FVector_NetQuantize from FVector) takes its base's conversion node, here Conv_VectorToTransform, and passes where its base is wanted as is. | Yes |

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
| <a id="string-literals.label-ready"></a>`Label = "Ready";` | A literal where an FString, FName or FText is wanted is a constant of that type, like a value typed on a pin. No conversion node runs. | Yes |
| <a id="string-literals.fname-named-plain-name"></a>`FName Named = "plain name";`, `FText Literal = "plain text";` | The same as a default. | Yes |
| <a id="string-literals.fstring"></a>`FString()`, `FName()`, `FText()` | The empty string, None and the empty text. | Yes |
| <a id="string-literals.fname-umlaut-gr-e"></a>`FName Umlaut = "Größe";` | Non-ASCII text is written as UTF-16 and reads back exactly as written, in code and in defaults. It is the same FName you get by converting the string at run time. | Yes |
| <a id="string-literals.fstring-wide-l-wide"></a>`FString Wide = L"wide";` | An `L`, `u` or `U` literal is a Unicode string constant. | Yes |

Notes:
- A text literal, in code or as a default, is culture-invariant. It is not gathered for localization.

### Joining strings

| You write | What it does | Status |
|---|---|---|
| <a id="joining-strings.fstring-kills-count-ratio-ratio"></a>`FString("Kills: ") + Count + ", ratio " + Ratio` | Every `+` that involves a string converts each operand to a string (the ToString nodes) and joins them with Append. Strings, names, texts, numbers, bools, objects and structs with a ToString node can all be mixed. | Yes |
| <a id="joining-strings.key-label-key"></a>`Key = Label + "_key";` | The joined string converts to the destination: String to Name here. | Yes |
| <a id="joining-strings.caption-label-key-caption"></a>`Caption = Label + Key + Caption;` | String to Text. The text is culture-invariant, since it comes from a string. | Yes |
| <a id="joining-strings.kills-count"></a>`"Kills: " + Count`, `Count + " left"` | In C++ this is pointer arithmetic; AssetGen reads it as the concatenation you meant. | Yes |
| <a id="joining-strings.a-n-b"></a>`"a" + N + "b"`, `"a" + Ratio` | Refused by clang (pointer + pointer, pointer + float). Only one literal and an integer can start a chain. Start it with `FString("a")`. | Refused |
| <a id="joining-strings.fstring-this"></a>`FString(" (") + this + ")"` | An object converts to its name (ToString (Object)). A class converts to its name too, not its path. | Yes |
| <a id="joining-strings.fstring-home-home"></a>`FString("Home: ") + Home` | A struct with a Kismet ToString node (FVector, FRotator, FTransform, FLinearColor, FVector2D, FIntPoint...) converts to that node's text. | Yes |

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
| <a id="converting-strings.key-label"></a>`Key = Label;` | FString to FName (String to Name). | Yes |
| <a id="converting-strings.caption-key"></a>`Caption = Key;` | FName to FText (Name to Text). | Yes |
| <a id="converting-strings.fstring-s-caption"></a>`FString S = Caption;` | FText to FString (Text to String). FText to FName goes through FString. | Yes |
| <a id="converting-strings.fstring-a-count"></a>`FString A = Count;`, `FString C = bDone;` | A number or bool converts through its ToString node: 3 becomes "3", false becomes "false". | Yes |
| <a id="converting-strings.caption-big"></a>`Caption = Big;` with `int64 Big` | UE 4.27 has no int64 to string node, so an int64 goes Int64 to Text, then Text to String when a string is wanted. Integer text has no digit grouping. | Yes |
| <a id="converting-strings.fname-n-big"></a>`FName N = Big;` with `int64 Big` | Refused. clang calls the conversion ambiguous, and `FName(FString(Big))` is refused with "no Kismet conversion from int64 to FName". Convert through a local: `FString S = Big; Key = S;`. | Refused |
| <a id="converting-strings.count-int-label"></a>`Count = int(Label);`, `float F = float(Label);` | String to Int, String to Float. Text that is not a number gives 0. The cast must be explicit. FName and FText parse by way of FString. | Yes |
| <a id="converting-strings.fstring-path-soft"></a>`FString Path = Soft;` | A soft reference's path: [Types](#types). | Yes |

### Comparing and testing strings

| You write | What it does | Status |
|---|---|---|
| <a id="comparing-and-testing.s-gold"></a>`S == "gold"` | FString `==` ignores case (Equal, Case Insensitive), as UE's own FString `==` does. | Yes |
| <a id="comparing-and-testing.ukismetstringlibrary-equalequal"></a>`UKismetStringLibrary::EqualEqual_StrStr(A, B)` | The case-sensitive compare. | Yes |
| <a id="comparing-and-testing.a-b"></a>`A != B` with two FNames | Names compare ignoring case. | Yes |
| <a id="comparing-and-testing.a-b-2"></a>`A == B` with two FTexts | The Equal, Case Insensitive (text) node: texts compare ignoring case too. | Yes |
| <a id="comparing-and-testing.a-b-3"></a>`A < B` with two FStrings | Refused by clang (`invalid operands to binary expression`): no Kismet node orders strings. | Refused |
| <a id="comparing-and-testing.if-n"></a>`if (N)`, `!N` with `FName N` | Tests `N != None`. Conditions of every type: [Statements and control flow](#statements-and-control-flow). | Yes |

### String functions

| You write | What it does | Status |
|---|---|---|
| <a id="string-functions.ukismetstringlibrary-len-s"></a>`UKismetStringLibrary::Len(S)` | The Len node. Every string operation is a `UKismetStringLibrary` static: `Mid`, `Replace`, `ToUpper`, `ParseIntoArray` and the rest. | Yes |
| <a id="string-functions.s-len"></a>`S.Len()`, `S.ToUpper()` | The same nodes as methods, with the string first: `S.Len()` is `UKismetStringLibrary::Len(S)`. See [Methods on structs, text and soft pointers](#methods-on-structs-text-and-soft-pointers). | Yes |

### Formatting text

| You write | What it does | Status |
|---|---|---|
| <a id="formatting-text.format-name-value-key-map-key"></a>`Format("{Name}:{Value}", Key, Map[Key])` with `#include "../include/Format.h"` | The Format node over one FFormatArgumentData per argument, named by the placeholder it fills: the arguments fill the placeholder names in the order they first appear, so `"{A} {B} {A}"` takes two. Each argument's type picks its kind: int32 (or smaller) Int, float or double Float, FText Text, ETextGender Gender; an FString, FName, bool or int64 is the Text of it. A backtick escapes the next brace. The pattern must be a string literal. The result is an FText, which converts to an FString where one is wanted. | Yes |
| <a id="formatting-text.format-a-b-x"></a>`Format("{A} {B}", X)` | Refused by clang while it compiles the mod: the pattern names more or fewer placeholders than there are arguments, and the error names `Format_PlaceholdersAndArgumentsDiffer`. | Refused |

```cpp
#include "../include/Format.h"

FString Line(FName Key, int32 Hits) { return Format("{Name}: {Hits} hits", Key, Hits); }
```

## Constants

This section covers constants, `consteval` functions and inline class variables. A constant has no storage in a
Blueprint. Each use of it is its value, worked out when the mod is built, and a member default computed from constants
is folded the same way. The rule to remember: build-time evaluation covers constants, constant expressions and
`consteval` calls, and a default reaches a `consteval` call only through a constant. A plain `constexpr` function call
is not run at build time.

### Constants and constant expressions

| You write | What it does | Status |
|---|---|---|
| <a id="constants-and-constant.constexpr-int32-kstep-3"></a>`constexpr int32 kStep = 3;` at namespace scope | No storage. Each use, in a function or a default, is the value 3. | Yes |
| <a id="constants-and-constant.const-int32-kmask-1-4"></a>`const int32 kMask = 1 << 4 \| kStep;` at namespace scope | A top-level `const` variable is a constant too. | Yes |
| <a id="constants-and-constant.static-constexpr-int32-kslots"></a>`static constexpr int32 kSlots = kStep * 4;` in a class | A class-scope constant. `static const` works the same way. | Yes |
| <a id="constants-and-constant.int32-budget-kslots-2-1"></a>`int32 Budget = kSlots * 2 + 1;` | A default from a constant expression is folded when the mod is built. Each step is rounded to its C++ type: `1 / 2.f` is 0.5 and `1 << 31` wraps. | Yes |
| <a id="constants-and-constant.bool-ready-kstep-2-kstep"></a>`bool Ready = kStep > 2 && kStep < 10;` | Comparisons, `&&`, `\|\|` and `?:` fold too, as do casts, enum constants and unary `-`, `~` and `!`. | Yes |
| <a id="constants-and-constant.int32-counter-5"></a>`int32 Counter = 5;` at namespace scope | Not a constant: a variable every class of the mod shares. See [Global variables](#global-variables). | Yes |
| <a id="constants-and-constant.tarray-int32-rolls"></a>`TArray<int32> Rolls = {UKismetMathLibrary::RandomInteger(3)};` | Refused (`a default is a value known when the mod is built`): a default cannot call a function. Compute it in `ReceiveBeginPlay`. See [Classes and variables](#classes-and-variables). | Refused |
| <a id="constants-and-constant.sizeof-fvector"></a>`sizeof(FVector)`, `alignof(FTransform)` in code | The game's layout: [Pointers and memory](#pointers-and-memory). Refused in a default. | Yes |

Notes:
- How constant subexpressions fold inside a function body: [The optimizer](#the-optimizer).

### consteval functions

| You write | What it does | Status |
|---|---|---|
| <a id="consteval-functions.static-constexpr-int32-kseed-fnv"></a>`static constexpr int32 kSeed = Fnv("types");` | clang runs a `consteval` function while it compiles the mod, and AssetGen uses the result. The body can be any C++: loops, `uint32` math, pointers. | Yes |
| <a id="consteval-functions.int32-seed-kseed"></a>`int32 Seed = kSeed;` | The result as a default, through the constant. | Yes |
| <a id="consteval-functions.int32-seed-fnv-types"></a>`int32 Seed = Fnv("types");` | Refused (`a default is a value known when the mod is built`): a call in a default counts only through a constant, `constexpr int32 kSeed = Fnv("types");`. | Refused |
| <a id="consteval-functions.return-n-fnv-angry"></a>`return N + Fnv("angry");` | The result as a constant in code. | Yes |
| <a id="consteval-functions.fnv-name"></a>`Fnv(Name)` with a run-time argument | Refused by clang (`call to consteval function ... is not a constant expression`). The arguments must be constants. | Refused |
| <a id="consteval-functions.int32-v-twice-3"></a>`int32 V = Twice(3);` with a `constexpr` (not `consteval`) `Twice` | Refused with the default rule's message, which points at `consteval`. A plain `constexpr` call is not evaluated at build time, in a default or in code: [Inline functions and templates](#inline-functions-and-templates). | Refused |

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
| <a id="inline-class-variables.static-inline-const-float-khold"></a>`static inline const float kHold = 0.75f;` | A value that is not a Blueprint variable. It cooks nothing: no variable and no default. Each use is its initializer, evaluated where it is used, here the literal 0.75. | Yes |
| <a id="inline-class-variables.static-inline-const-fname-ktag"></a>`static inline const FName kTag = "types";` | Each use is the Name constant. | Yes |
| <a id="inline-class-variables.static-inline-const-tarray-int32"></a>`static inline const TArray<int32> kPrimes = {2, 3, 5, 7, 11};` | Each use is a Make Array. `kPrimes[I]` and `kPrimes.Num()` each make the array. | Yes |
| <a id="inline-class-variables.static-inline-const-tmap-int32"></a>`static inline const TMap<int32, float> kRates = {{1, 0.5f}, {3, 1.5f}};` | Each use is a Make Map; a TSet is a Make Set. | Yes |
| <a id="inline-class-variables.kprimes-contains-n"></a>`kPrimes.Contains(N)` | Makes no array: `N` is evaluated once and compared with each element in turn. This holds for an inline TArray or TSet of constants whose `==` is exact: numbers, bools, enums, names, strings, objects, classes and soft references. | Yes |
| <a id="inline-class-variables.for-int32-p-kprimes"></a>`for (int32 P : kPrimes)` | Makes no array when an inline TArray holds at most 16 constants and the loop variable has the element type: each pass picks its element by the loop index. Anything else (17 or more elements, a TSet or TMap, elements that are not all constants, a loop variable of another type) makes the container once, before the loop. | Yes |
| <a id="inline-class-variables.khold"></a>`kHold`, `this->kHold` | Both name the constant. | Yes |
| <a id="inline-class-variables.me-khold"></a>`Me()->kHold` | Refused (`kHold is static: name it without the object in front`): the object would never be evaluated. | Refused |
| <a id="inline-class-variables.static-inline-float-loose-0"></a>`static inline float Loose = 0.25f;` | Refused wherever it is used (`a Blueprint class has no static storage`). Make it `static inline const` or `static constexpr`, or a plain member if it has to keep a value. | Refused |
| <a id="inline-class-variables.khold-1-0f"></a>`kHold = 1.0f;` | Refused by clang: the member is `const`. | Refused |

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
  (a rotator's `==` wraps its angles). `UE_NO_OPTIMIZE` on the function turns the no-array `Contains` off.
- A function-local `static` is something else again: [Latent calls](#latent-calls).

## Enums

A variable of a game enum is what the editor makes of it: an Enum variable (an enum property over a byte) for an
`enum class`, a Byte bound to the enum for any other. A mod's own enum needs `UE_ENUM`, which cooks it as a
UserDefinedEnum asset in the mod's package. The rule to remember: a mod enum without `UE_ENUM` is only a set of number
constants, not a Blueprint type.

### Game enums

| You write | What it does | Status |
|---|---|---|
| <a id="game-enums.eendplayreason-lastreason"></a>`EEndPlayReason LastReason;` | A variable of the game's enum: a Byte bound to its UEnum, as the editor makes a variable of an enum that is no `enum class` (here a namespaced one, which the game's C++ holds as `TEnumAsByte`). Its enumerators are byte constants. | Yes |
| <a id="game-enums.eattachmentrule-rule"></a>`EAttachmentRule Rule = EAttachmentRule::KeepWorld;` | A variable of a game `enum class`: an EnumProperty over a ByteProperty named UnderlyingType, as the editor makes it and as the game's own members and parameters of it are. The same goes for a parameter, a return value, a local, a container's element or key, a UE_STRUCT member, a delegate's parameter and a `UE_DEFAULTS` tag on a native member: an override or a delegate of a native signature then has the native's types. The value is the same byte, its enumerators byte constants, and a default is written as the enumerator's name in an EnumProperty tag. | Yes |
| <a id="game-enums.a-game-enum-that-no"></a>A game enum that no property uses | UeApi's `Types.json` gives each enum's form, which genueapi reads off how the Dumper-7 dump's properties of the enum (a native delegate's parameters among them), and the game's Blueprints' variables, locals and containers' elements, are reflected (`form`: `EnumClass` or `TEnumAsByte`). 240 of the SDK's 1445 enums have no such property, and a variable of one is a Byte bound to the enum. One the game uses, ESteamVRInputStringBits, is only the element of a native function's `TArray` parameter, whose inner property neither the dump nor Dumper-7's usmap tells apart and no game package holds. Their form is not read yet rather than unknown: the editor takes it from the enum itself (`UEnum::GetCppForm`), which the dump does not carry yet. | Yes |
| <a id="game-enums.lastreason-eendplayreason-quit"></a>`LastReason == EEndPlayReason::Quit`, `<` | Comparisons work. | Yes |
| <a id="game-enums.switch-reason"></a>`switch (Reason)` | A switch works on any enum. | Yes |
| <a id="game-enums.eendplayreason-last"></a>`EEndPlayReason Last = EEndPlayReason::Quit;` as a default | The default is written as the enumerator's name. | Yes |
| <a id="game-enums.ecreaturesize-size"></a>`ECreatureSize Size;` | Refused (`a uint8, int32 or int64 enum only`). 36 of the SDK's 1445 enums (35 uint32, 1 uint16) have an underlying type other than uint8, int32 or int64, and none of them can be a variable. | Refused |

### Mod enums: UE_ENUM

| You write | What it does | Status |
|---|---|---|
| <a id="mod-enums-ue.enum-class-emood-uint8-calm"></a>`enum class EMood : uint8 { Calm, Angry = 5, Sleepy };` `UE_ENUM(EMood);` | Cooks EMood as a UserDefinedEnum asset in the mod's package, with the C++ names and values and a closing `EMood_MAX` one past the largest value. From then on it is an ordinary Blueprint enum for variables, parameters, constants and switch. | Yes |
| <a id="mod-enums-ue.enum-class-erank-uint8-low"></a>`enum class ERank : uint8 { Low, Mid, High };` with no `UE_ENUM`, named by a member, parameter, return value, local or container of a mod class or function | Cooked as `UE_ENUM(ERank)` would cook it, in the package of the mod whose folder declares it. An enum nothing reflected names stays a plain C++ enum, and so does one of another width: those need `UE_ENUM`, which is still accepted for any. | Yes |
| <a id="mod-enums-ue.emood-mood-emood-angry"></a>`EMood Mood = EMood::Angry;` | A default is written as the enumerator's name. | Yes |
| <a id="mod-enums-ue.enum-class-espan-int32-tiny"></a>`enum class ESpan : int32 { Tiny = -3, Wide = 70000 };` `UE_ENUM(ESpan);` | An int32 or int64 enum is cooked the same way with its wide values. A variable of it holds the full width, as UHT reflects a native `enum class : int32`. | Yes |
| <a id="mod-enums-ue.enum-class-ebad-int16-a"></a>`enum class EBad : int16 { A, B };` `UE_ENUM(EBad);` | Refused (`declare it : uint8 (a Blueprint enum), : int32 or : int64`), even if no variable uses it. An enum with no fixed type is refused too. | Refused |
| <a id="mod-enums-ue.enum-class-eo-uint8-a"></a>`enum class EO : uint8 { A = 255 };` `UE_ENUM(EO);` | Refused (`A is out of range`): `_MAX` needs the next value, so a uint8 enum's largest value is 254. | Refused |
| <a id="mod-enums-ue.enum-class-egear-uint8-low"></a>`enum class EGear : uint8 { Low, High, EGear_MAX };` | The UE C++ idiom: a declared `EGear_MAX` one past the largest value is the closing `_MAX` itself, not a second entry. Any other value for it is refused. | Yes |
| <a id="mod-enums-ue.ue-enum-emissing"></a>`UE_ENUM(EMissing);` | Refused (`names no enum with enumerators`). | Refused |
| <a id="mod-enums-ue.ue-enum-edialogrestriction"></a>`UE_ENUM(EDialogRestriction);` for a mod enum named like a game or engine enum, in any namespace | Refused (`/Script/FSD already has an enum EDialogRestriction`). The engine keeps every enumerator's name, `EDialogRestriction::None`, in one global table where the first enum loaded wins, so lookups by name would answer with the game's. Rename it. Two mods' enums of one name clash the same way, which the compiler cannot see; give each mod's enums names of their own. | Refused |
| <a id="mod-enums-ue.ue-enum-in-esystems-game"></a>`UE_ENUM_IN(ESystems, "/Game/_MyMods/Shared");` | For an enum in a header several mods include. Only the source whose `UE_MOD_PACKAGE` is exactly that path cooks it; every other mod imports it from there. `UE_STRUCT_IN` does the same for structs: [Structs](#structs). | Yes |
| <a id="mod-enums-ue.elocal-l"></a>`ELocal L;` where `ELocal` is `: int32`, `: int64` or of no fixed type, with no `UE_ENUM`, or declared in a header no mod owns | Refused (`unimplemented property L: ELocal`). The enum's constants still fold to numbers, `(int32)ELocal::B`, but it is not a Blueprint type. Add `UE_ENUM`. | Refused |
| <a id="mod-enums-ue.enum-kcap-1000-kfar-5000000000"></a>`enum { kCap = 1000, kFar = 5000000000 };` | Constants of an enum with no fixed type are int-sized, or int64 when a value needs it, as C++ makes them. They fold to their values. | Yes |
| <a id="mod-enums-ue.static-constexpr-const-char-emood-ueenumnames"></a>`static constexpr const char* EMood__UeEnumNames = "Very_Angry=Very_Angry=Very Angry";` beside `UE_ENUM(EMood)` | Entries `Cpp=Cooked` or `Cpp=Cooked=Label`, joined by a vertical bar. `Cooked` is the name the enumerator is cooked under in place of its C++ one, for an enum rebuilt from an editor's (`NewEnumeratorN`), whose old assets and saved games hold those names. `Label` is its display name, what `.String()` and the editor show; give the C++ name as `Cooked` to label an enumerator without renaming it. An enumerator neither renamed nor labelled shows its C++ name. | Yes |
| <a id="mod-enums-ue.emood-mood"></a>`EMood Mood;` of a `uint8` UE_ENUM | A Byte bound to the enum, which is what the editor makes of a UserDefinedEnum: its form is namespaced, never `enum class`, whatever the C++ says. | Yes |

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
| <a id="enum-conversions-and.int32-m"></a>`(int32)M` | Byte to Int. | Yes |
| <a id="enum-conversions-and.emood-x"></a>`(EMood)X` | Int to Byte. It wraps modulo 256 and does not check that the value is an enumerator, as the C++ cast does not. | Yes |
| <a id="enum-conversions-and.bool-m"></a>`(bool)M` | True when the value is nonzero. (`if (M)` on an `enum class` is a clang error, as in C++.) | Yes |
| <a id="enum-conversions-and.switch-m"></a>`switch (M)` with `using enum EMood;` | Works, `using enum` included. A value that matches no case falls through past the switch, as in C++. How a switch runs: [Statements and control flow](#statements-and-control-flow). | Yes |

### UE_ENUM_MAP

| You write | What it does | Status |
|---|---|---|
| <a id="ue-enum-map.ue-enum-map-emood-fname"></a>`UE_ENUM_MAP(EMood, FName, MoodNames);` | Declares the member `TMap<EMood, FName> MoodNames` with a default the compiler fills in when the mod is built: one pair per enumerator in declaration order, with the C++ name as the text. The closing `_MAX` is left out. | Yes |
| <a id="ue-enum-map.ue-enum-map-fstring-emood"></a>`UE_ENUM_MAP(FString, EMood, MoodsByName);` | The enum can be on either side, with FName or FString on the other. | Yes |
| <a id="ue-enum-map.ue-enum-map-eendplayreason-fname"></a>`UE_ENUM_MAP(EEndPlayReason, FName, Reasons);` | Any enum the source can see works, the game's included. | Yes |
| <a id="ue-enum-map.tmap-emood-fname-moodnames-ue"></a>`TMap<EMood, FName> MoodNames = UE_ENUM_MAP(EMood);` | The default alone, for a member you declare yourself: the same table. | Yes |
| <a id="ue-enum-map.ue-enum-map-int32-fname"></a>`UE_ENUM_MAP(int32, FName, M);`, `UE_ENUM_MAP(EMood, EMood, M);`, `UE_ENUM_MAP(EMood, int32, M);` | Refused by clang, naming both types: "static assertion failed due to requirement '__EnumMapPair__<int, FName>': UE_ENUM_MAP(Key, Value, Name): one of Key and Value is an enum and the other FName or FString". | Refused |
| <a id="ue-enum-map.ue-enum-map-tenum-emood"></a>`UE_ENUM_MAP(TEnum<EMood>, FName, M);` | Refused the same way: write the enum itself, `UE_ENUM_MAP(EMood, FName, M);`. | Refused |
| <a id="ue-enum-map.tmap-int32-fname-m-ue"></a>`TMap<int32, FName> M = UE_ENUM_MAP(EMood);` | Refused by clang, "no viable conversion from '__EnumMapInit__<EMood>' to 'TMap<int32, FName>'": the one-argument form is the default of a TMap between that enum and FName or FString. | Refused |
| <a id="ue-enum-map.ue-enum-map-fname-emood"></a>`UE_ENUM_MAP(FName, EMood);`, `UE_ENUM_MAP(FName, EMood, A, B);` | Refused by clang: "UE_ENUM_MAP takes (Enum), or (Key, Value, Name) to declare the member". Written as an initializer, `= UE_ENUM_MAP(FName, EMood)`, clang says only "expected expression". | Refused |
| <a id="ue-enum-map.tmap-emood-fname-names-ue"></a>`TMap<EMood, FName> Names = UE_ENUM_MAP(EMood);` in a function body | The same table, made where it is written as the braced map `{ { EMood::Calm, "Calm" }, ... }` would be: the enum on either side, FName or FString on the other. It is made each time the line runs, so a table read often is cheaper as a member. | Yes |

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
| <a id="tenum.tenum-emood-mood-emood-calm"></a>`TEnum<EMood> Mood = EMood::Calm;` | An EMood variable, member, parameter or local. It switches, compares and assigns like a plain EMood. | Yes |
| <a id="tenum.mood"></a>`Mood = {};`, `TEnum<EMood>()`, `K2_DetachFromActor({}, {}, {})` | The zero enumerator, as for a plain EMood: assigned, passed (to a game function too), returned, as a struct literal's member or an array element. | Yes |
| <a id="tenum.mood-name"></a>`Mood.Name()` | The enumerator's name as an FName, from `KismetNodeHelperLibrary::GetEnumeratorName` on EMood's UEnum. | Yes |
| <a id="tenum.mood-string"></a>`Mood.String()` | Its display name as an FString, from `GetEnumeratorUserFriendlyName`: the label typed in the editor for a Blueprint enum. | Yes |
| <a id="tenum.name"></a>`.Name()` / `.String()` on an `int32` or `int64` enum | Refused ("a uint8 enum only"): the engine's two lookups take a uint8. | Refused |
| <a id="tenum.tmap-tenum-emood-tsubclassof"></a>`TMap<TEnum<EMood>, TSubclassOf<AActor>> Spawns;` | A map keyed by EMood, its value any type a map takes, a template too. | Yes |
| <a id="tenum.moods-num"></a>`Moods.Num()` on a `TArray<TEnum<EMood>>`; `UE_DISPATCHER(OnMood, TEnum<EMood> M)` | A container of EMood with a container's methods, and a dispatcher whose Add and Broadcast are a dispatcher's. Only `TEnum<E>` itself has `.Name()` / `.String()`. | Yes |

```cpp
TEnum<EMood> Mood = EMood::Angry;

FString Describe() { return "Mood: " + Mood.String(); }   // "Mood: Angry"
FName MoodName(TEnum<EMood> M) { return M.Name(); }        // an EMood argument converts
```

### TEnumMask

`TEnumMask<E>` (UeApi `Types.h`) is a Blueprint bitmask over an enum: an `int32` to the compiler, the same property the
editor makes for a Bitmask variable over a Bitmask Flags enum. Bit N stands for the enumerator whose value is N, while
the enum itself stays 0, 1, 2, ... Its methods are the bit functions of `KismetNodeHelperLibrary`, with the enumerator
as the bit index. It converts to and from `int32` both ways, so whole masks combine with `|`, `&` and `~`. No editor
metadata is written, because a cooked package carries none.

| You write | What it does | Status |
|---|---|---|
| <a id="tenummask.tenummask-emood-moods-0x21"></a>`TEnumMask<EMood> Moods = 0x21;` | An int32 variable, member, parameter or local. A member's default is the number. | Yes |
| <a id="tenummask.moods-has-emood-angry"></a>`Moods.Has(EMood::Angry)` | Whether the enumerator's bit is set: `BitIsMarked`. The engine tests `> 0`, so bit 31 never reads as set. | Yes |
| <a id="tenummask.moods-add-m"></a>`Moods.Add(M)`, `Moods.Remove(M)` | Set or clear the enumerator's bit: `MarkBit`, `ClearBit`. An enumerator past 31 does nothing. | Yes |
| <a id="tenummask.moods-clear"></a>`Moods.Clear()` | Clear every bit: `ClearAllBits`. | Yes |
| <a id="tenummask.int32-moods"></a>`int32(Moods)`, `Moods = Other \| 4;`, `if (Moods)` | Plain int32 reads and writes. | Yes |
| <a id="tenummask.map-k-add-m"></a>`Map[K].Add(M)`, `GetMask().Clear()` on a `TEnumMask` | Refused ("writes the mask"): the three writers take the mask by reference, and Blueprint has no reference to a map element or a returned value. Copy it to a local, then store it back. | Refused |
| <a id="tenummask.a-native-enum-whose-values"></a>A native enum whose values are already flags (1, 2, 4, ...) | Not what `TEnumMask` counts: bit N is value N. Test such an enum with `&` on an int32. | No |

```cpp
TEnumMask<EMood> Allowed = 0x21;                          // Calm (bit 0) and Angry (bit 5)

bool CanBe(EMood M) { return Allowed.Has(M); }
void Forbid(EMood M) { Allowed.Remove(M); }
```

## Structs

Any reflected struct of the game or the engine, and a mod's own `UE_STRUCT`, can be a variable, a parameter, a local, a
return value or a container element. The rule to remember: a struct is a value. Assigning it or passing it by value
copies it, as in C++.

### Struct variables and members

| You write | What it does | Status |
|---|---|---|
| <a id="struct-variables-and.fvector-home"></a>`FVector Home;`, `FHitResult Hit;`, `FTransform Placed;` | A struct variable. All 2983 reflected structs in the DRG SDK can be one. | Yes |
| <a id="struct-variables-and.home-z-twice-x-1"></a>`Home.Z = Twice.X + 1;` | Reads and writes a member in place (Break Struct, Set Members in Struct). | Yes |
| <a id="struct-variables-and.nested-inner-kills-1"></a>`Nested.Inner.Kills += 1;` | Nested members work to any depth, in place. | Yes |
| <a id="struct-variables-and.k2-getactorlocation-x"></a>`K2_GetActorLocation().X` | A member of a struct a call returns. The value is stored in a temporary and read from there. | Yes |
| <a id="struct-variables-and.s-kills"></a>`&S.Kills` with a member `S` | The address of S, found at run time through the ReadProperty helper as `&Member` is, plus Kills' offset in the struct. Without the helper the build stops ("include ReadProperty.h"). See [Pointers and memory](#pointers-and-memory). | Yes |
| <a id="struct-variables-and.s-kills-2"></a>`&S.Kills` with a local or parameter `S` | Refused: a Blueprint variable has no address ("a Blueprint variable has none the VM hands out"). | Refused |

### Making struct values

| You write | What it does | Status |
|---|---|---|
| <a id="making-struct-values.fvector-1-2-3"></a>`FVector(1, 2, 3)`, `FLinearColor(1, 0.5f, 0, 1)` | A whole-struct constructor: one struct value built from the arguments. Constants, locals and the object's own variables make one literal value, as the editor's literal pin does. | Yes |
| <a id="making-struct-values.fvector2d-1-0f-m-0"></a>`FVector2D(1.0f, M == 0 ? 2.0f : 3.0f)`, `{Twice(M), 1}`, `V = {V.Y, V.X}` | A member that computes anything else (a call, `?:`, `&&`, a member of a struct) makes the editor's Make Struct instead: a fresh value, then one store per member, left to right as C++ runs a braced list. The variable assigned is written after all of them, so `V = {V.Y, V.X}` swaps. | Yes |
| <a id="making-struct-values.fcolor-255-128-0"></a>`FColor(255, 128, 0)` | R, G, B and A, with A 255 when left out, like the engine's constructor. Each argument lands on the member its parameter is named after, although FColor stores B, G, R, A. | Yes |
| <a id="making-struct-values.flightmassdirectionallightsettings"></a>`FLightmassDirectionalLightSettings(1.5f, 2.5f, true, 4.5f)` | A struct with a parent struct: the arguments come in C++ order, the parent's members first, and each lands on its member (the engine lists the struct's own members first). | Yes |
| <a id="making-struct-values.fmaterialattributesinput-3-in-ex"></a>`FMaterialAttributesInput(3, "In", "Ex", 0)`, `Handle = FTimerHandle();` | A struct with a member the engine marks Transient (PropertyConnectedBitmask here, FTimerHandle's one member Handle) is the Make Struct above, never a literal: the engine's literal skips that member and leaves it as the variable assigned held it, which a variable assigned again, or a local in a loop, still holds. The Make Struct sets it, to the value given or zero, so `Handle = FTimerHandle();` resets a live handle, as in C++. | Yes |
| <a id="making-struct-values.fvector"></a>`FVector()`, `FVector4()`, `FTransform()`, `FTransform T{};` | What the engine's constructor makes. `FVector`, `FVector2D`, `FRotator`, `FLinearColor`, `FColor` and the `FVector_NetQuantize` types, whose constructor sets nothing, are their zeros, a literal. `FVector4()` is the literal (0, 0, 0, 1), its constructor's W; so is an FVector4 declared with no initializer, each time a loop comes round, since the engine makes a fresh FVector4 variable as zeros. Any other engine struct, `FTransform()` (the identity) or `FQuat()` among them, is the editor's Make Struct with nothing set: a temporary the function's frame constructs as the engine does. So `SpawnActor<T>(Class, FTransform())` spawns at the origin at scale 1. | Yes |
| <a id="making-struct-values.fhitresult"></a>`FHitResult()`, `FStats{}` | Make Struct with nothing set, for a struct without a whole-struct constructor: the struct keeps its own defaults (`FHitResult::Time` is 1). | Yes |
| <a id="making-struct-values.fstats-s-kills-k-alive"></a>`FStats S = {.Kills = K, .Alive = true};` | Make Struct: a fresh value, then one store for each member given. Members left out keep the struct's defaults. | Yes |
| <a id="making-struct-values.killsof-kills-k"></a>`KillsOf({.Kills = K})` | A braced value as an argument. | Yes |
| <a id="making-struct-values.fstats-s-k-2-0f"></a>`FStats S = {K, 2.0f};` | Positional braces go by member declaration order. | Yes |
| <a id="making-struct-values.fstats-s-2-0f"></a>`FStats S = {{}, 2.0f};` | A `{}` for a member is a fresh value of its type, as in C++, not the member's default: zero, an empty container, None, an engine struct's zeros, or a `UE_STRUCT`'s own defaults. | Yes |

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
- For an identity transform, write `FTransform(FQuat(0, 0, 0, 1), FVector(0, 0, 0), FVector(1, 1, 1))` or
  `FTransform(FRotator(), FVector())`.
- `FTransform(Rotation, Translation, Scale)` with an FRotator, UE's own order, and `FTransform(Location, Rotation, Scale)`,
  Make Transform's order, are both `UKismetMathLibrary::MakeTransform(Location, Rotation, Scale)`; the types tell them
  apart. See [Struct conversions](#struct-conversions).

### Struct operators

| You write | What it does | Status |
|---|---|---|
| <a id="struct-operators.home-home-2-0f"></a>`Home + Home * 2.0f`, `(A + B) / 2`, `Tint * 0.5f` | The Kismet math node for that pair of types (Add_VectorVector, Multiply_VectorFloat, Divide_VectorInt...). Only the pairs Kismet has exist. | Yes |
| <a id="struct-operators.2-0f-v"></a>`2.0f * V` | The float converts to a vector first, which gives the same result. | Yes |
| <a id="struct-operators.home-twice"></a>`Home == Twice`, `Home != Twice` | For FVector, FVector2D, FVector4 and FMatrix this is Kismet's Equal (Not Equal) node with an error tolerance of 0, so it compares every component exactly, as UE C++'s `==` does. The editor's node defaults to 1e-4; for that, call `UKismetMathLibrary::EqualEqual_VectorVector(A, B, 1e-4f)`. | Yes |
| <a id="struct-operators.facing-aim"></a>`Facing == Aim` with two FRotators or two FQuats | The Equal node with a tolerance of 0, with a warning: it compares rotations, so angles 360 degrees apart (a quaternion and its negation) are equal, where UE C++ compares the components. | Warns |
| <a id="struct-operators.home-0"></a>`Home == 0`, `Home != 0.5f` | The number fills all three components first (Conv_FloatToVector), so `Home == 0` is true for the zero vector. UE C++ has no such operator: its FVector constructor from a float is explicit. | Yes |
| <a id="struct-operators.tint-other"></a>`Tint == Other` | FLinearColor, FTransform and other structs use their own Kismet Equal node. FLinearColor's has no tolerance. | Yes |
| <a id="struct-operators.home-w"></a>`Home += W;` | Refused by clang (`no viable overloaded '+='`): structs have no compound assignment. Write `Home = Home + W;`. | Refused |
| <a id="struct-operators.v"></a>`-V` | Refused by clang (`invalid argument type 'FVector' to unary expression`). Call `UKismetMathLibrary::NegateVector(V)`. | Refused |
| <a id="struct-operators.a-b"></a>`A + B` with two FRotators | Kismet has no such node. Stored in or returned as an FRotator it is a clang error. Call `UKismetMathLibrary::ComposeRotators(A, B)`. | Refused |
| <a id="struct-operators.a-b-2"></a>`A == B` with two `UE_STRUCT`s | Refused by clang (`invalid operands to binary expression`). The editor has no Equal node for them either. Compare the members that matter. | Refused |

```cpp
FVector Mid(FVector A, FVector B) { return (A + B) / 2; }
FVector Away(FVector V) { return UKismetMathLibrary::NegateVector(V); }
FRotator Turn(FRotator A, FRotator B) { return UKismetMathLibrary::ComposeRotators(A, B); }
bool Same(FVector A, FVector B) { return A == B; }  // exact, as in C++
bool Near(FVector A, FVector B) { return UKismetMathLibrary::EqualEqual_VectorVector(A, B, 1e-4f); }
```

Notes:
- Two FRotators with `+` are joined as strings: both convert to FString, so `auto S = A + B;` or `FString S = A + B;`
  compiles, and `S` is text, not a rotation. Only storing the result in an FRotator shows the mistake.

### Struct conversions

| You write | What it does | Status |
|---|---|---|
| <a id="struct-conversions.facing-twice"></a>`Facing = Twice;` (FVector to FRotator) | The rotation of the direction, not a copy of X, Y and Z. | Yes |
| <a id="struct-conversions.ftransform-placed-home"></a>`FTransform Placed = Home;` | A translation. | Yes |
| <a id="struct-conversions.ftransform-facing-home-size"></a>`FTransform(Facing, Home, Size)`, `FTransform(Facing, Home)` | UE's constructor order, Rotation (an FRotator), Translation, Scale: the Make Transform node, `MakeTransform(Home, Facing, Size)`. Left out, the scale is 1. | Yes |
| <a id="struct-conversions.ftransform-home-facing-size"></a>`FTransform(Home, Facing, Size)` | Make Transform's own order, Location, Rotation, Scale, with the same result. UE C++ has no such constructor; UeApi declares it so the node's order reads as written. | Yes |
| <a id="struct-conversions.ftransform-a-b-c"></a>`FTransform(A, B, C)` with three FVectors | Refused by clang ("call to constructor of 'FTransform' is ambiguous"): a vector converts to a rotator, so both orders, and the FQuat constructor, would take it. Write the rotator as one: `FTransform(FRotator(A), B, C)`. | Refused |
| <a id="struct-conversions.fvector-v-2-0f"></a>`FVector V = 2.0f;`, `FVector V = 2;` | A float fills all three components; an int converts to float first. | Yes |
| <a id="struct-conversions.flinearcolor-l-fcolor-255-0"></a>`FLinearColor L = FColor(255, 0, 0);` | FColor and FLinearColor convert both ways through sRGB. | Yes |
| <a id="struct-conversions.fvector-v-flat"></a>`FVector V = Flat;` (FVector2D) | Z is 0. | Yes |
| <a id="struct-conversions.fvector-v-facing"></a>`FVector V = Facing;` (FRotator) | Refused by clang: left out on purpose, because a rotator is not a direction. Call `UKismetMathLibrary::GetForwardVector(Facing)`. | Refused |

Notes:
- A struct converts implicitly wherever Kismet has a conversion node, and the conversion means what that node means.

### Struct copies

| You write | What it does | Status |
|---|---|---|
| <a id="struct-copies.fstats-t-stats"></a>`FStats T = Stats;` | A copy. Changing `T` leaves `Stats` alone. | Yes |
| <a id="struct-copies.setkills-k-t"></a>`SetKills(K, T)` with `void SetKills(int32 K, FStats &S)` | Writes through the reference into `T`, the copy. | Yes |
| <a id="struct-copies.for-fstats-s-many"></a>`for (FStats S : Many)` | `S` is a copy; `for (FStats &S : Many)` writes the element. See [Loops](#loops). | Yes |
| <a id="struct-copies.weights-find-key-out"></a>`Weights.Find(Key, Out)` | `Out` receives a copy; writing it leaves the map alone. | Yes |

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
| <a id="struct-defaults.ffloatinterval-delay"></a>`FFloatInterval Delay = FFloatInterval(1, 5);` | A constructor call as a member default. It is written into Class Defaults. | Yes |
| <a id="struct-defaults.fterrainplacementbox-min-max"></a>`FTerrainPlacementBox(Min, Max, ETerrainPlacementBoxType::Terrain, UE_KEEP)` | `UE_KEEP` for a constructor argument leaves that member unwritten, so it holds what the engine's constructor sets, as braces leave out a member of a struct with no constructor. Only where the value starts fresh: a container's element, a class's own default. bpdecomp writes it for a member the cooked value leaves out. | Yes |
| <a id="struct-defaults.gameplaytags"></a>`GameplayTags = FGameplayTagContainer({FGameplayTag("Enemy.Type.Regular")}, {});` | Written in the container's own form, its tags' FNames (FGameplayTagContainer::Serialize); ParentTags is rebuilt by the engine at load. | Yes |
| <a id="struct-defaults.fx-overrideparameters"></a>`Fx->OverrideParameters.SortedParameterOffsets = {FNiagaraVariableWithOffset("User.Radius", FNiagaraTypeDefinition(StaticStructOf<FNiagaraFloat>(), 2), 0)};` | A Niagara component's parameter store, as the cook writes it: each FNiagaraVariable / FNiagaraVariableWithOffset in its own form (the Name, the type definition, then VarData or the Offset), and `UserParameterRedirects` a TMap of them. A handle is given as the FNiagaraTypeDefinition it names; `StaticStructOf<T>()` is the struct of its values. | Yes |
| <a id="struct-defaults.classstructorenum-staticstructof"></a>`.ClassStructOrEnum = StaticStructOf<FNiagaraFloat>()`, `UObject *S = StaticStructOf<FLinearColor>();` | A struct's UScriptStruct as an object: the engine struct's `/Script` one, or a UE_STRUCT's asset. | Yes |
| <a id="struct-defaults.fvector-offset-0-0-50"></a>`FVector Offset = {0, 0, 50};` | Positional braces as a default. | Yes |
| <a id="struct-defaults.fnested-deep-inner-time-1"></a>`FNested Deep = {.Inner = {.Time = 1.5f}, .Stamp = 7};` | Designated braces, nested. Members left out are zero, or take their own default initializer. | Yes |
| <a id="struct-defaults.fhitresult-mine-distance-5-0f"></a>`FHitResult Mine = {.Distance = 5.0f};` | Designated (or positional) braces of an engine struct whose header has no constructor: the members given are written, the ones left out are not, so they hold what the engine's constructor sets (`FHitResult::Time` is 1), as the editor's Make Struct leaves them. | Yes |
| <a id="struct-defaults.fheld-h-hit-distance-5"></a>`FHeld H = {.Hit = {.Distance = 5.0f, .Time = 1.0f}};` where FHeld declares `FHitResult Hit = {.Time = 0.5f};` | New braces for a struct member with an initializer of its own. The value starts as the struct's default instance, which holds that initializer, so a member both braces leave out (FaceIndex) is the engine's in either and stays unwritten. A member the initializer gives and the new braces leave out would load the initializer's value instead of the engine's: `H = {.Hit = {.Distance = 5.0f}}` is refused, naming `H.Hit.Time`. The same holds for a `UE_STRUCT` or another mod's `UE_STRUCT_IN` struct, in a struct's member initializer and in a container's element. | Yes |
| <a id="struct-defaults.fcolor-lamp-fcolor-255-128"></a>`FColor Lamp = FColor(255, 128, 0);` | FColor's argument order holds in defaults too. | Yes |
| <a id="struct-defaults.fstats-fresh-fstats"></a>`FStats Fresh = FStats();`, `FStats Cleared = {{}, {}};` | A `UE_STRUCT`'s `T()` is its defaults, and writes nothing. A `{}` for a member is a fresh value of its type (zero, empty, None, a `UE_STRUCT`'s own defaults), written even where the member's default differs. In a function, `Code(FStats())` and `FStats S = FStats();` are the Make Struct of its defaults, as `FStats S{}` is. | Yes |
| <a id="struct-defaults.fhitresult-hit"></a>`FHitResult Hit = {};`, `FHeld H = {FHitResult(), 5};`, `FTransform T = FTransform();` | An engine struct by `{}` or `T()`: none of its members is written, so it holds what the engine's constructor sets (`FHitResult::Time` is 1, `FTransform()` is the identity, `FFindFloorResult()`'s HitResult has Time 1), however deep it sits in a struct value. The UeApi header's `T() = default;` says nothing of those values. Only `FVector`, `FVector2D`, `FRotator`, `FLinearColor`, `FColor` and the `FVector_NetQuantize` types, whose constructor sets nothing, are written as zeros. `FVector4` is written as (0, 0, 0, 1), its constructor's W, by `{}`, `T()` or no initializer at all, a `UE_STRUCT`'s member too: the engine makes a fresh one as zeros. A `UE_STRUCT` member with an initializer of its own, given `T()` in braces (`{FHitResult(), 5}` where the member is `FHitResult Hit = {.Time = 0.5f};`), is refused as in `UE_DEFAULTS`: see [Class defaults](#class-defaults). | Yes |

Notes:
- Every value in a default must be known when the mod is built: [Classes and variables](#classes-and-variables).

### Mod structs: UE_STRUCT

| You write | What it does | Status |
|---|---|---|
| <a id="mod-structs-ue.struct-fstats-ue-struct-int32"></a>`struct FStats { UE_STRUCT; int32 Kills; ... };` | Cooks the struct as a UserDefinedStruct asset in the mod's package, the editor's Structure asset. It can then be a member, parameter, local, container element or member of another struct. | Yes |
| <a id="mod-structs-ue.struct-fstats-int32-kills-float"></a>`struct FStats { int32 Kills; float Time; };` with no `UE_STRUCT`, named by a member, parameter, return value, local or container of a mod class or function, or by a member of another struct so made | Cooked as a `UE_STRUCT` is, in the package of the mod whose folder declares it: a plain struct (no base, no marker) whose members are values a struct holds. `UE_STRUCT` says the same and is still accepted. | Yes |
| <a id="mod-structs-ue.struct-ftable-int32-x-constexpr"></a>`struct FTable { int32 X; }; constexpr FTable kTable = {5};`, or a struct nothing reflected names | Stays plain C++: no asset. A constexpr variable or function is read at compile time only. Write `UE_STRUCT` to cook one anyway, such as one only other mods use. | Yes |
| <a id="mod-structs-ue.float-time-1-0f"></a>`float Time = 1.0f;` inside the struct | A member initializer is the struct's default value for that member. | Yes |
| <a id="mod-structs-ue.tarray-int32-hits"></a>`TArray<int32> Hits;` inside the struct | A member can have any Blueprint type, containers and enums included. | Yes |
| <a id="mod-structs-ue.ue-struct-in-game-mymods"></a>`UE_STRUCT_IN("/Game/_MyMods/Shared");` | For a struct in a header several mods include. Only the source whose `UE_MOD_PACKAGE` is exactly that path cooks it; every other mod imports it from there. | Yes |
| <a id="mod-structs-ue.tarray-fenemyspawnwave-waves"></a>`TArray<FEnemySpawnWave> Waves = {{.MinAmount = 2, .MaxAmount = 5}};`, `#include "UeApi/Game/FEnemySpawnWave.h"` | A game Blueprint's struct (a UserDefinedStruct the game cooks). genueapi writes it under `UeApi/Game/` with `UE_GAME_STRUCT(Package, Name)`, its members under their engine names (`MinAmount_13_06D0...`, off the object dump) and its default instance's values as initializers (off `--game`). It is imported, never cooked, and works like another mod's `UE_STRUCT_IN` struct: braces write each member they leave out with its initializer. A game Blueprint class that holds one includes its header. genueapi leaves a game struct out when it has no package in the `--game` content, when one of its members has no UeApi type, or when its default instance holds a value other than a number or bool that is not zero (a struct, text, soft path, enum or array), and then also every struct that holds it. | Yes |

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
needs a container variable, not one a call returns (a range-for makes its own).

### Container variables

| You write | What it does | Status |
|---|---|---|
| <a id="container-variables.tarray-int32-scores"></a>`TArray<int32> Scores;`, `TArray<FVector> Points;` | An Array variable, as a member, parameter or local. | Yes |
| <a id="container-variables.tset-int32-seen"></a>`TSet<int32> Seen;` | A Set variable. | Yes |
| <a id="container-variables.tmap-fname-float-weights"></a>`TMap<FName, float> Weights;` | A Map variable. | Yes |
| <a id="container-variables.tmap-aplayercharacter-int32"></a>`TMap<APlayerCharacter *, int32> Kills;`, `TMap<AFSDPlayerState *, float> Damage;` | A Map keyed by an object reference, such as one entry per player: `Kills[Player] += 1`, `Kills.Remove(Player)` and `for (auto &[P, N] : Kills)` work as with any key. The game replaces a player's character (see [examples/WaitForPlayer.cpp](examples/WaitForPlayer.cpp)), so an entry keyed by the old character stops matching. Key by the player state, or by its `GetPlayerName()` as an `FString`, to keep one entry per player. | Yes |
| <a id="container-variables.tarray-tsoftclassptr-aactor"></a>`TArray<TSoftClassPtr<AActor>> Kinds;` | The element type can be any Blueprint type: numbers, strings, enums, structs, objects, class and soft references, interfaces. | Yes |
| <a id="container-variables.tarray-uobject-all-actors"></a>`TArray<UObject *> All = Actors;`, `Count(Pawns)` for a `const TArray<AActor *>&` | A container of objects passes where one of a base class is wanted: a by-value or `const&` parameter (an engine function's too), a member, a local, a struct member or a return. The editor links an Actor array pin to an Object array one the same way (ArePinTypesCompatible). It is a copy of the same pointers, so no conversion runs, and a callee that changes the source during the call still sees the copy. A `TSet` works the same. A `TMap` passes when its key and its value are each the same type or an upcast: `TMap<AActor *, APawn *>` for a `TMap<UObject *, AActor *>`. | Yes |
| <a id="container-variables.tarray-iscorable"></a>`TArray<IScorable *>` or `TArray<TScriptInterface<IScorable>>` from a `TArray<Nugget *>`; `TArray<AActor *>` from a `TArray<UObject *>`; `TArray<int64>` from a `TArray<int32>` | Refused by clang with `no viable conversion`, naming both types. Only an object pointer to a base class passes; an interface is no UObject. Fill an interface array element by element. | Refused |
| <a id="container-variables.fill-actors"></a>`Fill(Actors)` for a `TArray<UObject *>&` | Refused by clang: `non-const lvalue reference ... cannot bind to a value of unrelated type`. The types are related, but a write through the reference could put a UObject into the Actor array. Make the parameter `const`, or declare the variable a `TArray<UObject *>`. | Refused |
| <a id="container-variables.tmap-fname-tarray-fname-groups"></a>`TMap<FName, TArray<FName>> Groups;` | A container of containers, through a generated wrapper struct. See below. | Yes |
| <a id="container-variables.ue-replicated-tset-int32-seen"></a>`UE_REPLICATED(TSet<int32>, Seen);` | Refused (`a TMap or TSet does not replicate`): [Replication](#replication). | Refused |

### Container methods

| You write | What it does | Status |
|---|---|---|
| <a id="container-methods.scores-add-7"></a>`Scores.Add(7)` | A method is the Blueprint node, with the container as its target. The methods are listed under the table. `Add` returns the new index. | Yes |
| <a id="container-methods.scores-num"></a>`Scores.Num()` | The same as `Length()`. | Yes |
| <a id="container-methods.weights-find-alpha-w"></a>`Weights.Find("alpha", W)` | Results come back through reference parameters: `Find(Key, Out)` on a map, `Get(Index, Out)`, `Keys(OutArray)`, `Random(OutItem, OutIndex)`, `Seen.Union(Other, Result)`. | Yes |
| <a id="container-methods.seen-toarray-list"></a>`Seen.ToArray(List)`, `Weights.Keys(Names)`, `GetAllActorsOfClass(C, Found)` | An array an engine function only fills is emptied just before the call, as the editor does, so afterwards it holds exactly what the call put there: `ToArray` itself would add to what `List` held. An array the function also reads (`UPARAM(ref)`, such as RunAssetsThroughFilter's) is passed as it is. | Yes |
| <a id="container-methods.items-remove-2"></a>`Items.Remove(2)` | Removes the element at index 2 (the Remove Index node). UE C++'s `Remove(Item)` removes by value; this does not. | Yes |
| <a id="container-methods.items-removeitem-2"></a>`Items.RemoveItem(2)` | Removes the elements equal to 2 (the Remove node). | Yes |
| <a id="container-methods.items-find-5"></a>`Items.Find(5)` | The index of 5, or -1. | Yes |
| <a id="container-methods.items-insert-9-0"></a>`Items.Insert(9, 0)` | The item, then the index. | Yes |
| <a id="container-methods.items-set-0-9-true"></a>`Items.Set(0, 9, true)` | The index, the item, then `bSizeToFit`. | Yes |
| <a id="container-methods.a-identical-b"></a>`A.Identical(B)` | Whether two arrays hold the same elements. `A == B` is refused by clang: containers have no `==`. | Yes |
| <a id="container-methods.items-empty"></a>`Items.Empty()`, `Items.Sort()`, `Items.Pop()` | Refused by clang. `Empty`, `Emplace`, `FindOrAdd`, `IsEmpty`, `Last`, `Pop`, `Sort` and the like are UE C++, not Blueprint nodes, and the SDK does not declare them. | Refused |
| <a id="container-methods.off-active-wanted"></a>`Off = Active - Wanted;`, `A + B`, `A & B` on TSets | Set_Difference, Set_Union and Set_Intersection, the result in a temp that is the value. Both sides must be set variables or another of these (`(A + B) - C`). | Yes |
| <a id="container-methods.seen-0"></a>`Seen[0]` on a TSet | Refused by clang: a set has no `[]`. | Refused |
| <a id="container-methods.getitems-num"></a>`GetItems().Num()`, `GetItems().Contains(3)` | A method that only reads (`Num`, `Contains`, `Find`, `Get`, `IsValidIndex` and the like) puts the result in a local and reads it there. | Yes |
| <a id="container-methods.getitems-add-3"></a>`GetItems().Add(3)` | Refused (`a container operation needs a variable, not a computed value`). A change would go into a copy. Store the result in a local first. | Refused |
| <a id="container-methods.tarray-int32-r-getitems-return"></a>`TArray<int32> R = GetItems(); return R.Num();` | The local stays a variable, read once or not: a container function reads its container where it lies, so the call is never folded into its place. The same for a parameter of an `inline` function. | Yes |

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
- The "needs a variable" rule also applies to `[]`. A braced list, an inline class variable and an inline function's
  result already count as variables. A range-for over a computed value stores it in a local first.

### Indexing

| You write | What it does | Status |
|---|---|---|
| <a id="indexing.scores-0-scores-0-1"></a>`Scores[0] = Scores[0] + 1;` | `A[i]` is the Get (a ref) node: it reads or writes the element in place. | Yes |
| <a id="indexing.points-0-z-5"></a>`Points[0].Z = 5;`, `Grid[0][0] = 7;` | A member or a nested array of an element changes in place. | Yes |
| <a id="indexing.float-w-weights-alpha"></a>`float W = Weights["alpha"];` | A map read is the Find node into a temporary: a copy of the value, or the value type's default (0, empty, None, null) for a missing key. It never adds the key. | Yes |
| <a id="indexing.counts-key-seed"></a>`Counts[Key] = Seed;` | A map store is the Add node: insert or replace. | Yes |
| <a id="indexing.counts-key-2"></a>`Counts[Key] += 2;`, `Local[7]++` | Reads with Find, then stores with Add. On an empty map, `Local[7]++` leaves 7 -> 1. | Yes |
| <a id="indexing.table-nextkey-inner-kills-k"></a>`Table[NextKey()].Inner.Kills = K;` | Blueprint has no reference to a map element, so this becomes: copy the element, set the member, store it back. The value is evaluated first, as C++17 does, and the key only once. A missing key is added. | Yes |
| <a id="indexing.lists-k-add-x"></a>`Lists[K].Add(X);` | A container method on a map element runs on a copy of the element. A method that writes stores the copy back; a read such as `Num` or `Contains` stores nothing. | Yes |
| <a id="indexing.addto-table-1-inner-kills"></a>`AddTo(Table[1].Inner.Kills, 10)` with `void AddTo(int32 &V, int32 By)` | Warns: `V` gets a copy of the map element's member, stored back after the call. See [Functions](#functions). | Warns |

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
| <a id="copies-and-braced.tarray-int32-t-items-t"></a>`TArray<int32> T = Items; T.Add(K);` | Assigning a container copies it. `Items` is unchanged. | Yes |
| <a id="copies-and-braced.items-append-twice"></a>`Items.Append(Twice());` | A container that a call returns, passed to `Append` or `Union`, is stored in a local first. | Yes |
| <a id="copies-and-braced.tarray-int32-primes-2-3"></a>`TArray<int32> Primes = {2, 3, 5};` | A braced member default, written into Class Defaults element by element. | Yes |
| <a id="copies-and-braced.tmap-fname-int32-cost-gold"></a>`TMap<FName, int32> Cost = {{"Gold", 5}};` | A map's elements are `{key, value}` pairs. | Yes |
| <a id="copies-and-braced.tmap-fstring-int32-m-a"></a>`TMap<FString, int32> M = {{"a", 5}, {"A", 1}};`, `TSet<int32> S = {3, 3};` in a default | A key given twice is written once, where it first appears, holding the last element given, as C++'s value holds it. An FString key compares without case, as an FName does, so this M is `{"A": 1}`. | Yes |
| <a id="copies-and-braced.tarray-fvector-points-1-2"></a>`TArray<FVector> Points = {{1, 2, 3}, FVector(4, 5, 6)};` | Struct elements can be braces or constructor calls. | Yes |
| <a id="copies-and-braced.tarray-int32-rolls"></a>`TArray<int32> Rolls = {UKismetMathLibrary::RandomInteger(3)};` | Refused: every element of a default must be known when the mod is built. See [Classes and variables](#classes-and-variables). | Refused |
| <a id="copies-and-braced.tarray-int32-l-4-5"></a>`TArray<int32> L = {4, 5, 6};` in a function | The Make Array node: a temporary filled at once, made afresh each time the code runs. | Yes |
| <a id="copies-and-braced.tset-int32-s-1-2"></a>`TSet<int32> S = {1, 2};`, `TMap<int32, float> M = {{1, 0.5f}};` in a function | Make Set and Make Map. | Yes |
| <a id="copies-and-braced.items"></a>`Items = {};`, `Count({})`, `TArray<int32>()`, `return {};` in a function | An empty container: a Make Array (Set, Map) with no element. | Yes |
| <a id="copies-and-braced.size-tarray-int32-1-2"></a>`Size(TArray<int32>{1, 2, M})`, `TSet<int32>{1, M}`, `TArray<int32>({1, 2})` in a function | The typed list, braced or in parentheses: the same Make Array (Set, Map) as the untyped braces. | Yes |

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
- In a default, a repeated key is written once (above). In a function, a Make Set or Make Map list is written as listed,
  duplicates included; what the engine makes of those has not been checked, so keep such lists free of them.

### Containers of containers

| You write | What it does | Status |
|---|---|---|
| <a id="containers-of-containers.tarray-tarray-int32-grid"></a>`TArray<TArray<int32>> Grid;`, `TMap<FName, TArray<FName>> Groups;` | Blueprint has no container of containers, so the inner container becomes the `Value` member of a generated wrapper struct, such as `FNC_TArray_FName`. The C++ reads as usual. | Yes |
| <a id="containers-of-containers.grid-0-add-5"></a>`Grid[0].Add(5);`, `Grid[0][0] = 7;` | An array's inner container changes in place. | Yes |
| <a id="containers-of-containers.tarray-fname-again-groups-first"></a>`TArray<FName> Again = Groups["first"];` | A map's value is read as a copy, like any map read. | Yes |
| <a id="containers-of-containers.groups-find-key-out"></a>`Groups.Find(Key, Out)` with a container `Out` | Goes through a wrapper temporary; `Out` gets the inner container. | Yes |
| <a id="containers-of-containers.ue-replicated-tarray-tset-int32"></a>`UE_REPLICATED(TArray<TSet<int32>>, Sets);` | Refused: a container that holds a TMap or TSet does not replicate. See [Replication](#replication). | Refused |

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
| <a id="walking-a-container.for-int32-p-scores"></a>`for (int32 P : Scores)`, `for (auto &[K, V] : Counts)` | Range-for over a TArray, TSet or TMap: [Loops](#loops). | Yes |

## Operators

This section covers the C++ operators in a function body: arithmetic, bitwise, comparison, logical, conditional and
assignment operators, and the order in which their parts run. Each operator becomes the Kismet math node the editor
places for the same types, and the type is the one C++ gives after its usual promotions. The rule to remember:
Blueprint math never traps. Integer overflow wraps, and a division by zero gives 0.

### Arithmetic

| You write | What it does | Status |
|---|---|---|
| <a id="arithmetic.kills-3-assists-1"></a>`Kills * 3 + Assists - 1` | The editor's +, - and x nodes for the operand type (Add_IntInt, Subtract_Int64Int64, Multiply_FloatFloat). C++'s promotions pick the type: a uint8 or bool operand becomes int, int32 mixed with int64 is int64, and anything mixed with float is float. | Yes |
| <a id="arithmetic.x-1"></a>`X + 1` with X at the int32 maximum | Wraps around (two's complement), for int32 and int64 alike. C++ leaves signed overflow undefined. | Yes |
| <a id="arithmetic.players-squads"></a>`Players / Squads`, `Players % Squads` | The / and % nodes (Divide_IntInt, Divide_Int64Int64, Divide_FloatFloat, Percent_IntInt). Integer division truncates toward zero and the sign of % follows the dividend, as in C++: -7 / 3 is -2 and -7 % 3 is -1. | Yes |
| <a id="arithmetic.hits-shots"></a>`Hits / Shots` with Shots at 0 | Gives 0 and logs a script warning ("Divide by zero", or "Modulo by zero" for %). The mod does not crash, but it quietly gets 0 where C++ would trap or give inf or NaN. | Yes |
| <a id="arithmetic.id-count"></a>`Id % Count` on int64 | Refused ("unimplemented binary operator % on Int64Int64"): UE 4.27 has no int64 % node. `%=` on an int64 is refused the same way. Write `Id - (Id / Count) * Count`. | Refused |
| <a id="arithmetic.x"></a>`-X`, `-F` | An integer is `0 - X`. A float is `-0.0 - F`, so the sign of zero flips as C++'s `-F` does. A literal such as `-5` is the constant itself. | Yes |
| <a id="arithmetic.x-2"></a>`+X` | Compiles to X. | Yes |

Notes:

- A uint8 result stored back into a uint8 wraps as in C++: 255 + 1 stored in a uint8 is 0.
- double, int16 and uint32 are not Blueprint types, and a parameter of one of them is refused. See [Types](#types).
- `++`, `--` and the compound assignments are under Assignment and updates below.

### Bitwise operators and shifts

| You write | What it does | Status |
|---|---|---|
| <a id="bitwise-operators-and.a-0xff-b-1"></a>`(A & 0xFF) \| (B ^ 1)` | The Bitwise AND, OR and XOR nodes (And_IntInt, Or_IntInt, Xor_IntInt, or the Int64Int64 versions). | Yes |
| <a id="bitwise-operators-and.any-hit"></a>`Any \|= Hit`, `A & B`, `A ^ B` on bools | The OR, AND and XOR Boolean nodes (BooleanOR, BooleanAND, BooleanXOR). Both sides run, as in C++. A constant side decides the value or drops out, but a side that calls something still runs. Under UE_NO_OPTIMIZE the bools are promoted to int as in C++ (Conv_BoolToInt) and the result converted back (Conv_IntToBool). | Yes |
| <a id="bitwise-operators-and.bdone-true"></a>`bDone == true`, `A != B` on bools | The Equal and Not Equal Boolean nodes (EqualEqual_BoolBool, NotEqual_BoolBool), not an int compare of two promoted bools. | Yes |
| <a id="bitwise-operators-and.b-0xff"></a>`~B & 0xFF` | `~` is the Bitwise NOT node (Not_Int, Not_Int64). | Yes |
| <a id="bitwise-operators-and.hi-16"></a>`Hi << 16` | Kismet has no shift node. `X << N` is a multiply by 2^N, which wraps as a shift does. | Yes |
| <a id="bitwise-operators-and.x-1"></a>`X >> 1` | Clears the low N bits and then divides, so it floors as C++ does: -3 >> 1 is -2 and -7 >> 1 is -4. A uint8 is promoted to int first, so 255 >> 1 is 127. | Yes |
| <a id="bitwise-operators-and.x-3ll"></a>`X << 3LL`, `X << SHIFT` | The amount may be any integer literal, with any suffix, or a macro that expands to one. The width comes from the left side alone, so `int32 >> 1LL` is int32 math. | Yes |
| <a id="bitwise-operators-and.x-kshift"></a>`X << kShift`, `X >> (2)` | The amount may be any constant C++ knows when it compiles: a constexpr or const variable, a const local, an enumerator, a cast such as `(int32)3`, `sizeof(...)`, a template's value parameter, and an inline function's parameter when the caller passes a constant. | Yes |
| <a id="bitwise-operators-and.1-kshift"></a>`1 << kShift` | When both sides are constants the whole shift folds to a constant. | Yes |
| <a id="bitwise-operators-and.x-32"></a>`X << 32` | Refused ("bit shift amount out of range"). The amount must be 0..31 for an int32 and 0..63 for an int64 left side. | Refused |
| <a id="bitwise-operators-and.x-n"></a>`X << N`, `X <<= N` with a variable N | Refused ("bit shift with a non-constant amount"): Kismet has no shift node, and a run-time amount cannot become a constant multiplier. Multiply or divide by a power of two instead. | Refused |

```cpp
static constexpr int32 kShift = 3;
int32 Pack(int32 Hi, int32 Lo) { return (Hi << 16) | (Lo & 0xFFFF); }
int32 Half(int32 X) { return X >> 1; }                    // -3 >> 1 is -2, as in C++
int32 Eight(int32 X) { return (X << kShift) | (1 << kShift); } // 1 << kShift folds to 8
```

Notes:

- There is no unsigned (logical) shift, because uint32 is not a Blueprint type.
- `X << -1` is out of range too ("bit shift amount out of range: -1"). Clang adds its own warning for a negative or
  too-large amount (-Wshift-count-negative, -Wshift-count-overflow).

### Comparisons

| You write | What it does | Status |
|---|---|---|
| <a id="comparisons.health-25-f"></a>`Health < 25.f`, `A >= B`, `A == B` | The comparison node for the operand type: Less_IntInt, GreaterEqual_FloatFloat, EqualEqual_Int64Int64 and so on. This covers numbers and bools. | Yes |
| <a id="comparisons.m-emood-calm"></a>`M < EMood::Calm` | An enum compares through its underlying type. A uint8 enum uses the Byte nodes (Less_ByteByte), and an int64-backed enum compares all 64 bits. | Yes |
| <a id="comparisons.b-7"></a>`B == 7`, `B != Other`, `(uint8)M != (uint8)EMood::Calm` on uint8s or byte enums | The byte compare, EqualEqual_ByteByte / NotEqual_ByteByte, the editor's == on bytes. C++ promotes both sides to int, but that changes no equality, so no Conv_ByteToInt is made. A side may be a byte, a byte enum, a cast to uint8 or a constant 0..255. | Yes |
| <a id="comparisons.b-300"></a>`B == 300`, `B != -1` on a uint8 | A constant that is no byte keeps C++'s int compare, so these are always false and always true, as clang warns. | Yes |
| <a id="comparisons.a-this"></a>`A == this`, `P == nullptr` | Object identity, the Equal and Not Equal (Object) nodes. How `P != nullptr` differs from `if (P)` is in [Working with other objects](#working-with-other-objects). | Yes |
| <a id="comparisons.a-b"></a>`A < B` on object pointers | Refused ("unimplemented binary operator < on ObjectObject"): Kismet has only == and != for objects. | Refused |
| <a id="comparisons.a-b-2"></a>`A == B` on FString or FName | Compares case-insensitively, so "abc" == "ABC" is true for both. See [Strings and text](#strings-and-text). | Yes |

### Logical operators and ?:

| You write | What it does | Status |
|---|---|---|
| <a id="logical-operators-and.bfiring"></a>`!bFiring` | The NOT Boolean node. Many negations cost nothing instead; see [The optimizer](#the-optimizer). | Yes |
| <a id="logical-operators-and.x-0-10-x-2"></a>`X != 0 && 10 / X > 2` | C++ short-circuiting is kept: the right side's calls, reads and divisions run only when C++ would run them, so this never divides by zero. `\|\|` runs its right side only when the left is false. | Yes |
| <a id="logical-operators-and.x-0-x-2-x"></a>`X > 0 ? X * 2 : X < -5 ? -1 : 7` | Only the chosen arm runs. The operator compiles to an if/else that stores into a temp, or straight into the variable being assigned, and `return C ? A : B;` becomes two returns. Nested ternaries work. | Yes |
| <a id="logical-operators-and.fstring-c-on-off"></a>`FString(C ? "On" : "Off")`, `FString("x") + (C ? "+" : "-")` | A select of two strings: the temp is an FString, whether the two literals have one length (a `const char[2]` in C++) or not. | Yes |
| <a id="logical-operators-and.c-y-n"></a>`C ? 'y' : 'n'` | Blueprint has no char: the select is an int32, the code of the char picked, which is what C++ promotes a char to. | Yes |
| <a id="logical-operators-and.c-left-right-5"></a>`(C ? Left : Right) = 5` | Refused ("assignment to ConditionalOperator"): Blueprint has no reference to whichever variable the condition picks. Write `if (C) Left = 5; else Right = 5;`. | Refused |
| <a id="logical-operators-and.add5-c-a-b"></a>`Add5(C ? A : B)` into a `T&` parameter | The callee gets a copy, stored back into the picked variable after the call, with a warning. See [Functions](#functions). | Warns |
| <a id="logical-operators-and.a-1-b-2"></a>`A = 1, B = 2;`, `++I, --J` | The comma operator as a statement: each side runs in turn, as two statements. A side that does nothing (`I, J++`) is dropped. A declaration of several variables, `int32 I = 0, J = N;`, is not the comma operator and works too. | Yes |
| <a id="logical-operators-and.if-bool-bok-get-out"></a>`if (bool bOk; Get(Out, bOk), !bOk)`, `switch (N += 10, N)` | An if or switch condition with a comma: the left side runs once, before the test, as an init-statement does, and the right side is the condition. This is how an out-parameter call is tested in one line. | Yes |
| <a id="logical-operators-and.int32-n-bump-m"></a>`int32 N{(Bump(), M)};`, `F((Bump(), M))`, `return (N += 1, N * 2);`, `switch (Bump(), T)` over a `TEnum<E>`, `S = {(Bump(), M), 2}` | A comma inside a statement's expression: the left side runs first, as a statement of its own, and the comma is worth its right side, read where C++ reads it. This holds wherever nothing in the statement runs before the comma: an initialiser, a return value, an assignment's right side, an argument, a braced list's first member. Beside another argument that is no constant (C++ leaves their order open, a parenthesised constructor's `FVector(Get(), (Bump(), M), 0.f)` included; braces fix theirs) the whole argument holding the comma runs first, into a temporary. A left-out argument whose default is a constant counts as that constant. Passed to a reference parameter, `const T&` included, the comma is its right side's variable itself, which the callee reads when it runs, after every argument: only the left side runs first, and the right side must be a variable (see the refusal below). | Yes |
| <a id="logical-operators-and.while-next-x-x-0"></a>`while (Next(X), X > 0)`, `while ((Bump(), N) < 5)`, `A && (Bump(), B)`, `C ? (Bump(), X) : Y`, `Obj->F((Bump(), M))` | Where no statement before the one holding the comma fits, the comma runs in place: its left side, then a copy of its right side, read right after it. That is a loop condition, which runs it again on every trip; the right side of `&&` / `\|\|` and an arm of `?:`, which run it only when C++ does; and an argument of a call on another object, whose object C++ evaluates first. Passed to a reference parameter there, `while (IncRef(0, (Bump(), N)) < 20)`, it is the right side's variable itself, which must then be a plain variable. | Yes |
| <a id="logical-operators-and.g-bump-m"></a>`{G(), (Bump(), M)}`, `L[(Bump(), I)] = G()` | Refused ("the comma operator after something its statement runs first ..."). C++ runs G first: braces evaluate their members in order, and an assignment's right side runs before its left. The comma's left side would run before G. Write the left side as a statement of its own. | Refused |
| <a id="logical-operators-and.while-bump-s-x-3"></a>`while ((Bump(), S).X < 3)` | Refused ("the comma operator used as a place, not a value ..."): a member taken of the comma, or the comma bound to an operator's or a constructor's reference, needs the place itself, and a loop condition has no statement before it to hold the left side. Write the left side as a statement of its own. A comma assigned to or updated there works, and so does a member or an element of one, `while (((Bump(), S).X += 1) < 3)`: see [Assignment and updates](#assignment-and-updates). | Refused |
| <a id="logical-operators-and.f-g-bump-l-i"></a>`F(G(), (Bump(), L[I]))` where F takes `int32&` or `const int32&`, `Get() + (Bump(), L[0])` over FString | Refused: a comma bound to a reference beside an argument that may run first must end in a variable. That covers a call's reference parameter and an operator's reference operand (FString's `+` takes `const FString&`; the message names the operator). A temporary would not be the place the callee writes or reads, and `L[I]` located again after G would not be the one C++ binds. In a loop condition, the right of `&&` / `\|\|` / `?:` or a call on another object, a comma passed to a reference parameter must end in a variable even alone. | Refused |
| <a id="logical-operators-and.bump-m-g-0-5"></a>`(Bump(), M) * (G() + 0.5)` | Refused for the double math (`no Kismet conversion from float to double`), as `M * (G() + 0.5)` is. The comma runs first into a temporary, and the conversion to double stays outside it. A comma whose own value is a type no Blueprint variable holds is refused naming that type. | Refused |
| <a id="logical-operators-and.int32-d-1-4"></a>`int32 D = (1, 4);` as a class default | The right side, when the left side does nothing. A left side that does something would run when the game builds the object, which a default cannot: refused as any computed default. | Yes |

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
| <a id="assignment-and-updates.total-kills-3"></a>`Total = Kills * 3;`, `Other->Score = 10;` | The Set node, on a local, a member or another object's field. A replicated property also gets the editor's FlushNetDormancy and RepNotify around the write; see [Replication](#replication). | Yes |
| <a id="assignment-and-updates.slots-i-value"></a>`Slots[I] = Value;` | Set Array Elem. | Yes |
| <a id="assignment-and-updates.scores-name-5"></a>`Scores[Name] = 5;` | The map Add node, which replaces an existing value. See [Containers](#containers). | Yes |
| <a id="assignment-and-updates.spots-name-x-1"></a>`Spots[Name].X = 1;` | A store into a map value's member is copied out, changed and stored back, as `T E = Map[K]; E.X = 1; Map[K] = E;`, with the key evaluated once. | Yes |
| <a id="assignment-and-updates.acc-i"></a>`Acc += I;`, and `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | `X = X op Y`: a math node and a Set. X is located once, so `Slots[NextSlot()] += By` calls NextSlot a single time, and Y is evaluated first. It works on everything plain `=` works on, map values and their members included. `<<=`, `>>=` and int64 `%=` follow the rules of the plain operators. | Yes |
| <a id="assignment-and-updates.int32-z-y-x-10"></a>`int32 Z = (Y += X) * 10;` | A compound assignment used as a value is X after the store. | Yes |
| <a id="assignment-and-updates.incref-1-b-1"></a>`IncRef(1, B += 1)`, `Peek(SetB(), ++B)`, where the parameter is `int32&` or `const int32&` | Passed to a reference parameter, a compound assignment or a prefix `++` / `--` is the variable itself, as a plain `=` is: the update runs first, as a statement, and the callee reads, and for `T&` writes, B after every argument. Its left side must be a plain variable (`IncRef(1, L[Idx()] += 1)` is refused). The same holds in a loop condition (`while (IncRef(0, B += 1) < 20)`), on the right of `&&` / `\|\|` or in `?:`, and in a call on another object (`P->IncRef(1, B += 1)`). There the call runs as one block each time C++ runs it: the object first, then the update, then the call on B. Behind something its statement runs first, `{G(), IncRef(0, B += 1)}`, it is refused, since the update would run before G. | Yes |
| <a id="assignment-and-updates.x"></a>`X++`, `++X`, `X--`, `--X` | The Increment Int and Decrement Int macros, with X located once. Postfix gives the value before the store, prefix the value after. Works on int32, int64, float and uint8 (a uint8 wraps at 255). On a raw pointer it steps by the element size; see [Pointers and memory](#pointers-and-memory). | Yes |
| <a id="assignment-and-updates.a-b-0"></a>`A = B = 0;`, `S1 = S2 = Name;`, `if ((X = Next()) > 3)`, `return A = N;` | A plain `=` used as a value: the assignment runs first, as a statement, and the value is the variable it wrote, read after it. Numbers, strings and structs alike. It works wherever the comma operator does, and like it, passed to a reference parameter (`const T&` included) it is that variable itself, with no copy. In a loop condition (`while ((V = Next()) > 0)`, `while (UsePair(T = S) < 100)` with a struct passed by value), on the right of `&&` / `\|\|` / `?:` or in an argument of a call on another object it runs in place, as the comma does there; a member taken of it in those places, `while ((T = S).X < 9)`, is refused ("an assignment used as a place, not a value ..."). Behind something its statement runs first, `{G(), (A = M)}`, it is refused ("an assignment used as a value after something its statement runs first ..."). The left side must be a plain variable (`A = L[0] = 1` is refused): it is read again. A comma there names one, so `int32 X = ((Bump(), N) = G());` is `(Bump(), N) = G();` then `X = N`, and `((Bump(), T).A = G())` reads T.A. | Yes |
| <a id="assignment-and-updates.bump-n-1"></a>`(Bump(), N) += 1`, `while (++(Bump(), N) < 5)`, `(Bump(), N) = G();`, `(N = Next()) += G()` | A comma or a plain `=` assigned to or updated writes the variable it names: the comma's right side, or the `=`'s left side after the assignment. The outer assignment's right side runs first, as C++17 orders it, then the comma's left side (or the inner assignment), then the store: `(Bump(), N) = G()` gives N what G returned before Bump ran; a struct's or FString's `=` reads an element on its right through its reference, so `(Bump(), T) = L[Count]` locates L[Count] before Bump and reads it after. In a loop condition, on the right of `&&` / `\|\|` and as a statement alike. A member or an element of one is the same member or element of the variable, its index run after the comma: `while (((Bump(), T).A += 1) < 5)`, `((Bump(), L)[0] += 1)`, `(Bump(), L)[Idx()] = G();` (G, then Bump, then Idx). The inner `=`'s left side must be a plain variable, or a comma that names one (`((Bump(), N) = G()) += 1`); `(L[Idx()] = M) += 1` is refused: it is named twice. An update or a struct's `=` there is the same: `while (((N += G()) += 1) < 20)` is `N += G(); N += 1` on every trip, `++(N += 1)` steps N twice, `(N += G()) = N + 5;` reads N + 5 first, and `while (((T = S).A += 1) < 5)` copies S, then updates T.A. `(L[Idx()] += M) += 1` is refused the same way. | Yes |

### Evaluation order

| You write | What it does | Status |
|---|---|---|
| <a id="evaluation-order.slots-cursor-nextslot"></a>`Slots[Cursor] = NextSlot();` | The right side of `=` or `op=` runs first, then the destination (object, array, index, map key) is located, as C++17 requires. This stores at the new Cursor. | Yes |
| <a id="evaluation-order.getpeer-setcursor-swappeerinline"></a>`GetPeer()->SetCursor(SwapPeerInline());` | The object a call runs on is evaluated before its arguments. When an argument needs statements of its own that could change the object, the object is saved into a local first. The same holds for `E1[E2]`, a container method's container and a dispatcher's Broadcast. | Yes |
| <a id="evaluation-order.bump-100-bumpinline"></a>`Bump() * 100 + BumpInline()`, `Pair(Bump(), BumpInline())` | An operand or argument that needs statements (an inline call, `&&`, `\|\|`, `?:`, a `++` or `op=` value, a pointer read) runs before the whole expression or call. C++ leaves this order unspecified. | Yes |
| <a id="evaluation-order.fintpoint-p-bump-m-n"></a>`FIntPoint P = {Bump(), M ? N : 0};` | A struct literal's members run left to right, each once, as C++ requires of braces: each computed member is its own statement. | Yes |

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
| <a id="conditions.if-target"></a>`if (Target)` | An object pointer is the Is Valid node, so an object that is pending kill counts as false. `Target != nullptr` can still be true for it; see [Working with other objects](#working-with-other-objects). | Yes |
| <a id="conditions.if-count"></a>`if (Count)` | An int32 goes through Conv_IntToBool. | Yes |
| <a id="conditions.if-f"></a>`if (F)` on a float, an int64, a byte or an enum | Compared with zero by a Not Equal node, so a NaN counts as true. | Yes |
| <a id="conditions.bool-f-x"></a>`(bool)F + X`, `bool IsSet(int32 V) { return V; }` | A number converts to bool the same way outside a condition: true when nonzero, as in C++. `(bool)F + X` adds 0 or 1. | Yes |
| <a id="conditions.if-tag"></a>`if (Tag)` | An FName is `Tag != None` (NotEqual_NameName). `!Tag` and `static_cast<bool>(Tag)` work too. | Yes |
| <a id="conditions.target-target-customtimedilation"></a>`Target && Target->CustomTimeDilation > 0` | Works as a guard: the Is Valid test decides, through the short-circuit, whether the read runs. | Yes |

An interface value is true when an object is behind it, a plain null test in which a pending-kill object still counts
([Interfaces](#interfaces)). A raw pointer is true when its address is nonzero
([Pointers and memory](#pointers-and-memory)).

### if

| You write | What it does | Status |
|---|---|---|
| <a id="if.if-health-0-die-else"></a>`if (Health <= 0) Die(); else Heal();` | The Branch node, compiled to a conditional jump. `else if` chains work. The condition converts to bool as described under Conditions. | Yes |
| <a id="if.if-int32-twice-v-2"></a>`if (int32 Twice = V * 2; Twice > 10)` | The init-statement runs once, before the test. An expression works as well: `if (N++; N > 3)`. | Yes |
| <a id="if.if-apawn-p-cast-apawn"></a>`if (APawn *P = Cast<APawn>(Obj))` | A declaring condition: P is made once and tested. | Yes |
| <a id="if.if-constexpr-sizeof-t-8"></a>`if constexpr (sizeof(T) == 8)` | Only the branch clang kept is compiled, and no jump is left behind, in a UE_NO_OPTIMIZE function too. | Yes |

Notes:

- A variable declared in a condition or an init-statement stays a function-wide local afterwards; see
  [Locals](#locals). Correct C++ cannot tell.
- A member template has no function of its own. Each instantiation a call names is inlined; see
  [Inline functions and templates](#inline-functions-and-templates).

### switch

| You write | What it does | Status |
|---|---|---|
| <a id="switch.switch-code"></a>`switch (Code)` on an int32, uint8, bool or enum | A jump table: the value is evaluated once into a local, one Select on it (EX_SwitchValue) picks the code offset of the matching case, and one jump (EX_ComputedJump) goes there. The case bodies are compiled in source order, so fallthrough, `[[fallthrough]]`, `default` in any position, `break` and `continue` behave as in C++. | Yes |
| <a id="switch.switch-id"></a>`switch (Id)` on an int64 | The same jump table, comparing all 64 bits. | Yes |
| <a id="switch.switch-int32-k-v-1"></a>`switch (int32 K = V + 1; K)` | An init-statement or a declaring condition runs once, then the plain switch. | Yes |
| <a id="switch.switch-ue-name-switch-kind"></a>`switch (UE_NAME_SWITCH(Kind))` | The editor's Switch on Name, shown below. | Yes |
| <a id="switch.case-1-3"></a>`case 1 ... 3:` | GNU case ranges are refused ("unimplemented statement IntegerLiteral"). List the cases. | Refused |
| <a id="switch.switch-x-case-1-return"></a>`switch (X) case 1: return 1;` | Refused ("`switch` needs a braced body"). | Refused |
| <a id="switch.case-0-r-1-case"></a>`case 0: { R = 1; case 1: R += 2; }` | A case label inside a nested block is refused ("unimplemented statement CaseStmt"). Every case label must sit directly in the switch body. | Refused |

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

- Every switch dispatches the same way, however many cases it has and however they are spread: one evaluation, one
  native scan of the case values, one jump. The editor's own Switch on Int instead compares the value with each case in
  turn, one Kismet call per case.
- A value no case names goes to `default`, or past the switch. It never makes the engine's "Switch statement failed
  to match case" warning: the scan's last entry is the value itself, which always matches, and carries that target.
- On a uint8, bool or byte enum, a case outside 0..255 (-128..127 for int8) can never match, so it is left out of the
  table. Its code is reached only by falling through into it, as in C++.
- `using enum` and `[[likely]]` inside a switch are fine.
- C++ switches only on integers, so UE_NAME_SWITCH and UE_NAME_CASE give clang a switch over hashes of the case
  texts. AssetGen stores the name once and dispatches through the same jump table, comparing names
  case-insensitively, as FName comparison is. Fallthrough and `default` work as in any switch.
- Every case of a name switch must be `UE_NAME_CASE("literal")`, or the switch is refused ("a case of UE_NAME_SWITCH
  needs UE_NAME_CASE"). Two identical texts are a duplicate-case error from clang. Two texts that differ only in case
  compile, but only the first can ever match.

### Jumps

| You write | What it does | Status |
|---|---|---|
| <a id="jumps.break"></a>`break;`, `continue;` | Plain jumps. `break` leaves the innermost loop or switch. `continue` goes to a for loop's increment, a do-while's test, or the loop around a switch. Inside an inline function's body they belong to that body's own loops. | Yes |
| <a id="jumps.goto-done"></a>`goto done;` | A jump, backward or forward, which can leave any number of loops at once. The editor's nearest equivalent is a wire looping back. Each expansion of an inline function gets its own labels. A declaration that a goto reaches again is made again, as one in a loop is. | Yes |
| <a id="jumps.return-found"></a>`return Found;`, `return;` | The Return Node. `return C ? A : B;` becomes two returns. In an inline function, return jumps to the end of the expanded body. Inside a TMap loop that writes values back, the values are stored first. | Yes |
| <a id="jumps.return-log-errors-bad"></a>`return Log(Errors, "bad");` in a void function | A void call, then a plain return, as C++ runs it. `return (void)X;` with an X that does nothing is a plain return. | Yes |
| <a id="jumps.goto"></a>`goto` inside a TMap loop that writes values back | Refused; see [Loops](#loops). | Refused |

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
| <a id="statements-and-blocks.int32-tmp-a-a-b"></a>`{ int32 Tmp = A; A = B; B = Tmp; }` | A bare block. Its statements join the enclosing list, and nothing is destroyed at the closing brace. | Yes |
| <a id="statements-and-blocks.row"></a>`;`, `Done: ;`, `[[fallthrough]];` | Compile to nothing. | Yes |
| <a id="statements-and-blocks.case-angry-likely-return-n"></a>`case Angry: [[likely]] return N;` | `[[likely]]` and `[[unlikely]]` are hints for another compiler. Only the statement they mark is compiled. | Yes |
| <a id="statements-and-blocks.static-assert"></a>`static_assert`, `using`, `typedef` in a body | Compile-time only. A local cannot yet use a type alias declared in the function; see [Locals](#locals). | Yes |
| <a id="statements-and-blocks.void-compute"></a>`(void)Compute();` | The call. | Yes |
| <a id="statements-and-blocks.c-open-close"></a>`C ? Open() : Close();`, `bReady && Launch();`, `X + 1;` | Refused ("unimplemented statement ConditionalOperator", or BinaryOperator): an expression statement must be a call, an assignment or an update. Write the if out. | Refused |
| <a id="statements-and-blocks.void-unused"></a>`(void)Unused;` | Refused ("unimplemented statement DeclRefExpr"). To silence an unused-parameter warning, leave the parameter unnamed. | Refused |
| <a id="statements-and-blocks.try-risky-catch-recover"></a>`try { Risky(); } catch (...) { Recover(); }` | Refused ("unimplemented statement CXXTryStmt"): Blueprint has no exceptions. | Refused |

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
| <a id="while-and-do.while-i-limit-100-limit"></a>`while (I < Limit && 100 / (Limit - I) > 1)` | The editor's WhileLoop without the flow stack: a test at the top and a jump back. The condition runs on every trip, with any statements it needs (an inline call, `&&`, `?:`, a pointer read). An inline function called in the condition gets fresh locals each trip. | Yes |
| <a id="while-and-do.while-true"></a>`while (true)` | No test at all. Leave with `break` or `return`. | Yes |
| <a id="while-and-do.while-int32-left-start-steps"></a>`while (int32 Left = Start - Steps)` | Left is made again and tested on every trip. | Yes |
| <a id="while-and-do.do-i-1-while-i"></a>`do { I += 1; } while (I < Limit);` | The body runs once before the first test, and `continue` jumps to the test. | Yes |
| <a id="while-and-do.do-r-1-if-x"></a>`do { R = 1; if (X > 0) break; R = 2; } while (false);` | Runs once with no test. `break` leaves it. | Yes |

Note: UE 4.27 compiles Blueprint's runaway-loop guard (1,000,000 iterations) out of Shipping and Test builds. In such
a build a loop that never ends hangs the game instead of being stopped.

### for

| You write | What it does | Status |
|---|---|---|
| <a id="for.for-int32-i-0-i"></a>`for (int32 I = 0; I < Count; ++I)` | `Init; while (Cond) { Body; Inc; }`: the editor's ForLoop, with any condition and increment. `continue` reaches the increment. Loops nest freely. | Yes |
| <a id="for.for"></a>`for (;;)`, `for (; I < N; I++)` | A missing condition is `true`, and a missing init or increment is left out. | Yes |
| <a id="for.for-int32-i-0-int32"></a>`for (int32 I = 0; int32 L = N - I; ++I)` | A declaring condition: L is made again and tested on every trip, and `continue` still reaches the increment. | Yes |
| <a id="for.for-int32-i-0-j"></a>`for (int32 I = 0, J = N; I < J; ++I, --J)` | Two variables, both updated in the increment. See [Operators](#operators) for the comma. | Yes |

### Range-for over a TArray

| You write | What it does | Status |
|---|---|---|
| <a id="range-for-over.for-int32-x-items"></a>`for (int32 X : Items)` | The editor's ForEachLoop: the array is walked by index, and X is a fresh copy on each pass, so assigning to X never changes the array. | Yes |
| <a id="range-for-over.for-int32-x-items-2"></a>`for (int32 &X : Items)` | X is another name for `Items[Index]`, so writes land in the array. `const T&` and `auto&&` work the same way. ForEachLoop's element is a copy, so it has no equivalent. | Yes |
| <a id="range-for-over.for-auto-x-y-points"></a>`for (auto [X, Y] : Points)` | A structured binding over TArray or TSet elements is refused ("a structured binding over a TArray"). Bind the element, `for (const FIntPoint &P : Points)`, and read `P.X`. | Refused |
| <a id="range-for-over.for-auto-item-index-items"></a>`for (auto &[Item, Index] : Items.Enumerate())` | The element and its index together, walked by index like `for (int32 &X : Items)`: Item is `Items[Index]` where it lives, so writes land in the array (a struct element's members too), and Index counts from 0. `const auto&` reads in place; `auto [Item, Index]` copies the element on each pass. A method, because `auto &[A, B] : Items` already means unpacking each element. TArray only. | Yes |
| <a id="range-for-over.index-0"></a>`Index = 0;`, `++Index` in that loop; `Item = 0;` under `const auto &[Item, Index]` | Refused by clang: `cannot assign to variable 'Index' with const-qualified type 'const int32'`, and the same for a const Item. The index is the loop's own, so no form of the loop lets the body change it. | Refused |
| <a id="range-for-over.auto-e-items-enumerate"></a>`auto E = Items.Enumerate();` | Refused ("unimplemented local E: TEnumerated<int>"): Enumerate() is only a range-for's range. `S.Enumerate()` on a TSet or TMap is clang's "no member named 'Enumerate'". | Refused |

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
and after a removal the walk runs past the new end. Do not change the array's length inside the loop; see
[Changing the container a loop walks](#changing-the-container-a-loop-walks).

### Range-for over a TSet

| You write | What it does | Status |
|---|---|---|
| <a id="range-for-over.for-const-int32-x-seen"></a>`for (const int32 &X : Seen)`, `for (auto &X : Seen)` | Walks the set where it lives, with no copy. X is the element in the set, and const: `int32 &` is a clang error, because a set element is its own key. | Yes |
| <a id="range-for-over.for-int32-x-seen"></a>`for (int32 X : Seen)` | By value: the same walk over the set itself, each pass copying the element into X. | Yes |
| <a id="range-for-over.seen-remove-x"></a>`Seen.Remove(X)` in the body | Unsafe, as in C++, by value too: the walk stays as it is, and the compiler warns. To remove while walking, walk a copy you take: `TArray<int32> All; Seen.ToArray(All); for (int32 X : All) ...`. See [Changing the container a loop walks](#changing-the-container-a-loop-walks). | Warns |

The set walk follows the TMap notes below: an element aligned past 8 bytes walks a To Array copy, free slots are
skipped, and it cooks the same four helper structs, named after the set type.

### Range-for over a TMap

| You write | What it does | Status |
|---|---|---|
| <a id="range-for-over.for-auto-key-value-scores"></a>`for (auto &[Key, Value] : Scores)` | Walks the map where it lives, with no copy and no local per name. Value is the value in the map: a write through it is what `Scores[Key]` reads next, a `T&` binds it with no copy, and a container value changes in place. Key is the key in the map, and const (`TPair<const K, V>`); a key that is an object pointer is a const pointer, so `Key->Hits += 1` still writes the object. `const auto &[Key, Value]` reads the same way. The 4.27 editor has no map ForEach; this replaces Keys, ForEachLoop and Find. | Yes |
| <a id="range-for-over.for-auto-pair-scores"></a>`for (auto &Pair : Scores)` | The same walk: `Pair.Key` and `Pair.Value` are the two names `auto &[Key, Value]` binds, and `const auto &K = Pair.Key; auto &V = Pair.Value;` are other names for them, with no copy. `const auto &Pair` and a by-value `auto Pair` work as their `[Key, Value]` forms do. Pair itself is used only through `.Key` and `.Value`: passing it whole is refused ("a TMap range-for's pair variable is read only as P.Key and P.Value"). | Yes |
| <a id="range-for-over.scores-key"></a>`Scores[Key]`, Key the current key | The current slot, as Value is: a read or a store (`Scores[Key] = 5`, `Scores[Key].Add(7)`) goes to the value in place, with no Find or Add. Key may be the binding, `Pair.Key`, or a reference to either (`const auto &K = Pair.Key`). | Yes |
| <a id="range-for-over.for-auto-key-value-scores-2"></a>`for (auto [Key, Value] : Scores)` | By value: the same walk over the map itself, each pass copying the key and the value into Key and Value, as C++ copies the element out. Value is a copy and nothing is written back; `Scores[Key]` is still the map's own value, in place. | Yes |
| <a id="range-for-over.scores-remove-key"></a>`Scores.Remove(Key)`, `Scores[Other] = 1` in the body | Unsafe, as in C++, by value too: the walk stays as it is, and the compiler warns. To remove while walking, walk a copy of the keys you take (below). See [Changing the container a loop walks](#changing-the-container-a-loop-walks). | Warns |
| <a id="range-for-over.goto"></a>`goto` inside a TMap loop that writes values back | Refused ("a goto inside a TMap range-for that writes its value back would skip the write"), even when the label is inside the loop. Only a reference loop over a pair aligned past 8 bytes writes values back: it walks a copy of the keys, and the body changes a container value through `auto &[Key, Value]`. Leave with `break`, or bind `const auto&`. | Refused |

```cpp
TMap<FName, int32> Scores;
void Update() {
  for (auto &[Key, Value] : Scores) Value += 10;    // in place
  for (auto [Key, Value] : Scores) Value += 1000;   // a copy: the map keeps its values
  TArray<FName> Keys;                               // removes: walks a copy of the keys, taken here
  Scores.Keys(Keys);
  for (const FName &Key : Keys) {
    if (Scores[Key] < 0) { Scores.Remove(Key); continue; }
    Scores[Key] += 1;
  }
}
```

Notes:

- The walk needs a key and a value that align to 8 bytes or less. Otherwise the loop walks a copy of the keys, each
  read of Value a Find and each store an Add, a container value copied and stored back at the end of each pass and
  before a `break` or `return`.
- A map that has lost elements has free slots. The walk reads the map's free list once, before the first pass, and
  skips those slots; a map with none, the usual case, costs one test per pass. The map is never copied or moved.
- The number of slots is read once, before the first pass, as C++ reads `end()` once.
- `break`, `continue` and `return` work in a map loop, and so does a `return` in an inline function the body expands.
- A walk cooks four helper structs into the mod's own folder, named after the map type, such as
  `FMapSlot_TMap_FName_int`, `FMapSlots_TMap_FName_int`, `FMapSlot_Free_TMap_FName_int` and
  `FMapSlots_Free_TMap_FName_int`. They ship with the mod like its other packages.

### Changing the container a loop walks

Every range-for follows the C++ iterator contract, by reference or by value: the body must not add to or remove from
the container it walks. The compiler compiles such a body as written, does not work around it,
and warns, with one message for every form:

```
warning: RangeTest::WarnRemove: unsafe to mutate iterated container (Scores.Remove)
```

| The body does | Warned as |
|---|---|
| a count-changing method on the walked container: Add, AddItems, AddUnique, Append, Clear, Emplace, Empty, FindAndRemoveChecked, FindOrAdd, Insert, Remove, RemoveAndCopyValue, RemoveItem, RemoveItems, Reset, Resize, Set, Shrink, Compact, a sort | `(Scores.Remove)` |
| a whole assignment to it | `(Scores =)` |
| a TMap store with any key but the current one, `Scores[Other] = 1`, which adds the key when the map lacks it | `(Scores[] =)` |
| the container passed to a non-const reference parameter | `(Prune(Scores))` |
| a call to one of the class's own methods whose body may change a container of that type, followed through the calls it makes | `(Prune())` |

What happens if you do it anyway: every walk reads its length, and a TSet or TMap walk its free slots, once, before the
first pass. An added element is not visited. In a TSet or TMap walk, a removed element leaves a free slot that a later
pass reads as an element (a crash for a string key); in a TArray walk, a removal walks past the end. To change the
container while walking it, walk a copy you take first: `Scores.Keys(Keys)`, `Seen.ToArray(All)`, or a TArray copy.

Not warned, and safe:

- `Scores[Key]` with the current key, which is the slot itself, and any write through Value.
- A change to another container, even one of the same type: `Positive.Add(Key, Value)` while walking Scores. Only the
  names are compared, so a reference or pointer that turns out to be the walked container is not seen.
- A change made by an engine function, which the compiler cannot see into.

### What a range-for walks

| You write | What it does | Status |
|---|---|---|
| <a id="what-a-range.for-int32-x-pickpeer-items"></a>`for (int32 X : PickPeer()->Items)` | The range expression is evaluated once, into a local, as C++ binds its range once. PickPeer runs once, and reseating a pointer in the body does not move the walk. | Yes |
| <a id="what-a-range.for-int32-x-getitems"></a>`for (int32 X : GetItems())`, `for (auto *C : A->GetComponentsByClass<UStaticMeshComponent>())` | The value is computed once, into a local, before the loop: the temporary C++ binds the range to. GetItems runs once, and an `int32 &X` loop writes that copy, not the container GetItems read. | Yes |
| <a id="what-a-range.for-int32-x-1-2"></a>`for (int32 X : {1, 2, 3})` | Refused: the range is a std::initializer_list, not a Blueprint container. A `TArray<int32>{1, 2, 3}` written in the range is refused as well. Declare `TArray<int32> L = {1, 2, 3};` and walk L, or walk a `static inline const` array. | Refused |
| <a id="what-a-range.for-int32-p-kprimes"></a>`for (int32 P : kPrimes)` | A `static inline const` array of constants; see [Constants](#constants). | Yes |
| <a id="what-a-range.for-int32-scale-3-int32"></a>`for (int32 Scale = 3; int32 X : Items)` | The C++20 init-statement runs once, before the loop. | Yes |

```cpp
TArray<int32> Items;
TArray<int32> GetItems() { return Items; }
int32 SumItems() {
  int32 Sum = 0;
  for (int32 X : GetItems()) Sum += X;   // GetItems runs once
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
| <a id="declaring-locals.int32-sum-0"></a>`int32 Sum = 0;` | A Local Variable. The initializer runs each time the declaration is reached. | Yes |
| <a id="declaring-locals.fintpoint-info"></a>`FIntPoint Info;` | With no initializer the local starts as zero, empty, None or the struct's defaults. It never holds garbage, unlike in C++. | Yes |
| <a id="declaring-locals.tarray-int32-acc"></a>`TArray<int32> Acc;` inside a loop | Reset to its starting value every time a loop, a goto or a repeated inline expansion reaches the declaration. That includes the locals of an inline function called in a loop or in a loop's condition. | Yes |
| <a id="declaring-locals.int32-i-0-hits-0"></a>`int32 I = 0, Hits = 0;` | Several variables in one declaration. | Yes |
| <a id="declaring-locals.const-int32-three-3"></a>`const int32 Three = 3;` | A local holding a constant that nothing writes is no variable at all; see [The optimizer](#the-optimizer). | Yes |

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
| <a id="names-and-scope.for-int32-i-0-i"></a>`for (int32 I = 0; I < 2; ++I)` after another loop over I | Locals of the same name and type in different blocks or loops are one variable. Correct C++ cannot observe this. | Yes |
| <a id="names-and-scope.int32-total-0"></a>`int32 Total = 0;` with a member Total | The local shadows the member, and the member is left alone. | Yes |
| <a id="names-and-scope.if-int32-rest-v-3"></a>`if (int32 Rest = V % 3)` | A variable declared in a condition or an init-statement stays a function-wide local afterwards. | Yes |

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
| <a id="reference-locals.int32-alias-count"></a>`int32 &Alias = Count;` | Another name for a local, a parameter or a member of this (`Score` or `this->Score`), or for a member of one of their structs (`float &Z = Spawn.Z;`). Reads and writes are that variable's own Get and Set. | Yes |
| <a id="reference-locals.int32-slot-slots-i"></a>`int32 &Slot = Slots[I];` | A TArray element (of this object or another), `*P` or `P[I]`: the reference keeps the place's address in a hidden int64 local, and reads and writes go straight to that memory. | Yes |
| <a id="reference-locals.const-fvector-home-k2"></a>`const FVector &Home = K2_GetActorLocation();` | A `const T&` bound to a temporary is a copy, which lives as long as the reference. | Yes |
| <a id="reference-locals.int32-r-other-score"></a>`int32 &R = Other->Score;` | Another object's field has no Blueprint reference, so R keeps the field's address, found by name at run time as `&Other->Score` is. That needs the ReadProperty class you supply (see [Pointers and memory](#pointers-and-memory)); without one the build stops ("include ReadProperty.h"). The `const` and `auto &` forms, and `O->Score` through a local pointer, are the same. Without the helper, write to the field directly, or pass it to an inline function's `T&` parameter, which binds the field itself. | Yes |
| <a id="reference-locals.float-x-other-loc-x"></a>`float &X = Other->Loc.X;` | X keeps the address of Loc, found by name at run time as `&Other->Loc` is, plus X's offset. That needs the ReadProperty class, as the row above does. | Yes |

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
- A reference that holds the address of a struct reads and writes it as `*P` does: with `FVector &V = Pts[0];`,
  `V.X = 2.f` writes the member and `FVector C = V;` copies the struct. A reference to a TArray is usable only through
  its elements: a TArray function called on a `TArray<int32> &` is refused ("a whole TArray<int32> through a
  pointer").

### Other declarations

| You write | What it does | Status |
|---|---|---|
| <a id="other-declarations.using-t-int32-t-a"></a>`using T = int32; T A = X;`, `typedef TArray<T> FList;` | An alias declared inside a function means its type from the declaration to the end of the block holding it, as in C++. The alias itself compiles to nothing. | Yes |
| <a id="other-declarations.auto-a-b-point"></a>`auto [A, B] = Point;` | Refused outside a TMap range-for ("a declaration statement declares nothing usable"). Read the members: `int32 A = Point.X;`. | Refused |
| <a id="other-declarations.auto-inc-int32-v-return"></a>`auto Inc = [](int32 V) { return V + 1; };` | A lambda is refused; see [Functions](#functions). | Refused |

## The optimizer

AssetGen optimizes each function's bytecode. It folds constants, drops work whose result nothing uses, lets locals
share variables, copies in the calls whose one body it knows and turns the bool an inline function returns into a
jump. None of this changes what a function does,
and a store to a member or to another object is never removed. The rule to remember: mark a method UE_PURE only when calling it has no side effects, because a
pure call whose result is unused is dropped.

### Constant folding

| You write | What it does | Status |
|---|---|---|
| <a id="constant-folding.n-kstep-kslots-2-1"></a>`N * kStep + kSlots * 2 + (1 << 4 \| kStep)` | Every operator expression over constants compiles to its value; here one multiply and two additions remain. Each step is rounded to its C++ type, so `1 / 2.f` is 0.5f and `1 << 31` wraps. This covers arithmetic, bitwise operators, shifts, comparisons, `&&`, `\|\|` and `?:`. | Yes |
| <a id="constant-folding.false-x"></a>`false && X`, `true \|\| X` | X is not compiled. A float constant is true when it is nonzero. | Yes |
| <a id="constant-folding.n-3-8-19"></a>`N * 3 + 8 + 19` | Only constant subexpressions fold. C++ groups `+` from the left, so both additions stay; `N * 3 + (8 + 19)` has one. | Yes |
| <a id="constant-folding.x-0"></a>`X + 0`, `X - 0`, `X * 1`, `X / 1`, `X \| 0`, `X ^ 0` | Compile to X with no node. A float keeps its + and -, because -0.0f + 0 is +0.0f. | Yes |
| <a id="constant-folding.x-0-2"></a>`X / 0` | A division or modulo by a constant zero is not folded. It runs and gives 0, and clang warns -Wdivision-by-zero. | Yes |

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
| <a id="constant-locals-and.const-int32-three-3"></a>`const int32 Three = 3;` | A local initialised to a constant that nothing writes is no variable: each read is the constant itself. A local the body also writes keeps its variable. Strings and containers are never folded this way, since they would be rebuilt at every use. | Yes |
| <a id="constant-locals-and.if-lim-3-break"></a>`if (Lim > 3) break;` with a constant Lim | An if whose condition folds compiles only the branch it takes. | Yes |
| <a id="constant-locals-and.while-false"></a>`while (false)`, `while (true)`, `do { } while (false)` | `while (false)` compiles to nothing, and the other two get no test. Code that a label or a case makes reachable is kept. | Yes |

### Negations

| You write | What it does | Status |
|---|---|---|
| <a id="negations.ammo-0"></a>`!(Ammo > 0)` | Becomes `Ammo <= 0`: an ordered comparison of integers or enums flips for free. | Yes |
| <a id="negations.a-b"></a>`!(A == B)` | Becomes `A != B` for the built-in comparisons (numbers, enums, bools, object pointers). FString and FName `==` is an operator call, so `!(S == T)` keeps its NOT node. | Yes |
| <a id="negations.x"></a>`!!X`, `if (!X) A(); else B();` | `!!X` is X, and an if on `!X` swaps its branches instead. | Yes |

Note: an ordered comparison of floats is never flipped, because a NaN is neither `A < B` nor `A >= B`. The result
stays what C++ says.

### What the optimizer removes

| You write | What it does | Status |
|---|---|---|
| <a id="what-the-optimizer.ukismetmathlibrary-abs-int-a"></a>`UKismetMathLibrary::Abs_Int(A);` | A pure call whose result is unused is dropped. Clang also warns -Wunused-value. | Yes |
| <a id="what-the-optimizer.int32-unused-a-3"></a>`int32 Unused = A * 3;` | A local that nothing reads is removed, with its stores. | Yes |
| <a id="what-the-optimizer.int32-rolled-ukismetmathlibrary"></a>`int32 Rolled = UKismetMathLibrary::RandomInteger(A);` | The unread local goes, but an impure initializer still runs as a call. | Yes |
| <a id="what-the-optimizer.return-x-a-b"></a>`return X + A * B;` after `int32 X = A * B;` | A repeated pure call is not reused: it is evaluated each time it appears. To compute it once, keep the result in a local and read the local. | Not yet |
| <a id="what-the-optimizer.if-isready-item"></a>`if (IsReady(Item))` over an inline bool function whose returns are all constants | Each `return true` and `return false` jumps straight to the branch it picks: no bool is stored and then tested. | Yes |
| <a id="what-the-optimizer.scale-value-tick"></a>`Scale(Value, Tick())` into `inline int32 Scale(int32 V, int32 By)` | A parameter the body only reads is the caller's local variable itself, not a copy, while nothing can change that variable during the call. Another argument that writes it, or a reference parameter bound to it, keeps the copy. | Yes |
| <a id="what-the-optimizer.bump-v"></a>`Bump(V)` in a `final` class or to a `final` method, `Base::Bump(V)`, or a static of a class this source cooks that is no function library | The call: the body is copied in, as an inline function's is. A function library's static stays a call unless it is `[[gnu::always_inline]]`. See [Calling your own functions](#calling-your-own-functions). | Yes |

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
| <a id="ue-no-optimize.ue-no-optimize-int32-raw"></a>`UE_NO_OPTIMIZE int32 Raw(int32 X, int32 Y)` | `[[clang::optnone]]`: the function is compiled as written. Unused pure calls, unread locals and constant branches stay, a harmless `&&` or `\|\|` keeps its branch, and no folding or negation rewrite happens. | Yes |
| <a id="ue-no-optimize.pragma-clang-optimize-off"></a>`#pragma clang optimize off` ... `#pragma clang optimize on` | The same for every function between the two lines. | Yes |

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
| <a id="methods-as-blueprint.int32-addcharges-int32-by"></a>`int32 AddCharges(int32 By) { ... }` | A Blueprint function, Public and BlueprintCallable. An editor Blueprint can call it, and an editor subclass can override it. | Yes |
| <a id="methods-as-blueprint.void-refill-int32-amount"></a>`void Refill(int32 Amount);` and `void AMyActor::Refill(...) { ... }` | The same function, defined outside the class or in a `.cpp` beside the header. The definition gives the body and parameters; the declaration gives `static` and the access. | Yes |
| <a id="methods-as-blueprint.int32-pick-int32-int32-b"></a>`int32 Pick(int32, int32 B)` | A parameter left unnamed is cooked as `P<its index>` (`P0` here; `_` is added while another parameter or a local of the body has that name, so `void Fill(int32&, int32 B) { int32 P0 = B * 2; ... }` names it `P0_`), as the editor names every pin. Callers pass it in its place. | Yes |
| <a id="methods-as-blueprint.int32-getcharges-const"></a>`int32 GetCharges() const` | A const Blueprint function, as a UFUNCTION declared const would be. | Yes |
| <a id="methods-as-blueprint.ue-pure-int32-doubled-const"></a>`UE_PURE int32 Doubled() const` | A pure function: the editor draws it as a node without exec pins. Each C++ call runs once, where it is written. An editor pure node, by contrast, runs again for each use. | Yes |
| <a id="methods-as-blueprint.ue-pure-static-int32-clamp01"></a>`UE_PURE static int32 Clamp01(int32 V)` | A pure static function. | Yes |
| <a id="methods-as-blueprint.int32-doubled-int32-p-const"></a>`int32 Doubled(int32 P) const` | BlueprintPure as well, as UHT makes a const BlueprintCallable. Only the flag: each C++ call still runs, and an unused one is kept, since `const` does not promise what `UE_PURE` does. A `void` const method is not pure, an override takes its parent's flags, and a static needs `UE_PURE`. | Yes |
| <a id="methods-as-blueprint.virtual-int32-priority"></a>`virtual int32 Priority()` | `virtual` is accepted and changes nothing. Every mod method is already called by name, and the most derived version runs. | Yes |
| <a id="methods-as-blueprint.virtual-int32-step-final"></a>`virtual int32 Step() final` | No subclass has a Step of its own: the function is cooked Final, and calls to it are direct, as in a `final` class. A subclass method named Step is refused. C++ allows `final` only on a virtual method. | Yes |
| <a id="methods-as-blueprint.virtual-int32-score-0"></a>`virtual int32 Score() = 0;` | An empty function that returns the default: 0, false, None or empty. A subclass's Score overrides it and names it as its super. A class that declares one, or inherits one with no version of its own, is cooked Abstract, which SpawnActor and CreateWidget refuse, as they refuse a class the editor marks Generate Abstract Class. `NewObject` and `AddComponentByClass` do not check the flag in a game, so a call to Score on such an object gets the default. A `SpawnActor` that names such a class warns, and a `NewObject` that names it is refused ([Objects and widgets](#objects-and-widgets)). | Yes |
| <a id="methods-as-blueprint.public"></a>`public:` / `protected:` / `private:` | Become the function's Public, Protected or Private flag, which the editor honours; see [Classes and variables](#classes-and-variables). | Yes |
| <a id="methods-as-blueprint.ue-category-turret-setup"></a>`UE_CATEGORY("Turret\|Setup");` | The category of the members that follow, written into the editor API stub; see [Classes and variables](#classes-and-variables). | Yes |
| <a id="methods-as-blueprint.ue-tooltip-ue-keywords"></a>`UE_TOOLTIP("...") UE_KEYWORDS("...")` | The function's tooltip and search keywords, written into the editor API stub; see [Classes and variables](#classes-and-variables). | Yes |
| <a id="methods-as-blueprint.ue-authority-only"></a>`UE_AUTHORITY_ONLY` / `UE_COSMETIC` | The editor's Authority Only and Cosmetic function flags; see [RPCs](#rpcs). | Yes |
| <a id="methods-as-blueprint.generate-api-true"></a>`generate_api: true` in `mods.yaml` | Writes an editor stub of the class, so a Blueprint made in the editor can place call nodes for its functions; see [Editor API stubs](GUIDE.md#editor-api-stubs). | Yes |
| <a id="methods-as-blueprint.int32-undef-int32-x"></a>`int32 Undef(int32 X);` with no body anywhere | No function is cooked, and a call to it is refused ("... declares and never defines"), as C++ would not link it. Give every method a body, or make it `virtual ... = 0` for an empty one. | Refused |
| <a id="methods-as-blueprint.amyactor-charges-5"></a>`AMyActor() { Charges = 5; }` | No function: a default constructor is read as the class's defaults, and one that takes parameters is refused. A destructor that does something is refused too, as the engine never runs it. See [Constructors](#constructors); do run-time setup in ReceiveBeginPlay. | Yes |
| <a id="methods-as-blueprint.int32-sum-const"></a>`int32 Sum() const { ... }` inside a UE_STRUCT | Refused: "a UE_STRUCT holds no functions". A Blueprint struct has none, so a call to it could not run; it used to compile into one. An inline method is refused where it is called (`called on another object`), and an inline static one works. Write a free inline function that takes the struct: `inline int32 SumOf(const FPair &P)`. | Refused |

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
| <a id="calling-your-own.bump-by"></a>`Bump(By)` or `this->Bump(By)` | A call by name, as the editor's call to a function of the same Blueprint. The VM runs the most derived version on the object, so a subclass's override runs even when the parent's code makes the call. | Yes |
| <a id="calling-your-own.peer-bump-1"></a>`Peer->Bump(1)` | Runs on that object and reaches the most derived version for its class. | Yes |
| <a id="calling-your-own.fact-v-1"></a>`Fact(V - 1)` inside `Fact` | Recursion. Each call gets its own frame, as a recursive Blueprint function does. Mutual recursion works the same way. | Yes |
| <a id="calling-your-own.other-twice-3"></a>`Other->Twice(3)` where `Twice` is inline | Refused; see the inline table below. | Not yet |
| <a id="calling-your-own.bump-by-2"></a>`Bump(By)` in a `final` class, or to a `final` method | Reaches that one function directly, the editor's call to a function no Blueprint can override, instead of by name. The body is copied into the caller, as an inline function's is, and the function is still cooked for every other caller: the editor, delegates, timers, other mods. A function the final class inherits, overrides, or implements for an interface it lists is not cooked Final (an override takes its parent's flags, an implementation keeps the interface function's BlueprintEvent), so a call to one whose body is not copied in (authority-only, an RPC, `noinline`) stays a call by name, as the editor's is; with no subclass, the name finds that one function. | Yes |
| <a id="calling-your-own.twice-v"></a>`Twice(V)`, a static of a class this source cooks | Its body is copied in the same way, unless the class is a function library. | Yes |
| <a id="calling-your-own.mylib-twice-v"></a>`MyLib::Twice(V)`, a static of a `UBlueprintFunctionLibrary` this source cooks | A call, as the editor's library node is: the library's function runs, and native code hooking it by name sees the call. | Yes |
| <a id="calling-your-own.gnu-always-inline-static-int32"></a>`[[gnu::always_inline]] static int32 Twice(int32 V)` in a function library | The function is cooked, and every call this source makes to it is copied in, as a static of any other class is. | Yes |
| <a id="calling-your-own.peer-bump-1-2"></a>`Peer->Bump(1)` in a `final` class | Direct, but not copied in: the body would need Peer as its `this`. | Yes |
| <a id="calling-your-own.gnu-noinline-int32-kept-int32"></a>`[[gnu::noinline]] int32 Kept(int32 V)` | Calls to Kept stay calls, wherever they could be copied in. | Yes |

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
- A function library's statics are never copied in unless they say `[[gnu::always_inline]]`: a library is code its
  callers share, as the editor's is. `static inline` in a library still expands with no function, as anywhere.
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
| <a id="parameters-and-return.fvector-offset-float-up-return"></a>`FVector Offset(float Up) { return ...; }` | The return type becomes the function's ReturnValue output. Any type a variable can have may be returned, and a caller can read a member of a returned struct directly: `GetTransform().Translation.Y`. | Yes |
| <a id="parameters-and-return.auto-half-int32-v-return"></a>`auto Half(int32 V) { return V / 2; }` | A deduced return type becomes the Blueprint function's return type. | Yes |
| <a id="parameters-and-return.int32-slot-int32-i-return"></a>`int32 &Slot(int32 I) { return Slots[I]; }` | Returns the address of what it returns, and each call is the memory there, as `*P` is: `Slot(2) = 7` writes the element, `Slot(2) += 1` calls Slot once, `int32 &R = Slot(2);` keeps the address, `Slot(2).X` reaches a member. The returned thing needs an address: a TArray element, memory through a pointer, a member of either, another object's member (through ReadProperty). A Blueprint caller, such as the editor, sees an int64: the address. | Yes |
| <a id="parameters-and-return.int32-bad-int32-l-3"></a>`int32 &Bad() { int32 L = 3; return L; }` | Refused: "returns a reference to L, a local: a Blueprint variable has no address to return". A by-value parameter and a `T&` parameter are refused the same way. Return a value or a pointer. | Refused |
| <a id="parameters-and-return.const-fstring-name-return-names"></a>`const FString &Name() { return Names[0]; }` | Returns a copy: the function's ReturnValue is an FString. The build warns ("returns const FString&: a Blueprint function returns a copy"): a caller that keeps the reference does not see the original change later. | Warns |
| <a id="parameters-and-return.int32-byval-tarray-int32-a"></a>`int32 ByVal(TArray<int32> A)` | An input pin. The callee gets its own copy, containers and structs included, and its writes stay in the callee. | Yes |
| <a id="parameters-and-return.void-around-int32-a-int32"></a>`void Around(int32 A, int32 &Lo, int32 &Hi)` | A pass-by-reference pin: writes reach the caller's variable, member or array element. This is how a function returns several values. | Yes |
| <a id="parameters-and-return.int32-plus1-const-int32-a"></a>`int32 Plus1(const int32 &A)` | Passed by reference, with no copy. A literal or computed argument is first stored in a hidden local, as the editor's compiler does, because the VM needs an address. The parameter is marked const (`CPF_ConstParm`), as the editor marks a const reference pin; `AActor *const &A` is the same. | Yes |
| <a id="parameters-and-return.int32-times-int32-x-int32"></a>`int32 Times(int32 X, int32 By = 3)` | A call that leaves the argument out passes the default expression, evaluated at that call each time, as in C++. This holds for Blueprint functions and inline ones. | Yes |
| <a id="parameters-and-return.t"></a>A function that calls Delay or UE_AWAIT and returns a value or takes a non-const `T&` | Refused: `a function that resumes later returns nothing and takes no non-const reference parameters`. Store the result in a member; see [Latent calls](#latent-calls). | Refused |

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
- What a reference return means follows the declaration, not the body, so a class shared through `UE_CLASS` returns the
  same thing to every mod that calls it. An inline function that returns `T&` is expanded instead, and its call is the
  place it returns, as before.

### Reference parameters and copy-back

A `T&` parameter needs a place it can refer to. Where Blueprint has none, AssetGen passes a copy and stores it back
after the call, with a warning.

| You write | What it does | Status |
|---|---|---|
| <a id="reference-parameters-and.bump-x-3"></a>`Bump(X, 3)`, `Bump(Arr[NextIdx()], By)`, `Bump(O->Calls, By)` | The parameter names that place, fixed at the call: the object and the index are evaluated once. An inline body sees its own writes; a Blueprint function gets the place by reference. Either way the writes reach the caller. | Yes |
| <a id="reference-parameters-and.add5-counts-k"></a>`Add5(Counts[K])` with a TMap `Counts` | Blueprint has no reference to a map element (Find copies it out), so the call gets a hidden copy that is stored back into `Counts[K]` after the call. The warning says `bound to a map element`. | Warns |
| <a id="reference-parameters-and.add5-c-x-y"></a>`Add5(C ? X : Y)` | The call gets a hidden copy, stored back after the call into whichever variable the condition picked. The condition is evaluated once, before the call. | Warns |
| <a id="reference-parameters-and.add5-c-m-k-x"></a>`Add5(C ? M[K()] : X)` | Refused (`where X or Y is found by a call`): both sides would have to be located before the call, but C++ runs only the picked side's call. Pick into a local, pass the local, then store it back. | Not yet |
| <a id="reference-parameters-and.bump-p"></a>`Bump(*P)` with a raw pointer `P` | Passes the memory itself, so the callee's write lands there; see [Pointers and memory](#pointers-and-memory). | Yes |

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
| <a id="static-functions-and.static-int32-twice-int32-v"></a>`static int32 Twice(int32 V)` | A static Blueprint function (Static, Final, Public, BlueprintCallable). `Twice(X)` and `AMyActor::Twice(1)` both run it on the class default object, so inside it `this` is that default object, which has no world. | Yes |
| <a id="static-functions-and.static-apawn-firstpawn-uobject"></a>`static APawn *FirstPawn(UObject *WorldContextObject = nullptr)` | The first parameter whose name starts with `WorldContext` is what every engine call inside the static receives as its world context, as the editor wires the hidden pin. A caller that leaves it out passes its own `this`, or its own world context when the caller is itself such a static. | Yes |
| <a id="static-functions-and.class-mylib-public"></a>`class MyLib : public UBlueprintFunctionLibrary` holding statics | The editor's Blueprint Function Library. Every call goes to the library's function, from this source too, unless the static is `[[gnu::always_inline]]` or `inline`; see [Calling your own functions](#calling-your-own-functions). Another mod that includes its header calls the statics like an engine library's: a final call on the library's default object, with a world context left out filled with the caller's `this`. | Yes |
| <a id="static-functions-and.lib-f-log-c-addlogentry"></a>`LIB_F_Log_C::AddLogEntry(Main, Severity, Prefix, Entry, this)`, a game Blueprint library's static taking one of the game's Blueprint enums | UeApi declares the enum parameter `uint8`, the byte the call passes. The game's Blueprint enums have no C++ type yet, so a method, field or delegate of one is left out of UeApi. | Yes |
| <a id="static-functions-and.a-static-that-calls-delay"></a>A static that calls Delay or UE_AWAIT | Refused: `a static function has no object whose ubergraph frame could keep its locals`; see [Latent calls](#latent-calls). | Refused |
| <a id="static-functions-and.static-int32-tell-int32-v"></a>`static int32 Tell(int32 V)` in a class whose mod parent has a static `Tell` too | Hides the parent's, as in C++, with a warning: "... hides Parent::Tell, a static: compiled as C++ name hiding ...". Every call to a static is bound to the one it names, so `Tell(V)` runs this one and `Parent::Tell(V)` the parent's. It is cooked with the parent's static as its super, as the editor links any function named like a parent's; the editor itself would refuse the name ("cannot be overridden"). | Warns |
| <a id="static-functions-and.static-float-tell-float-v"></a>`static float Tell(float V)` over a mod parent's `static int32 Tell(int32)` | Refused: "... is static and hides Parent::Tell, a static of another signature, ...". Its super would be the parent's static, a function of other parameters, which no editor links. Rename this one. | Refused |
| <a id="static-functions-and.a-method-that-is-not"></a>A method that is not static named like a mod parent's static, or a static named like a parent's method | Refused: "... is static, and the editor takes a function of that name in a subclass for an override of it ..." or "... is static, and the Parent::Tell it hides is not ...". The editor makes a function named like a parent's an override of it, and an override must agree with it on Static. The parent may be another mod's class, from the header it shares. Rename one of them. | Refused |
| <a id="static-functions-and.0"></a>A static or a method named like one a mod parent of this source declares and never defines (no body, not `= 0`) | The parent compiles no function of that name, so this one hides nothing a Blueprint has: no warning, no refusal, no super, and its parameters may differ from the declaration's (`float Scale(float)` over a declared-only `int32 Scale(int32)`): a call to the declared-only one is refused, as C++ would not link it, so none lays out the parent's parameters. A function above the parent of that name, a native one too, is found as if the parent did not declare it. | Yes |

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

### Free functions

A Blueprint has no free functions. One that is not `inline` is a static of `BPL_<source>`, a function library the
compiler generates for each source file and cooks in the mod's package: `BPL_MyMod` for `MyMod.cpp`. A call is a call
to that static on the library's default object, as to a static of a library you write yourself.

| You write | What it does | Status |
|---|---|---|
| <a id="free-functions.int32-twice-int32-v"></a>`int32 Twice(int32 V) { ... }` at namespace scope | A static of the source's library. Only the free functions a call reaches are compiled, and those they call in turn. | Yes |
| <a id="free-functions.namespace-util-int32-twice-int32"></a>`namespace Util { int32 Twice(int32 V) { ... } }` | `Util__Twice` in the library, beside a `Twice` of another namespace. | Yes |
| <a id="free-functions.int32-fact-int32-n-return"></a>`int32 Fact(int32 N) { return N <= 1 ? 1 : N * Fact(N - 1); }` | A library function may call itself and other free functions. | Yes |
| <a id="free-functions.int32-later-int32-v"></a>`int32 Later(int32 V);` called before its definition further down | The call finds the definition, and the default arguments of the first declaration. | Yes |
| <a id="free-functions.apawn-firstpawn-return"></a>`APawn *FirstPawn() { return UGameplayStatics::GetPlayerPawn(0); }` | Each library function has a hidden world context, as a function of the editor's function library does. A call fills it in with its `this`, or with its own world context in a static, so an engine call in the body finds the caller's world. | Yes |
| <a id="free-functions.apawn-firstpawn-uobject"></a>`APawn *FirstPawn(UObject *WorldContextObject = nullptr)` | A world context parameter you write replaces the hidden one; see [World context in inline helpers](#world-context-in-inline-helpers). | Yes |
| <a id="free-functions.int32-ov-int32-v"></a>`int32 Ov(int32 V)` beside `int32 Ov(float V)`, neither `inline`, both called | Refused where the second is called (`is overloaded`): a library has one function per name. Rename one, or make it `inline`. An inline overload beside a non-inline one is fine. | Refused |
| <a id="free-functions.a-free-function-that-calls"></a>A free function that calls Delay or UE_AWAIT | Refused (`a free function is one unless it is inline`): a static has no object to keep a waiting function's locals. Make the helper `inline`, so that it expands into the method that waits; see [Latent calls](#latent-calls). | Refused |
| <a id="free-functions.int32-helper-int32-v"></a>`int32 Helper(int32 V);` declared here and defined in another source | Refused at the call (`call to an unknown function: Helper`). A free function's body must be in the source that calls it, or in a header that source includes. To share one between mods, write a function library class. | Refused |

```cpp
int32 Twice(int32 V) { return V * 2; }
namespace Util {
int32 Clamp(int32 V, int32 Lo = 0, int32 Hi = 10) { return V < Lo ? Lo : V > Hi ? Hi : V; }
}
APawn *FirstPawn() { return UGameplayStatics::GetPlayerPawn(0); }   // gets the caller's world context

class AMyActor : public AActor {
public:
  int32 Use(int32 V) { return Twice(V) + Util::Clamp(V); }   // BPL_MyMod: Twice, Util__Clamp
  APawn *Mine() { return FirstPawn(); }                       // passes this
};
```

Notes:

- The library is per source, not per mod: each source of a mod is compiled on its own, so a mod with two sources has
  two libraries, each holding what its own source calls. A function both call is in both.
- In the editor API stub the library is a Blueprint Function Library, its functions callable from Blueprints made
  in the editor.
- An error in a library function's body is prefixed `BPL_<source>::<Name>: `, the name as the library spells it
  (`Util__Twice`).
- Do not name a class of your own `BPL_<source>`.

### Overloading

A Blueprint class has one function per name. Overloads work when they are all `inline`, or when the one non-inline
overload is the one with the most parameters.

| You write | What it does | Status |
|---|---|---|
| <a id="overloading.inline-int32-pick-int32-v"></a>`inline int32 Pick(int32 V)` beside `inline int32 Pick(bool B)` | Each call expands the overload C++ picked, and one overload may call another. None of them is a Blueprint function. | Yes |
| <a id="overloading.int32-get-int32-a-int32"></a>`int32 Get(int32 A, int32 B)` beside `inline int32 Get(int32 A)` | The overload with the most parameters is the Blueprint function named `Get`; the inline ones beside it expand normally. | Yes |
| <a id="overloading.inline-int32-get-int32-a"></a>`inline int32 Get(int32 A, int32 B)` beside a non-inline `int32 Get(int32 A)`, then `Get(V)` | Refused: `an overload set may not mix inline and non-inline functions`. Make the non-inline overload the one with the most parameters, or rename it. | Not yet |
| <a id="overloading.int32-ov-int32-a"></a>`int32 Ov(int32 A)` beside `int32 Ov(int32 A, int32 B)`, neither inline | Refused: "a second function of that name". A Blueprint class has one function per name. Give them different names, or make the extra overloads `inline`. | Refused |
| <a id="overloading.int32-get"></a>`int32 Get()` beside `int32 get()` | Refused: "differs from Get only in case". An FName ignores case, so both would be one function. The same holds for two variables, a variable and a function, and a name an ancestor already has: `int32 get()` in a class whose parent has `Get()` is refused, while `Get()` itself overrides it. | Refused |
| <a id="overloading.int32-none"></a>`int32 None;`, `int32 none();`, a `UE_STRUCT` member `int32 NONE;` | Refused: "None is UE's empty name". The name None is UE's empty name in any case, which the Blueprint editor refuses for a variable or a function. A saved value's members end at one of that name, so a `UE_STRUCT` member called None would cut the struct's value short in a class default. The same for a component and an interface's member. | Refused |

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
| <a id="lambdas-function.auto-l-x-int32-y"></a>`auto L = [X](int32 Y) { return X + Y; };` | Refused, as are an immediately invoked lambda and a lambda passed to an inline template, each with its own `unimplemented` message. Write an inline method or a free function. | Refused |
| <a id="lambdas-function.auto-p-inc1-return-p"></a>`auto *P = &Inc1; return P(X);` | Refused (`unimplemented local`): a Blueprint has no function pointers. To call a function later, bind it to a dispatcher or pass it as a delegate (`{this, &AMine::Handle}`); see [Event dispatchers](#event-dispatchers). | Refused |
| <a id="lambdas-function.inline-fpairx-operator-fpairx-l"></a>`inline FPairX operator+(FPairX L, FPairX R)` | Refused where it is used (`unimplemented operator overload`). Only the engine's operators that UeApi lists, and string `+`, are lowered. Write a named inline function. | Refused |

## Calling engine and game functions

UeApi declares the engine's and the game's reflected functions as C++ methods and statics, and you call them as in
C++. AssetGen writes the call the editor's node would make. The rule to remember: leave out the world context and a
latent call's `FLatentActionInfo`, because the compiler fills them in.

### Engine and game calls

| You write | What it does | Status |
|---|---|---|
| <a id="engine-and-game.k2-setactorlocation-home-false"></a>`K2_SetActorLocation(Home, false, Hit, false);` | Calls the engine's function on this actor, as the editor's call node does. | Yes |
| <a id="engine-and-game.char-server-setrunning-false"></a>`Char->Server_SetRunning(false);` | The same on another object. The engine applies its own authority, cosmetic and RPC routing to the call. | Yes |
| <a id="engine-and-game.fhitresult-hit"></a>`FHitResult Hit;` passed to a `T&` parameter | A `T&` parameter of an engine function is an output and needs a variable, as in C++. | Yes |
| <a id="engine-and-game.ukismetmathlibrary-randominteger"></a>`UKismetMathLibrary::RandomInteger(10)` | A native static is a direct library call. | Yes |
| <a id="engine-and-game.ugameplaystatics-applydamage"></a>`UGameplayStatics::ApplyDamage(...)`, `PlaySound2D(...)` | An authority-only or cosmetic static is called on the library's default object, so the engine checks where it may run, as for the editor's node. | Yes |
| <a id="engine-and-game.ugameplaystatics-getplayerpawn-0"></a>`UGameplayStatics::GetPlayerPawn(0)` in a class that is no actor, component, widget, GameInstance or subsystem | Warns: "passes self as GetPlayerPawn's world context, and an object of this class finds its world only through its Outer". The editor wires self to the pin only in a class with a world. Make the object with an actor or component as its Outer, or pass a world context. | Warns |
| <a id="engine-and-game.a-static-of-a-game"></a>A static of a game Blueprint library, or of another mod's library | A final call on that class's default object. | Yes |
| <a id="engine-and-game.json-getpath"></a>`Json->GetPath(...)` on a game Blueprint, `T->Fire(2)` on another mod's class | A call by name, as the editor calls a Blueprint function that is not final: on an object of a subclass that overrides it, the override runs. A function the header marks `final`, a static, and a parent call (`Turret::Fire(N)` in an override) are bound to that class's function. | Yes |
| <a id="engine-and-game.const-t"></a>A call or an expression passed to an engine function's `const T&` parameter | Stored in a hidden local first, because the VM needs an address. | Yes |
| <a id="engine-and-game.target-k2-destroyactor"></a>`Target->K2_DestroyActor();` with `Target` null | The call is skipped (the engine logs "Accessed None"), and a returned value reads as zero. C++ would crash here; a Blueprint does not. The same holds for a mod function called on a null object. | Yes |
| <a id="engine-and-game.makecarverequests-owner"></a>`MakeCarveRequests(Owner->GetTransform());`, `FVector S = Key->GetActorScale3D(); Scale / S` with the object null | A struct a call on a null object returns reads as zero where it is passed on, as it does through the editor's local: the call names its own ReturnValue for the engine to clear. Without that, a native's parameter would keep its default-constructed value (an FVector's garbage, an FTransform's identity) and a mod function's its struct's defaults. | Yes |
| <a id="engine-and-game.seen-getpriority"></a>`Seen->GetPriority()` on a `TScriptInterface<ITagged>` | A call by name to the object behind the interface; see [Interfaces](#interfaces). | Yes |
| <a id="engine-and-game.other-getclass"></a>`Other->GetClass()`, `GetName()`, `GetOuter()`, `GetTypedOuter<T>(Obj)` | UObject's C++ helpers and their Objects.h relatives, which are not reflected functions. The compiler maps them; see [Working with other objects](#working-with-other-objects). | Yes |

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
| <a id="the-world-context.ugameplaystatics-getplayerpawn-0"></a>`UGameplayStatics::GetPlayerPawn(0)` | UeApi declares every reflected function with a `UObject*` parameter named `WorldContext...` twice, with and without it. Leaving it out passes `this`, which is what the editor wires to the hidden pin. | Yes |
| <a id="the-world-context.worldcontextobject"></a>The same call inside a static that has a `WorldContextObject` parameter | Passes the static's own parameter. | Yes |
| <a id="the-world-context.ugameplaystatics-getplayerpawn"></a>`UGameplayStatics::GetPlayerPawn(nullptr, 0)` | An explicit argument, `nullptr` included, is kept. | Yes |
| <a id="the-world-context.ukismetsystemlibrary-delay-0-5f"></a>`UKismetSystemLibrary::Delay(0.5f)` | A latent function's `FLatentActionInfo` is left out as well, and the compiler fills it in. Never pass it yourself: with the world context written too it is refused (`leave the FLatentActionInfo argument out`); see [Latent calls](#latent-calls). | Yes |
| <a id="the-world-context.ukismetsystemlibrary-delay-0-5f-2"></a>`UKismetSystemLibrary::Delay(0.5f, Info)`, the latent info passed without the world context | Refused: `leave the FLatentActionInfo argument out`, as with the world context written. See [Latent calls](#latent-calls). | Refused |

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
| <a id="results-you-ignore.bump-1"></a>`Bump(1);` | A call made for its effects runs even when its value is unused. | Yes |
| <a id="results-you-ignore.doubled"></a>`Doubled();` where `Doubled` is UE_PURE, or an engine BlueprintPure function without outputs | Removed when its arguments do nothing either. AssetGen prints nothing about it; clang warns `ignoring return value of function declared with pure attribute`. | Yes |

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
| <a id="math-and-utility.ukismetmathlibrary-clamp-v-0-10"></a>`UKismetMathLibrary::Clamp(V, 0, 10)` | Math is the engine's Blueprint math library, the functions behind the editor's math nodes: Clamp, FClamp, Min, FMin, FMax, Abs, Sqrt, Sin, Cos, Lerp, FInterpTo, RandomInteger, RandomFloatInRange and the rest of `UKismetMathLibrary` in `UeApi/Engine.h`. Each call is a direct library call. | Yes |
| <a id="math-and-utility.a-b"></a>`A + B`, `A < B`, `Dir * 2.0f` | Operators on numbers and vectors need no written call: they become the math library's operator functions; see [Operators](#operators). | Yes |
| <a id="math-and-utility.s-x"></a>`S == "x"` on an FString | Ignores case, as FString's `==` does in UE C++; see [Strings and text](#strings-and-text). | Yes |
| <a id="math-and-utility.fmath-h"></a>`FMath::Clamp(V, 0, 10)`, `FMath::Max(A, B)`, `FMath::Abs(X)` with `#include "../include/FMath.h"` | UE C++'s spellings from `AssetGen/include/FMath.h`. Each is an inline function whose body is the math node for its argument type: `FMath::Max` is Max for int32, MaxInt64 for int64 and FMax for float, as UE's template picks the arithmetic. Two arguments of different types are ambiguous, as they are a deduction error in UE. The header has Max, Min, Clamp, Abs, Sign, Square, Sqrt, Pow, Exp, Loge, Fmod (Percent_FloatFloat), Frac, FloorToInt, CeilToInt, RoundToInt, TruncToInt, the trigonometry, DegreesToRadians, RadiansToDegrees, Lerp, IsNearlyEqual, RandRange, FRandRange, FRand and RandHelper. | Yes |
| <a id="math-and-utility.fmath-clamp-v-0-10"></a>`FMath::Clamp(V, 0, 10)` without FMath.h | clang error `use of undeclared identifier 'FMath'`: the engine's FMath is not part of the reflected API, so UeApi does not declare it. | Refused |
| <a id="math-and-utility.std-min-a-b"></a>`std::min(A, B)`, `std::sqrt(X)`, `__builtin_sqrtf(X)` | Refused, even with `#include <algorithm>` or `<cmath>` (`call to an unknown function`): a mod has no C++ standard library to call into. Use UKismetMathLibrary, or write the helper as an inline function. | Refused |
| <a id="math-and-utility.if-constexpr-is-same-t"></a>`if constexpr (__is_same(T, int32))`, `static_assert(__is_same(int32, int))` | clang's type-trait builtins work in place of `<type_traits>` where clang decides them at build time. | Yes |
| <a id="math-and-utility.bool-b-is-same-a"></a>`bool B = __is_same(A, B);`, `if (__is_same(A, B))`, `return __is_same(A, B) ? 1 : 0;` | The constant true or false, compared through aliases and typedefs as clang compares types: `__is_same(int32, int)` is true. | Yes |
| <a id="math-and-utility.bool-b-is-base-of"></a>`bool B = __is_base_of(AActor, T);` and the other traits | Refused as a value ("unimplemented argument TypeTraitExpr"). Put the test in `if constexpr` or a `requires` clause. | Refused |

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
| <a id="messages-on-screen.ugamefunctionlibrary"></a>`UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(Msg)` | Posts `Msg` as a game message, through the game state. This is what to use instead of the Print String node. | Yes |
| <a id="messages-on-screen.ugamefunctionlibrary-2"></a>`UGameFunctionLibrary::GetFSDGameState()->PostGameMessage(Msg)` in a static with a `WorldContextObject` parameter | The game state of that parameter's world. | Yes |

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
| <a id="overriding-engine-and.void-receivebeginplay"></a>`void ReceiveBeginPlay() { ... }` | Overrides the parent's event, like adding the event node in the editor. The engine calls it. | Yes |
| <a id="overriding-engine-and.void-receivetick-float"></a>`void ReceiveTick(float DeltaSeconds)` | Also turns ticking on: it sets `PrimaryActorTick.bCanEverTick` on an actor's class default object, `PrimaryComponentTick.bCanEverTick` on a component's, as the editor's compiler does. AActor and UActorComponent leave ticking off by default. | Yes |
| <a id="overriding-engine-and.void-tick-fgeometry-mygeometry"></a>`void Tick(FGeometry MyGeometry, float InDeltaTime)`, `void OnPaint(FPaintContext &Context) const` in a widget class | Also turns that event on, for this class alone: a subclass runs it only through an override of its own. See [Parent classes](#parent-classes). | Yes |
| <a id="overriding-engine-and.void-onjumped-acharacter"></a>`void OnJumped() { ACharacter::OnJumped(); ... }` | Overrides a BlueprintNativeEvent (an event with a C++ default). The Blueprint version replaces the default, and the parent call runs it. | Yes |
| <a id="overriding-engine-and.bool-canjumpinternal-const"></a>`bool CanJumpInternal() const` | An override copies its parent's access, event, net, authority, cosmetic, const and pure flags, whatever section it is written in. It drops Native, because the Blueprint version is script. | Yes |
| <a id="overriding-engine-and.namespace-game"></a>A class in `namespace Game::...` deriving a game Blueprint, redefining one of its functions | An override the game's code reaches by name, with the parent function's flags; see [examples/GameBlueprintChild.cpp](examples/GameBlueprintChild.cpp). | Yes |
| <a id="overriding-engine-and.class-aturret-public-aactor"></a>`class ATurret : public AActor, public ITargetable` with its functions defined | Implements an interface; see [Interfaces](#interfaces). | Yes |
| <a id="overriding-engine-and.void-receivetick-float-2"></a>`void ReceiveTick(float DeltaSeconds) override` | clang error `only virtual member functions can be marked 'override'`: UeApi declares engine functions non-virtual. Leave it off; the name alone makes the override. | Refused |
| <a id="overriding-engine-and.ue-server"></a>`UE_SERVER`, `UE_CLIENT` or `UE_MULTICAST` on an override | Refused: `an override takes its parent's replication; drop the RPC marker`; see [RPCs](#rpcs). | Refused |

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
  Types are compared, not spellings: `ETemperature&` written inside the enum's namespace matches
  `::Ns::ETemperature&` written outside it, and an alias matches what it names. Two types that share a short name
  stay two.
- An override keeps its parent's access: a ReceiveBeginPlay override is protected even when written under `public:`.
- AssetGen reads a parent's flags from the SDK's `Events.json`, which lists engine and game classes. A mod method named
  like an engine function that is not an event there (K2_DestroyActor, say) is refused: "AActor::K2_DestroyActor is
  native and no Blueprint event, so no function replaces it". C++ and calls bound to it keep running the engine's.
  An override of a function of another mod's class (from a `UE_CLASS` header) gets plain flags, not the parent's.
- An event override is not BlueprintCallable, so the editor API stub leaves it out.

### Overriding your own parent's methods

| You write | What it does | Status |
|---|---|---|
| <a id="overriding-your-own.int32-bump-int32-by"></a>`int32 Bump(int32 By)` in a subclass of a mod class that declares `Bump` | The nearest ancestor that declares the method is its super, and the override inherits that function's access, net, pure and const flags. Calls by name anywhere, the parent's code included, reach the most derived version. | Yes |
| <a id="overriding-your-own.twice-by"></a>`Twice(By)` where `Twice` is inherited, not redeclared | An ordinary call by name. A subclass's override is what runs, including one compiled later in another mod. | Yes |

Notes:

- An `inline` method of the parent cannot be overridden, because it is not a function. A call on `this` inside its
  expanded body still goes by name, so it reaches the subclass's version (see `ViaInline` below).

### Calling the parent

| You write | What it does | Status |
|---|---|---|
| <a id="calling-the-parent.superbase-receivebeginplay"></a>`SuperBase::ReceiveBeginPlay();` inside an override | Runs the parent's own implementation once and comes back: the editor's "Add call to parent function". Write the class you derive from; C++ has no `Super`. | Yes |
| <a id="calling-the-parent.aactor-receivebeginplay"></a>`AActor::ReceiveBeginPlay();` | Calls the engine's function. For a BlueprintImplementableEvent it does nothing, because there is no parent body. For a BlueprintNativeEvent it runs the C++ default. | Yes |
| <a id="calling-the-parent.superbase-twice-1"></a>`SuperBase::Twice(1)` in a class that does not declare Twice | SuperBase's Twice, its body copied in: C++ runs that function without dispatch, so an object of a subclass that overrides Twice does not reach its own. `this->SuperBase::Twice(1)` is the same call. | Yes |
| <a id="calling-the-parent.noinline"></a>The same call to a function whose body cannot be copied in: authority-only, cosmetic, a server or client RPC, `noinline`, one that waits, one whose body makes such a call itself, or any from a `UE_NO_OPTIMIZE` caller | SuperBase's function, without dispatch. Blueprint calls a parent's function that way only from a class that has its own function of that name, so AssetGen adds one to your class: an override of the method that only calls the nearest parent's version with the same arguments, compiled like one you write, with an override's flags and super. The call is then bound to SuperBase's function, and a subclass's override of the method overrides the added one. A call by name runs what it ran before: the added override passes it on, and the engine routes both calls the same way. | Yes |
| <a id="calling-the-parent.the-same-call-to-a"></a>The same call to a multicast RPC | A call by name, with a warning: an object of a subclass that overrides the function runs its override. On a server a multicast runs locally and is also sent, so an added override would send it once, then again when it calls the parent's. An override you declare yourself does that too, as an editor override that calls its parent does. | Warns |
| <a id="calling-the-parent.supertest-thrice-1"></a>`SuperTest::Thrice(1)` in SuperTest's own code | SuperTest's own Thrice, its body copied in: C++ runs that one without dispatch, also on an object of a subclass that overrides Thrice. | Yes |
| <a id="calling-the-parent.the-same-call-to-one"></a>The same call to one of the class's own functions whose body cannot be copied in | A call by name, with a warning: on an object of a subclass that overrides the function, the override runs. A Blueprint calls its own class's function without dispatch only when that function is `final`, and then the call is bound to it. | Warns |
| <a id="calling-the-parent.other-superbase-bump-1"></a>`Other->SuperBase::Bump(1)` | Refused: "runs SuperBase's Bump on another object without dispatch". C++ runs SuperBase's own Bump there; a Blueprint calls a function that way only as self's parent call, and by name the call would reach Other's override. The same however the object is written: a variable, `this->Other`, a call, `GetOther()->SuperBase::Bump(1)`, or a cast, `Cast<SuperBase>(A)->SuperBase::Bump(1)`. Call it by name, or put the code in a function of another name. Only a qualifier that a macro takes as an argument, `#define ON(O, Q) O->Q::Bump(1)`, is not read: that call goes by name with no message. | Refused |

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
always expanded this way, `inline` or not. The rule to remember: an inline method expands only on `this`. For a helper
that takes an object, write a free function.

### Inline functions

| You write | What it does | Status |
|---|---|---|
| <a id="inline-functions.inline-int32-clamp-int32-v"></a>`inline int32 Clamp(int32 V, int32 Lo, int32 Hi) { ... }` | Never a Blueprint function. Each call copies the body into the caller, like an editor macro, with its own locals, and `return` jumps to the end of the copy. Loops, switch, goto, nested inline calls and early returns all work. | Yes |
| <a id="inline-functions.inline"></a>`inline` on the declaration, or only on the out-of-line definition | Either one makes the method or free function inline. | Yes |
| <a id="inline-functions.late-counter"></a>`Late(Counter)` | Each argument is evaluated once, before the body runs. A by-value parameter keeps the argument's value at the call, even if the body changes what it was read from. A constant or read-only argument is used in place, without a copy. | Yes |
| <a id="inline-functions.inline-bool-attachto"></a>`inline bool AttachTo(USceneComponent *Child, USceneComponent *Parent)` at namespace scope | A free inline function, expanded at every call. A free function can act on any object; an inline method expands only on `this`. | Yes |
| <a id="inline-functions.int32-helper-int32-x"></a>`int32 Helper(int32 X) { ... }` at namespace scope, without `inline` | Not expanded: a Blueprint function of the library generated for the source, called on its default object; see [Free functions](#free-functions). | Yes |
| <a id="inline-functions.static-inline-int32-addone-int32"></a>`static inline int32 AddOne(int32 V)` in a library class | Expanded at the call, from its own class or from any class that includes the header, in any mod. The library does not export it. | Yes |
| <a id="inline-functions.bump-by"></a>`Bump(By)` inside an inline body or a member template that a subclass expands | A call on `this` goes by name, so the object's most derived method runs, including an override compiled later or one in a class between. | Yes |
| <a id="inline-functions.other-twice-3"></a>`Other->Twice(3)` where `Twice` is an inline method | Refused: `called on another object (only this)`, because `this` in the body stays the caller's self. Make the method non-inline, or write a free function that takes the object. | Not yet |
| <a id="inline-functions.an-inline-function-that-calls"></a>An inline function that calls itself, directly or through another inline | Refused (`calls itself`): the expansion would never end. Make it non-inline; a Blueprint function may recurse. One overload calling another is fine. | Refused |
| <a id="inline-functions.inline-int32-nope-int32-x"></a>`inline int32 Nope(int32 X);` with no body | Refused at the call (`has no body`). clang also warns with `-Wundefined-inline`. | Refused |
| <a id="inline-functions.onhit-add-this-amine-handle"></a>`OnHit.Add(this, &AMine::Handle)` where `Handle` is inline | Refused: a delegate binds a function by name, and an inline function is none. Drop `inline`; see [Event dispatchers](#event-dispatchers). | Refused |
| <a id="inline-functions.static-int32-count-0"></a>`static int32 Count = 0;` inside an inline function | Refused: each expansion would keep its own. Use a member variable; see [Latent calls](#latent-calls). | Refused |

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

### World context in inline helpers

| You write | What it does | Status |
|---|---|---|
| <a id="world-context-in.firstpawn"></a>`FirstPawn()` where `FirstPawn(UObject *WorldContextObject = nullptr)` is inline | A parameter whose name starts with `WorldContext`, left at its default, becomes the caller's `this`. Inside a static with its own WorldContext parameter it becomes that parameter. A Blueprint function or an engine function gets the same value in the same place. | Yes |
| <a id="world-context-in.firstpawn-other"></a>`FirstPawn(Other)`, `FirstPawn(nullptr)` | An explicit argument, `nullptr` included, is kept. | Yes |
| <a id="world-context-in.an-engine-call-inside-the"></a>An engine call inside the inline body that leaves out its own world context | Gets the caller's context, not the inline's WorldContextObject parameter. Pass the parameter on explicitly, as below. | Yes |

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

- This holds for inline methods, free functions, inline or not, and static inline members of a library.
- Only the name is checked (`WorldContextObject`, `WorldContext`, `WorldContextObject_0`...), not the type. Any written
  default is replaced, not only `nullptr`.
- A static member can call a free function or a static inline member, but not an inline method: that needs an
  object, and clang enforces it.
- Older AssetGen builds passed the written default (null) instead.

### Templates

| You write | What it does | Status |
|---|---|---|
| <a id="templates.spawnactor-aactor-aactor"></a>`SpawnActor<AActor>(AActor::StaticClass(), Where)` from `Objects.h` | Each instantiation clang makes is expanded at its call like any inline function. No function exists per instantiation; see [Creating objects](#creating-objects). | Yes |
| <a id="templates.template-class-t-inline-t"></a>`template <class T> inline T Max2(T A, T B)` | A free function template is expanded at each call, like a member template. | Yes |
| <a id="templates.template-class-t-t-max2"></a>`template <class T> T Max2(T A, T B)` without `inline` | Expanded at each call all the same: a template is never a function of the source's library. | Yes |
| <a id="templates.template-class-t-int32-widthof"></a>`template <class T> int32 WidthOf()` in a class | A member template is expanded at its call, with or without `inline`. No Blueprint function is made, and only calls on `this` expand. Its body is its own class's code, as an inline method's is: a call in it is read there, wherever it is copied. | Yes |
| <a id="templates.inline-auto-twicen-number-auto"></a>`inline auto TwiceN(Number auto V)`, or `int32 AutoP(auto V)` in a class | An `auto` parameter makes the function a template, expanded at each call. | Yes |
| <a id="templates.template-class-t-concept-number"></a>`template <class T> concept Number = requires(T A) { A + A; };` | Concepts and requires-clauses are checked by clang and cost nothing at run time. A wrong type is a "constraints not satisfied" error on the calling line. | Yes |
| <a id="templates.template-class-t-inline-int32"></a>`template <class... T> inline int32 Fwd(T... V) { return Sum2(V...); }` | Pack expansion into a call works. | Yes |
| <a id="templates.sizeof-v"></a>`sizeof...(V)`, `sizeof...(T)` | The number of arguments in the pack, of a parameter pack or of a template's type pack, as a constant; 0 for an empty pack. | Yes |
| <a id="templates.template-int32-n-inline-int32"></a>`template <int32 N> inline int32 Times(int32 X) { return X * N; }` | A template's value parameter is the constant the call gave it. | Yes |
| <a id="templates.if-constexpr-sizeof-t-8"></a>`if constexpr (sizeof(T) == 8)` | Only the branch clang kept is compiled; no test or jump is left in the bytecode. | Yes |

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
  `static_assert`). As a value only `__is_same` is read; any other is refused ("unimplemented argument TypeTraitExpr").

### constexpr and consteval functions

| You write | What it does | Status |
|---|---|---|
| <a id="constexpr-and-consteval.constexpr-int32-sqm-int32-x"></a>`constexpr int32 SqM(int32 X) const` in a class | An ordinary Blueprint function that runs in the game; `constexpr` changes nothing. Mark it `inline` to expand it at calls, or `consteval` to run it at build time. | Yes |
| <a id="constexpr-and-consteval.inline-constexpr-int32-sq-int32"></a>`inline constexpr int32 Sq(int32 X)` at namespace scope | Expanded like any inline function and computed in the game. | Yes |
| <a id="constexpr-and-consteval.constexpr-int32-sq-int32-x"></a>`constexpr int32 Sq(int32 X)` at namespace scope, without `inline` | A Blueprint function of the source's library, computed in the game like any [free function](#free-functions): AssetGen does not run a plain constexpr call at build time. Use `consteval` for that. | Yes |
| <a id="constexpr-and-consteval.consteval-int32-fnv-const-char"></a>`consteval int32 Fnv(const char *S)` | clang runs the call while compiling, and only the answer reaches the Blueprint; see [Constants](#constants). | Yes |
| <a id="constexpr-and-consteval.int32-k-sq-3"></a>`int32 K = Sq(3);` as a member initializer, with a plain constexpr `Sq` | Refused (`a default is a value known when the mod is built`), and so is a consteval `Sq` called there directly. A default reaches a function call only through a constant: `static constexpr int32 kSq = Sq(3);` with `Sq` a consteval function at namespace scope, then `int32 K = kSq;`. See [Constants](#constants). | Refused |

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
| <a id="declaring-components.ue-component"></a>`UE_COMPONENT(UStaticMeshComponent, Mesh);` | Adds a component, as Add Component in the Components panel does. `Mesh` is an ordinary object variable. A construction-script node builds the component from a template (`Mesh_GEN_VARIABLE`) each time the actor spawns and stores it in `Mesh`. | Yes |
| <a id="declaring-components.ustaticmeshcomponent-mesh"></a>`UStaticMeshComponent *Mesh;` and `Mesh = CreateDefaultSubobject<UStaticMeshComponent>("Mesh");` in the constructor, or `UStaticMeshComponent *Mesh = CreateDefaultSubobject<UStaticMeshComponent>("Mesh");` | The UE C++ form of the row above: Mesh is the component, its template `Mesh_GEN_VARIABLE`. The line may also stand in `UE_DEFAULTS`. Its other lines (`Mesh->SetupAttachment(Pivot);`, `Mesh->bVisible = false;`) are defaults, as there. | Yes |
| <a id="declaring-components.mesh-createdefaultsubobject"></a>`Mesh = CreateDefaultSubobject<UStaticMeshComponent>("Body");` | Warns: "the component is named Mesh, after its member". The construction script names a component by its variable. | Warns |
| <a id="declaring-components.mesh-sethiddeningame-true-false"></a>`Mesh->SetHiddenInGame(true, false);` in a function | By the time the construction script and `ReceiveBeginPlay` run, the variable holds this actor's own instance. Use it like any object pointer; a change affects only this actor. | Yes |
| <a id="declaring-components.ue-component-uhealthcomponent"></a>`UE_COMPONENT(UHealthComponent, Health);` | One of DRG's own component classes works like an engine one. | Yes |
| <a id="declaring-components.ue-component-2"></a>`UE_COMPONENT(UInstancedStaticMeshComponent, Pile);` | Static mesh, instanced static mesh, hierarchical instanced static mesh, SkyAtmosphere and AtmosphericFog components write native data after their properties. AssetGen writes exactly the bytes a cooked template of that class carries, and a class derived from one of them gets the same bytes. Other component classes need nothing extra. | Yes |
| <a id="declaring-components.ue-component-umodelcomponent-bsp"></a>`UE_COMPONENT(UModelComponent, Bsp);` | Refused: a model component "belongs to a level's BSP" and cannot be a template. | Refused |
| <a id="declaring-components.ue-component-ucharges-ammo"></a>`UE_COMPONENT(UCharges, Ammo);`, with a component class the mod declares | A component of the mod's own class, as the editor adds a component Blueprint in the Components panel: a construction-script node whose template is made from that class's default object. `UE_DEFAULTS` sets its defaults as for an engine component (`Ammo->Count = 3;`). An abstract class (a method `= 0`) is refused as "abstract": the engine never instances an abstract class. | Yes |
| <a id="declaring-components.ue-component-utexture2d-icon"></a>`UE_COMPONENT(UTexture2D, Icon);` | Refused: "is not a UActorComponent". | Refused |
| <a id="declaring-components.ue-component-3"></a>`UE_COMPONENT` in a class that is not an actor | Refused: "only an actor has a construction script". | Refused |
| <a id="declaring-components.ue-component-4"></a>`UE_COMPONENT` in a `UE_INTERFACE` | Refused: "an interface cannot declare a UE_COMPONENT". Declare the component on each class that implements the interface. | Refused |

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
| <a id="the-root-and.ue-component-uscenecomponent"></a>`UE_COMPONENT(USceneComponent, Root);` as the first scene component | Becomes the actor's root. | Yes |
| <a id="the-root-and.a-later-scene-component"></a>a later scene component | Attaches directly to the root, unless `SetupAttachment` places it. | Yes |
| <a id="the-root-and.uprojectilemovementcomponent"></a>a component that is not a scene component, such as `UProjectileMovementComponent` | Is created with no attachment, wherever it is declared. | Yes |
| <a id="the-root-and.ue-component"></a>no `UE_COMPONENT` at all | The actor gets the default scene root, as in the editor: a `USceneComponent` named DefaultSceneRoot, made by the class's construction script, which a reference can name across the network, and held in the class's variable `DefaultSceneRoot`. Below a parent that gives the actor a root already, it gets none. | Yes |
| <a id="the-root-and.defaultsceneroot"></a>a member named `DefaultSceneRoot` in an actor class | Refused: the construction script stores the default scene root in the variable of that name, a second one or the one a subclass declares. | Refused |
| <a id="the-root-and.only-components-that-are-not"></a>only components that are not scene components | The actor gets the engine's default scene root too, as in the editor, so a movement component has a root to move. Below a parent that gives the actor a root already, a Blueprint parent or a native one such as ACharacter (its capsule), it gets none, as in the editor. | Yes |
| <a id="the-root-and.ue-defaults-tip-setupattachment"></a>`UE_DEFAULTS { Tip->SetupAttachment(Glow); }`, Glow another `UE_COMPONENT` of the class | Tip attaches to Glow, as in a C++ constructor: in the construction script it is one of Glow's child nodes. It keeps its own location, rotation and scale, relative to Glow. | Yes |
| <a id="the-root-and.tip-setupattachment-glow-fname"></a>`Tip->SetupAttachment(Glow, FName("Muzzle"));` | The same at a socket or bone of Glow (the node's AttachToName). The socket is a literal name or none; a variable or a call there is refused, since it would be dropped. | Yes |
| <a id="the-root-and.glow-setupattachment-lamp"></a>`Glow->SetupAttachment(Lamp);`, Lamp a component of a mod or game Blueprint parent | Glow attaches to the inherited Lamp. Its node names Lamp and the parent class whose construction script makes it (ParentComponentOrVariableName, ParentComponentOwnerClassName), as the editor saves a component dropped on an inherited one. A game Blueprint's component needs the UeApi header to mark it as a construction-script node (`Scene__UeScsNode`); one without it is refused with what to regenerate. | Yes |
| <a id="the-root-and.glow-setupattachment-mesh"></a>`Glow->SetupAttachment(Mesh);` in an `ACharacter` child | Glow attaches to a native default subobject. The node names the subobject by its object name, `CharacterMesh0` for `Mesh` (bIsParentComponentNative), which UeApi records. It stays `CharacterMesh0` further down, in an `APlayerCharacter` child, though `FPMesh` is a skeletal mesh too: a subclass cannot rename a subobject its parent makes. A native member that is no default subobject is refused: attach to it at run time. | Yes |
| <a id="the-root-and.glow-setupattachment"></a>`Glow->SetupAttachment(RootComponent);` | Glow attaches to the actor's root, whichever component that is, as a constructor's call does. Below a parent that gives the actor a root, such as `ACharacter`'s capsule or a Blueprint parent's root, its node is a root node naming no parent, which the construction script attaches to that root. With no root to inherit, Glow is never the root itself: it hangs from the first scene component `SetupAttachment` leaves alone, or, with none, from the default scene root, which stays, as the editor keeps a component added under it. It keeps its own location, rotation and scale, relative to the root. `SetupAttachment(RootComponent, FName("Sock"))` attaches at that socket of the root, the node's AttachToName. | Yes |
| <a id="the-root-and.lamp-setupattachment-own"></a>`Lamp->SetupAttachment(Own);` for an inherited Lamp, `A->SetupAttachment(B); B->SetupAttachment(A);`, or one component attached twice | Refused. An inherited component stays where its own class puts it. A cycle has no node the construction script starts from, so none of its components would be made. | Refused |
| <a id="the-root-and.trigger-oncomponentbeginoverlap"></a>`Trigger->OnComponentBeginOverlap.Add(this, &AMine::OnTouch);` in the constructor or `UE_DEFAULTS` | Binds a component's event, as the editor's component-bound event node does ("On Component Begin Overlap (Trigger)"): each actor of the class made calls OnTouch on itself when its Trigger fires. The component is any this class has a property for, its own `UE_COMPONENT`, a Blueprint parent's or a native parent's (`MovementComponent->OnProjectileStop` in an `AProjectile` child), found by that name as the actor is made. A dispatcher the editor can bind, to a function of this class whose parameters match it. | Yes |
| <a id="the-root-and.pivot-setupattachment-lamp"></a>`Pivot->SetupAttachment(Lamp);` in a function | Attaches at once, keeping the relative transform: `K2_AttachToComponent` with KeepRelative for location, rotation and scale, and no welding. That is what the engine's own `SetupAttachment` leads to when the component registers; called on a component already registered, as any of a constructed actor is, the engine's own does nothing, so the compiler warns, naming the function. Call `AttachToComponent` to pick the rules yourself. | Warns |

Notes:

- If the parent class already has a root, such as `ACharacter`'s capsule or a mod parent's first scene component, this
  class's first scene component attaches under that root instead of replacing it.
- With no root to inherit, the root is the first scene component that `SetupAttachment` leaves alone, even if a
  component declared before it is attached elsewhere, to `RootComponent` too.
- The root's own location, rotation and scale are a special case: see [Class defaults](#class-defaults).

### Adding components at run time

| You write | What it does | Status |
|---|---|---|
| <a id="adding-components-at.addcomponentbytype"></a>`AddComponentByType<USceneComponent>(this)` | The Add Component by Class node (`AActor::AddComponentByClass`), returning the component as a `T*`. It is created at the owner's origin, and a scene component attaches to the owner's root. | Yes |
| <a id="adding-components-at.addcomponentbytype-ucharges-this"></a>`AddComponentByType<UCharges>(this, ChargesClass, true)` | The class can be picked at run time as a `TSubclassOf<T>`; it is `T` by default. The third argument is bManualAttachment: with `true`, a scene component is not attached. A component class the mod declares itself works here. | Yes |
| <a id="adding-components-at.addcomponentbytype-uabstractcomp"></a>`AddComponentByType<UAbstractComp>(this)`, where the class has a `= 0` method left or is a UE_FINAL_AS base | Refused: "is an abstract class ... which the engine may not construct". AddComponentByClass constructs it with NewObject, which asserts on an abstract class in a Development game. Add a subclass that defines every `= 0` method, or the UE_FINAL_AS leaf, which the message names. Only a class the call names is checked. | Refused |
| <a id="adding-components-at.addcomponentdeferred-ucharges"></a>`AddComponentDeferred<UCharges>(this)`, then `FinishComponent(this, C)` | The Add Component node with exposed pins. Registration, and with it the component's BeginPlay, waits for `FinishComponent` (`AActor::FinishAddComponent`), so the values you set in between are the first ones it sees. | Yes |
| <a id="adding-components-at.attachtocomponent-muzzle-barrel"></a>`AttachToComponent(Muzzle, Barrel)` | `K2_AttachToComponent` with one attachment rule for location, rotation and scale (SnapToTarget unless you pass another) and welding on. Returns whether it attached. | Yes |
| <a id="adding-components-at.attachtocomponent-gun-hand-fname"></a>`AttachToComponent(Gun, Hand, FName("hand_r"), EAttachmentRule::KeepRelative, false)` | The same at a socket, with another rule and without welding. For a different rule per channel, call `Gun->K2_AttachToComponent(...)` yourself. | Yes |

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

## Widget layouts

A widget class's designer layout, its widget tree, is declared in the class with `UE_WIDGET` and laid out in its
constructor, as a UMG widget's C++ `RebuildWidget` would: the widgets are added to panels, and each setter is read as
the property it sets. The constructor never runs; AssetGen cooks what it says as the widget Blueprint the editor's
designer would save. The rule to remember: the first `UE_WIDGET` is the tree's root, and every other one sits in a
panel.

| You write | What it does | Status |
|---|---|---|
| <a id="widget-layouts.ue-widget-ucanvaspanel-root"></a>`UE_WIDGET(UCanvasPanel, Root);` in a `UUserWidget` class | Adds a widget to the class's tree, as dropping one in the designer does, and makes `Root` a variable of the class (the designer's Is Variable). When a widget of the class is made, `Root` holds its own copy. The first `UE_WIDGET` is the tree's root. The class is cooked as a widget Blueprint's class (WidgetBlueprintGeneratedClass). | Yes |
| <a id="widget-layouts.root-addchildtocanvas-title"></a>`Root->AddChildToCanvas(Title);` in the constructor | Puts Title in the canvas, as dragging it there in the designer does: a slot of the panel's slot class (`UCanvasPanelSlot`) holds it. `AddChildToVerticalBox`, `AddChildToOverlay` and the other panels' adders work the same way. | Yes |
| <a id="widget-layouts.root-addchild-box"></a>`Root->AddChild(Box);`, `Play->SetContent(Label);` | The same through the untyped adders. The slot is the panel's own kind, `<Panel>Slot` (`ButtonSlot` for a button), as the panel makes it. Take it as its class with `Cast<UButtonSlot>(Play->SetContent(Label))`; a cast to another slot class is refused, since it would be null. | Yes |
| <a id="widget-layouts.root-addchildtocanvas-title-2"></a>`Root->AddChildToCanvas(Title)->SetPosition(FVector2D(20, 10));` | Sets the slot's layout: `SetPosition` and `SetSize` are the canvas slot's offsets (left and top, width and height), `SetOffsets`, `SetAnchors`, `SetMinimum`, `SetMaximum`, `SetAlignment` and `SetLayout` the rest of its LayoutData. The parts no setter gives keep the slot's default. | Yes |
| <a id="widget-layouts.ucanvaspanelslot-at-root"></a>`UCanvasPanelSlot* At = Root->AddChildToCanvas(Title);` then `At->SetSize(FVector2D(200, 40));` | Several setters on one slot, through a local. | Yes |
| <a id="widget-layouts.uoverlayslot-at-box"></a>`UOverlaySlot* At = Box->AddChildToOverlay(Mark);` then `At->Padding.Top = 6.0f;`, `At->HorizontalAlignment = ...;` | A slot member assigned through the local, whole or a part of a struct, as a widget's own members are. A part is written as the cook writes it, the struct holding only that part; the rest keeps the slot class's default (a `UBorderSlot`'s Padding is 4, 2, 4, 2). | Yes |
| <a id="widget-layouts.cast-uverticalboxslot-box"></a>`Cast<UVerticalBoxSlot>(Box->AddChild(Hint))->SetPadding(FMargin(4, 4, 4, 4));`, `->SetHorizontalAlignment(...)` | Any other slot setter, `SetPadding`, `SetHorizontalAlignment`, `SetVerticalAlignment`, `SetSize(FSlateChildSize)`, `SetZOrder`, `SetAutoSize`: read as the property it sets, `Foo` or `bFoo` of the slot's class, of the argument's type. | Yes |
| <a id="widget-layouts.title-settext-ftext-score"></a>`Title->SetText(FText("Score"));`, `Title->Text = FText("Score");` | A widget's own defaults, set by a setter (read the same way, on the widget's class) or assigned as a member, as `UE_DEFAULTS` sets a component's. | Yes |
| <a id="widget-layouts.ue-widget-uwtcard-card"></a>`UE_WIDGET(UWtCard, Card);`, a widget class of the mod or the game | A user widget inside this one. It is made from its own class, its own tree included. | Yes |
| <a id="widget-layouts.ue-widget"></a>a class below a mod widget class with `UE_WIDGET`s, declaring none | Shows its parent's tree, as a widget Blueprint below another does: it is cooked as a widget Blueprint's class too, and its parent's variables hold its widgets. | Yes |
| <a id="widget-layouts.ue-widget-2"></a>a `UE_WIDGET` no `AddChild` call puts in a panel, or a ring of panels holding one another | Refused: "no panel holds it". The engine walks the tree from its root, and would never reach it. | Refused |
| <a id="widget-layouts.ubutton"></a>a widget added twice, the root added, a second child of a content widget (`UButton`, `USizeBox`, `UBorder`) | Refused: a widget has one parent, the root none, and a content widget holds one child. | Refused |
| <a id="widget-layouts.title-setopacity-0-5f"></a>`Title->SetOpacity(0.5f);`, a setter with no property of its name and type | Refused: "AssetGen reads a setter as the property it sets, and ... has no Opacity of its argument's type". Assign the member instead. | Refused |
| <a id="widget-layouts.grid-addchildtogrid-cell-1-2"></a>an adder's other arguments, `Grid->AddChildToGrid(Cell, 1, 2)` | Refused: set them with the slot's setters. | Refused |
| <a id="widget-layouts.ue-widget-3"></a>`UE_WIDGET` in a class that is not a `UUserWidget` | Refused: "a UE_WIDGET is a widget of a UUserWidget's tree". | Refused |
| <a id="widget-layouts.play-onclicked-add-this"></a>`Play->OnClicked.Add(this, &UScoreCard::OnPlay);` in the constructor | Binds a widget's event, as the designer's "On Clicked (Play)" node does: each widget of the class made calls OnPlay when its own Play is clicked. Any widget dispatcher the editor can bind (`OnHovered`, a mod widget's `UE_DISPATCHER`), to a function of this class. A class below inherits it. | Yes |
| <a id="widget-layouts.ue-widget-unamedslot-body"></a>`UE_WIDGET(UNamedSlot, Body);` | A named slot: a place in this widget's layout that a widget nesting this one fills. The class lists it in its named slots. | Yes |
| <a id="widget-layouts.ue-slot-frame-body-inner"></a>`UE_SLOT(Frame->Body, Inner);` in the constructor | Fills the named slot Body of the nested user widget Frame with Inner, a `UE_WIDGET` of this class that sits in no panel, as dropping a widget on a named slot in the designer does. Inner is still this class's variable. Body is a named slot of Frame's class: a `UE_WIDGET(UNamedSlot, ...)` of a mod class, a named slot variable of a game one. | Yes |
| <a id="widget-layouts.ue-bind-title-text-uscorecard"></a>`UE_BIND(Title, Text, &UScoreCard::GetScore);` in the constructor | Binds Title's Text to GetScore, as the designer's Bind button does: the widget reads it from GetScore each time it is drawn. GetScore returns the property's type (`FText` here) and takes nothing; the widget must have a binding for it (`TextDelegate`), which clang checks. | Yes |
| <a id="widget-layouts.ue-slot"></a>a `UE_SLOT` content in a panel too, a slot no `UE_WIDGET` of the mod class it is on, an event bound to another object's function, a `UE_BIND` to a function of a class this one is not | Refused: a widget has one parent; a mod class's slot that is not in its tree is null where the engine looks for it; the engine finds the bound function on this widget, by name. | Refused |
| <a id="widget-layouts.ue-animation-pop"></a>`UE_ANIMATION(Pop);`, `UE_ANIMATION(Fade, 1.5f);` in the class | A widget animation, as the designer's Animations panel adds one: a `UWidgetAnimation*` variable of the class that holds it once a widget is made, for `PlayAnimation(Pop, ...)`. It lasts until its last key, or the length given in seconds. | Yes |
| <a id="widget-layouts.ue-keys-pop-mark-rendertransform"></a>`UE_KEYS(Pop, Mark->RenderTransform.Scale.X, (0, 0.5, Cubic, 1.159), (0.25, 0.7, Cubic));` in the constructor | Keys one channel of a `UE_WIDGET`'s render transform in the animation: `Translation.X`, `Translation.Y`, `Angle`, `Scale.X`, `Scale.Y`, `Shear.X` or `Shear.Y`. Each key is `(time in seconds, value)`, Linear unless it says `Constant` or `Cubic`; a slope in value per second after `Cubic` is its tangent (the curve editor's User), and without one the tangent is Auto, as the editor computes it. The animation binds the widget by name, as Sequencer does. | Yes |
| <a id="widget-layouts.ue-keys"></a>an animation with no keys and no length, two `UE_KEYS` on one channel, a slope after a key that is not `Cubic`, a channel outside the render transform, `UE_ANIMATION` in a class with no `UE_WIDGET` | Refused: an animation runs as long as its keys; a channel's keys are one curve; only a cubic key has a tangent; AssetGen keys the 2D transform track alone; the animation binds widgets of the class's tree. | Refused |

```cpp
class UFrame : public UUserWidget {                  // a box with a named slot that whoever nests it fills
public:
  UE_WIDGET(UVerticalBox, Box);
  UE_WIDGET(UNamedSlot, Body);

  UFrame() { Box->AddChildToVerticalBox(Body); }
};

class UScoreCard : public UUserWidget {
public:
  UE_WIDGET(UCanvasPanel, Root);                     // the root
  UE_WIDGET(UTextBlock, Title);
  UE_WIDGET(UButton, Play);
  UE_WIDGET(UTextBlock, Label);
  UE_WIDGET(UFrame, Frame);
  UE_WIDGET(UTextBlock, Hint);

  UScoreCard() {
    UCanvasPanelSlot *At = Root->AddChildToCanvas(Title);
    At->SetPosition(FVector2D(20, 10));
    At->SetSize(FVector2D(200, 40));
    Root->AddChildToCanvas(Play)->SetOffsets(FMargin(20, 60, 160, 48));   // left, top, width, height
    Cast<UButtonSlot>(Play->SetContent(Label))->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
    Root->AddChildToCanvas(Frame)->SetPosition(FVector2D(20, 120));
    UE_SLOT(Frame->Body, Hint);                     // Hint sits in Frame's Body
    Label->Text = FText("Play");
    Hint->Text = FText("Click to score");
    Play->OnClicked.Add(this, &UScoreCard::OnPlay);
    UE_BIND(Title, Text, &UScoreCard::GetScore);
  }

  int32 Score = 0;
  void OnPlay() { Score += 1; }
  FText GetScore() const { return UKismetTextLibrary::Conv_IntToText(Score, false, true, 1, 324); }
};
```

Notes:

- `CreateWidget<UScoreCard>(Player, UScoreCard::StaticClass())` makes one, and `AddToViewport` shows it. Each widget's
  variable holds that widget's own copy from `Construct` on.
- A setter is read as one property, so one that does more than set it (a text block's `SetOpacity`, which changes its
  colour) is refused rather than half done.
- The text is culture-invariant, as a literal in code is: it is not gathered for translation.
- A layout can be checked without the editor: the `wrender` command of the framework DLL draws a widget class to a PNG
  and writes each widget's box to a JSON beside it.
- `python tools/widgetloop.py <mod dir> <Class> [<w> <h> | desired] --watch --open` is the live editor: each time a
  source of the mod is saved, it cooks the mod, swaps its pak into the running game (`pak reload`, no restart) and
  draws the class again, a few seconds after the save. It prints each widget that was added, removed, moved or
  resized, with its old box; when the compile fails, it prints clang's error and the last good picture stays.
  `--open` shows the PNG in the browser, on a page that reloads it as it changes. Without `--watch` it runs one turn
  and prints every box.

## Class defaults

`UE_DEFAULTS { ... }` sets the defaults a class does not declare itself: those of its components, and those of
properties a parent class declares. It is what editing a component or Class Defaults in the details panel writes. The
block is data: AssetGen reads its assignments when it compiles, and nothing in it ever runs. The rule to remember:
every statement is `Field = value;` or `Component->Field = value;`, and a variable the class declares takes its default
from its own initializer instead. The one call it takes is `Component->SetupAttachment(Parent);`, which places a
component: see [The root and attachment](#the-root-and-attachment). An object instanced in a default, such as a status
effect's items, is made with `UE_INSTANCED` and may be held in a local: see [Instanced objects](#instanced-objects). A
default constructor is read the same way, as C++ would leave the object: see [Constructors](#constructors).

### UE_DEFAULTS

| You write | What it does | Status |
|---|---|---|
| <a id="ue-defaults.ue-defaults-initiallifespan-3-0f"></a>`UE_DEFAULTS { InitialLifeSpan = 3.0f; }` | This class's default for a property a parent declares (an engine, game or mod class), written on its default object as Class Defaults would. No `Super::` is needed. It works in any class, not only an actor. | Yes |
| <a id="ue-defaults.ue-defaults-reach-min-4"></a>`UE_DEFAULTS { Reach.Min = 4.0f; Hit.Distance = 6.0f; Part->Range.Steps = 7; }` | One member of a struct the class inherits, a mod's or an engine struct written as tags, on the class or on a component: the rest stays as the parent has it. The struct is written holding only the members set, as the editor saves a struct that differs in one field, and the loader reads them over the parent's value. A later statement on the same member wins. A path into a struct with a native serializer (FVector, FRotator, ...) is refused: "is a Vector, which is written whole: assign all of it". | Yes |
| <a id="ue-defaults.lamp-intensity-1500-0f"></a>`Lamp->Intensity = 1500.0f;` | A default on the template of a component this class declares, as editing the component in the details panel writes it. A zero or `false` is still written, because a template is compared with the component class's own defaults, where `bVisible` is true. | Yes |
| <a id="ue-defaults.lamp-lightcolor-fcolor-255-128"></a>`Lamp->LightColor = FColor(255, 128, 0);`, `Lamp->LightColor = {255, 128, 0};` | A struct value, by constructor or by braces. Each argument goes to the member its parameter is named after, so `FColor` takes R, G, B, A as in C++, although it stores B, G, R, A. Braces on an SDK struct that has a constructor call that constructor. Only a struct with no constructor is filled by member position. | Yes |
| <a id="ue-defaults.ids-2-3"></a>`Ids = {2, 3};`, `Score = {{"a", 5}, {"b", 2}};` on a TSet or TMap a parent declares | This class's whole value, as Class Defaults would set it. The default object loads a set or map over its parent's value, so the compiler writes what differs, as the editor saves it: the parent's elements (a map's keys) this value lacks, as removed, then the elements the parent lacks or maps to another value. The parent's value is known where a header gives it: an initializer, a `UE_DEFAULTS` or a `UE_STRUCT` member's default instance, in a class of this mod or of another mod's shared header (`UE_CLASS_IN`, or `UE_CLASS` where the header gives the member a value). Below an engine or game class, whose value no header says, the elements are added to that class's own, with a warning naming the member, and so below a class whose `UE_DEFAULTS` sets the member over one (its default object holds that class's elements too; the statement's own are still removed). The same holds for a TSet or TMap inside a UE_STRUCT a parent declares, when you assign the struct whole (`Held = {{2, 3}, 3};`). A key given twice holds the last value given, as in C++, and an FString key compares without case: `{{"a", 5}, {"A", 1}}` is `{"A": 1}`. | Yes |
| <a id="ue-defaults.mesh-staticmesh-sm-crate-b"></a>`Mesh->StaticMesh = &SM_Crate_B;` | Points an object property at an asset: a `UE_ASSET_AT`, a UeAssets name or an asset the mod cooks. The same as picking the asset in the details panel. | Yes |
| <a id="ue-defaults.index-0-7"></a>`Index_0 = 7;`, for a member the SDK spells `Index_0` | Written under the engine's real name, `Index`. See [Classes and variables](#classes-and-variables). | Yes |
| <a id="ue-defaults.button-widgetstyle-normal-drawas"></a>`Button->WidgetStyle.Normal.DrawAs = ESlateBrushDrawType::Box;`, `PrimaryActorTick.bStartWithTickEnabled = false;` | A part of a struct member, at any depth, written as the cook writes a struct that differs from its default in a few members: the tag holds only the parts set, and the rest keeps the archetype's value (the parent's, the component class's, the widget class's). Several statements on one member make one tag, a later one over an earlier. | Yes |
| <a id="ue-defaults.lamp-relativelocation-z-50-0f"></a>`Lamp->RelativeLocation.Z = 50.0f;` | Refused: "RelativeLocation is a Vector, which is written whole: assign all of it". A struct with a native serializer (Vector, Vector2D, Rotator, LinearColor, Color, Guid) is read as one value, never in parts. Assign the whole struct: `Lamp->RelativeLocation = FVector(0.0f, 0.0f, 50.0f);`. | Refused |
| <a id="ue-defaults.home-z-5-0f"></a>`Home.Z = 5.0f;` on a member this class declares | Refused: "is declared here - give it an initializer instead", as for a whole member. | Refused |
| <a id="ue-defaults.getowner-initiallifespan-5-0f"></a>`GetOwner()->InitialLifeSpan = 5.0f;` | Refused: "every statement is ...". A member of another object is no default of this class's. It used to compile as this class's own `InitialLifeSpan`. | Refused |
| <a id="ue-defaults.int32-charges"></a>`int32 Charges;` with `UE_DEFAULTS { Charges = 3; }` | Refused: "is declared here - give it an initializer instead". Write `int32 Charges = 3;`. | Refused |
| <a id="ue-defaults.ue-defaults"></a>`UE_DEFAULTS;` in the class, `void UTurret::UeDefaults__() { Charges = 3; }` outside it | Refused: "UE_DEFAULTS has no body in the class". The statements are read where the macro is written, which is also all a mod that includes the class's header sees. Write `UE_DEFAULTS { Charges = 3; }` in the class. | Refused |
| <a id="ue-defaults.ue-defaults-2"></a>`UE_DEFAULTS { ... }` in a `UE_STRUCT` or a `UE_INTERFACE` | Refused: "has no UE_DEFAULTS". Neither cooks a default object of its own, and the block used to be dropped. Give each variable its initializer; set an interface's inherited variable in the `UE_DEFAULTS` of a class that implements it. | Refused |
| <a id="ue-defaults.extra-bvisible-false"></a>`Extra->bVisible = false;`, where `Extra` is a plain pointer member | Refused: "is not a UE_COMPONENT". Only a `UE_COMPONENT`, this class's or a parent's, has a template to hold defaults. | Refused |
| <a id="ue-defaults.lamp-intensity-100-0f"></a>`Lamp->Intensity += 100.0f;`, an `if`, a call such as `K2_DestroyActor();` | Refused: "every statement is `Field = value;`, `Component->Field = value;` or `Component->SetupAttachment(Parent);`". The block never runs, so logic in it could do nothing. | Refused |
| <a id="ue-defaults.lamp-intensity"></a>`Lamp->Intensity = UKismetMathLibrary::RandomFloat();`, `InitialLifeSpan = sizeof(FVector);` | Refused: "a default is a value known when the mod is built". A value here follows the rules for a member's initializer: see [Classes and variables](#classes-and-variables). Compute anything else in `ReceiveBeginPlay` or `UserConstructionScript`. | Refused |
| <a id="ue-defaults.instigator-nullptr"></a>`Instigator = nullptr;`, `Mesh->StaticMesh = nullptr;`, `Count = {};`, `Rule = EAttachmentRule();`, `Offset = FVector();` | The type's zero, written over the parent's value: null, 0, `false`, the zero enumerator, None, an empty string or container, and the zeros of `FVector`, `FVector2D`, `FRotator`, `FLinearColor`, `FColor` and the `FVector_NetQuantize` types, whose engine constructor sets nothing. A `UE_STRUCT`'s `{}` or `T()` is its own defaults, and so is another mod's (`UE_STRUCT_IN`): its header says each member's initializer, written. The same as `Count = 0;`. | Yes |
| <a id="ue-defaults.hit-fhitresult"></a>`Hit = FHitResult();`, `Hit = {};`, `Floor = FFindFloorResult();`, `Xf = FTransform();`, a `UE_STRUCT`'s `{}` that holds an FHitResult | Refused: "holds what the engine's FHitResult constructor sets". Any other engine struct than those above holds what the engine's constructor sets (`FHitResult::Time` is 1, `FTransform()` is the identity), which no header says, so it cannot be written over the parent's value. Leave the statement out, or give the members in braces: `Hit = {.Time = 1.0f};`. `V4 = FVector4();` is written, (0, 0, 0, 1). | Refused |
| <a id="ue-defaults.hit-distance-6-0f"></a>`Hit = {.Distance = 6.0f};` | The members given are written. One the braces leave out holds what the engine's constructor sets, which no header says: it is left unwritten where the parent's value is a fresh one (declared in a mod parent with no initializer, `{}` or `T()`, or with braces that leave it out, followed down, and set by no `UE_DEFAULTS` on the way), and refused over any other value: "Hit.FaceIndex, left out of the braces, holds what the engine's FHitResult constructor sets". Give every member, or leave the statement out. A `UE_STRUCT` value's braces, `H = {.N = 5};`, refuse a member of such a struct they leave out the same way. Braces nested for a `UE_STRUCT`'s member, `H = {.Hit = {.Distance = 5.0f}};`, are over a fresh value where that member has no initializer of its own, or one whose braces, followed down, leave it out: with `FInner In = {.K = 4};`, `O = {.In = {.Hit = {.Distance = 3.0f}}};` leaves the rest of Hit unwritten, and where the initializer gives Hit `{.Time = 0.5f}`, the Time the new braces leave out is refused. Another mod's struct (`UE_STRUCT_IN`) is no engine struct: a member its braces leave out takes the initializer its header gives it, written. | Yes |
| <a id="ue-defaults.hit-time-3-0f"></a>`Hit = {.Time = 3.0f};` then `Hit = {.Distance = 6.0f};` | The last statement on a member is its value, as the last assignment is in C++: the earlier one writes nothing, so `Hit` is Distance 6 with Time the engine's 1, not 3. | Yes |
| <a id="ue-defaults.handle-ftimerhandle"></a>`Handle = FTimerHandle();` | Writes nothing, with a warning: FTimerHandle's one member is Transient, which the engine never loads from a default, so the value stays the parent's. | Warns |
| <a id="ue-defaults.bhasscriptimplementedtick-true"></a>`bHasScriptImplementedTick = true;` in a widget class that overrides `Tick`, `bHasScriptImplementedPaint = false;` in one that does not override `OnPaint` | Changes nothing, with a warning: "so this line changes nothing". AssetGen sets both flags from the class's own `Tick` and `OnPaint`, as the editor's widget compiler does on every compile. Drop the line. | Warns |
| <a id="ue-defaults.bhasscriptimplementedtick-true-2"></a>`bHasScriptImplementedTick = true;` in a widget class that does not override `Tick`, `bHasScriptImplementedPaint = false;` in one that overrides `OnPaint` | Refused: "follows whether the class overrides Tick itself", since the compile sets the flag back. Override the event for it to run, or drop the override to turn it off: see [Parent classes](#parent-classes). | Refused |
| <a id="ue-defaults.lantern-initiallifespan-5-0f"></a>`Lantern() { InitialLifeSpan = 5.0f; }` | A `UE_DEFAULTS` statement, written in the constructor: see [Constructors](#constructors). | Yes |

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

### Constructors

The engine never runs a Blueprint class's constructor: a new object starts as a copy of its class's default object. So
a default constructor is read as that object's values, what C++ would leave in it. A member initializer, or an
assignment to a variable the class declares, is that variable's initializer. Any other statement is a `UE_DEFAULTS`
statement, under the same rules, read after the class's own `UE_DEFAULTS` block.

| You write | What it does | Status |
|---|---|---|
| <a id="constructors.lantern-charges-5-range-10"></a>`Lantern() : Charges(5) { Range = 10.0f; Range = 20.0f; }` | The same as `int32 Charges = 5;` and `float Range = 20.0f;`: each replaces the variable's in-class initializer, and the last one written wins, as in C++. | Yes |
| <a id="constructors.lantern-initiallifespan-2-0f"></a>`Lantern() { InitialLifeSpan = 2.0f; Lamp->Intensity = 250.0f; Lamp->SetupAttachment(Root); }` | The same as these statements in `UE_DEFAULTS`. A class that also has a `UE_DEFAULTS` block reads the constructor's after it, so on the same member the constructor's value wins. | Yes |
| <a id="constructors.fstats-speed-2-5f-hp"></a>`FStats() : Speed(2.5f) { HP = 50; }` in a `UE_STRUCT` | The struct's default instance, as `int32 HP = 50;` and `float Speed = 2.5f;` would make it. A `UE_STRUCT`'s constructor with parameters still places a value's arguments: see [Structs](#structs). | Yes |
| <a id="constructors.lantern-default"></a>`Lantern() = default;`, `virtual ~Lantern() {}` | Nothing to read. | Yes |
| <a id="constructors.lantern-int32-incharges-charges"></a>`Lantern(int32 InCharges) : Charges(InCharges) {}` | Refused: "the engine makes a Blueprint class's objects with no arguments". Nothing would ever call it. | Refused |
| <a id="constructors.lantern"></a>`Lantern();` in the class, `Lantern::Lantern() { ... }` outside it | Refused: "has no body in the class". As with `UE_DEFAULTS`, a mod that includes the class's header sees only the class. | Refused |
| <a id="constructors.lantern-k2-destroyactor"></a>`Lantern() { K2_DestroyActor(); }`, a local, an `if` | Refused: "a constructor never runs in the game". Each statement must be one `UE_DEFAULTS` takes. Do run-time setup in `ReceiveBeginPlay`. | Refused |
| <a id="constructors.lantern-charges-max-1"></a>`Lantern() { Charges = Max + 1; }` | Refused: "a default is a value known when the mod is built". A value follows the rules for an initializer, so it cannot read a member, even one the constructor set before it. | Refused |
| <a id="constructors.lantern-2"></a>`~Lantern() { ... }` | Refused: "a destructor never runs in the game". The engine frees the object without it. Do teardown in `ReceiveEndPlay`. | Refused |
| <a id="constructors.ue-interface"></a>A `UE_INTERFACE`'s constructor setting a variable of the interface it extends | Refused: "a UE_INTERFACE's constructor sets its own members". An interface has no `UE_DEFAULTS`. | Refused |

### The root's transform

The engine puts an actor's root at the spawn transform and ignores the location, rotation and scale on the root's
template. This is only about a class whose parent has no root: below a mod or game Blueprint parent, or a native one
with a scene component such as `ACharacter`, the first scene component attaches under the inherited root and keeps
its own location, rotation and scale like any other.

| You write | What it does | Status |
|---|---|---|
| <a id="the-root-s.root-relativerotation-frotator-0"></a>`Root->RelativeRotation = FRotator(0.0f, 90.0f, 0.0f);` on a plain `USceneComponent` root | AssetGen moves the root's location, rotation and scale onto every component attached to it, composed the way UE composes a child with its parent, and removes them from the root. The children end up where the editor's viewport would show them, however the actor is spawned. The root itself stays at the spawn transform. | Yes |
| <a id="the-root-s.body-relativescale3d-fvector-2"></a>`Body->RelativeScale3D = FVector(2.0f, 2.0f, 2.0f);` on a root that is a mesh, a light or any class but `USceneComponent` | Warns: "is the actor's root, which the engine puts at the spawn transform". Such a root cannot pass its transform on without losing its own, so the value stays on the template, where the engine does not apply it. Declare a `USceneComponent` first and let the mesh attach to it. | Warns |

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
| <a id="components-a-parent.lamp-intensity-250-0f"></a>`Lamp->Intensity = 250.0f;`, where a mod parent declares `Lamp` | Overrides that component's defaults for this class only, as the editor does for an inherited component. No `Super::` is needed: the compiler finds the class that declares the member. A grandchild that sets `Lamp->bVisible = false;` keeps the 250 as well: defaults fold down the chain as C++ constructors do. | Yes |
| <a id="components-a-parent.enemy-killshout-nullptr"></a>`enemy->KillShout = nullptr;` in a child of `ENE_Spider_ExploderTank_C`, where `ENE_PF_SpiderBase_C` declares `enemy` | A game Blueprint between the class and the component's owner can have a template of the component of its own (a record of its InheritableComponentHandler, which the SDK lists as `UeIchRecords`). The override is then built on that one, as the engine finds it first, and keeps what that Blueprint sets: here `ENE_Spider_ExploderTank_C:Enemy_GEN_VARIABLE`, not the owner's node. A UeApi from a genueapi older than version 5 lists no records, so with a game Blueprint between this is refused, saying to regenerate it. | Yes |
| <a id="components-a-parent.capsulecomponent-capsuleradius"></a>`CapsuleComponent->CapsuleRadius = 55.0f;` in an `ACharacter` child | A C++ parent's component is a default subobject. AssetGen overrides it under the subobject's real name and class, which can differ from the member's: `ACharacter`'s `CapsuleComponent` is `CollisionCylinder`. The SDK records the name as `<Member>__UeSubobject`, for a subclass too: `Mesh` in an `APlayerCharacter` child is still `CharacterMesh0`. Where two of the class's subobjects fit the member and neither has its name, it records the one the game's Blueprints of the class name on their default objects: `ABomber`'s `GooSoundComponent` is `GooAudioComponent`, not `WingSound`. | Yes |
| <a id="components-a-parent.capsulecomponent-capsuleradius-2"></a>`CapsuleComponent->CapsuleRadius = 70.0f;` in a child of a mod class that sets the capsule too | Builds on the parent's override: the child keeps what the parent set, such as its half height, and changes only the radius. The parent is loaded first. A mod parent that leaves the capsule alone still carries one for its child to build on. | Yes |
| <a id="components-a-parent.temperature"></a>`temperature->TemperatureChangeScale = 2.0f;` in a child of a game Blueprint (`ENE_Spider_Grunt_Normal_C`) | The C++ ancestor's component is a default subobject of the game Blueprint's default object, which its package exports: the override is built on that export, and the class loads after every default subobject the parent exports, restated or not, and every object nested in one, such as the bonus instanced in `WPN_Pickaxe_C`'s `Damage` (the SDK lists them as `UeDefaultSubobjects`). The SDK spells a name as the game's object dump does, which can differ in case from the parent's package (`temperature`, `Temperature`); names compare without case, so both are one object. | Yes |
| <a id="components-a-parent.staticmesh-relativescale3d"></a>`StaticMesh->RelativeScale3D = FVector(2.0f, 2.0f, 2.0f);` in a child of a game Blueprint | A game Blueprint's component is a construction-script node, as a mod parent's is. The override is keyed on that node's GUID, which the SDK records as `<Component>__UeScsNode`. The node's real name is used, even when it contains spaces. | Yes |
| <a id="components-a-parent.controller-battachtopawn-true"></a>`Controller->bAttachToPawn = true;` in an `APawn` child | Refused: "UeApi does not say which default subobject Controller is". Either the member is not a default subobject, and you set the value at run time, or the SDK predates the markers, and you regenerate it with genueapi. | Refused |
| <a id="components-a-parent.uescsnode"></a>a component of a game Blueprint whose header has no `__UeScsNode` marker | Refused: "its header does not say which SCS node it is". Regenerate the SDK from a dump made with the Dumper-7 fork, which writes the markers. | Refused |
| <a id="components-a-parent.ue-class"></a>a component of a mod parent that another mod owns (pinned with `UE_CLASS`) | Not yet: refused with the same SCS node message, and its advice to re-dump the game does not apply. Set the value at run time, for example in `ReceiveBeginPlay`. | Not yet |

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

### Instanced objects

Some properties hold an object made for the class itself rather than an asset: a status effect's `StatusEffects`, a
projectile attack component's `Projectiles`, a damage component's bonuses (the editor's Instanced, EditInlineNew
values). In the editor you pick a class in the details panel and fill in its fields. `UE_INSTANCED(Type, Outer, "Name")`
is that object. The cook writes it as an export of its own under `Outer`, with its own defaults, and the property points
at it.

| You write | What it does | Status |
|---|---|---|
| <a id="instanced-objects.statuseffects-ue-instanced"></a>`StatusEffects = {UE_INSTANCED(UStatChangeStatusEffectItem, this, "Slow")};` | A new object of that class under the default object, which the property points at. It starts from its class's defaults. | Yes |
| <a id="instanced-objects.statuseffects-newobject"></a>`StatusEffects = {NewObject<UStatChangeStatusEffectItem>(this, "Slow")};` in the constructor | The UE C++ spelling of `UE_INSTANCED`, which is that macro's expansion now: the same object, in the constructor or `UE_DEFAULTS`. Without a name it is the run-time Construct Object of [Creating objects](#creating-objects). | Yes |
| <a id="instanced-objects.udotstatuseffectitem-burn-ue"></a>`UDotStatusEffectItem *Burn = UE_INSTANCED(UDotStatusEffectItem, this, "Burn");` then `Burn->damageAmount = FRandRange{2.0f, 3.0f};` and `StatusEffects = {Burn};` | A local holds the object so its own members can be set: each `Burn->Field = value;` is a default of that object, under the same rules as any `UE_DEFAULTS` statement. | Yes |
| <a id="instanced-objects.burn-damageimpulseoverride-ue"></a>`Burn->DamageImpulseOverride = UE_INSTANCED(UDamageImpulse, Burn, "Impulse");` | An object under another instanced object, nested as deep as needed. | Yes |
| <a id="instanced-objects.attack-projectiles-ue-instanced"></a>`Attack->Projectiles = {UE_INSTANCED(UNormalProjectileAttack, Attack, "Shot")};` | An object under a component's template: the class's own `UE_COMPONENT`, a component a parent declares (mod or game Blueprint), or a C++ parent's default subobject. | Yes |
| <a id="instanced-objects.ue-instanced"></a>`UE_INSTANCED(UDotStatusEffectItem, this, "DotStatusEffectItem_0")` in a child of `STE_Spider_Shooter_Acid_C` | Named as an object the parent already holds there, it is this class's copy of that one: the engine finds it as its archetype, so a member the child leaves unset keeps the parent's value. This works for a mod parent and a game Blueprint, under the default object or under a component the parent sets up (the SDK lists a game Blueprint's objects as `UeDefaultSubobjects` and `UeTemplateSubobjects`). The parent's object must be of the same class. | Yes |
| <a id="instanced-objects.a-name-the-parent-holds"></a>A name the parent holds, with another class | Refused: "DotStatusEffectItem_0 is a /Script/FSD.DotStatusEffectItem in STE_Spider_Shooter_Acid_C, which UStatChangeStatusEffectItem's DotStatusEffectItem_0 would be a copy of; make it that class, or name it otherwise for an object of its own". | Refused |
| <a id="instanced-objects.ue-instanced-2"></a>`UE_INSTANCED(UDotStatusEffectItem, this)`, or a name that is not a string literal | Refused: "UE_INSTANCED names its object". The name is the export's, and it is how a subclass reaches the object. The editor's own names are `<Class>_<N>`, such as `DotStatusEffectItem_0`. | Refused |
| <a id="instanced-objects.ue-instanced-3"></a>Two `UE_INSTANCED` of one name under one outer | Refused: "makes Burn twice; one object holds one name under its outer". | Refused |
| <a id="instanced-objects.ue-instanced-getowner"></a>`UE_INSTANCED(..., GetOwner(), ...)`, or a local that holds no `UE_INSTANCED` | Refused: "UE_INSTANCED's outer is `this`, one of the class's components, or a local holding another UE_INSTANCED". | Refused |
| <a id="instanced-objects.ue-instanced-4"></a>`UE_INSTANCED(...);` as a statement of its own | Refused: "UE_INSTANCED makes an object a default points at". An object nothing points at would be a default that does nothing. | Refused |
| <a id="instanced-objects.ue-instanced-5"></a>`UE_INSTANCED` in a member initializer | Refused: "UE_INSTANCED makes an object in a class's UE_DEFAULTS, pointed at by one of its defaults". | Refused |
| <a id="instanced-objects.ue-instanced-6"></a>`UE_INSTANCED`, or `NewObject<T>(Outer, "Name")`, in a function | Refused: "call to an unknown function: NewObject". At run time, make an object with `NewObject<T>(Outer)`, no name: see [Creating objects](#creating-objects). | Refused |
| <a id="instanced-objects.an-object-under-a-game"></a>An object under a game Blueprint's component, with an SDK from genueapi older than version 4 | Refused: "UeApi was written by a genueapi older than version 4". Regenerate the SDK. | Refused |

```cpp
#include "UeApi/Game/ENE_ShootingPlant_C.h"

class ToxicSlow : public UStatusEffect {
  UE_DEFAULTS {
    UDotStatusEffectItem *Burn = UE_INSTANCED(UDotStatusEffectItem, this, "Burn");
    Burn->damageAmount = FRandRange{2.0f, 3.0f};
    Burn->DamageImpulseOverride = UE_INSTANCED(UDamageImpulse, Burn, "Impulse");
    StatusEffects = {Burn, UE_INSTANCED(UStatChangeStatusEffectItem, this, "Slow")};
  }
};

class ToxicSlowKid : public ToxicSlow {
  UE_DEFAULTS {
    UDotStatusEffectItem *Burn = UE_INSTANCED(UDotStatusEffectItem, this, "Burn");  // the parent's Burn, copied
    Burn->OverrideDamageImpulse = true;                                             // its damage stays 2 to 3
    StatusEffects = {Burn};
  }
};

class SteadyPlant : public Game::Enemies::ShootingPlant::ENE_ShootingPlant_C {
  UE_DEFAULTS {
    UNormalProjectileAttack *Shot = UE_INSTANCED(UNormalProjectileAttack, ProjectileAttack, "NormalProjectileAttack_0");
    Shot->RandomAngle = 5.0f;                                                       // the rest stays the game's
    ProjectileAttack->Projectiles = {Shot};
  }
};
```

Notes:

- A class's objects are only those its `UE_DEFAULTS` make. A child that wants the parent's object with a change names it
  again; a child that leaves the property alone keeps the parent's list, and the engine gives it copies when it loads.
- bpdecomp writes a cooked class's instanced objects this way, one local per object, under its export's name.

## Creating objects

`Objects.h`, in AssetGen's `include/` folder, spells the editor's Spawn Actor from Class, Construct Object from Class
and Create Widget nodes as C++ templates. Include it by its path from your source: the examples write
`#include "../include/Objects.h"`. Adding and attaching components is in [Components](#components). The rule to
remember: each helper takes one kind of class, and the world context of the engine calls inside it is the calling
object.

### Spawning actors

| You write | What it does | Status |
|---|---|---|
| <a id="spawning-actors.spawnactor-aactor-aactor"></a>`SpawnActor<AActor>(AActor::StaticClass(), Where, this)` | The Spawn Actor from Class node: `BeginDeferredActorSpawnFromClass`, then `FinishSpawningActor`. It returns the new actor as a `T*`. The arguments are the class, the transform, the owner (none by default) and the collision handling (`Undefined` by default). The new actor's construction script and BeginPlay run inside the call. | Yes |
| <a id="spawning-actors.spawnactordeferred-spawner"></a>`SpawnActorDeferred<Spawner>(Spawner::StaticClass(), Where)`, then `FinishSpawning(Twin, Where)` | The Spawn Actor node with Expose on Spawn pins. The actor is created but not finished. Set its variables, and `FinishSpawning` runs its construction script and BeginPlay with those values in place. Pass the same transform to both calls. | Yes |
| <a id="spawning-actors.attachtoactor-plain-twin-fname"></a>`AttachToActor(Plain, Twin, FName(), EAttachmentRule::KeepWorld)` | `K2_AttachToActor` with one rule for location, rotation and scale (SnapToTarget unless you pass another) and welding on. | Yes |

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
| <a id="objects-and-widgets.newobject-uenemycontext-this"></a>`NewObject<UEnemyContext>(this)` | The Construct Object from Class node (`UGameplayStatics::SpawnObject`): a new object of `T`, with `this` as its Outer. `T` can be neither an actor nor a component, the same rule the editor's node applies. | Yes |
| <a id="objects-and-widgets.newobject-uobject-this-kind"></a>`NewObject<UObject>(this, Kind)` | The class picked at run time, as a `TSubclassOf<T>`. | Yes |
| <a id="objects-and-widgets.createwidget-uuserwidget"></a>`CreateWidget<UUserWidget>(PlayerController, HudClass)` | The Create Widget node (`UWidgetBlueprintLibrary::Create`), with the calling object as world context. The owning player may be null. | Yes |
| <a id="objects-and-widgets.newobject-aactor-this"></a>`NewObject<AActor>(this)`, `SpawnActor<UObject>(UObject::StaticClass(), Where)` | Refused by clang on the calling line: "no matching function", then a note naming the constraint `T` fails. `SpawnActor` and `SpawnActorDeferred` take an actor class, `AddComponentByType` and `AddComponentDeferred` a component class, `CreateWidget` a widget class, and `NewObject` any other class. | Refused |
| <a id="objects-and-widgets.spawnactor-ashape-ashape"></a>`SpawnActor<AShape>(AShape::StaticClass(), Where)`, where the class has a `= 0` method left or is a UE_FINAL_AS base | Warns: "is an abstract class" (for a UE_FINAL_AS base, naming the leaf to spawn). SpawnActor makes no actor of an abstract class and returns None. Only a class the call names is checked (`X::StaticClass()`); one picked at run time is not. | Warns |
| <a id="objects-and-widgets.newobject-uspec-this"></a>`NewObject<USpec>(this)`, where the class has a `= 0` method left or is a UE_FINAL_AS base | Refused: "is an abstract class ... which the engine may not construct". SpawnObject makes one in a Shipping game and asserts in a Development one; the editor's Construct Object node refuses the class too. Construct a subclass that defines every `= 0` method, or the UE_FINAL_AS leaf the message names. Only a class the call names is checked (`X::StaticClass()`, or `NewObject`'s default). | Refused |
| <a id="objects-and-widgets.newobject-uprobe-nullptr"></a>`NewObject<UProbe>(nullptr)` | Warns: "SpawnObject with no Outer (None) makes nothing and returns None". Pass the object that owns it, such as `this`. | Warns |

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
- The engine marks `Create` cosmetic, so on a dedicated server the call is skipped and returns None, as the editor's
  node's is; see [Working with other objects](#working-with-other-objects).

### StaticClass on the mod's classes

| You write | What it does | Status |
|---|---|---|
| <a id="staticclass-on-the.spawner-staticclass"></a>`Spawner::StaticClass()`, on one of the mod's own classes | Names that class (`Spawner_C`), as the editor's class picker does, although C++ finds the `StaticClass` of the engine parent. It also works written through a macro in the mod's own source. | Yes |
| <a id="staticclass-on-the.newobject-uenemycontext-this"></a>`NewObject<UEnemyContext>(this)` | A helper's default `T::StaticClass()` argument names `T`. | Yes |
| <a id="staticclass-on-the.ukismetsystemlibrary"></a>`UKismetSystemLibrary::DoesImplementInterface(Obj, IAimable::StaticClass())`, on one of the mod's own interfaces | Names the interface's class (`IAimable_C`), as `ITargetable::StaticClass()` names a game interface's. `UE_INTERFACE` declares the `StaticClass`; it is no function of the interface asset. | Yes |
| <a id="staticclass-on-the.weapons-rifle-staticclass"></a>`Weapons::Rifle::StaticClass()` | Names `Rifle_C`: the qualifier is read in as many parts as it is written with. So does an alias, `using RifleT = Weapons::Rifle;` then `RifleT::StaticClass()`, as [examples/GameBlueprintChild.cpp](examples/GameBlueprintChild.cpp) does. | Yes |
| <a id="staticclass-on-the.staticclassof-egg"></a>`StaticClassOf<Egg>()`, with `class Egg;` declared before | `Egg::StaticClass()` where Egg is only declared so far, as a value and as a default. Two classes whose defaults name each other's class (`Hen` holds `TSubclassOf<AActor> EggClass`, `Egg` holds Hen's class) cannot both have the other complete, and clang needs a class complete for `X::StaticClass()`; a template argument need only be declared. | Yes |

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
  UClass *Named() { return Weapons::Rifle::StaticClass(); } // Rifle_C
  UClass *ViaAlias() { return RifleT::StaticClass(); }      // Rifle_C
};
```

Notes:

- A `StaticClass()` call written through a macro that a `UeApi` header defines names the engine class that declares
  `StaticClass`, not the mod's class. Write the call in the mod's own source, or through a macro defined there.
- `StaticClass()` on engine and game classes, and `GetClass()`, are in [Types](#types).

### Helpers of your own

| You write | What it does | Status |
|---|---|---|
| <a id="helpers-of-your.template-derives-aactor-t-inline"></a>`template <Derives<AActor> T> inline T *SpawnAbove(...)` | Every `Objects.h` function is an inline template, and AssetGen splices its body into the caller. A helper costs exactly the engine calls inside it and adds no function of its own. Write more the same way in your source. | Yes |

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
| <a id="properties-of-other.char-inventorycomponent"></a>`Char->InventoryComponent->MiningItem` | The Get node with a Target pin: the property is read from that object, and a chain reads each object in turn. Properties of game classes, of the mod's other classes and of data assets read the same way. A read through a null object gives zero or empty, and the game logs "Accessed None". | Yes |
| <a id="properties-of-other.getdefault-amyenemy-kind-health"></a>`GetDefault<AMyEnemy>(Kind)->Health`, `GetMutableDefault<AMyEnemy>(Kind)->Describe()` | The Get Class Defaults node: the property is read from, or the function called on, the class default object of the class in `Kind`, in place, with nothing loaded or copied. As in UE, `GetDefault` gives a `const` pointer and `GetMutableDefault` one a non-const function can be called on. Nothing casts the object, so `Kind` must be known to hold the class: a `TSubclassOf<AMyEnemy>` or of a subclass, `TSubclassOf<AMyEnemy>(Raw)`, or `AMyEnemy::StaticClass()`. A bare `UClass *` is refused unless the class asked for is `UObject`, since a read off another class's object reads foreign memory. A null class gives zero, and the game logs "Accessed None reading a Class". | Yes |
| <a id="properties-of-other.uobject-defaults"></a>`UObject *Defaults = GetMutableDefault<UObject>(Class);`, `AMyEnemy *Defaults = GetMutableDefault<AMyEnemy>(Kind);` | The class default object itself, as an object pointer to keep or pass on. A null class gives None. | Yes |
| <a id="properties-of-other.getmutabledefault-amyenemy-kind"></a>`GetMutableDefault<AMyEnemy>(Kind)->Health = 50;` | Writes the property on the class default object, in place. A null class writes nothing, and the game logs "Accessed None reading a Class". | Yes |
| <a id="properties-of-other.getmutabledefault-amyenemy-kind-2"></a>`GetMutableDefault<AMyEnemy>(Kind)->OnSpawned.Add(this, &AMyDirector::Track);`, `.Remove(...)`, `.Broadcast(...)` | Binds a handler on the class default object's dispatcher, removes it, or fires it, in place, as on any other object. The binding is the default object's own. | Yes |
| <a id="properties-of-other.other-hits-1"></a>`Other->Hits += 1;`, `Other->Cursor = Next();` | The Set node with a Target pin. As in C++17, the right side is evaluated before the object is located, so if the right side changes what the pointer names, the write goes to the new object. | Yes |
| <a id="properties-of-other.getpeer-slots-add-5"></a>`GetPeer()->Slots.Add(5);`, `GetPeer()->SlotMap[FName("a")] = 7;` | A container on another object is changed where it lives, as the editor's container nodes do with a Target. The object is located before the arguments are evaluated, which is C++ order. | Yes |
| <a id="properties-of-other.ukismetsystemlibrary"></a>`UKismetSystemLibrary::SetStructurePropertyByName(Obj, "Home", Home);`, `UBlueprintMapLibrary::SetMapPropertyByName(Obj, "Spots", (TMap<int, int>&)Spots);`, `UKismetArrayLibrary::SetArrayPropertyByName(Obj, "Hits", (TArray<int>&)Hits);` | The engine's set-by-name nodes, whose value pin is a wildcard. UeApi spells the wildcard `FGenericStruct`, `TArray<int>`, `TSet<int>` or `TMap<int, int>`: a struct passes as it is, and a container through a reference cast to that type, which only lets clang pass it. The variable itself is passed, as the editor wires the pin to it, and the engine copies it with the named property's type, without checking that the two types match: make sure they do. | Yes |
| <a id="properties-of-other.getpeer-onpeerhit-broadcast-3"></a>`GetPeer()->OnPeerHit.Broadcast(3);` | Fires the dispatcher of another object of the same class. Another mod class's, a game Blueprint's and a BlueprintCallable native dispatcher can be broadcast too; see [Event dispatchers](#event-dispatchers). | Yes |

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
| <a id="validity-and-comparison.if-obj"></a>`if (Obj)`, `!Obj`, `Obj ? A : B` | The Is Valid node: true only for an object that is not null and is not being destroyed (pending kill). This is stricter than C++'s null test. | Yes |
| <a id="validity-and-comparison.if-cls"></a>`if (Cls)`, `!Cls`, `Cls ? A : B` on a `TSubclassOf<T>` | The Is Valid Class node: true for a class that is set and not stale. A `UClass *` tests with Is Valid, as an object. | Yes |
| <a id="validity-and-comparison.obj-nullptr"></a>`Obj != nullptr`, `Obj == nullptr` | The Equal and Not Equal (Object) nodes against None: a plain pointer comparison. An object that is being destroyed still compares `!= nullptr`. Use `if (Obj)` to treat it as gone. | Yes |
| <a id="validity-and-comparison.hit-this"></a>`Hit != this`, `Other->GetClass() == GetClass()` | `EqualEqual_ObjectObject` and `NotEqual_ObjectObject`: object identity. | Yes |
| <a id="validity-and-comparison.target-target-initiallifespan-0"></a>`Target && Target->InitialLifeSpan > 0.0f` | The right side is not read when `Target` is null, as in C++. Blueprint's AND node would evaluate both sides, so AssetGen keeps a branch here. | Yes |
| <a id="validity-and-comparison.a-b"></a>`A < B` on object pointers | Refused: Blueprint compares objects only with `==` and `!=`. See [Operators](#operators). | Refused |

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
| <a id="uobject-helpers.obj-getclass"></a>`Obj->GetClass()` | Get Class. The SDK forwards it to `UGameplayStatics::GetObjectClass(Obj)`. It works on `this` and on any other object. | Yes |
| <a id="uobject-helpers.obj-getname"></a>`Obj->GetName()` | Get Object Name (`UKismetSystemLibrary::GetObjectName`). A class with a `GetName` function of its own, such as `UFSDSaveGame`, calls that one instead. | Yes |
| <a id="uobject-helpers.obj-getouter"></a>`Obj->GetOuter()` | Reads the object's outer pointer from memory, with no engine call. On a null object it crashes, as in C++. For a null-safe call, use `UKismetSystemLibrary::GetOuterObject(Obj)`. | Yes |
| <a id="uobject-helpers.gettypedouter-aactor-obj"></a>`GetTypedOuter<AActor>(Obj)` | From `Objects.h`: the nearest outer of `Obj` that is a `T`, or null. `Obj` itself is never a candidate, and a null `Obj` gives null. Each step is one outer read and one Cast. | Yes |
| <a id="uobject-helpers.getoutermosttypedouter-ulevel"></a>`GetOutermostTypedOuter<ULevel>(Obj)` | From `Objects.h`: the same walk, keeping the farthest outer that is a `T`. | Yes |
| <a id="uobject-helpers.getouter"></a>`GetOuter()` or `GetTypedOuter<T>()` in a function that waits (`Delay`, `UE_AWAIT`) | Reads the outer when the line runs, before the wait or after the function resumes. | Yes |
| <a id="uobject-helpers.obj-isa-aactor-staticclass"></a>`Obj->IsA(AActor::StaticClass())` | Get Class, then Class Is Child Of: `UKismetMathLibrary::ClassIsChildOf(UGameplayStatics::GetObjectClass(Obj), Class)`. False on a null object. Needs `UeApi/Engine.h`. | Yes |
| <a id="uobject-helpers.class-ischildof-aactor"></a>`Class->IsChildOf(AActor::StaticClass())` | Class Is Child Of (`UKismetMathLibrary::ClassIsChildOf`). False on a null class. | Yes |
| <a id="uobject-helpers.obj-class"></a>`Obj->Class` | Refused by clang: "no member named 'Class' in 'UObject'". The SDK declares none of UObject's raw fields (`Class`, `Outer`, `Name`). Use the helpers above. | Refused |

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
  into the mod; see [Pointers and memory](#pointers-and-memory).
- The forwarders need the header that declares their target. Without it the compiler says "which is not declared
  here: include its UeApi header"; `UGameplayStatics` and `UKismetSystemLibrary` are in `UeApi/Engine.h`.

### Methods on structs, text and soft pointers

The editor drags a struct pin out to every library function that takes the struct first. UeApi declares each of those
as a method of the struct, so calls chain the way the nodes do. FString, FName, FText, `TSoftObjectPtr<T>` and
`TSoftClassPtr<T>` get them too.

| You write | What it does | Status |
|---|---|---|
| <a id="methods-on-structs.assetdata-getexporttextname"></a>`AssetData.GetExportTextName()` | `UAssetRegistryHelpers::GetExportTextName(AssetData)`: the value goes first, the arguments after it. | Yes |
| <a id="methods-on-structs.path-tosoftclassptr"></a>`Path.ToSoftClassPtr()`, `V.ToString()` | A one-argument `Conv_XToY` is `To<Y>()`: `Conv_SoftClassPathToSoftClassRef`, `Conv_VectorToString`. | Yes |
| <a id="methods-on-structs.soft-loadclassasset-blocking"></a>`Soft.LoadClassAsset_Blocking()` | The same on a soft pointer, `UKismetSystemLibrary::LoadClassAsset_Blocking(Soft)`. | Yes |
| <a id="methods-on-structs.a-library-function-taking-a"></a>A library function taking a world context, or a latent one, as a method | Not declared: call the static. | Not yet |

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
| <a id="the-player-and.ugamefunctionlibrary"></a>`UGameFunctionLibrary::GetLocalPlayerCharacter(this)` | DRG's getter for the character this machine controls, already an `APlayerCharacter`, so no cast is needed. `this` can be left out. It returns null until the character exists, so a mod that starts in `ReceiveBeginPlay` waits for it in a `Delay` loop. | Yes |
| <a id="the-player-and.ugameplaystatics"></a>`UGameplayStatics::GetPlayerController(0)`, `GetPlayerPawn(0)`, `GetPlayerCharacter(0)` | The Get Player Controller, Get Player Pawn and Get Player Character nodes. They return engine types, so `Cast<APlayerCharacter>` the result to reach DRG's members. The index counts the player controllers in this world. A client has only its own, so 0 is the local player. | Yes |
| <a id="the-player-and.ugamefunctionlibrary-2"></a>`UGameFunctionLibrary::GetFSDGameState()` | One of DRG's typed getters: `GetFSDGameState`, `GetFSDGameMode`, `GetFSDGameInstance`, `GetFSDSaveGame`, `GetCSGWorld`, `GetNumPlayers` and more. The world context is filled in. | Yes |
| <a id="the-player-and.for-aplayercharacter-p-gs"></a>`for (APlayerCharacter *P : GS->ActivePlayerCharacters)` | The game state's list of every player's character, walked in place with no copy. | Yes |

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
| <a id="finding-actors-and.ugameplaystatics"></a>`UGameplayStatics::GetAllActorsOfClass(APlayerCharacter::StaticClass(), Found)` | Get All Actors Of Class. The result comes back through the `TArray<AActor *>&` parameter: the engine empties the array, then adds every actor of the class, subclasses included, from the calling object's world. Cast each element to reach the class's own members. | Yes |
| <a id="finding-actors-and.getallactorsofclasswithtag"></a>`GetAllActorsOfClassWithTag(AActor::StaticClass(), FName("Loot"), Loot)`, `GetAllActorsWithTag(FName("Loot"), Loot)` | The With Tag nodes. They match an actor's `Tags` array. | Yes |
| <a id="finding-actors-and.getallactorswithinterface"></a>`GetAllActorsWithInterface(ITargetable::StaticClass(), Found)` | Get All Actors With Interface: every actor that implements the interface. | Yes |
| <a id="finding-actors-and.ugameplaystatics-getactorofclass"></a>`UGameplayStatics::GetActorOfClass(AMyManager::StaticClass())` | Get Actor Of Class: the first actor of the class (subclasses included) that the world's actor iterator reaches, or null. With several actors of the class, which one comes back depends on that order, so use it for a class the world holds one of. | Yes |
| <a id="finding-actors-and.a-getcomponentbyclass"></a>`A->GetComponentByClass(UHealthComponentBase::StaticClass())` | Get Component by Class: the actor's first component of that class, or null. It is typed `UActorComponent*`, so Cast it. `K2_GetComponentsByClass(Class)` returns all of them, and `GetComponentsByTag(Class, Tag)` also filters by component tag. | Yes |
| <a id="finding-actors-and.a-getcomponentsbyclass"></a>`A->GetComponentsByClass<UStaticMeshComponent>()`, `GetComponentsByClass<UMyPart>()` | Get Components by Class: every component of that class, as a `TArray<UStaticMeshComponent *>` that needs no cast. It is `K2_GetComponentsByClass(UStaticMeshComponent::StaticClass())`, whose array of `UActorComponent *` holds only that class. | Yes |
| <a id="finding-actors-and.uactorfunctionlibrary"></a>`UActorFunctionLibrary::GetPlayersInRange(K2_GetActorLocation(), 1000.0f, true)` | DRG's getter for the player characters within a radius, as a `TArray<APlayerCharacter *>` that needs no cast. The last argument is MustBeAlive. DRG marks it authority-only: on a client the call is skipped and returns an empty array. | Yes |
| <a id="finding-actors-and.ue-authority-only"></a>an engine or game static marked `UE_AUTHORITY_ONLY` or `UE_COSMETIC` | Called on its class's default object, as the editor's node calls it, so the engine decides where it runs. For a static of a function library, an actor or a component class, which is nearly all of them, an authority-only one is skipped on a client and a cosmetic one on a dedicated server. A skipped call returns zero or None and leaves its output parameters as they were. | Yes |

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
- The statics the engine skips this way include `UGameplayStatics::ApplyDamage`, `ApplyPointDamage` and
  `ApplyRadialDamage`, `GetPlayersInRange`, `LockCharacters`, `DropToTarget`, the `SpawnEnemies` statics and
  `PlayCueOnAll` on a client (authority-only), and `UWidgetBlueprintLibrary::Create` (`CreateWidget`), the
  `SetInputMode` statics, `RemoveAllWidgets`, `SpawnSound2D`, `PlaySound2D` and `SpawnDecalAtLocation` on a dedicated
  server (cosmetic). A mod's own `UE_AUTHORITY_ONLY` and `UE_COSMETIC` functions, and the engine's member functions,
  are skipped as the editor's are: see [RPCs](#rpcs).

### Subsystems

| You write | What it does | Status |
|---|---|---|
| <a id="subsystems.udamagesubsystem-get"></a>`UDamageSubsystem::Get()`, `UUGCSubsystem::Get()` | The editor's Get <Subsystem> node. Every subsystem class in the SDK has a static `Get`, which calls `USubsystemBlueprintLibrary`'s getter for the class's kind (engine, game instance or world) and returns the subsystem already typed. | Yes |
| <a id="subsystems.getsubsystem-utracermanager"></a>`GetSubsystem<UTracerManager>()` | The same as `UTracerManager::Get()`. | Yes |
| <a id="subsystems.utracermanager-get-other"></a>`UTracerManager::Get(Other)` | The world or game instance subsystem of `Other`'s world or game instance. Left out, the context is `this`, or a static function's own `WorldContext...` parameter. An engine subsystem's `Get` takes no argument, because there is one engine. | Yes |
| <a id="subsystems.bp-tracermanager-c-get"></a>`BP_TracerManager_C::Get()` | A subsystem the game implements as a Blueprint, for its Blueprint variables. DRG has two: `BP_TracerManager_C` (world) and `BP_FadeScreenSubSystem_C` (game instance), each in its `UeApi/Game` header. | Yes |
| <a id="subsystems.utracermanager"></a>`(UTracerManager *)USubsystemBlueprintLibrary::GetWorldSubsystem(this, UTracerManager::StaticClass())` | What `Get` expands to, written by hand. The C-style cast compiles to nothing. | Yes |

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
| <a id="declaring-data-assets.umooddef-md-big-health-500"></a>`UMoodDef MD_Big = {.Health = -500.5f, .Title = "Big"};` | An asset of the class, cooked as `<mod package>/MD_Big`. Only the members the braces name are written, and the rest keep the class defaults. A member named with a zero value is still written. | Yes |
| <a id="declaring-data-assets.umooddef-md-plain"></a>`UMoodDef MD_Plain = {};` | An asset with the class defaults only. | Yes |
| <a id="declaring-data-assets.umooddef-md-zero-health"></a>`UMoodDef MD_Zero = {.Health = {}};` | `{}` for a member is its zero, written as `.Health = 0` is. A struct, container or name member's `{}` is written too: zeros, an empty container, None, or a `UE_STRUCT`'s own defaults, not the class default. | Yes |
| <a id="declaring-data-assets.uhitdef-hd-near-h-hit"></a>`UHitDef HD_Near = {.H = {.Hit = {.Distance = 5.0f}}};` | An asset starts as its class's default object, so braces for a struct member are written over the default object's value, as a `UE_DEFAULTS` statement's are over a parent's. A member they leave out that the engine's constructor sets (`FHitResult`'s FaceIndex) stays unwritten where the default object's value of it is the engine's too. Where the class's own braces or a `UE_DEFAULTS` gave it another value (`H = {.Hit = {.FaceIndex = 3}}`), or the class is a game or engine class, whose value no header says, it is refused, naming the member. | Yes |
| <a id="declaring-data-assets.uenemydescriptor-ed-mine"></a>`UEnemyDescriptor ED_Mine = {.SpawnSpread = 250.0f, .IdealSpawnSize = 4};` | An asset of a game or engine class. | Yes |
| <a id="declaring-data-assets.namespace-moods-umooddef-angry"></a>`namespace Moods { UMoodDef Angry = {.Health = 50}; }` | A namespace is a folder: the asset is cooked at `<mod package>/Moods/Angry`. See [Mod sources and packages](#mod-sources-and-packages). | Yes |
| <a id="declaring-data-assets.delay-ffloatinterval-1-0f-5"></a>`.Delay = FFloatInterval(1.0f, 5.0f)`, `.Delay = {2.0f, 6.0f}` | A struct member, by constructor or by braces, one value per member, as in any default. | Yes |
| <a id="declaring-data-assets.waves-3-5-8"></a>`.Waves = {3, 5, 8}` | A container member takes a braced list. | Yes |
| <a id="declaring-data-assets.ids-3"></a>`.Ids = {3}`, `.Score = {{"a", 5}}` on a TSet or TMap whose class default is `{1, 2}`, `{{"a", 1}, {"c", 3}}` | The asset's whole value, as C++ means it. The asset loads a set or map over its class default's value, so the compiler writes what differs, as the cooker does: the class default's elements (a map's keys) this value lacks, as removed, then the elements the class default lacks or maps to another value. `.Ids = {}` empties it. The class default is known where a header gives it: an initializer, a `UE_DEFAULTS` on the way or a `UE_STRUCT` member's default instance, in a class of this mod or of another mod's shared header (`UE_CLASS_IN`, or `UE_CLASS` where the header gives the member a value). A game or engine class's value no header says: there the elements are added to that class's own, with a warning naming the member, and so over a class whose `UE_DEFAULTS` sets the member over one. The same holds for a TSet or TMap inside a struct member. A key given twice holds the last value given, as in C++, and an FString key compares without case: `{{"a", 5}, {"A", 1}}` is `{"A": 1}`. A TArray is replaced whole. | Yes |
| <a id="declaring-data-assets.next-md-calm"></a>`.Next = &MD_Calm` | One asset pointing at another. | Yes |
| <a id="declaring-data-assets.enemyclass-game-a-bp-a"></a>`.EnemyClass = "/Game/A/BP_A.BP_A_C"` | A soft object or soft class member takes the asset's path as a string, written the way the cooker writes it. The path rules are in [Types](#types). | Yes |
| <a id="declaring-data-assets.ubigdef-md-huge-health-900"></a>`UBigDef MD_Huge = {{.Health = 900}, false};` | Sets a member a mod base class declares: the base gets a brace list of its own as the first element. C++ cannot designate a base's member (`{.Health = 900}` is a clang error), and after a nested base list the class's own members go by position. | Yes |
| <a id="declaring-data-assets.spawnclass-amyenemy-staticclass"></a>`.SpawnClass = AMyEnemy::StaticClass()`, on a `TSubclassOf` member | The class, as picking it in the asset's details does. `UE_ASSET_EDIT` takes one the same way ([Editing game packages](#editing-game-packages)). See [Classes and variables](#classes-and-variables). | Yes |

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
| <a id="pointing-at-data.umooddef-picked-md-big"></a>`UMoodDef *Picked = &MD_Big;` | `&Asset` is that asset wherever a default expects an object: a variable, or an element or value of a `TArray`, `TSet` or `TMap` default. It is what picking the asset in the details panel writes. | Yes |
| <a id="pointing-at-data.umooddef-def-md-calm"></a>`UMoodDef *Def = &MD_Calm;` in a function, `(&MD_Big)->Health` | In a function body, `&Asset` is an object constant, like an asset picked on a node's pin. Read and write its members through the pointer, as for any object. | Yes |
| <a id="pointing-at-data.float-h-md-big-health"></a>`float H = MD_Big.Health;` | Refused: "is an asset, not a value". Take its address: `(&MD_Big)->Health`. | Refused |

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
| <a id="ue-asset-at.ue-asset-at-uenemydescriptor-ed"></a>`UE_ASSET_AT(UEnemyDescriptor, ED_Spider_Grunt, "/Game/Enemies/Spider/Grunt/ED_Spider_Grunt");` | Names an asset in another package, so `&ED_Spider_Grunt` points at it in defaults and in function bodies. The object's name is the last segment of the path. Only an asset the source uses is imported. | Yes |
| <a id="ue-asset-at.game-dir-package-object"></a>`"/Game/Dir/Package.Object"` | Names an object whose name is not its package's. In the game's content the difference is mostly letter case. | Yes |
| <a id="ue-asset-at.ue-asset-at-uenemydescriptor"></a>`UE_ASSET_AT(UEnemyDescriptor, Spider, ...)` in two namespaces | Two different assets: an asset is keyed by its qualified name. | Yes |
| <a id="ue-asset-at.ed-spider-grunt"></a>`(&ED_Spider_Grunt)->SpawnAmountModifier` | Reads the game asset's value when the game runs. | Yes |
| <a id="ue-asset-at.ed-spider-grunt-spawnspread-2"></a>`ED_Spider_Grunt.SpawnSpread * 2`, `ED_Spider_Grunt.SpawnRarityModifiers[1].Rarity` | A member read by value is the value the asset's package holds, read when the mod is built off the extracted game pak (`--game`; bpbuild: `game_content`), and a constant there: in code, and in a default. A number, a bool, an enum, a name or a string. A step is `.Member` or a `TArray`'s `[i]`, as in `UE_ASSET_EDITS`. A member the asset has no value of is its archetype's, its class's default object, followed through the game's packages; a Blueprint class's own variable that its default object leaves out is zero. The SDK carries no values, so none can go stale with it. The compile prints each value it reads, `value <- <package> (<path> = <value>, ...)`, and bpbuild rebuilds a mod when the game's copy of a package it read is newer, after a re-extract. | Yes |
| <a id="ue-asset-at.ed-spider-grunt-2"></a>`ED_Spider_Grunt.SpawnAmountModifier`, a member the asset leaves to a native class | Refused: "sets no value of its own, so it holds EnemyDescriptor's C++ default, which no package holds". That value is the C++ constructor's, in the game's code. Read it when the game runs, `(&ED_Spider_Grunt)->SpawnAmountModifier`. | Refused |
| <a id="ue-asset-at.a-member-whose-type-ueapi"></a>a member whose type UeApi gives otherwise than the package, an enumerator UeApi's enum lacks | Refused: "the game's package holds a FloatProperty there, where UeApi declares a IntProperty: regenerate UeApi from the same game build as the extracted pak". A stale SDK never compiles in a value it misreads. | Refused |
| <a id="ue-asset-at.a-reference-a-text-a"></a>a reference, a text, a map's value, an element past the end of the game's array, an asset of a mod pak | Refused, each with its reason ("only a number, a bool, an enum, a name or a string is read when the mod is built", "the game's array has 0 elements", "is not in <game folder>"). Read it when the game runs, through the pointer. | Refused |
| <a id="ue-asset-at.ue-asset-edit"></a>a value that this mod's own `UE_ASSET_EDIT` or `UE_ASSET_EDITS` changes | Refused: "this mod's edit changes the value read when the mod is built". The game loads the edited value. | Refused |
| <a id="ue-asset-at.game"></a>a value read without `--game` | Refused: "a game asset's value is read off its package when the mod is built: pass the folder /Game is in". | Refused |
| <a id="ue-asset-at.otherdata"></a>`&OtherData`, another mod's asset of a class that a shared header declares without `UE_CLASS` | Refused: each mod that includes the header cooks its own copy of the class, so the reference "would load as null". The message prints the `UE_CLASS` line that pins the class to its owner; see [Mod sources and packages](#mod-sources-and-packages). | Refused |
| <a id="ue-asset-at.changing-a-game-data-asset"></a>changing a game data asset, or a game Blueprint's defaults or functions, in the pak | `UE_ASSET_EDIT`, `UE_ASSET_EDITS` and `UE_PATCH`: see [Editing game packages](#editing-game-packages). | Yes |

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
  float Spread() { return ED_Spider_Grunt.SpawnSpread * 2; }             // read when the mod is built (--game)
};
```

Notes:

- A wrong path compiles, and the reference can then only load as null.
- A value read when the mod is built is the game's as the extracted pak holds it. After a game update, extract the pak
  again; bpbuild then rebuilds every mod that read a changed package. A lone `assetgen compile` does not check.
- Inside a namespace that has the class's name, as in the `UeAssets` headers, spell the class `::UEnemyDescriptor`.
- The unpinned-class check looks only at a `UE_ASSET_AT` the source uses, and not at an asset the same source
  defines.
- A soft reference names an asset by its path instead and loads nothing with the class: see [Types](#types).

### The UeAssets headers

| You write | What it does | Status |
|---|---|---|
| <a id="the-ueassets-headers.include-ueassets"></a>`#include "UeAssets/UEnemyDescriptor.h"` | One header per asset class, generated from the game's AssetRegistry.bin by `tools/genueassets.py`. Each asset is a `UE_ASSET_AT` in a namespace that mirrors its folder. | Yes |
| <a id="the-ueassets-headers.ueassets-uenemydescriptor-game"></a>`&UeAssets::UEnemyDescriptor::Game::Enemies::Spider::Grunt::ED_Spider_Grunt` | Points at that game asset, by its path. | Yes |
| <a id="the-ueassets-headers.genueassets-py-pak-mod-pak"></a>`genueassets.py ... --pak <mod pak>` | Also names the assets of a mod pak, which has no registry, from the pak's own package headers. | Yes |

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
| <a id="lists-of-assets.ue-asset-all-uenemydescriptor"></a>`UE_ASSET_ALL(UEnemyDescriptor);` in a namespace | Declares `All`, a `TArray<TSoftObjectPtr<UEnemyDescriptor>>` filled when the mod is built. It holds every `UE_ASSET_AT` in that namespace and the namespaces inside it whose class is the class given or derives from it. They are soft pointers, so nothing loads until you load one. | Yes |
| <a id="lists-of-assets.ueassets-usoundwave-all"></a>`UeAssets::USoundWave::All` | Each `UeAssets` header declares one: every game asset of that class. | Yes |

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

## Editing game packages

A mod can change an asset or a Blueprint the game already has, rather than add one beside it. `UE_ASSET_EDIT` and
`UE_ASSET_EDITS` change a game data asset's values, and a class marked `UE_PATCH` changes a game Blueprint's class
defaults, its components' defaults and its functions. The compiler reads the package as the game cooked it, changes
only what the source names, and writes the package back at its own path, so the mod's pak holds a copy that replaces
the game's. The rule to remember: an edit starts from the game's own package, so every compile that edits needs
`--game <the folder /Game is in>`, the `Content` folder of the extracted game pak (bpbuild: `game_content` in
`mods.yaml`).

### Editing a game asset: UE_ASSET_EDIT

| You write | What it does | Status |
|---|---|---|
| <a id="editing-a-game.ue-asset-edit-ed-spider"></a>`UE_ASSET_EDIT(ED_Spider_Grunt) {.SpawnSpread = 800.0f, .IdealSpawnSize = 12};` at namespace scope | Edits a game asset that a `UE_ASSET_AT` names, every one in the `UeAssets` headers included. Each member the braces name replaces the asset's value, or is added where the asset had none. The members the braces leave out keep the asset's values, and every other byte of the package stays the game's. | Yes |
| <a id="editing-a-game.canbeusedforconstantpressure"></a>`.CanBeUsedForConstantPressure = false`, `.Count = 0` | A zero is written like any other value, since it replaces the asset's own. | Yes |
| <a id="editing-a-game.veteranclasses-ed-spider-grunt"></a>`.VeteranClasses = {&ED_Spider_Grunt, &ED_Spider_Exploder}`, `.SpawnClass = APlayerCharacter::StaticClass()` | A container member is replaced whole. An object or class the package did not reference before is added to its imports. | Yes |
| <a id="editing-a-game.ue-asset-edits-ed-spider"></a>`UE_ASSET_EDITS { ED_Spider_Grunt.SpawnRarityModifiers[1].Rarity = 2.0f; }` | Assigns part of a member's value by path: the rest of the array, and of that element, keeps its bytes. A step is `.Member` or `[i]`, a `TArray` element whose index is a constant, to any depth. | Yes |
| <a id="editing-a-game.ue-asset-edits-ed-spider-2"></a>`UE_ASSET_EDITS { ED_Spider_Exploder.SpawnSpread = 100.0f; ED_Spider_Grunt.IdealSpawnSize = 12; }` | A block can assign whole members too, of several assets. Its statements apply in order, so a later one lands on what an earlier one wrote. | Yes |
| <a id="editing-a-game.ue-asset-edit"></a>`UE_ASSET_EDIT` and `UE_ASSET_EDITS` of one asset in one source | Both land in the one edited package. | Yes |
| <a id="editing-a-game.primaryactortick-bcanevertick"></a>`PrimaryActorTick.bCanEverTick = true`, a path into a member the object has no value of | The object takes that member from its class (its archetype), so the new value holds only the path, and the engine fills in the rest. This works through members of structs written as tags. Through a native struct (FVector, FRotator, ...) or an array element, the value's other parts are unknown when the mod is built: refused with "is not set on". Assign the whole member. | Yes |
| <a id="editing-a-game.points-2-x-1-0f"></a>`Points[2].X = 1.0f` on an array of two | Refused: "Points has 2 elements". | Refused |
| <a id="editing-a-game.ue-asset-edits-def-score"></a>`UE_ASSET_EDITS { Def.Score["a"] = 1; Def.Ids.Add(7); Def.Score.Remove("b"); }` | One element of a `TMap` or `TSet` put, added or removed, as C++'s `operator[]`, `Add` and `Remove` leave the container: merged into the value the asset holds, so its other elements stay. The tag is a delta over the archetype's value, so a key removed is listed as removed, and an asset with no value of its own gets a tag of the one element. An FString key compares without case. A path through members of structs written as tags reaches one too (`Def.Held.Score["a"] = 1`), and so does a patch's `UE_DEFAULTS`. | Yes |
| <a id="editing-a-game.def-arr-0-score-a"></a>`Def.Arr[0].Score["a"] = 1`, `Def.Score["a"].X = 1` | Refused: a map inside an array's element or a native struct "cannot be changed on its own yet; assign the whole element"; a map's value "is assigned whole, `Map[Key] = value;`". | Refused |
| <a id="editing-a-game.ue-asset-edit-md-big"></a>`UE_ASSET_EDIT(MD_Big) {...}`, an asset the mod cooks itself | Refused: "the target is a UE_ASSET_AT ...; a mod's own asset takes its values where it is declared". Set the value in the asset's own braces. | Refused |
| <a id="editing-a-game.game"></a>any edit without `--game` | Refused: "an edit starts from the game's own package: pass the folder /Game is in". | Refused |

```cpp
#include "UeAssets/UEnemyDescriptor.h"

using namespace UeAssets::UEnemyDescriptor::Game::Enemies::Spider;

UE_ASSET_EDIT(Grunt::ED_Spider_Grunt) {.SpawnSpread = 800.0f, .IdealSpawnSize = 12};

UE_ASSET_EDITS {
  Exploder::ED_Spider_Exploder.SpawnSpread = 100.0f;            // a whole member
  Grunt::ED_Spider_Grunt.SpawnRarityModifiers[1].Rarity = 2.0f; // one member of one element
}
```

Notes:

- `UE_ASSET_EDIT`'s braces take whole members. To change part of one, assign it by path in `UE_ASSET_EDITS`.
- An edit adds no registry rows (`registry -> none`): the game's registry already lists the asset.

### Patching a game Blueprint: UE_PATCH

A class with `UE_PATCH;` among its members is not a class of its own. It derives from the game Blueprint it edits, and
its `UE_DEFAULTS` and methods are written into that Blueprint's own package.

| You write | What it does | Status |
|---|---|---|
| <a id="patching-a-game.class-slowgrunt-public-ene"></a>`class SlowGrunt : public ENE_Spider_Grunt_Normal_C { UE_PATCH; UE_DEFAULTS { CustomTimeDilation = 0.5f; } };` | Edits the Blueprint's class defaults: the tags of its default object, `Default__<Name>_C`. A member of the Blueprint or of any class above it can be set, and a zero is written like any value. | Yes |
| <a id="patching-a-game.healthcomponent-maxhealth-180-0f"></a>`HealthComponent->MaxHealth = 180.0f;`, a component a native class declares | Edits the default subobject the engine copies that component from. | Yes |
| <a id="patching-a-game.meleeattack-centerontarget-true"></a>`MeleeAttack->CenterOnTarget = true;`, a component the Blueprint adds | Edits its template in the Blueprint's Components panel. | Yes |
| <a id="patching-a-game.comp-field-value"></a>`Comp->Field = value;`, a parent Blueprint's component that this Blueprint overrides | Edits this Blueprint's override of the component, as setting it in this Blueprint's Components panel does. | Yes |
| <a id="patching-a-game.a-parent-blueprint-s-component"></a>a parent Blueprint's component that this Blueprint does not override | Not yet: the package has no template of it to edit. Refused with "the class does not override that inherited component". Patch the Blueprint that declares the component, which changes every child that does not override it too. | Not yet |
| <a id="patching-a-game.loose-bvisible-false"></a>`Loose->bVisible = false;`, a member that is no component of the Blueprint | Refused: "Loose is not one of <Class>'s components (UeApi gives it no SCS node)". | Refused |
| <a id="patching-a-game.primaryactortick-bcanevertick"></a>`PrimaryActorTick.bCanEverTick = true;`, `MeleeAttack->Montages[0] = &ANIM_Attack;` | Paths into a value, by the rules of `UE_ASSET_EDITS` above. | Yes |
| <a id="patching-a-game.score-a-9"></a>`Score["a"] = 9;`, `Ids.Add(7);`, `Score.Remove("b");` | One element, merged into the default object's value as in `UE_ASSET_EDITS` above. | Yes |
| <a id="patching-a-game.ids-7"></a>`Ids = {7};`, `Score = {{"a", 9}};` on a `TSet` or `TMap` | The default object loads exactly that value: the elements the parent's value has and this one drops are written as removed, as the cook writes a set or map. `Ids = {}` empties it. The same holds for one inside a struct, by path or in a whole struct. A key given twice holds the last value, and an FString key compares without case. | Yes |
| <a id="patching-a-game.class-tweaks-public-aactor-ue"></a>`class Tweaks : public AActor { UE_PATCH; ... };` | Refused: "a patch derives from the game Blueprint it edits (a class in a /Game package); a native class's defaults come from its C++ constructor, not from a package". | Refused |
| <a id="patching-a-game.int32-extra-4"></a>`int32 Extra = 4;` in a patch | A variable added to the Blueprint, after its own: its methods and the patch's read and write it, and the initializer is a tag of its default object, which every instance and a game subclass start from. A `UE_DEFAULTS` value of it wins over the initializer, as the constructor's would. | Yes |
| <a id="patching-a-game.class-tweaks-public-bp-turret"></a>`class Tweaks : public BP_Turret_C, public IPoke { UE_PATCH; ... };` | The Blueprint implements the interface: listed in its Interfaces as the editor lists one, with the patch's methods for it, an empty one for each it leaves out, and the interface's variables as the class's own. Checked offline; not run in the game yet. | Yes |
| <a id="patching-a-game.int32-count"></a>`int32 count;` beside the Blueprint's `Count` | Refused: "already has a variable Count, and an FName ignores case; rename it". | Refused |
| <a id="patching-a-game.ue-replicated-int32-extra"></a>`UE_REPLICATED(int32, Extra);` in a patch | Refused: "a patch's own member does not replicate". It would renumber the class's replicated properties against a player without the mod. | Refused |
| <a id="patching-a-game.ue-component"></a>`UE_COMPONENT(...)`, `UE_WIDGET(...)` or `UE_DISPATCHER(...)` in a patch | Refused: "a patch adds no component or widget" / "no dispatcher". The construction script and widget tree stay the game's. Spawn one at run time, or declare a class of your own. | Refused |
| <a id="patching-a-game.an-interface-whose-function-the"></a>an interface whose function the Blueprint already has, left out of the patch | Refused: "would replace with an empty one; write the method in the patch". `Parent::Fn()` in that method runs the game's. | Refused |
| <a id="patching-a-game.ue-defaults"></a>a patch with neither `UE_DEFAULTS`, methods nor members | Refused: "nothing to edit". | Refused |

### Replacing a game Blueprint's functions

| You write | What it does | Status |
|---|---|---|
| <a id="replacing-a-game.void-getenemyspawnedcount-int32"></a>`void GetEnemySpawnedCount(int32 &SpawnCount) { SpawnCount = 42; }` in a patch | Replaces the Blueprint's own function of that name. The function keeps the game's flags and parameters, so every caller still fits, and takes the method's locals and code. | Yes |
| <a id="replacing-a-game.ene-spider-grunt-normal-c"></a>`ENE_Spider_Grunt_Normal_C::GetEnemySpawnedCount(SpawnCount);` in the replacement | Runs the game's body, which the package keeps beside it as `<Function>__Vanilla`. An RPC's body runs there as a plain function, where the RPC has already arrived. | Yes |
| <a id="replacing-a-game.a-method-the-blueprint-does"></a>a method the Blueprint does not define | Added to it: a helper that the other methods call, or an override of a function it inherits, such as `void ReceiveTick(float DeltaSeconds)`. An override changes this Blueprint and its children only; patch the parent that defines the function to change them all. | Yes |
| <a id="replacing-a-game.void-receivetick-float"></a>`void ReceiveTick(float DeltaSeconds)` added to an actor that never ticks | Also sets `PrimaryActorTick.bCanEverTick` on the default object, as the editor does for a class that gains the event. A widget's added `Tick` or `OnPaint` sets its flag the same way. | Yes |
| <a id="replacing-a-game.a-variable-at-namespace-scope"></a>a variable at namespace scope that a patch's method uses | Kept as any namespace-scope variable is: its class is cooked into the mod's own package. See [Global variables](#global-variables). | Yes |
| <a id="replacing-a-game.a-method-whose-parameters-differ"></a>a method whose parameters differ from the game function's | Refused: "its parameters ... are not the game function's ...; a replacement takes the same ones, in the same order". Copy them from the SDK header: the type, the name and the direction (a Blueprint output is `T &`). | Refused |
| <a id="replacing-a-game.a-method-that-the-sdk"></a>a method that the SDK header declares and the game's package lacks | Refused: "has no function of that name of its own, though its declaration has one: the declaration is not the game's". Regenerate the SDK from the same game build as the extracted pak. | Refused |
| <a id="replacing-a-game.ukismetsystemlibrary-delay-1-0f"></a>`UKismetSystemLibrary::Delay(1.0f);` in a patch's method, or another latent call | Waits, as in a class of your own. The code after the wait runs from an ubergraph the patch adds to the Blueprint, `ExecuteUbergraph_<Patch>`; the Blueprint's own stays the game's. The engine keeps a frame across a wait only for a class's own ubergraph, so what the method keeps across it (its locals, its parameters) are transient variables of the Blueprint, `<Method>_<Name>`. A second call while the first waits shares them, as a Blueprint's event graph does. Checked in DRG: a patched grunt's `ReceiveBeginPlay` waited and resumed with its local intact. | Yes |

```cpp
#include "UeApi/Game/ENE_Spider_Grunt_Normal_C.h"

class SlowGrunt : public ENE_Spider_Grunt_Normal_C {
  UE_PATCH;
  UE_DEFAULTS {
    CustomTimeDilation = 0.5f;            // the Blueprint's class defaults
    HealthComponent->MaxHealth = 180.0f;  // a native component's default subobject
    PrimaryActorTick.bCanEverTick = true; // one member of a struct
  }
  void GetEnemySpawnedCount(int32 &SpawnCount) {
    ENE_Spider_Grunt_Normal_C::GetEnemySpawnedCount(SpawnCount); // the game's body
    SpawnCount = SpawnCount + 1;
  }
};
```

Notes:

- A component whose cooked data lists the properties the engine copies from its template keeps that list valid: an
  edit of a listed property keeps it, and any other edit makes the engine copy the whole template.
- Functions that a timeline or a component event calls by name stay bound to the same functions after a patch
  replaces them.
- A patch cannot add input: see [Timers and input](#timers-and-input).

### Building edits

| You write | What it does | Status |
|---|---|---|
| <a id="building-edits.assetgen-compile-source-ueapi"></a>`assetgen compile <source> <UeApi> <out-dir> --game <extracted pak>/FSD/Content` | Reads each package an edit names from that folder and writes the edited copy at the same path under the `Content` folder the out dir sits in, as for a class in a `Game::` namespace (see [Building mods](GUIDE.md#building-mods)). It prints one line per package: `edit -> <package>.uasset (the game's, edited: ...)`. | Yes |
| <a id="building-edits.game-content-extracted-pak-fsd"></a>`game_content: <extracted pak>/FSD/Content` in `mods.yaml` | bpbuild passes it to every compile as `--game`. An edited package counts as the mod's output, so a newer copy of it in `game_content`, from a new extract after a game update, rebuilds the mod. | Yes |

Notes:

- Extract the pak with UE 4.27's own UnrealPak: `UnrealPak.exe FSD-WindowsNoEditor.pak -Extract <dir>`, then pass
  `<dir>/FSD/Content`. Extract it again after every game update, so the edit starts from the current package.
- The edited package is the whole package, so of two paks that edit the same asset, the game loads only one copy.
  Put every edit of an asset in one mod.
- The compiler reads each edited value and function back before it writes the package. The test suite also reads
  every edited package back whole (`assetgen roundtrip`), checks that only the named values differ and every other
  export is byte-identical, and runs the replaced functions in its offline VM.
  Loaded in the game so far: a data asset's members, a Blueprint's class defaults and a native component's. A component
  template's edit, a path, a set or map delta and a replaced function are checked offline only.
- A Blueprint that derives from a patched one, and an asset that references it, are not rebuilt. Both are expected to
  keep working while the names and types the patch keeps stay the same; this is not checked yet.

## Event dispatchers

An event dispatcher is a list of bindings that its owner calls all at once. This section covers declaring one on a
mod class, binding handlers to it and to the game's own dispatchers, broadcasting it, and passing a method as a single
delegate value. The rule to remember: a binding is an object and a function name, so a handler is a non-inline method
of the object you bind (`this` or another) whose parameters match the dispatcher's exactly. [examples/Scoreboard.cpp](examples/Scoreboard.cpp)
declares, binds and broadcasts one.

### Declaring a dispatcher

| You write | What it does | Status |
|---|---|---|
| <a id="declaring-a-dispatcher.ue-dispatcher-onscored-int32"></a>`UE_DISPATCHER(OnScored, int32 Points, AActor *By);` | The editor's Event Dispatcher (My Blueprint > Event Dispatchers). The class gets the dispatcher OnScored and its signature function, which holds the parameter list. Game Blueprints can bind to it and call it, as with one made in the editor. | Yes |
| <a id="declaring-a-dispatcher.ue-dispatcher-onready"></a>`UE_DISPATCHER(OnReady);` | A dispatcher with no parameters. Its handlers are `void F()`, and `Broadcast()` takes no arguments. | Yes |
| <a id="declaring-a-dispatcher.tmulticastinlinedelegate-void"></a>`TMulticastInlineDelegate<void(int32 Points)> OnHit;` | Refused: "a dispatcher is declared with UE_DISPATCHER". The macro also declares the signature function, which is where the parameter names survive. Use UE_DISPATCHER. | Refused |
| <a id="declaring-a-dispatcher.tmulticastsparsedelegate-void"></a>`TMulticastSparseDelegate<void(int32 Points)> OnHit;` | Refused with the generic "unimplemented property" message. A mod class cannot own a sparse dispatcher. Use UE_DISPATCHER. | Refused |
| <a id="declaring-a-dispatcher.ue-dispatcher"></a>`UE_DISPATCHER(...)` inside a `UE_INTERFACE` | Refused: "an interface cannot declare a UE_DISPATCHER". A dispatcher cannot move to the implementing classes the way an interface variable does. Declare it on each implementing class. | Refused |

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
| <a id="binding-handlers.onscored-add-this-scorer"></a>`OnScored.Add(this, &Scorer::HandleScored);` | Bind Event to OnScored with a Create Event wired in. From then on, every broadcast of OnScored calls HandleScored on this object. | Yes |
| <a id="binding-handlers.ondestroyed-add-this-scorer"></a>`OnDestroyed.Add(this, &Scorer::HandleDestroyed);` | Binds to a game or engine dispatcher of this object or of its component, inline or sparse: OnDestroyed, `Box->OnComponentBeginOverlap`. The handler runs whenever the game broadcasts it. | Yes |
| <a id="binding-handlers.c-onfirepressed-add-this-scorer"></a>`C->OnFirePressed.Add(this, &Scorer::OnFire);` | Bind Event with another object as Target, also through a component: `C->InventoryComponent->OnItemEquipped`. The bound function is still this object's own. | Yes |
| <a id="binding-handlers.onhit-add-this-pbase-basehandle"></a>`OnHit.Add(this, &PBase::BaseHandle);` | In a child of a mod class: Add, Remove and Clear work on a dispatcher the mod parent declares, and a handler the parent defines can be bound. | Yes |
| <a id="binding-handlers.onhit-add-h-helper-take"></a>`OnHit.Add(H, &Helper::Take);` on a dispatcher a mod or game Blueprint parent declares | Binds Take on H, as on a dispatcher of this class. The delegate is typed with the parent's own `OnHit__DelegateSignature`, imported from its package, as the editor's Create Event does on an inherited dispatcher. The class makes no function of that name, which would hide the parent's. | Yes |
| <a id="binding-handlers.add"></a>The same `Add` twice | The handler is bound once. The engine skips a second binding with the same object and function. | Yes |
| <a id="binding-handlers.onscored-remove-this-scorer"></a>`OnScored.Remove(this, &Scorer::HandleScored);` | Unbind Event from OnScored. Removes the binding that has this object and this function name. It works on every dispatcher Add works on. | Yes |
| <a id="binding-handlers.onscored-clear"></a>`OnScored.Clear();` | Unbind all Events from OnScored. Empties the whole list. | Yes |
| <a id="binding-handlers.proxy-oncompleted-add-this"></a>`Proxy->OnCompleted.Add(this, &Scorer::Done);` | An async node with its output pins wired to custom events. Each Add binds one outcome, and one method can serve several. | Yes |
| <a id="binding-handlers.e-eventtriggereddelegate-add"></a>`E->EventTriggeredDelegate.Add(this, &Scorer::Ping);` on a native dispatcher that is not BlueprintAssignable | Refused: "EventTriggeredDelegate is a native dispatcher that is not BlueprintAssignable (AGameEvent::EventTriggeredDelegate)". The editor's Bind, Unbind and Unbind All nodes refuse one ("Event Dispatcher is not 'BlueprintAssignable'"); only the engine's own code binds it. 17 of the game's are; Remove and Clear are refused the same. A UeApi from before genueapi marked them says nothing, and then Add is taken. | Refused |
| <a id="binding-handlers.onhit-isbound"></a>`OnHit.IsBound()`, `OnHit.AddUnique(...)`, `OnHit.Contains(...)` | Refused by clang: "no member named 'IsBound'". A dispatcher has Add, Remove, Clear and Broadcast, and nothing else. Add already skips a duplicate. To know whether your own dispatcher has listeners, count them where you call Add and Remove. | Refused |

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
| <a id="handler-functions.void-handlescored-int32-points"></a>`void HandleScored(int32 Points, AActor *By)` | A handler bound on `this` is a non-inline member function of the class or of a mod ancestor. It returns void and takes exactly the dispatcher's parameter types. clang checks this at the Add call. An inherited native or game Blueprint function bound on `this` follows the rules of one bound on another object below. | Yes |
| <a id="handler-functions.const-fhitresult-hit"></a>`const FHitResult &Hit` where the header says `FHitResult Hit` | Binds: a parameter may be the header's type by value or by `const&`. A native signature's UeApi spelling does not say which the engine's is (the dump keeps no reference flag), and the engine hands the handler the value either way. `TSubclassOf<UObject>` and `UClass*` match the same way, and so do `TEnum<E>` and `E`. | Yes |
| <a id="handler-functions.const"></a>A `const` handler, or a parameter of another type | clang error: "no matching member function for call to 'Add'". Copy the parameter list as the header spells it. | Refused |
| <a id="handler-functions.onhit-add-held"></a>`OnHit.Add(Held);`, `OnHit.Remove(Held);` with `TDelegate<void(int32 Points)> Held` | Adds or removes the delegate value as it is: the editor's Add / Remove node fed a delegate pin. Its parameters match the dispatcher's as a handler's do. | Yes |
| <a id="handler-functions.inline-void-handle-int32-points"></a>`inline void Handle(int32 Points)` as a handler | Refused: "a delegate cannot bind Handle: an inline function is expanded where it is called". An inline method never becomes a function of the class. Drop `inline`. | Refused |
| <a id="handler-functions.onhit-add-h-helper-handlehit"></a>`OnHit.Add(H, &Helper::HandleHit);` | Binds HandleHit on the object H points to: the editor's Create Event with its Object pin wired. The binding is made where the Add runs, with H as it is then. HandleHit must be a function of H's class or of an ancestor of it: a mod class's method, a game Blueprint's function, or a native BlueprintCallable one (`OnRate.Add(Pawn, &AActor::SetActorTickInterval)`). Remove takes the same pair off. | Yes |
| <a id="handler-functions.onhit-add-h-other-handlehit"></a>`OnHit.Add(H, &Other::HandleHit);` where H's class has no HandleHit | Refused: "a delegate on a Helper cannot bind Other::HandleHit: the engine looks it up by name on that object, whose class has no such function". The broadcast would skip it. | Refused |
| <a id="handler-functions.ontick-add-a-aactor-receivetick"></a>`OnTick.Add(A, &AActor::ReceiveTick);` | Refused: "a delegate cannot bind AActor::ReceiveTick on another object: the editor binds a BlueprintCallable function that is not pure or latent, and ReceiveTick is not BlueprintCallable". An event the engine calls is not one Blueprint code can bind, on another object or on `this` ("on this object"), nor a pure or latent function. A mod method that overrides such an event is refused the same. | Refused |
| <a id="handler-functions.onuse-add-item-aitem-server"></a>`OnUse.Add(Item, &AItem::Server_StartUsing);`, `OnPing.Add(this, &AActor::OnRep_Instigator);` | Refused the same way: "... and Server_StartUsing is not BlueprintCallable". A native function the engine does not mark BlueprintCallable, such as an RPC or a RepNotify, is not one the editor binds. UeApi's `NotCallable.json` names them. | Refused |
| <a id="handler-functions.onping-add-pc-aplayercontroller"></a>`OnPing.Add(PC, &APlayerController::ClientClearCameraLensEffects);` | Binds it: an RPC the engine marks BlueprintCallable (49 are, `Server_ResetHUD` among them) is one the editor binds, on another object or on `this`. Being an RPC says nothing either way. | Yes |
| <a id="handler-functions.onhit-add-h-helper-half"></a>`OnHit.Add(H, &Helper::Half);` where Helper declares Half and never defines it | Refused: "a delegate cannot bind Helper::Half, which Helper declares and never defines". No function of that name exists. | Refused |

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
| <a id="broadcast.onscored-broadcast-n-this"></a>`OnScored.Broadcast(N, this);` | Call OnScored. Every bound handler runs right away, one after another, before Broadcast returns. | Yes |
| <a id="broadcast.getpeer-onpeerhit-broadcast-x"></a>`GetPeer()->OnPeerHit.Broadcast(X);` | Fires the dispatcher of another object of the same class. The object is evaluated before the arguments, so an argument that changes what GetPeer() returns does not redirect the broadcast. | Yes |
| <a id="broadcast.ue-dispatcher-onlist-tarray"></a>`UE_DISPATCHER(OnList, TArray<int32> &Items);` | A non-const reference parameter. The declaration compiles, but a Broadcast is refused: "Items is a non-const reference, which a Broadcast never writes back to the caller". The engine copies each argument into a parameter block of its own. Take it by value or by `const &`. | Refused |
| <a id="broadcast.onhit-broadcast-1"></a>`OnHit.Broadcast(1);` on a dispatcher a mod parent declares | Broadcasts through the parent's signature function, as the editor's node on a child does. | Yes |
| <a id="broadcast.t-onhit-broadcast-p"></a>`T->OnHit.Broadcast(P);`, `Burrow->OnBurrowComplete.Broadcast(true);` | The Call node with another object as Target: a dispatcher another mod class declares, or a game Blueprint's (every Blueprint dispatcher is BlueprintCallable). It broadcasts through that class's signature function, imported from its package. | Yes |
| <a id="broadcast.state-onterraingenerated"></a>`State->OnTerrainGenerated.Broadcast();` | A native dispatcher the engine marks BlueprintCallable (41 of the game's). UeApi does not say which signature function the engine gives it, so the broadcast names one of the class's own with the same parameters and warns that it does; the handlers get the same arguments. | Warns |
| <a id="broadcast.ondestroyed-broadcast-this"></a>`OnDestroyed.Broadcast(this);` | Refused: "OnDestroyed is a native dispatcher (AActor::OnDestroyed) that is not BlueprintCallable". The editor's Call node refuses a native dispatcher without BlueprintCallable ("Event Dispatcher is not 'BlueprintCallable'"), and none of the engine's has it. The message names the opt-in, `UE_FORCE_BROADCAST` (next row). With a UeApi from before genueapi marked them, every native dispatcher is refused this way, the message saying the UeApi does not tell. | Refused |
| <a id="broadcast.ue-force-broadcast-ondestroyed"></a>`UE_FORCE_BROADCAST(OnDestroyed).Broadcast(this);`, `UE_FORCE_BROADCAST(Spawner->OnEnemySpawned).Broadcast(P, D);` | Broadcasts a native dispatcher that is not BlueprintCallable anyway, and warns: "UE_FORCE_BROADCAST(OnDestroyed).Broadcast: AActor::OnDestroyed is not BlueprintCallable, so the editor's Call node would refuse it". The flag is the editor's rule only: the VM's broadcast does not check it, so every bound handler runs with the arguments, as when the engine broadcasts it. Inline and sparse dispatchers both work. Only Broadcast takes the marker; `UE_FORCE_BROADCAST(D).Add(...)` is refused. | Warns |

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
| <a id="delegate-values.this-scorer-handletimer"></a>`{this, &Scorer::HandleTimer}` | Create Event wired into a delegate pin, written as the argument of a call that takes a TDelegate (timers, engine callbacks). The rules are those of a handler: a non-inline method, and the signature of the `TDelegate<...>` in the header. | Yes |
| <a id="delegate-values.peer-scorer-handletimer"></a>`{Peer, &Scorer::HandleTimer}` | The same with the Object pin wired: HandleTimer bound on Peer, which the timer then calls. The rules are those of a handler on another object. | Yes |
| <a id="delegate-values.tdelegate-void-this-scorer"></a>`TDelegate<void()>(this, &Scorer::HandleTimer)` | The same value, spelled out. | Yes |
| <a id="delegate-values.k2-cleartimerdelegate"></a>`K2_ClearTimerDelegate({})` | Refused: "a delegate value is {this, &Class::Function}". There is no empty or unbound delegate. | Refused |
| <a id="delegate-values.d"></a>`.D = {}` in an asset's braces, `{}` as a map's value in a default (`TMap<FString, TDelegate<void()>> M = {{"A", {}}};`) | An unbound delegate, the one value a default holds (`D = {this, &C::F}` in UE_DEFAULTS is refused: a default is known when the mod is built). Written as the engine writes one, a null object and the name None; an asset's tags name no signature function. | Yes |
| <a id="delegate-values.tdelegate-void-callback"></a>`TDelegate<void()> Callback;` as a class variable, a function parameter or a local | A delegate variable (DelegateProperty). `Callback = {this, &Class::Method};` stores a value, and `Callback` passes it on, to a timer or another function. Its signature function is one the class makes per delegate type, as the editor makes one per dispatcher. One C++ type is one however it is spelled (`TDelegate<void(int32)>` and `TDelegate<void(int)>`, an enum named short or with its namespace), and two types are two, a reference and a const reference among them (`void(FVector&)`, `void(const FVector&)`). It is named `<Name>__DelegateSignature` after what needs it first: a variable or parameter of the class, the function a value binds, or the class's parameter or variable through which a class compiled with it, and generated before it - a subclass or not -, overrides a function, hands it a value or stores one into it (`Delegate__DelegateSignature` when that is a local the compiler made up). The name is numbered past a name the class, a mod or game Blueprint parent, or a mod child compiled with it already uses (not past a native parent's own delegate signatures, such as UWidget's `GetText__DelegateSignature`, which UeApi does not list). A struct makes none, so a delegate member of one is refused; a mod interface's TDelegate variable is its implementer's property. | Yes |
| <a id="delegate-values.void-use-tdelegate-void-d"></a>`void Use(TDelegate<void()> D) override` of a mod parent's `Use`; `Take({Peer, &Scorer::Ping})` with Take a mod parent's or a sibling class's of this mod; `ParentVar = {Peer, &Scorer::Ping}`, `Other->Var = {Peer, &Scorer::Ping}`, `ParentArr[0] = {Peer, &Scorer::Ping}`, `Other->Map[K] = {Peer, &Scorer::Ping}` | The override's parameter names the signature function of the class that declares `Use` first, imported from its package, as the editor's override copies its parent's parameters, even where the override spells the type another way (`int` for `int32`, an enum with or without its namespace); a static hiding a parent's static does the same. A value bound on another object is typed with the signature of the parameter or variable it goes to, as a Create Event wired to that pin is, on this object or another, and one stored into an element of such a variable's TArray or a value of its TMap with the element's; a comma value `(Bump(), D)` handed to such a parameter is held in a local of its type. `ParentArr.Add({Peer, &Scorer::Ping})` and the other container functions still type the value with a signature of this class's own, which runs the same. A call whose body is copied in (`Parent::Take(...)`, a static of a class this source cooks) has no parameter to wire, and the value keeps a signature of this class's own. Only classes the same compile cooks: one another mod cooks (UE_CLASS / UE_CLASS_IN) keeps a signature of this class's own, which runs the same, since its name there is not known here. | Yes |

The timers in [Timers and input](#timers-and-input) show delegate values in use.

## Timers and input

A timer calls a method of `this` after a delay or at a fixed rate. It is an engine call that takes a delegate value.
A key or an input action calls a method marked `UE_INPUT_KEY` or `UE_INPUT_ACTION`, the editor's InputKey and InputAction
events, once the actor receives input; a mod can also poll the player controller, or bind the game's own action
dispatchers. The rule to remember: the engine keeps at most one timer per object and method, so setting a timer again
restarts it. [examples/Beacon.cpp](examples/Beacon.cpp) runs a looping timer, and
[examples/HotKeys.cpp](examples/HotKeys.cpp) reacts to keys.

### Timers

| You write | What it does | Status |
|---|---|---|
| <a id="timers.k2-settimerdelegate-this-pulser"></a>`K2_SetTimerDelegate({this, &Pulser::OnPulse}, 0.5f, true, 0.0f, 0.0f)` | Set Timer by Event. The third argument is Looping: `true` calls OnPulse every 0.5 s until the timer is cleared, `false` calls it once. The last two are the initial start delay and its variance. It returns the timer's handle. | Yes |
| <a id="timers.ftimerhandle-pulse"></a>`FTimerHandle Pulse;` | The return value promoted to a variable. An FTimerHandle member is an ordinary struct variable. | Yes |
| <a id="timers.k2-clearandinvalidatetimerhandle"></a>`K2_ClearAndInvalidateTimerHandle(Pulse)` | Clear and Invalidate Timer by Handle. Stops the timer and resets Pulse to an invalid handle, because it takes the handle by reference. | Yes |
| <a id="timers.k2-cleartimerhandle-pulse"></a>`K2_ClearTimerHandle(Pulse)` | Clear Timer by Handle. Stops the timer, but takes the handle by value, so Pulse keeps its old value. | Yes |
| <a id="timers.k2-cleartimerdelegate-this"></a>`K2_ClearTimerDelegate({this, &Pulser::OnPulse})` | Clear Timer by Event. Stops the timer without a handle. | Yes |
| <a id="timers.k2-pausetimerhandle-pulse"></a>`K2_PauseTimerHandle(Pulse)`, `K2_UnPauseTimerHandle(Pulse)` | Pause and resume the timer. AssetGen fills the world context in as self. | Yes |
| <a id="timers.k2-istimeractivehandle-pulse"></a>`K2_IsTimerActiveHandle(Pulse)`, `K2_GetTimerRemainingTimeHandle(Pulse)`, `K2_IsValidTimerHandle(Pulse)` | Ask about the timer. The first two get self as world context; K2_IsValidTimerHandle takes only the handle. | Yes |
| <a id="timers.k2-settimer-this-poll-1"></a>`K2_SetTimer(this, "Poll", 1.0f, true, 0.0f, 0.0f)` | Set Timer by Function Name. AssetGen passes the name as a plain string. The engine looks the method up by that name at run time. With `this` and a literal name, AssetGen checks the method the way the engine will (see below). | Yes |
| <a id="timers.k2-cleartimer-this-poll"></a>`K2_ClearTimer(this, "Poll")` | Clear Timer by Function Name. K2_PauseTimer, K2_UnPauseTimer, K2_IsTimerActive, K2_GetTimerRemainingTime and the other by-name calls find the timer by the same name. | Yes |
| <a id="timers.ufsdwidgetblueprintlibrary"></a>`UFSDWidgetBlueprintLibrary::SetTimerForNextTick({this, &Pulser::Later})` | DRG's own next-frame timer. It takes a delegate value, returns a handle, and gets self as world context. | Yes |

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
  takes no parameters, and AssetGen refuses an inline method. With a name, the engine finds these mistakes only at run
  time. An inline method never becomes a function of the class, and a misspelled name matches nothing, so the engine
  logs "SetTimer passed a bad function (...) or object (...)" and returns an invalid handle. A method that takes
  parameters or returns a value is found, but the engine refuses it with "SetTimer passed a function (...) that
  expects parameters.": its test is the function's parameter size, which counts the return value. In each case no
  timer is set. AssetGen refuses all four when the object is `this` and the name a literal; any other by-name call
  compiles unchecked.
- SetTimerForNextTick is DRG's function, and only compiling it was checked. Going by its name and by the engine's
  FTimerManager function of the same name, it calls the method once on the next frame. The engine libraries in the
  SDK have no next-tick timer of their own.

### Timer, tick or Delay

| You write | What it does | Status |
|---|---|---|
| <a id="timer-tick-or.void-receivetick-float"></a>`void ReceiveTick(float DeltaSeconds)` | Work that must run every frame. Overriding ReceiveTick turns ticking on for the actor. | Yes |
| <a id="timer-tick-or.ukismetsystemlibrary-delay-0-5f"></a>`UKismetSystemLibrary::Delay(0.5f);` in a loop | Waits for something, then carries on in the same method with its locals intact. Waiting in ReceiveBeginPlay for the local player is the typical case. See [Latent calls](#latent-calls). | Yes |
| <a id="timer-tick-or.k2-settimerdelegate"></a>`K2_SetTimerDelegate(...)` | Calls a separate method at a fixed rate, or once after a delay, when the call must be paused, queried or stopped from elsewhere through its handle. | Yes |

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
| <a id="reacting-to-input.ue-input-key-f5-void"></a>`UE_INPUT_KEY("F5") void Toggle();` | An InputKey event: the engine calls Toggle each time F5 goes down while the actor receives input. The method returns void and takes nothing, or `FKey Key`, the key that fired it. | Yes |
| <a id="reacting-to-input.ue-input-key-f5-released"></a>`UE_INPUT_KEY("F5", Released)`, `UE_INPUT_KEY("F5", Pressed, Released)` | The key coming up; both edges, a call for each. Pressed alone is the default. These are the node's Pressed and Released pins. | Yes |
| <a id="reacting-to-input.ue-input-key-s-ctrl"></a>`UE_INPUT_KEY("S", Ctrl, Shift)` | A chord: the node's Shift, Ctrl, Alt and Cmd tick boxes. The modifiers named must be held. | Yes |
| <a id="reacting-to-input.ue-input-key-space-void"></a>`UE_INPUT_KEY("Space") void Jump();` | Refused: "names no key the engine has, so it would never fire: the closest is SpaceBar". The engine binds a key by its name and says nothing of a name it has no key of. | Refused |
| <a id="reacting-to-input.ue-input-action-use-void"></a>`UE_INPUT_ACTION("Use") void Used(FKey Key);` | An InputAction event: the game's Use action, whichever key the player has mapped to it. It takes the same edges and options as a key, but no modifiers. | Yes |
| <a id="reacting-to-input.noconsume"></a>`NoConsume`, `WhenPaused`, `KeepParent` | Options, written after the key or action like an edge: the node's Consume Input unticked, Execute when Paused ticked, Override Parent Binding unticked. | Yes |
| <a id="reacting-to-input.ue-defaults-autoreceiveinput"></a>`UE_DEFAULTS { AutoReceiveInput = EAutoReceiveInput::Player0; }` | Auto Receive Input in the class defaults: the actor enables input for the first player as it is spawned. Without it, or a call to `EnableInput(PC)`, an actor's input events never fire. | Yes |
| <a id="reacting-to-input.pc-wasinputkeyjustpressed"></a>`PC->WasInputKeyJustPressed(ToggleKey)` in ReceiveTick | Polls the local player controller. True during the one input frame in which the key went down, so check it every tick. | Yes |
| <a id="reacting-to-input.pc-isinputkeydown-k"></a>`PC->IsInputKeyDown(K)`, `PC->WasInputKeyJustReleased(K)`, `PC->GetInputKeyTimeDown(K)` | True while the key is held; true during the frame in which it comes up; how long it has been held. | Yes |
| <a id="reacting-to-input.fkey-f5"></a>`FKey{"F5"}`, `FKey{FName("F5")}` | A key value, named by the engine's key names. It works as an argument, a local, a class variable and a `static inline const` member. | Yes |
| <a id="reacting-to-input.fkey-space"></a>`FKey{"Space"}` | Compiles, with a warning naming the closest key, SpaceBar: no input matches it. It is not refused, as code may compare it with a key the engine makes up as it runs. | Warns |
| <a id="reacting-to-input.fkey-f5-2"></a>`FKey("F5")`, `FKey(FName("F5"))`, `FKey K{FName("F5")};`, `FKey K = {"F5"};` | The same key as `FKey{"F5"}`, and the same warning for a name the engine has no key of. So is any one-member struct's constructor in parentheses, `FFrameNumber(5)`. | Yes |
| <a id="reacting-to-input.fkey-name"></a>`FKey(Name)`, `FKey(FName(S))`, `FKey(S)` with a run-time FName or FString | The key of that name, made when the line runs: a string is made a name first (Conv_StringToName), as FKey's own constructor takes one. No warning can be given for a name not known when the mod is built. | Yes |
| <a id="reacting-to-input.pc-wasinputkeyjustpressed-f5"></a>`PC->WasInputKeyJustPressed("F5")` | clang error: "no viable conversion from 'const char[3]' to 'FKey'". | Refused |
| <a id="reacting-to-input.c-onfirepressed-add-this-toggler"></a>`C->OnFirePressed.Add(this, &Toggler::OnFire)` | For the player's own actions, bind the game's dispatchers on APlayerCharacter: OnFirePressed, OnFireReleased, OnJumpPressed, OnSecondaryFirePressed, OnLaserPointerPressed, OnTerrainScannerPressed and others. They fire for whatever key the player has mapped to that action. | Yes |
| <a id="reacting-to-input.inputcomponent-bindkey"></a>`InputComponent->BindKey(...)`, `BindAction(...)` | clang error: "no member named 'BindKey' in 'UInputComponent'". They are native C++ only and absent from the SDK; mark a method instead. | Refused |

```cpp
class Hotkey : public AActor {
  UE_DEFAULTS { AutoReceiveInput = EAutoReceiveInput::Player0; }
public:
  bool bOn = false;
  FName LastUsed;
  UE_INPUT_KEY("F5") void Toggle() { bOn = !bOn; }
  UE_INPUT_KEY("F5", Ctrl) void Off() { bOn = false; }
  UE_INPUT_ACTION("Use", NoConsume) void Used(FKey Key) { LastUsed = Key.KeyName; }
};

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

- A marked method is the event itself. AssetGen writes what the editor writes for the node, an entry of the class's
  InputKeyDelegateBinding or InputActionDelegateBinding object naming the method, and the engine binds it when the
  actor's input is enabled and calls the method by name. So a subclass's override of a marked method is what a key
  calls on the subclass, and a subclass need not repeat its parent's markers: the engine binds each class's events up
  the chain.
- Only an actor gets input. A possessed pawn and a player controller bind their events by themselves; any other actor
  binds them when it enables input, through Auto Receive Input in its defaults or `EnableInput(PC)` from code.
  `DisableInput(PC)` stops them. A marker on a class that is no actor is refused.
- The method returns void and takes nothing, or the key as `FKey Key` or `const FKey& Key`. Anything else is refused:
  the engine hands the method the key's bytes as its parameters, so a parameter of another type, or a result, would
  read them as its own. A static or inline method is refused too: neither is a function of the object to call.
- An actor that enables input gets it first, before the player controller and the pawn, and a binding consumes its
  key by default: the game's own use of F5 stops while the actor has input, and an action consumes the keys mapped to
  it. Write NoConsume to leave the input to the game as well.
- A chord masks the plain key: with both `UE_INPUT_KEY("F5")` and `UE_INPUT_KEY("F5", Ctrl)`, Ctrl+F5 calls only the
  Ctrl one. Without modifiers, a key fires whatever modifiers are held.
- Override Parent Binding, on unless KeepParent says otherwise, drops the bindings of the same key or action and edge
  made on the actor's input component before this class's: a parent class's, and on a pawn its native ones, which the
  pawn binds first (Pawn.cpp 378-386).
- Name keys by the engine's names: "F5", "LeftShift", "SpaceBar", "LeftMouseButton", "A", "Gamepad_FaceButton_Bottom".
  The digit keys are "Zero", "One", "Two" and so on, not "1", and the backtick is "Tilde". Case does not matter, as
  it does not to the engine. A key a keyboard layout has no name for is named by its character, such as "+" or "æ".
  The names are UE 4.27's and those of the input plugins the game ships: keyboard, mouse, gamepad, touch, Oculus and
  SteamVR controllers, and FSDRawInput's GenericUSBController keys. A name the engine has no key of is refused on a
  marker, with the closest it has, and an `FKey{...}` of one warns the same way.
- An action is one of the game's input settings, which AssetGen cannot read, so its name is not checked: one the game
  does not have compiles, and never fires.
- A class variable such as ToggleKey is written into the class defaults. `FKey ToggleKey = {"F5"};` is refused with
  the default-value message; write `FKey{"F5"}`.
- Other one-argument struct constructors in parentheses are refused the same way: `FFrameNumber(5)` and
  `FBoneReference(FName("hand_r"))`. The braced forms `FFrameNumber{5}` and `FBoneReference{"hand_r"}` compile. See
  [Structs](#structs).
- The key state read is the player controller's. A key that an open menu or text box takes may never reach it.

## Timelines

A timeline plays curves over time, as the editor's Timeline node does. While it plays, the engine writes each track's
value at the current time into a variable of the actor every frame, then calls the timeline's update method. It calls
the finished method at the end, and an event track's method each time the timeline passes one of its times. The rule
to remember: a timeline is a component the engine makes as the actor spawns, so start it from code or with AutoPlay,
and read its tracks' variables in the update method. [examples/FadingLamp.cpp](examples/FadingLamp.cpp) fades and
lifts a light.

### Declaring a timeline

| You write | What it does | Status |
|---|---|---|
| <a id="declaring-a-timeline.ue-timeline-fade-2-0f"></a>`UE_TIMELINE(Fade, 2.0f);` | A timeline 2 seconds long: the Timeline node's template. `Fade` is a `UTimelineComponent *` variable. The engine makes the component as the actor spawns, before the construction script and BeginPlay, and stores it there. | Yes |
| <a id="declaring-a-timeline.ue-timeline-fade-2-0f-2"></a>`UE_TIMELINE(Fade, 2.0f, AutoPlay, Loop);` | Options after the length, the template's tick boxes: AutoPlay (plays from the start as the actor spawns), Loop, Replicated (the component replicates its play state), IgnoreTimeDilation, and LastKeyframe (the length is the time of the last key of any track, the editor's Use Last Keyframe). | Yes |
| <a id="declaring-a-timeline.ue-float-track-fade-alpha"></a>`UE_FLOAT_TRACK(Fade, Alpha, (0, 0), (1, 1, Cubic), (2, 0));` | A float track of Fade and its curve. Each key is `(time, value)`, interpolated Linear to the next key unless Cubic or Constant follows the value, as the curve editor's key menu sets it. A Cubic key's tangents are the editor's Auto ones. The macro declares `float Alpha`, which the timeline sets every frame it plays. | Yes |
| <a id="declaring-a-timeline.ue-vector-track-fade-lift"></a>`UE_VECTOR_TRACK(Fade, Lift, (0, 0, 0, 0), (2, 0, 0, 100));` | A vector track: each key is `(time, x, y, z)`. Declares `FVector Lift`. | Yes |
| <a id="declaring-a-timeline.ue-color-track-fade-tint"></a>`UE_COLOR_TRACK(Fade, Tint, (0, 1, 1, 1, 1), (2, 1, 0, 0, 1));` | A linear color track: each key is `(time, r, g, b, a)`. Declares `FLinearColor Tint`. | Yes |
| <a id="declaring-a-timeline.ue-timeline-update-fade-void"></a>`UE_TIMELINE_UPDATE(Fade) void OnFade();` | The node's Update pin: the engine calls OnFade every frame the timeline plays, after it has written the tracks' variables, and when SetNewTime moves it. | Yes |
| <a id="declaring-a-timeline.ue-timeline-finished-fade-void"></a>`UE_TIMELINE_FINISHED(Fade) void OnFaded();` | The Finished pin: called when the timeline reaches its end playing forward, or its start playing in reverse. A looping timeline never finishes. | Yes |
| <a id="declaring-a-timeline.ue-timeline-event-fade-0"></a>`UE_TIMELINE_EVENT(Fade, 0.5, 1.5) void Blink();` | An event track: the engine calls Blink each time the timeline passes 0.5 s and 1.5 s, forward or in reverse. | Yes |
| <a id="declaring-a-timeline.ue-float-track-curve-fade"></a>`UE_FLOAT_TRACK_CURVE(Fade, Alpha, UeAssets::UCurveFloat::Game::LevelElements::Blueprints::Hologram_FadeIn);` | A track over a curve asset, the editor's External Curve: the timeline plays the asset's keys as the game has them. The curve is any UE_ASSET_AT of a curve, so the game's (the UeAssets headers name them all) or another mod's, or a curve of this mod's own. Declares `float Alpha`, as UE_FLOAT_TRACK does. | Yes |
| <a id="declaring-a-timeline.ue-vector-track-curve-fade"></a>`UE_VECTOR_TRACK_CURVE(Fade, Lift, Path);`, `UE_COLOR_TRACK_CURVE(Fade, Tint, Glow);` | A vector track over a UCurveVector, a color track over a UCurveLinearColor. | Yes |
| <a id="declaring-a-timeline.ue-float-curve-ramp-0"></a>`UE_FLOAT_CURVE(Ramp, (0, 0), (1, 1, Cubic));` at namespace scope | A curve asset of the mod's own, as the Content Browser's Miscellaneous > Curve makes one: a UCurveFloat cooked at `<mod package>/Ramp`, its keys written as a track's are. `UE_VECTOR_CURVE` and `UE_COLOR_CURVE` make a UCurveVector and a UCurveLinearColor. `&Ramp` points at it in a track, a default or code, and another mod names it with UE_ASSET_AT. In a namespace, it goes to that folder. | Yes |
| <a id="declaring-a-timeline.ue-float-track-curve-fade-2"></a>`UE_FLOAT_TRACK_CURVE(Fade, Alpha, Path)` where Path is a UCurveVector | Refused: "a UCurveVector: the track's float is played from a UCurveFloat". | Refused |

```cpp
#include "UeAssets/UCurveFloat.h"
UE_FLOAT_CURVE(Ramp, (0, 0), (1, 1, Cubic), (2, 0));   // a curve asset of the mod's own

class FadingLamp : public AActor {
public:
  UE_COMPONENT(UPointLightComponent, Lamp);
  UE_TIMELINE(Fade, 2.0f);
  UE_FLOAT_TRACK(Fade, Brightness, (0, 0), (1, 5000, Cubic), (2, 0));
  UE_VECTOR_TRACK(Fade, Lift, (0, 0, 0, 0), (2, 0, 0, 100));
  int32 Peaks = 0, Runs = 0;

  UE_TIMELINE_UPDATE(Fade) void OnFade() {
    FHitResult Hit;
    Lamp->SetIntensity(Brightness);
    Lamp->K2_SetRelativeLocation(Lift, false, Hit, false);
  }
  UE_TIMELINE_FINISHED(Fade) void OnFaded() {   // three runs, then it stays dark
    Runs = Runs + 1;
    if (Runs < 3) Fade->PlayFromStart();
  }
  UE_TIMELINE_EVENT(Fade, 1.0) void Peak() { Peaks = Peaks + 1; }
  UE_TIMELINE(Glow, 2.0f);                      // two tracks over curve assets
  UE_FLOAT_TRACK_CURVE(Glow, Fog, UeAssets::UCurveFloat::Game::LevelElements::Blueprints::Hologram_FadeIn);
  UE_FLOAT_TRACK_CURVE(Glow, Pulse, Ramp);
  void ReceiveBeginPlay() { Fade->PlayFromStart(); }
};
```

### Playing a timeline

The node's input pins are functions of the component, called on the timeline's variable.

| You write | What it does | Status |
|---|---|---|
| <a id="playing-a-timeline.fade-play"></a>`Fade->Play()` | Play: plays forward from where the timeline is. | Yes |
| <a id="playing-a-timeline.fade-playfromstart"></a>`Fade->PlayFromStart()` | Play from Start: back to time 0, then forward. | Yes |
| <a id="playing-a-timeline.fade-reverse"></a>`Fade->Reverse()`, `Fade->ReverseFromEnd()` | Reverse from where it is; or from the end. | Yes |
| <a id="playing-a-timeline.fade-stop"></a>`Fade->Stop()` | Stop: halts it where it is. Play goes on from there. | Yes |
| <a id="playing-a-timeline.fade-setnewtime-1-0f"></a>`Fade->SetNewTime(1.0f)` | Set New Time: moves it to 1 s without passing the event tracks' times, and calls the update method. | Yes |
| <a id="playing-a-timeline.fade-setplayrate-2-0f"></a>`Fade->SetPlayRate(2.0f)`, `Fade->SetLooping(true)`, `Fade->SetTimelineLength(3.0f)` | The component's settings, changed as it runs. | Yes |
| <a id="playing-a-timeline.fade-getplaybackposition"></a>`Fade->GetPlaybackPosition()`, `Fade->IsPlaying()`, `Fade->IsReversing()` | Where it is and how it plays. IsReversing is the node's Direction output: true while it plays backward. | Yes |

Notes:

- AssetGen writes what the editor compiles the node into: a TimelineTemplate in the class's Timelines list, a curve
  object per track (CurveFloat, CurveVector or CurveLinearColor, a CurveFloat of event keys for an event track), the
  timeline's variable and one per track, and the direction variable the engine writes as it plays
  (`Fade__Direction_<guid>`). The engine makes the component from the template as each actor spawns
  (UBlueprintGeneratedClass::CreateTimelineComponent) and binds the marked methods by name.
- The editor makes a stub event function for each pin that jumps into the event graph. A marked method is the event
  itself, so a subclass's override of it is what the timeline calls on the subclass. A pin with no method marked calls
  nothing.
- The marked methods return void and take nothing: the engine calls them through a delegate with no parameters. One
  method may serve several timelines and event tracks.
- Only an actor has timelines, and only a class of the mod's own: the engine makes them from the actor's class while
  it spawns. A subclass gets its parent's timelines as well, and may declare its own.
- The track's variable is an ordinary variable between frames: code may read it anywhere, and the timeline overwrites
  what code writes to it the next frame it plays.
- The length is where the timeline stops, or wraps when it loops, whatever the keys say. A key past the end is never
  reached, and a track holds its last key's value after it. Keys are sorted by time, as the editor sorts them.
- A key's time and values are numbers, written as C++ literals: `1`, `0.5`, `-2.5f`. AssetGen reads them itself, so
  a constant's name or an expression is refused.
- A track over a curve asset names it, as the editor's External Curve does: the class holds no curve of its own for
  it, and the asset loads with the class. Its keys are the asset's, so a game curve plays what the game has. The
  timeline's length is still its own (or its last key's, with LastKeyframe), not the curve's. Such a track and a curve
  of the mod's own have been loaded in DRG, each external track holding its asset.
- `Fade->SetFloatCurve(Curve, "Alpha")` hands a track another curve at run time, the track named like its variable.

## Interfaces

An interface is a set of functions that unrelated classes implement. This section covers holding an object through an
interface (TScriptInterface), implementing the game's interfaces, and declaring the mod's own with UE_INTERFACE. The
rule to remember: an implementation is matched to the interface function by name only, so copy the interface's
signature exactly and never mark an implementation `inline`. [examples/Scoreboard.cpp](examples/Scoreboard.cpp)
declares and implements one.

### Interface values

| You write | What it does | Status |
|---|---|---|
| <a id="interface-values.tscriptinterface-iaimable"></a>`TScriptInterface<IAimable> Current;` | An interface-typed variable, parameter or return value (InterfaceProperty: the object plus the interface pointer, 16 bytes). The interface can be the game's (a /Script interface such as IHealth), a game Blueprint Interface (through its `UeApi/Game/` header), or one the mod declares. | Yes |
| <a id="interface-values.tscriptinterface-iaimable-seen"></a>`TScriptInterface<IAimable> Seen = Other;` | Cast To IAimable. If the object's class does not implement the interface, or the object is null, the result is empty, so testing it is the cast's success check. | Yes |
| <a id="interface-values.tscriptinterface-iaimable-target"></a>`TScriptInterface<IAimable> Target = Mark;` | Converts one interface value to another. The result is empty if the object behind it does not implement the target interface. Converting to the same interface is a plain copy. | Yes |
| <a id="interface-values.health-getobject"></a>`Health.GetObject()` | The object behind the interface. GetObject() is the only method a TScriptInterface has. | Yes |
| <a id="interface-values.current-getpriority"></a>`Current->GetPriority()` | Calls the interface function on the object behind it, with the interface value as Target. The implementation is whatever that object has under that name. | Yes |
| <a id="interface-values.tscriptinterface-itargetable"></a>`TScriptInterface<ITargetable>(Player)->GetTargetHealthComponent()` | Calls the interface function on an object directly, as the editor does when the object's class implements the interface: InterfaceContext of the object, with no Cast To in between. The way to call a game interface's function on a game object, since UeApi does not record which interfaces a game class implements. | Yes |
| <a id="interface-values.if-seen"></a>`if (Seen)`, `!Seen`, `Seen == nullptr`, `Seen != nullptr`, `Seen ? A : B` | Whether an object is behind the interface. | Yes |
| <a id="interface-values.health-nullptr"></a>`Health = nullptr;`, `TakeIface(nullptr)` | The null interface. It clears both halves of the value. | Yes |
| <a id="interface-values.cast-iaimable-other"></a>`Cast<IAimable>(Other)` | Refused for a mod interface: "Cast<> to an unknown class". Write `TScriptInterface<IAimable> I = Other;` instead. | Refused |

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
| <a id="implementing-an.class-singer-public-aactor"></a>`class Singer : public AActor, public ICurveSourceInterface` | Class Settings > Implemented Interfaces. The first base is the UE parent, and every base after it is an implemented interface. A class can implement several, game and mod interfaces alike. | Yes |
| <a id="implementing-an.float-getcurvevalue-fname"></a>`float GetCurveValue(FName CurveName) const { return 0.5f; }` | An implementation is an ordinary method, matched to the interface function by name. Only functions the interface marks BlueprintNativeEvent or BlueprintImplementableEvent can be implemented; AssetGen checks this against the SDK's Events.json. | Yes |
| <a id="implementing-an.a-function-left-out"></a>A function left out | Gets an empty stub, as the editor compiles one. The stub returns the zero value (0, false, None, null) and leaves out-parameters unchanged. | Yes |
| <a id="implementing-an.a-function-left-out-that"></a>A function left out that a parent this source cooks already has, without listing the interface | The parent's function implements it, as in C++: AssetGen adds an override of it that only calls the parent's (or expands it, if inline), the editor's override with a parent call, so calls through the interface and by name run the parent's body. An empty stub would replace it for every caller, and with no function at all a call would find the interface's own empty one first. | Yes |
| <a id="implementing-an.the-same-where-the-parent"></a>The same, where the parent's function has a parameter with no name | The added override names it (`P0`, `P1`, ... by position) and passes it on, as the editor's override names every pin. | Yes |
| <a id="implementing-an.the-same-where-the-parent-2"></a>The same, where the parent's function is a multicast | Refused: "... no override can call it for ITrigger, and an empty one would replace it; declare Pistol::Pull". An override would send a multicast twice on a server. | Refused |
| <a id="implementing-an.final"></a>The same, where the parent's function is `final`, or has another signature than the interface's | Refused: "Pistol implements ITrigger, whose Pull needs a function of that name in Pistol, and the Rifle::Pull it inherits is final ..." or "... whose Pull is int32 (int32), and the Rifle::Pull it inherits is float (float): a Blueprint class has one function of a name ...". No function of a final one's name may follow it, and callers through the interface pass the interface's parameters. Rename one of them. | Refused |
| <a id="implementing-an.int32-pull-int32-n"></a>`int32 Pull(int32 N)` implementing ITrigger, where a parent has `float Pull(float)` | Refused: "Pistol::Pull implements ITrigger::Pull, int32 (int32), and replaces the Rifle::Pull it inherits, float (float) ...". A Blueprint function overrides the parent's of its name too, so it would have two signatures. The parent may be another mod's class, from the header it shares; the same goes for ITrigger's stub when the class leaves `Pull` out. Rename one of them. | Refused |
| <a id="implementing-an.pull"></a>The same, where the parent has no `Pull` but implements another interface, IAim, whose `Pull` is `float Pull(float)`, and leaves it out; or the class leaves ITrigger's `Pull` out too | Refused: "Pistol::Pull implements ITrigger::Pull, int32 (int32), and replaces the Pull of IAim, float (float), that Rifle implements ..." or "Pistol implements ITrigger, whose Pull is int32 (int32), and replaces the Pull of IAim ...". The parent's stub for IAim is a function of that name, and the editor refuses an override of another signature ("Cannot override ... declared in a parent with a different signature"). Rename one of the interface functions. | Refused |
| <a id="implementing-an.pull-2"></a>The same, where the parent's function is static, or the class declares its own `Pull` beside the parent's static | Refused: "... is static: the editor takes such a function for an override of the static and refuses it ...; rename Rifle::Pull". A static implements no interface function, but the editor makes any function of its name in a subclass an override of it, and an override of a static must be static. | Refused |
| <a id="implementing-an.const"></a>An implementation that returns a value, is `const`, or has out-parameters | Compiled as a function, where the editor would make a function graph rather than an event. It takes the interface function's flags, so callers through the interface find it. A void event such as ShowDamageEffects is compiled as a function too. | Yes |
| <a id="implementing-an.class-pistol-public-weapons"></a>`class Pistol : public Weapons::Rifle` | A subclass implements the interface through its parent without listing it. Redefining an interface function is an ordinary override, and `Weapons::Rifle::Pull(Times)` calls the parent's. | Yes |
| <a id="implementing-an.class-nativeonly-public-aactor"></a>`class NativeOnly : public AActor, public IHealth` | Refused: "IHealth::GetHealth is native only ..., so a Blueprint cannot implement IHealth". The editor refuses the same. Calling IHealth through `TScriptInterface<IHealth>` on the game's objects still works. | Refused |
| <a id="implementing-an.listing-an-interface-a-mod"></a>Listing an interface a mod parent already implements | Refused: "... which its parent Beacon already implements". The child's stubs would override the parent's implementations. Override single functions in the child instead. | Refused |
| <a id="implementing-an.class-twoshare-public-aactor"></a>`class TwoShare : public AActor, public IAimable, public IMarkable` (IMarkable extends IAimable) | Refused: "... which both extend IAimable". List only the most derived interface, which already implements its parents. | Refused |
| <a id="implementing-an.a-base-after-the-parent"></a>A base after the parent that is not an interface | Refused: "... which is not an interface". A struct there gives "implements an undeclared interface". | Refused |
| <a id="implementing-an.an-interface-whose-implementable"></a>An interface whose implementable function uses a type AssetGen cannot write | Refused: "takes a type AssetGen cannot write yet". No interface in the current SDK triggers it. | Refused |

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
| <a id="declaring-an-interface.class-iaimable-public-ue"></a>`class IAimable { public: UE_INTERFACE; void OnAimed(AActor *By); int32 GetPriority(); };` | A Blueprint Interface asset, cooked at `<UE_MOD_PACKAGE>/IAimable` as class IAimable_C, with one empty function per method. The asset registry lists `<UE_MOD_PACKAGE>/IAimable.IAimable_C`, as for a class. Functions can return values and take reference (out) parameters. | Yes |
| <a id="declaring-an-interface.class-turret-public-aactor"></a>`class Turret : public AActor, public IAimable` | Implements it as for a game interface: matched by name, with stubs for the functions left out. There is no Events.json check, because every function of a mod interface can be implemented. | Yes |
| <a id="declaring-an-interface.virtual-int32-getpriority-0"></a>`virtual int32 GetPriority() = 0;` on the interface | The same as a declaration without a body. A class that leaves it out gets the empty stub and is not cooked Abstract. | Yes |
| <a id="declaring-an-interface.int32-getpriority-return-1"></a>`int32 GetPriority() { return 1; }` on the interface | A default implementation. A class that leaves the function out gets this body, with `this` being that class, instead of an empty stub. Blueprint Interface functions have no bodies in the editor. | Yes |
| <a id="declaring-an-interface.class-imarkable-public-iaimable"></a>`class IMarkable : public IAimable { public: UE_INTERFACE; ... };` | An interface that extends one other. A class that lists IMarkable alone implements both: a cast to either succeeds, and it gets stubs for the parent's functions it leaves out. | Yes |
| <a id="declaring-an-interface.class-iprioritytarget-public"></a>`class IPriorityTarget : public ITargetable { public: UE_INTERFACE; ... };` | A mod interface that extends a game interface (FSD's Targetable). An implementer gets the game interface's functions too, checked against Events.json. | Yes |
| <a id="declaring-an-interface.int32-marks-3"></a>`int32 Marks = 3;` on the interface | A variable on an interface, AssetGen's own feature: the engine's interfaces hold no state. It becomes a property of every class that directly implements the interface, not declared again in their subclasses. The initializer is its default, and the implementer's or a subclass's UE_DEFAULTS can set another; a subclass's TSet or TMap is written over the implementer's value, as any inherited one is. Its name is held to the implementer's as its own variables are: one its parents already have (`Tags` on an actor), or that a subclass declares again, is refused. | Yes |
| <a id="declaring-an-interface.uturretdef-td-big-marks-5"></a>`UTurretDef TD_Big = {{}, {.Marks = 5}, 100};` | A data asset of a class that implements IMarkable gives the interface's variables in the interface's own braces, after the base's (here UPrimaryDataAsset's `{}`), then the class's own members. | Yes |
| <a id="declaring-an-interface.ue-replicated-using-int32-score"></a>`UE_REPLICATED_USING(int32, Score, OnRep_Score);` on the interface | A replicated variable. Each implementing class gets the replicated property and an OnRep_Score of its own: its own definition, else the interface's body, else an empty stub. See [Replication](#replication). | Yes |
| <a id="declaring-an-interface.tscriptinterface-imarkable-m"></a>`TScriptInterface<IMarkable> M = Other; return M->Marks;` | Refused: "Marks is a variable of the interface IMarkable, which holds no state itself". Read it through an object of an implementing class, or declare a getter function on the interface. | Refused |
| <a id="declaring-an-interface.int32-marks"></a>`int32 Marks;` in a class that implements IMarkable | Refused: "the variable Marks of the interface IMarkable is declared twice". The interface's variable already is this class's property; set its default in UE_DEFAULTS. | Refused |
| <a id="declaring-an-interface.class-iboth-public-iaimable"></a>`class IBoth : public IAimable, public IMarkable` | Refused: "an interface extends one interface at most". A UClass has one super. | Refused |
| <a id="declaring-an-interface.class-iactorish-public-aactor"></a>`class IActorish : public AActor { public: UE_INTERFACE; };` | Refused: "IActorish extends AActor, which is not an interface". An interface's only base is another interface, the mod's or the game's. | Refused |
| <a id="declaring-an-interface.ue-component"></a>`UE_COMPONENT(UStaticMeshComponent, Hull);` on an interface | Refused: "an interface cannot declare a UE_COMPONENT". Declare it on the implementing actor. | Refused |
| <a id="declaring-an-interface.generate-api-true"></a>`generate_api: true` with a `UE_INTERFACE` | The interface's editor API stub is a Blueprint Interface asset at the interface's own path, so a Blueprint made in the UE 4.27 editor can list it under Implemented Interfaces and implement its functions. Cooked by the editor, that Blueprint uses the interface the mod's pak cooks: a call through the interface, from the mod or from another Blueprint, runs its implementation. In DRG, a mod's call through the interface ran an editor-made Blueprint's implementation. The interface's variables are not in the stub, since the editor's interfaces hold none, so an editor Blueprint that implements it does not have them. See [Editor API stubs](GUIDE.md#editor-api-stubs). | Yes |

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
| <a id="replicated-variables.ue-replicated-int32-score-5"></a>`UE_REPLICATED(int32, Score) = 5;` | A replicated variable, the editor's Replication: Replicated. The server sends its value to clients. A write on a client changes only that client's copy. The initializer after the macro is the class default, as for any member. | Yes |
| <a id="replicated-variables.ue-replicated-using-bool-bopen"></a>`UE_REPLICATED_USING(bool, bOpen, OnRep_Open);` | Replication: RepNotify. When a new value arrives on a client, the engine calls `OnRep_Open()` there. Your own assignment calls it too, on the machine that writes (see the next table). | Yes |
| <a id="replicated-variables.ue-replicated-if-float-aim"></a>`UE_REPLICATED_IF(float, Aim, SkipOwner);` | The editor's Replication Condition. The third argument is an `ELifetimeCondition` name, with or without the `COND_` prefix. | Yes |
| <a id="replicated-variables.ue-replicated-using-if-tarray"></a>`UE_REPLICATED_USING_IF(TArray<int32>, Slots, OnRep_Slots, OwnerOnly);` | RepNotify and a condition on one variable. An array replicates, and its OnRep fires for the array as a whole. | Yes |
| <a id="replicated-variables.ue-defaults-breplicates-true"></a>`UE_DEFAULTS { bReplicates = true; }` | A class that declares a replicated variable or an RPC of its own gets `bReplicates = true` in its Class Defaults, and a subclass inherits it. A class with neither keeps its parent's setting: set it by hand in `UE_DEFAULTS`. A `bReplicates` the class sets in its own `UE_DEFAULTS` wins over the automatic one: `bReplicates = false;` keeps a component with an RPC unreplicated, as the editor leaves a component until you tick Component Replicates. | Yes |
| <a id="replicated-variables.ue-replicated-if-float-aim-2"></a>`UE_REPLICATED_IF(float, Aim, OwnerAndSimulated);` | Refused: "unknown replication condition". The name is case-sensitive, so `ownerOnly` is refused too. | Refused |
| <a id="replicated-variables.ue-replicated-tscriptinterface"></a>`UE_REPLICATED(TScriptInterface<IMark>, Target);`, or a replicated `UE_STRUCT` holding a TMap or an interface at any depth | Refused: "an interface does not replicate", "a TMap or TSet in FBag does not replicate". The engine sends a struct member by member, and sends nothing for a TMap, a TSet or an interface. | Refused |
| <a id="replicated-variables.ue-replicated-tset-int32-ids"></a>`UE_REPLICATED(TSet<int32>, Ids);` | Refused: "a TMap or TSet does not replicate". The engine replicates neither, so the variable would never leave the server. A `TMap` or `TSet` inside a replicated array is refused too. Replicate two arrays, keys and values, and rebuild the map in the OnRep. | Refused |
| <a id="replicated-variables.ue-replicated-tmap-fname-int32"></a>`UE_REPLICATED(TMap<FName, int32>, Scores);` | Never reaches AssetGen. The comma in `TMap<FName, int32>` splits the macro argument, and clang stops with "too many arguments provided to function-like macro invocation". A type alias for the map is no way around it: through an alias declared in the class, the variable is refused as above ("a TMap or TSet does not replicate"), and an alias at namespace scope of a container or value type is refused as a variable's type ("unimplemented property"). | Refused |
| <a id="replicated-variables.void-onrep-ammo-int32-oldammo"></a>`void OnRep_Ammo(int32 OldAmmo)` | Refused: the RepNotify "must be a method of the class taking no parameters". A Blueprint RepNotify takes none, so C++'s previous-value form has no equivalent. The same message appears when no method of that name exists. Keep the previous value in a member and compare it in the OnRep. | Refused |
| <a id="replicated-variables.int32-onrep-ammo"></a>`int32 OnRep_Ammo()` | Refused: the RepNotify "must return void". The engine calls it with no room for a result. | Refused |
| <a id="replicated-variables.inline-void-onrep-ammo"></a>`inline void OnRep_Ammo()` | Refused: the RepNotify "is inline, so no function of the class". An inline method is no Blueprint function, so a client would never find the OnRep. Drop `inline`. | Refused |
| <a id="replicated-variables.ue-replicated-int32-a"></a>`UE_REPLICATED(int32, A);` in a `UE_STRUCT` | Refused: "UE_REPLICATED on a struct member has no effect". UE has no per-member replication for a struct. Replicate the class variable that holds the struct, which replicates it as a whole. | Refused |

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
| <a id="writing-replicated.bopen-true"></a>`bOpen = true;` | The editor's Set w/ Notify node. On an actor, AssetGen calls `FlushNetDormancy()` first, so a dormant actor still sends the change. Then it writes. Then, for a RepNotify variable, it calls `OnRep_Open()` on whichever machine runs the code, the server included. Native UE C++ never calls an OnRep on the server by itself, so this differs from C++. | Yes |
| <a id="writing-replicated.other-bopen-false"></a>`Other->bOpen = false;` | The flush, the write and the OnRep all go to `Other`, not to self. | Yes |
| <a id="writing-replicated.slots-0-4"></a>`Slots[0] = 4;` | An element write counts as a set of the whole array: flush, write, `OnRep_Slots()`. | Yes |
| <a id="writing-replicated.score-1"></a>`Score += 1;` or `++Score;` | Rewritten as `Score = Score + 1`, with the target found once, so it gets the same flush and OnRep. | Yes |
| <a id="writing-replicated.me-bopen-true"></a>`Me()->bOpen = true;` | The object expression runs once per statement, as C++ runs it, and the flush, the write and the OnRep all use that one result. | Yes |
| <a id="writing-replicated.slots-add-3"></a>`Slots.Add(3);` | A container call changes the array in place and adds neither the flush nor the OnRep. The editor does the same: only its Set node adds them. The change still replicates on the next update of an actor that is not dormant. Call `FlushNetDormancy()` and the OnRep yourself, or assign the whole array. | Yes |
| <a id="writing-replicated.target-z-100-0f"></a>`Target.Z = 100.0f;` | A write to one member of a replicated struct gets no flush and no OnRep. To get both, copy the value, change the copy and assign it back. | Yes |
| <a id="writing-replicated.breplicatemovement-true"></a>`bReplicateMovement = true;` | An engine or game C++ replicated property: the flush and the write, and no OnRep call, because a native RepNotify is meant for clients. The editor does the same. Where the engine has a setter, such as `SetReplicateMovement`, the setter may do work that a raw write skips. | Yes |
| <a id="writing-replicated.glass-thrown-true"></a>`Glass->Thrown = true;` | A replicated variable that one of the game's Blueprints declares works like one of yours: flush, write, then its OnRep (`OnRep_Thrown`), called on that object. Here `Glass` is a `Game::GameElements::Bar::Bar_Glass_Physics_C *` from `UeApi/Game/Bar_Glass_Physics_C.h`. | Yes |

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
| <a id="replicated-variables-on.ue-replicated-using-int32-points"></a>`UE_REPLICATED_USING(int32, Points, OnRep_Points) = 0;` in a `UE_INTERFACE` | Each class that implements the interface gets `Points` as a replicated variable of its own, with the replication settings written on the interface. A write calls the RepNotify as usual. | Yes |
| <a id="replicated-variables-on.void-onrep-points-seen-1"></a>`void OnRep_Points() { Seen += 1; }` in the implementing class | The RepNotify belongs to the implementing class. AssetGen looks for it on the class first, then on the interface. A class that leaves it out gets the interface's out-of-line body when there is one (`void IScored::OnRep_Points() { Mirror = Points; }`), otherwise an empty function, and every write still calls it. | Yes |
| <a id="replicated-variables-on.an-onrep-declared-on-neither"></a>an OnRep declared on neither | Refused: the RepNotify "must be a method of the class taking no parameters". If the interface leaves the declaration out, every implementing class has to define it. | Refused |

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
| <a id="host-or-client.ukismetsystemlibrary-isserver"></a>`UKismetSystemLibrary::IsServer(this)` | The editor's Is Server node. True on a listen-server host, on a dedicated server and in a solo (standalone) game. False on a client. `IsServer()` without the argument passes self. `IsDedicatedServer()` and `IsStandalone()` work the same way. | Yes |
| <a id="host-or-client.hasauthority"></a>`HasAuthority()`, `Other->HasAuthority()` | The editor's Has Authority node. True when this machine holds the authoritative copy of the actor: on the server for an actor that replicates, and on any machine for an actor that machine spawned itself. | Yes |
| <a id="host-or-client.getlocalrole-enetrole-role"></a>`GetLocalRole() == ENetRole::ROLE_AutonomousProxy` | Get Local Role. Tells a client's own copy (`ROLE_AutonomousProxy`) from other players' copies (`ROLE_SimulatedProxy`). | Yes |
| <a id="host-or-client.watched-islocallycontrolled"></a>`Watched->IsLocallyControlled()` | Is Locally Controlled, on a pawn. True only on the machine whose own controller possesses it: on the host, the host's pawn; on a client, that client's own. A pawn with no controller gives false. DRG's `UGameFunctionLibrary::GetLocalPlayerCharacter(this)` returns this machine's player directly. | Yes |
| <a id="host-or-client.getowner"></a>`GetOwner()` in a component | A component has no role of its own, so it asks its owner: `AActor *O = GetOwner(); if (O) bAuth = O->HasAuthority();`. | Yes |

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
| <a id="declaring-rpcs.ue-server-ue-reliable-void"></a>`UE_SERVER UE_RELIABLE void ServerOpen(bool bValue);` | The editor's custom event with Replicates: Run on Server. Called on the owning client, it runs on the server. Called on the server, it runs right there. The marker may sit on the declaration in the class or on the definition. | Yes |
| <a id="declaring-rpcs.ue-client-void-clientping-int32"></a>`UE_CLIENT void ClientPing(int32 Seq) { Seen = Seq; }` | Replicates: Run on owning Client. Called on the server, it runs on the client that owns the actor. | Yes |
| <a id="declaring-rpcs.ue-multicast-void-playcreak-seen"></a>`UE_MULTICAST void PlayCreak() { Seen = 2; }` | Replicates: Multicast. Called on the server, it runs on the server and on every client. Called on a client, it runs only there. | Yes |
| <a id="declaring-rpcs.ue-multicast-ue-reliable-void"></a>`UE_MULTICAST UE_RELIABLE void PlayHit() { Local = 5; }` | The Reliable checkbox. An RPC without `UE_RELIABLE` is unreliable. | Yes |
| <a id="declaring-rpcs.ue-reliable-void-ping-n"></a>`UE_RELIABLE void Ping() { N = 1; }` | Refused: "UE_RELIABLE needs UE_SERVER, UE_CLIENT or UE_MULTICAST". | Refused |
| <a id="declaring-rpcs.ue-server-int32-serverscore"></a>`UE_SERVER int32 ServerScore() { return N; }` | Refused: "an RPC returns void". An RPC runs on another machine and cannot hand a value back. Answer with a Client RPC or a replicated variable. | Refused |
| <a id="declaring-rpcs.ue-server-void-serverbump-int32"></a>`UE_SERVER void ServerBump(int32 &Count) { Count += 1; }` | Warns once per parameter it writes: "reaches the caller only when the call runs locally". The receiving machine works on a copy, so the caller sees the write only when the call ran locally, as when the server calls its own Server RPC. Take the parameter by value or `const &`. | Warns |
| <a id="declaring-rpcs.ue-server-void-serversync-tmap"></a>`UE_SERVER void ServerSync(TMap<FName, int32> Scores) { Kept = Scores; }` | Refused: "an RPC parameter cannot be a TMap or TSet". The engine sends nothing for either, so the argument would arrive empty. Pass two arrays, keys and values. | Refused |
| <a id="declaring-rpcs.ue-server-void-send-fbag"></a>`UE_SERVER void Send(FBag Sack)`, where `FBag` holds a `TMap`; `UE_SERVER void S(TScriptInterface<IMark> T)` | Refused: "an RPC parameter cannot hold what does not replicate". A struct is sent member by member, and neither a TMap nor an interface sends anything. Pass the object as a `UObject*` or an actor pointer instead of an interface. | Refused |
| <a id="declaring-rpcs.ue-server-inline-void-serverping"></a>`UE_SERVER inline void ServerPing() { Pings += 1; }` | Refused: "an RPC, authority-only or cosmetic marker on an inline method does nothing". An inline method is expanded at each call and is no Blueprint function, so no call to it is routed. The same holds for `UE_AUTHORITY_ONLY` and `UE_COSMETIC`, and for a marker on the method's definition out of the class, `UE_SERVER inline void AMine::ServerPing() { ... }`, which used to compile. Drop `inline`. | Refused |
| <a id="declaring-rpcs.ue-server-static-void-s"></a>`UE_SERVER static void S(int32 X) { }` | Refused: "a static function cannot be an RPC". The engine routes a static function without looking at its net flags, so it would run locally and never be sent. Use a member function. | Refused |
| <a id="declaring-rpcs.ue-multicast-ue-server-void"></a>`UE_MULTICAST UE_SERVER void Also() { }` | Refused: "an RPC goes one way". The sender picks one direction and the receiver checks its own, so a second marker would drop the call on one side. `UE_MULTICAST UE_CLIENT` is refused the same way. | Refused |

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
| <a id="authority-only-and.ue-authority-only-void"></a>`UE_AUTHORITY_ONLY void GrantReward() { Credits += 10; }` | The editor's Authority Only function flag (BlueprintAuthorityOnly). A call on a machine without authority over the actor is skipped instead of failing. | Yes |
| <a id="authority-only-and.ue-cosmetic-void-playsparks-seen"></a>`UE_COSMETIC void PlaySparks() { Seen = 3; }` | The Cosmetic function flag (BlueprintCosmetic). The engine skips the call on a dedicated server. A listen-server host and clients run it. | Yes |

Notes:

- The engine skips a call only on an actor or a component of one. On any other object the function always runs.
- UeApi marks the engine's and the game's functions the same way, and a call to one of their member functions is
  skipped the same way. So is a call to an engine or game static marked authority-only or cosmetic, which goes to its
  class's default object; the list is in [Working with other objects](#working-with-other-objects).

### Calling RPCs

| You write | What it does | Status |
|---|---|---|
| <a id="calling-rpcs.serveropen-false"></a>`ServerOpen(false);`, `Other->PlayCreak();` | A call to an RPC, or to an authority-only or cosmetic function, goes through the engine's call routing, which runs it here, sends it over the network or drops it. A plain method is called directly. | Yes |
| <a id="calling-rpcs.char-server-setrunning-false"></a>`Char->Server_SetRunning(false);` | The game's and the engine's RPCs carry the same markers in UeApi. A call to one is an ordinary call, and the engine routes it. | Yes |

### RPCs in subclasses

| You write | What it does | Status |
|---|---|---|
| <a id="rpcs-in-subclasses.void-serveropen-bool-bvalue-seen"></a>`void ServerOpen(bool bValue) { Seen = bValue ? 7 : 8; }` in a subclass | An override without a marker keeps the parent's net flags (Server, Reliable) and is still that RPC. | Yes |
| <a id="rpcs-in-subclasses.void-onrep-open-seen-10"></a>`void OnRep_Open() { Seen = 10; }` in a subclass | When the parent's code assigns the variable, the subclass's OnRep runs in the parent's place, because the call goes by name. | Yes |
| <a id="rpcs-in-subclasses.ue-server-void-serveropen-bool"></a>`UE_SERVER void ServerOpen(bool bValue) { Seen = 2; }` in a subclass | Refused: "an override takes its parent's replication; drop the RPC marker". The engine asserts on a mismatch. Any RPC marker on any override is refused, one on an engine event such as `UE_MULTICAST void ReceiveBeginPlay()` included. | Refused |

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
| <a id="waiting-with-delay.ukismetsystemlibrary-delay-0-5f"></a>`UKismetSystemLibrary::Delay(0.5f);` | The editor's Delay node. The code after it runs when the delay ends, with the method's locals intact. Leave out the world context (`Delay(this, 0.5f)` is the same call) and the `FLatentActionInfo`: AssetGen supplies both. | Yes |
| <a id="waiting-with-delay.ukismetsystemlibrary"></a>`UKismetSystemLibrary::RetriggerableDelay(HoldTime);` | Retriggerable Delay. A call while one is pending restarts the countdown instead of being ignored, so the code after it runs once, `HoldTime` after the last call. All callers of the method share its one call site, so a tick and any other caller restart the same countdown. | Yes |
| <a id="waiting-with-delay.upendinglatentactionlibrary"></a>`UPendingLatentActionLibrary::WaitOneFrame();` | Any latent function, static or a method, has a UeApi overload without the `FLatentActionInfo` and without the world context, and that is the one you call. AssetGen fills in the resume point, an ID for this call site and self. | Yes |
| <a id="waiting-with-delay.mover-findnearestpathfinderpoint"></a>`Mover->FindNearestPathfinderPoint_Async(Pos, 500.0f, Ok, At);` | A latent function that answers through reference parameters. `Ok` and `At` are locals in the frame, so the action can fill them in, and the code after the call reads them. This is how the editor wires a latent node's output pins. | Yes |
| <a id="waiting-with-delay.ukismetsystemlibrary-delay-this"></a>`UKismetSystemLibrary::Delay(this, 1.0f, FLatentActionInfo(0, 1, "ExecuteUbergraph_Door", this));` | Refused: "leave the FLatentActionInfo argument out". The resume point exists only once AssetGen has laid out the event graph. | Refused |
| <a id="waiting-with-delay.ukismetsystemlibrary-delay-1-0f"></a>`UKismetSystemLibrary::Delay(1.0f, FLatentActionInfo(0, 1, "ExecuteUbergraph_Door", this));` | Refused the same way, without the world context too. | Refused |

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
| <a id="when-the-code.findplayer-ready-true"></a>`FindPlayer(); Ready = true;` | A call to a method that waits returns when that method reaches its first wait, and the rest of it runs later. The caller never waits for it to finish, as with an editor custom event that contains a Delay. The code before the first wait runs inside the caller's call. | Yes |
| <a id="when-the-code.log-add-tag-ukismetsystemlibrary"></a>`Log.Add(Tag); UKismetSystemLibrary::Delay(Seconds); Log.Add(Tag + 1);` | Parameters and locals keep their values across the wait: parameters are copied into the frame, and locals live there. Member variables are read fresh after the resume. | Yes |
| <a id="when-the-code.while-c-nullptr"></a>`while (C == nullptr)` with a Delay inside | Each round waits before the next one starts, and the loop variables survive each wait. A `for` loop works the same way. This differs from a Delay in the body of the editor's For Loop node, which does not pause the loop. | Yes |
| <a id="when-the-code.pause-2-0f"></a>`Pause(2.0f);`, an inline helper that calls Delay | The helper is expanded into the caller, so the caller becomes the method that waits, and it resumes after the helper's Delay. Each expansion is a call site of its own. The caller has to follow the latent rules. A non-inline helper that waits returns to its caller at its first wait instead. | Yes |
| <a id="when-the-code.void-receivebeginplay"></a>`void ReceiveBeginPlay()` with a Delay inside | An override of an engine event can wait. The engine still calls it as that event. | Yes |
| <a id="when-the-code.twice-1-twice-2"></a>`Twice(1); Twice(2);` before `Twice` resumes | An object has one frame for a method that waits, not one per call. The second call starts at the top and overwrites the pending call's parameters and locals, and its Delay is ignored because one is already pending at that call site. The method resumes once, with the second call's values. An editor event graph behaves the same; a C++ coroutine would not. | Yes |
| <a id="when-the-code.uuserwidget-w-createwidget"></a>`UUserWidget *W = CreateWidget<UUserWidget>(P); UKismetSystemLibrary::Delay(1.0f); Kept = W;` | The frame holds an object local weakly: unless the object is flagged RF_StrongRefOnFrame, as SpawnObject's (`NewObject`) and the callback proxies' are, a garbage collection during the wait can take it, and the local then reads None. The editor's event graph is the same. The compiler warns where a local may hold an object the method made or loaded (not one read out of a property, nor an actor, component or async action) across a wait and reads it after: keep such an object in a member. | Warns |
| <a id="when-the-code.tarray-int32-nums"></a>`TArray<int32> Nums;` with no initializer | Empty on the object's first call. On a later call it holds whatever the previous call left in it, where C++ would give a new empty array. Initialize the local, or `Clear()` it at the top. Inside a loop it is reset every round, as usual. | Yes |

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
| <a id="where-a-method.static-void-later-uobject"></a>`static void Later(UObject *WorldContextObject)` with a Delay inside | Refused: "a static function has no object whose ubergraph frame could keep its locals". | Refused |
| <a id="where-a-method.int32-countlater"></a>`int32 CountLater()` with a Delay inside | Refused: "a function that resumes later returns nothing and takes no non-const reference parameters". The caller gets control back at the first wait, before there is a value. Store the result in a member, or broadcast a dispatcher when done. | Refused |
| <a id="where-a-method.void-saylater-const-fstring-msg"></a>`void SayLater(const FString &Msg)` with a Delay inside | Msg is copied into the event graph's frame when the method is called, through EX_LocalOutVariable, as the editor's event stubs copy one (a ReceiveHit's `const FHitResult &Hit`); the code after the wait reads that copy. | Yes |
| <a id="where-a-method.void-filllater-int32-out"></a>`void FillLater(int32 &Out)` with a Delay inside | Refused with the CountLater message: a write after the wait would land in the frame's copy, the caller having gone on. Store the result in a member. | Refused |
| <a id="where-a-method.uobject"></a>a Delay in a `UObject` child class | Warns: "finds its world only through its Outer". Actor, ActorComponent, UserWidget, GameInstance and Subsystem classes have a world of their own; any other object uses its Outer's. Make the object with an actor or component as its Outer, or the wait does nothing and the method never resumes. The editor offers Delay only where the class has a world. | Warns |
| <a id="where-a-method.v-read64-addr"></a>`V = __Read64__(Addr);` in a method that waits | Reads the memory when the line runs: a read after the wait sees what the memory holds then. Writes through a pointer, `GetOuter()` and `GetTypedOuter` work the same way. See [Pointers and memory](#pointers-and-memory). | Yes |
| <a id="where-a-method.void-executeubergraph-door-int32"></a>`void ExecuteUbergraph_Door(int32 EntryPoint);` | Refused: "ExecuteUbergraph_<Class> is the name of a class's ubergraph". The engine resumes a class's latent calls through that function, found by name on the object, so a method of that name in the class or a subclass would catch them. | Refused |

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
| <a id="async-loads.uobject-obj-ukismetsystemlibrary"></a>`UObject *Obj = UKismetSystemLibrary::LoadAsset(Icon);` | The editor's Async Load Asset node, written as a call that returns. The method waits for the load, and the value is the loaded object, or null if the load failed. | Yes |
| <a id="async-loads.auto-cls-ukismetsystemlibrary"></a>`auto Cls = UKismetSystemLibrary::LoadAssetClass(EnemyClass);` | Async Load Class Asset. The value is the loaded class, or null. | Yes |
| <a id="async-loads.ukismetsystemlibrary-loadasset"></a>`UKismetSystemLibrary::LoadAsset_Blocking(Icon)` | An ordinary call that does not wait, for a caller that must not become latent. | Yes |

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
| <a id="static-locals.static-int32-count-stage-10"></a>`static int32 Count = Stage + 10;` in a method that waits | The static lives in the object's frame. Its initializer runs on that object's first call only, and the value is kept after that: once per object, not once per program as in C++. A plain local of the same name elsewhere in the method is a separate variable and does not reset it. | Yes |
| <a id="static-locals.static-int32-seen"></a>`static int32 Seen;` | Starts at zero on the object's first call and keeps its value across calls and loop rounds. A loop around it does not reset it, as it would a plain local. | Yes |
| <a id="static-locals.static-int32-count-n"></a>`static int32 Count = N;` in a method that never waits | Refused: "static Count lives in the ubergraph's frame". Only the frame of a method that waits outlives a call. Make `Count` a member. | Refused |
| <a id="static-locals.static-int32-count-0"></a>`static int32 Count = 0;` in an inline helper | Refused: "each expansion would keep its own", unlike C++'s single static. | Refused |
| <a id="static-locals.static-constexpr-int32-kfive-5"></a>`static constexpr int32 kFive = 5;` | A static that holds a constant nothing writes is replaced by that constant. It needs no frame, so it works in any method, waiting or not. | Yes |

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
the values it carries, as the editor's async action node continues from an output pin. The method becomes latent, so
everything in [Latent calls](#latent-calls) applies to it. The rule to remember: the binding stays, and every later
broadcast runs the code after the await again. [examples/AwaitEvents.cpp](examples/AwaitEvents.cpp) waits this way on
the game's dispatchers and one of its own, and its comments show the async-action form.

### UE_AWAIT

| You write | What it does | Status |
|---|---|---|
| <a id="ue-await.fname-notify-ue-await-proxy"></a>`FName Notify = UE_AWAIT(Proxy->OnCompleted);` | Binds a generated event to the dispatcher and returns to the caller. When the dispatcher fires, the event stores its one parameter, and the method resumes after the await with that value. | Yes |
| <a id="ue-await.image-ue-await-task-onsuccess"></a>`Image = UE_AWAIT(Task->OnSuccess);` on an async action | For a `UBlueprintAsyncActionBase`, such as `UAsyncTaskDownloadImage`, AssetGen calls `Activate()` right after the bind, as the editor's async node does, at the first await of that variable on each path the method runs: in whichever branch of an `if` runs, and in a loop on the first round only. A later await on the same variable does not activate it again, until the variable is assigned a new action. An object that is not an async action, such as the montage proxy, is never activated: its factory already started the work. | Yes |
| <a id="ue-await.if-stop-ue-await-t"></a>`if (Stop) UE_AWAIT(T->OnSuccess);` then `UE_AWAIT(T->OnFail);` | Refused: `some paths reach it with T's async action already activated and some without`. Keeping the second await's `Activate()` would start the action twice on one path, dropping it would never start it on the other. The same for a loop that can skip its await, such as with `continue` before it. | Refused |
| <a id="ue-await.ue-await-target-ondestroyed"></a>`UE_AWAIT(Target->OnDestroyed);` | As a statement: waits for the dispatcher and drops its values. | Yes |
| <a id="ue-await.ue-await-task-onsuccess"></a>`UE_AWAIT(Task->OnSuccess);` with `Task` None | The bind, and `Activate()` for an async action, run only when the object passes IsValid, as the editor's async node tests its proxy. A None or pending-kill object binds nothing and logs no "Accessed None"; the method stays parked at the await. | Yes |
| <a id="ue-await.int32-code-ue-await-onready"></a>`int32 Code = UE_AWAIT(OnReady);` | The object in front of the dispatcher may be self, a member or a local. | Yes |
| <a id="ue-await.the-code-after-the-await"></a>the code after the await, on a later broadcast | The generated event stays bound, so each later broadcast runs the code after the await again: it acts as a handler, not a one-shot wait. The event has no name you can write, so only `Clear()` on the dispatcher stops it, and that drops every binding. | Yes |
| <a id="ue-await.auto-who-points-ue-await"></a>`auto [Who, Points] = UE_AWAIT(OnScored);` on a dispatcher of two or more parameters | Each name gets one of the dispatcher's values, in order, as the editor's async node gives each output a pin of its own; a later broadcast gives them its values. `const auto& [Who, Points]` is the same. `UE_AWAIT(OnScored);` as a statement waits and keeps none of them. `auto V = UE_AWAIT(OnScored);` is refused: `UE_AWAIT gives a dispatcher's two or more values one name each`. | Yes |
| <a id="ue-await.bool-waitdone-ue-await-proxy"></a>`bool WaitDone() { UE_AWAIT(Proxy->OnCompleted); return true; }` | Refused with the latent messages ("a function that resumes later returns nothing and takes no non-const reference parameters"; for a static method, "a static function has no object whose ubergraph frame could keep its locals"). A method that awaits returns `void`, takes no non-const reference parameters and is not `static`. | Refused |
| <a id="ue-await.ue-await-uasynctaskdownloadimage"></a>`UE_AWAIT(UAsyncTaskDownloadImage::DownloadImage(Url)->OnSuccess);` | Refused: "keep the object in a variable, it is used twice", once for the bind and once for `Activate()`. | Refused |
| <a id="ue-await.ue-await-temp"></a>`UE_AWAIT(Temp);` on a local delegate | Refused: `UE_AWAIT` takes a dispatcher property of an object. A multicast delegate as a local or a parameter is refused before `UE_AWAIT` sees it ("unimplemented local"). A single-cast `TDelegate` cannot be awaited at all. | Refused |
| <a id="ue-await.task-onsuccess-add-this-door"></a>`Task->OnSuccess.Add(this, &Door::Done);` | Callback style: a handler per outcome, and the method does not wait. `Add` never calls `Activate()`, so call `Task->Activate()` yourself after binding. See [Event dispatchers](#event-dispatchers). | Yes |

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
  int32 Score;
  UE_DISPATCHER(OnReady, int32 Code);
  UE_DISPATCHER(OnScored, AActor *Who, int32 Points);

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
  void WaitScore() {
    auto [Who, Points] = UE_AWAIT(OnScored);       // two values, a name for each
    if (Who == Target) Score += Points;
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
| <a id="raw-pointers-and.int32-p"></a>`int32 *P`, `void *Q`, `FVector *V`, `UObject **PP` | A pointer to anything but a UObject is an int64 address. Pointer parameters, return values and locals are Int64 variables in the Blueprint. The compiler keeps the pointee type, so `*P` knows what it reads. There is no editor node for any of this. | Yes |
| <a id="raw-pointers-and.fstring-s"></a>`FString *S`, `FName *N`, `TArray<int32> *A` | FString, FName and the containers are plain records, not objects, so pointers to them are raw addresses too. | Yes |
| <a id="raw-pointers-and.aactor-a"></a>`AActor *A`, `UObject *O` | A pointer to a UObject class stays an object reference that the garbage collector sees. | Yes |
| <a id="raw-pointers-and.aactor-a-other"></a>`AActor &A = *Other;`, `Others[1]` | An object pointer is not an address, so `*` and `[]` on one are refused ("which is not a raw pointer", "indexing that is not through a raw pointer"). Use the pointer as it is, `Other->Field`. For the object's memory, convert it: `(uint8 *)Other`. | Refused |
| <a id="raw-pointers-and.const-char-s"></a>`const char *S`, `wchar_t *W` | Never an address. A `char*` parameter or local is an FString, and a string literal is a string constant. Walk raw bytes with `int8*` or `uint8*`. | Yes |
| <a id="raw-pointers-and.if-p"></a>`if (P)`, `P != nullptr` | Compares the address with 0 (NotEqual_Int64Int64). `nullptr` is the int64 0. | Yes |
| <a id="raw-pointers-and.if-obj"></a>`if (Obj)` | On an object pointer this is an IsValid test, not a null check: see [Working with other objects](#working-with-other-objects). | Yes |

Notes:

- A class is a UObject when it has UE_CLASS or a base class. A class that is only forward-declared counts as one when
  its name is `U` or `A` followed by a capital letter, or ends in `_C`. Any other forward-declared class is raw memory,
  so include the SDK header that defines it.

### Reading and writing through a pointer

| You write | What it does | Status |
|---|---|---|
| <a id="reading-and-writing.int32-v-p"></a>`int32 V = *P;` | Reads the value at the address. | Yes |
| <a id="reading-and-writing.p-i"></a>`P[I]`, `2[P]` | Reads the element at P + I × sizeof(pointee); a literal I folds to a constant offset. This is pointer indexing, not `Items[I]` on a TArray. | Yes |
| <a id="reading-and-writing.p-42"></a>`*P = 42;`, `P[2] = 7;` | Writes the value straight into the memory. | Yes |
| <a id="reading-and-writing.int8-addr"></a>`*(int8 *)Addr`, `*(signed char *)Addr` | Reads one byte, sign-extended: 0x80 to 0xFF read as negative ints, as in C++. | Yes |
| <a id="reading-and-writing.uint8-addr"></a>`*(uint8 *)Addr`, `*(bool *)Addr` | Reads one byte as 0 to 255. A bool read is `byte != 0`. | Yes |
| <a id="reading-and-writing.fstring-s-fstring-addr"></a>`FString S = *(FString *)Addr;` | Copies the string out. The function owns its copy, which is freed when it returns. | Yes |
| <a id="reading-and-writing.ftext-t-ftext-addr"></a>`FText T = *(FText *)Addr;`, `*(FText *)Addr = T;` | Copies the text with the engine's FText copy, which shares the text data by reference count. Needs the FDerefTextView helper struct (see Helper code you supply, below). | Yes |
| <a id="reading-and-writing.uobject-addr"></a>`*(UObject **)Addr`, `*(UClass **)Addr` | Reads 8 bytes as an object or class reference. | Yes |
| <a id="reading-and-writing.int64-void-addr"></a>`(int64)*(void **)Addr` | A pointer to a raw pointer reads a plain int64 address. | Yes |
| <a id="reading-and-writing.int64-v-uint32-addr"></a>`int64 V = *(uint32 *)Addr;` | Not built: Kismet has no unsigned 32-bit integer, and an int32 read would make 0xFFFFFFFF compare as -1. Refused with "reading a uint32 through a pointer". Read an int32 and mask it, as below. | Not yet |
| <a id="reading-and-writing.uint32-addr-5"></a>`*(uint32 *)Addr = 5;` | Refused with "no Kismet conversion from int to uint32". Write through an int32 pointer: `*(int32 *)Addr = 5;`. | Refused |
| <a id="reading-and-writing.fvector-v-pv"></a>`FVector V = *Pv;`, `*Pv = FVector(1.0f, 2.0f, 3.0f);`, `Pv[1] = *Pv;` | Reads, writes or copies the whole struct, at its size in the game: 12 bytes for an FVector. A struct holding an FString or a TArray is copied deeply, as C++'s copy is. Any struct a variable can be works, the mod's own included. | Yes |
| <a id="reading-and-writing.int32-v-p16"></a>`int32 V = *P16;`, `*P16 = 7;` with `uint16 *P16` or `int16 *P16` | Two bytes. A read is an int32, since Blueprint has no 16-bit integer: 0 to 65535 for uint16, sign-extended for int16. A write stores the value's low 16 bits and touches nothing past them. | Yes |
| <a id="reading-and-writing.float-f-float-pd"></a>`float F = (float)*Pd;`, `*Pd = F;` with `double *Pd` | Blueprint has no double, so a read gives the float nearest the double, rounded as C++ converts it, and a write stores the float widened to a double. A value below the smallest normal float (about 1.2e-38) reads as 0 and is written as 0, where C++ keeps a denormal. | Yes |
| <a id="reading-and-writing.tarray-int32-a-pa"></a>`TArray<int32> A = *Pa;`, `TSet`, `TMap` | Refused with "a whole TArray<int32> through a pointer". Index the elements through the pointer's own data, or copy the container in a function that owns it. | Refused |

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

- A read takes the pointee's size in the game: 8 bytes for int64, uint64, double, FName and any pointer; 4 for int32
  and float; 2 for int16 and uint16; 1 for int8, uint8 and bool; an enum's underlying size (1, 4 or 8); 16 for
  FString; 24 for FText; a struct's size (see sizeof below).
- A whole struct goes through a struct the compiler cooks beside the class, `FDerefView_<Struct>`, as it cooks
  FDeref. Ship it with the class.
- A double is converted with integer math on its two halves, about 35 Kismet calls a read.
- The uint32 message also suggests an int64 read. An int64 read takes 8 bytes, so it includes the 4 bytes after the
  value. Use it only when the 8 bytes really are an int64.

### Struct members through a pointer

| You write | What it does | Status |
|---|---|---|
| <a id="struct-members-through.v-z-5-0f"></a>`V->Z = 5.0f;`, `float Y = V->Y;` | Reads or writes one member in place, at the address plus the member's offset. The struct is never copied, so a pointer to a struct of any size works. | Yes |
| <a id="struct-members-through.v-x"></a>`(*V).X` | The same as `V->X`. | Yes |

```cpp
TArray<FVector> Points;

float Lift() {
  Points.Add(FVector(1.0f, 2.0f, 3.0f));
  FVector *V = &Points[0];
  V->Z = V->Z + 100.0f;                 // writes Points[0].Z in place
  float *Y = &V->Y;                     // 4 bytes in
  return *Y + (*V).X;
}
```

Notes:

- On a struct the mod defines, `P->X` gives the function a hidden, unused local named `__Keep<Struct>__`. It keeps
  the struct's package loaded when only the bytecode names the struct. You see it in a disassembly, never in behaviour.

### Pointer arithmetic

| You write | What it does | Status |
|---|---|---|
| <a id="pointer-arithmetic.p-n"></a>`P + N`, `N + P`, `P - N` | Moves by N elements, as in C++: the byte offset is N × sizeof(pointee). The result is another address. | Yes |
| <a id="pointer-arithmetic.p-q"></a>`P - Q` | The number of elements between two pointers, as an int64. | Yes |
| <a id="pointer-arithmetic.p-n-2"></a>`P += N`, `P -= N`, `++P`, `--P`, `P++` | Steps a pointer variable in place, by elements. `P++` yields the value from before the step. | Yes |
| <a id="pointer-arithmetic.int64-p-4"></a>`(int64)P + 4`, `(uint8 *)P + 4` | Byte arithmetic: an int64 or a byte pointer moves by bytes. | Yes |

### Taking an address

| You write | What it does | Status |
|---|---|---|
| <a id="taking-an-address.items-i"></a>`&Items[I]` | The address of a TArray element: the array's data pointer plus I × the element size. The array must be a variable, a member or a local. | Yes |
| <a id="taking-an-address.makearr-0"></a>`&MakeArr()[0]` | Refused with "the address of an element needs an array variable". Store the array in a local first. | Refused |
| <a id="taking-an-address.p"></a>`&*P`, `&P[I]` | P itself, and P + I × sizeof(pointee). Nothing is read. | Yes |
| <a id="taking-an-address.obj-member"></a>`&Obj->Member`, `&Member` | The address of an object's property (`&Member` is one of the class's own), looked up by name at run time. Needs a class named ReadProperty that you supply (see Helper code you supply, below). The result is 0 when the object's class has no such property. | Yes |
| <a id="taking-an-address.pv-y"></a>`&Pv->Y`, `&(*Pv).Y`, `&Items[I].Y`, `&R.Y` | The address of a struct member: where the struct sits plus the member's offset in the game's layout, through any number of members (`&P[I].Inner.Y`). The struct may be memory through a pointer, a TArray element, a reference that keeps an address, or a call that returns a reference. | Yes |
| <a id="taking-an-address.obj-loc-y"></a>`&Obj->Loc.Y`, `&Loc.Y` | The same, with the struct's address found at run time through ReadProperty, as `&Obj->Member`'s is. | Yes |
| <a id="taking-an-address.c-externalcurve"></a>`&C->ExternalCurve` with `FRuntimeCurveLinearColor *C` | An engine struct's members are placed from its reflected ones, which is right only when nothing unreflected sits between them. When the ones UeApi has do not add up to the struct's size, the build stops: "FRuntimeCurveLinearColor's reflected members take 8 bytes and it takes 520 in the game" (its `ColorCurves` array is not in UeApi). Add the offset yourself, from the game's SDK. `&H->Distance` on an `FHitResult *H` is placed, as its weak members are in UeApi. | Refused |
| <a id="taking-an-address.local"></a>`&Local`, `&Param`, `&V.X` of a local struct | A local or a parameter is a Blueprint variable, which has no address the VM hands out. Refused with "a Blueprint variable has none the VM hands out". Point into an object, a TArray element or memory through a pointer instead. | Refused |
| <a id="taking-an-address.this-k"></a>`&this->K` with `static inline int32 K = 3;` | An inline class variable has no storage: each use is its initializer. Refused with "a static variable has no address". A bare `&K` gets the Blueprint-variable message instead. | Refused |

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
- Only an object's members have a run-time lookup. A struct member's offset is known when the mod is built: from the
  mod's own layout for its structs, from the reflected members for an engine struct.
- `float &R = Other->CustomTimeDilation;` makes the same GetPropertyAddress call and keeps the address it returns.
  Without a ReadProperty class it is refused with the same message, after `reference R: `. Reference locals are in
  [Locals](#locals).

### References to memory

| You write | What it does | Status |
|---|---|---|
| <a id="references-to-memory.int32-r-p"></a>`int32 &R = *P;`, `int32 &R = P[2];` | Keeps the address in a hidden int64 local. Reads and writes through R go to that memory. | Yes |
| <a id="references-to-memory.int32-alias-local"></a>`int32 &Alias = Local;` | Another name for the variable, with no storage of its own. Writes reach the original. | Yes |
| <a id="references-to-memory.const-fstring-r-makename"></a>`const FString &R = MakeName();` | A `const T&` bound to a temporary is an ordinary local holding a copy. | Yes |
| <a id="references-to-memory.bump-p"></a>`Bump(*P);` with `void Bump(int32 &V)` | Passes the memory itself, so the callee's writes land at the address. A T& parameter of a mod function is an out-parameter in the Blueprint. | Yes |
| <a id="references-to-memory.int32-get-int32-p-return"></a>`int32 &Get(int32 *P) { return *P; }` | Returns the address P holds, so `Get(P) = 5` writes there and `Get(P)` reads there. Reference returns are in [Functions](#parameters-and-return-values). | Yes |

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
| <a id="objects-and-addresses.int64-obj"></a>`(int64)Obj`, `(uint8 *)Obj` | The object's address, as an int64 or a byte pointer: the reference's 8 bytes, reinterpreted. `__AddrOf__(Obj)` is the explicit form. | Yes |
| <a id="objects-and-addresses.aactor-addr"></a>`(AActor *)Addr` | An int64 reinterpreted as an object reference. Nothing checks that an object lives there. | Yes |
| <a id="objects-and-addresses.aactor-0"></a>`(AActor *)0`, `(AActor *)5` | The literal 0 is null. Any other integer literal is widened to int64 first. | Yes |
| <a id="objects-and-addresses.aactor-n"></a>`(AActor *)N` with an int32 N | Refused with "an int becomes a pointer through int64". Store the value in an int64 variable first: `int64 Addr = N;` then `(AActor *)Addr`. `(AActor *)(int64)N` is refused too. | Refused |

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
| <a id="sizeof-and-alignof.sizeof-fvector"></a>`sizeof(FVector)`, `sizeof(*P)` | The type's size in the game, folded to a constant: 12 for FVector. This is not the size clang gives the SDK's stand-in type. Pointer arithmetic uses the same sizes. | Yes |
| <a id="sizeof-and-alignof.alignof-ftransform"></a>`alignof(FTransform)` | The type's alignment in the game, folded to a constant: 16 here. | Yes |
| <a id="sizeof-and-alignof.sizeof-aactor"></a>`sizeof(AActor)` | An object class by value has no layout the compiler knows. Refused with "unimplemented struct member type: AActor". `sizeof(AActor *)` is 8. | Refused |

Notes:

- The known layouts: the integer types, float, double, bool, FName (8), FString (16), FText (24), any pointer (8),
  enums, TArray and TScriptInterface (16), TSet and TMap (80), soft pointers (40), TSubclassOf (8), engine structs,
  and mod structs, laid out with each member at its natural alignment.
- sizeof and alignof work in code. A member default does not accept them: see
  [Classes and variables](#classes-and-variables).

### Where pointer code runs

| You write | What it does | Status |
|---|---|---|
| <a id="where-pointer-code.while-q-0-q"></a>`while (*Q != 0) { ++Q; }` | A condition that reads memory is read again before every iteration, as in C++. The compiler writes the loop as `while (true) { if (!Cond) break; ... }`. | Yes |
| <a id="where-pointer-code.value-p"></a>`Value = *P;` in a function that also calls `Delay(1.0f)` | Reads when the line runs. A function that waits runs in a generated event graph, and its locals, P included, live in that graph's frame, so a read after the wait uses the same P. Writes and inline functions work the same way. | Yes |

```cpp
int64 Addr = 0;
int64 Value = 0;

void Poll() {
  Value = *(int64 *)Addr;    // now
  UKismetSystemLibrary::Delay(1.0f);
  Value = *(int64 *)Addr;    // a second later
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

- The memory can change while the function waits, as it can in C++ between two reads. Latent functions are in
  [Latent calls](#latent-calls).
- Reads and writes are not checked. The VM does no bounds or validity check, so a wrong address reads garbage or
  raises an access violation in the game process. There is no soft failure. Check what you can first: that an object
  is valid, and that a property exists and has the type you expect.

### The FDeref scratch struct

| You write | What it does | Status |
|---|---|---|
| <a id="the-fderef-scratch.p"></a>`*P`, `P[I]`, `P->X` | Nothing to declare. Every read or write through a pointer goes through a 16-byte scratch struct, FDeref (`int64 Data; int32 Num; int32 Max;`). The compiler creates it on first use and cooks it as FDeref.uasset in the mod's package, beside the class. | Yes |
| <a id="the-fderef-scratch.struct-fderef-ue-struct-int64"></a>`struct FDeref { UE_STRUCT; int64 Data; int32 Num; int32 Max; };` | A struct the mod declares under this name is used instead, and none is created. | Yes |

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
| <a id="helper-code-you.obj-member"></a>`&Obj->Member` | Calls the Blueprint function `GetPropertyAddress` on the class named ReadProperty that the source can see. | Yes |
| <a id="helper-code-you.ftext-addr"></a>`*(FText *)Addr`, `__ReadText__(Addr)` | Imports the struct `/Game/_ElytrasMods/ReadProperty/FDerefTextView`, a path the compiler hard-codes. | Yes |
| <a id="helper-code-you.classof-x"></a>`__ClassOf__(x)` | Calls GetParmClassName on the class that uses it: see [Intrinsics](#intrinsics). | Yes |

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
| <a id="reading-memory.read64-a"></a>`__Read64__(A)` | Reads 8 bytes at the int64 address A, as an int64. | Yes |
| <a id="reading-memory.read32-a"></a>`__Read32__(A)` | Reads 4 bytes as an int32. | Yes |
| <a id="reading-memory.readfloat-a"></a>`__ReadFloat__(A)` | Reads 4 bytes as a float. | Yes |
| <a id="reading-memory.readbyte-a"></a>`__ReadByte__(A)` | Reads 1 byte as a uint8, 0 to 255. | Yes |
| <a id="reading-memory.readobject-a"></a>`__ReadObject__(A)`, `__ReadClass__(A)` | Reads 8 bytes as a UObject* or a UClass*. `__ReadClass__` assigns to a UClass* variable without a cast. | Yes |
| <a id="reading-memory.readname-a"></a>`__ReadName__(A)` | Reads 8 bytes as an FName. | Yes |
| <a id="reading-memory.readstring-a"></a>`__ReadString__(A)` | Copies the 16-byte FString at A. | Yes |
| <a id="reading-memory.readtext-a"></a>`__ReadText__(A)` | Copies the 24-byte FText at A. Needs the FDerefTextView helper struct: see [Pointers and memory](#pointers-and-memory). | Yes |
| <a id="reading-memory.read32-p"></a>`__Read32__(P)` with `int32 *P` | Every read also takes a pointer, and reads at the address it holds. | Yes |
| <a id="reading-memory.addrof-obj"></a>`__AddrOf__(Obj)` | The object's address as an int64, the same as `(int64)Obj`. | Yes |
| <a id="reading-memory.nameindex-name"></a>`__NameIndex__(Name)` | The FName's comparison index, its first 4 bytes, as an int32. A cheap identity to compare with a name read from memory. It ignores the name's number suffix. | Yes |

```cpp
#include "../include/Intrin.h"   // AssetGen's include/Intrin.h

UClass *ClassOfObject(UObject *Obj) {
  int64 Addr = __AddrOf__(Obj);      // the object's address, as (int64)Obj gives
  return __ReadClass__(Addr + 16);   // UObject::ClassPrivate, at 0x10 in UE 4.27
}
```

Notes:

- The reads work exactly like `*P` on the matching pointer type, with the same rules: nothing checks the address.

### __PtrCast__

| You write | What it does | Status |
|---|---|---|
| <a id="ptrcast.ptrcast-int64-obj"></a>`__PtrCast__<int64>(Obj)` | The object's address. | Yes |
| <a id="ptrcast.ptrcast-aactor-addr"></a>`__PtrCast__<AActor *>(Addr)` | The object at an address. | Yes |
| <a id="ptrcast.ptrcast-int32-addr"></a>`__PtrCast__<int32 *>(Addr)` | The address as a pointer, which is still an int64. | Yes |
| <a id="ptrcast.ptrcast-int32-addr-2"></a>`__PtrCast__<int32 &>(Addr)` | The int32 at Addr, as a value you can read and assign to. | Yes |
| <a id="ptrcast.ptrcast-int64-r"></a>`__PtrCast__<int64>(R)` with `int32 &R = *P` | The address R keeps, not the value there. | Yes |
| <a id="ptrcast.ptrcast-int64-r-2"></a>`__PtrCast__<int64>(R)` with `int32 &R = Local` | A reference to a Blueprint variable has no address. Refused with "a reference to a Blueprint variable, which has no address". | Refused |

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
| <a id="refat.refat-addr"></a>`__RefAt__(Addr)` | Passes the memory at Addr by reference to an engine function whose native code steps its wildcard argument itself (a CustomThunk). The function sees Addr, not the address of a temporary copy. Read as a plain value, it is 8 bytes of int64. | Yes |

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
| <a id="classof.classof-out"></a>`__ClassOf__(Out)` | The property class name (IntProperty, FloatProperty, ...) of a parameter or local of the current function, found at run time. It compiles to `GetParmClassName(<this function>, FName("Out"))` on the class that uses it; Out itself is not evaluated. Slower than a string literal, but it follows a rename or a retype. | Yes |
| <a id="classof.classof-obj-field"></a>`__ClassOf__(Obj->Field)` | Refused with "argument must be a bare parameter or local reference". | Refused |

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
| <a id="asm.asm-0b"></a>`__Asm__("0B");` | Appends the hex bytes to the function's bytecode unchanged (0x0B is EX_Nothing). Whitespace, commas and underscores are skipped, and case does not matter. A statement only. | Yes |
| <a id="asm.asm-0b-1"></a>`__Asm__("0B", 1);` | The same, with the VM memory size the bytes stand for. Pass it for a token that carries an FName, an object reference or a jump target. | Yes |

Notes:

- The one-argument form advances the VM memory offset, which jump targets count, by the byte length. That is right
  only for tokens whose on-disk and in-memory forms are the same size. A wrong size shifts every later jump target.
- The bytes are not checked. Invalid bytes still cook, and the VM runs them.
- A malformed string, such as an odd digit count or the `x` of `0x0B`, compiles with no error or warning, and the
  statement emits nothing. Write bare hex digits, two per byte, and check the result with `tools/walkscript.py`.

### __EmbedFile__

| You write | What it does | Status |
|---|---|---|
| <a id="embedfile.tarray-uint8-payload-embedfile"></a>`TArray<uint8> Payload = __EmbedFile__("data/blob.bin");` | Bakes a file's bytes into a `TArray<uint8>` member's default at build time: see [Classes and variables](#classes-and-variables). In a function body it is refused as an "unimplemented intrinsic". | Yes |

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
| `constexpr` function | Not run at build time. A method is an ordinary Blueprint function, and a free one a function of the source's library. For build time, write `consteval`. | [Inline functions and templates](#inline-functions-and-templates) |
| Constructor, `MyMod() { ... }` | Read as the class's defaults, never run: its member initializers and assignments as initializers, its other statements as UE_DEFAULTS. One with parameters is refused, and so is a destructor that does something. | [Constructors](#constructors) |
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
| `FKey{"F5"}`, `FKey("F5")` | A key for the player controller's key queries, such as IsInputKeyDown. | [Timers and input](#timers-and-input) |
| `FLatentActionInfo` | Leave it out of a latent call. The compiler supplies it. | [Latent calls](#latent-calls) |
| `for (auto &X : Items)`, `for (auto &[Key, Value] : Map)`, `for (auto &P : Map)` | Range-for: a TArray by index, a TSet and a TMap over their own slots, a TMap by key and value or as a pair. | [Loops](#loops) |
| FString methods, `S.Len()` | UKismetStringLibrary statics as methods, the string first: `S.Len()` is `UKismetStringLibrary::Len(S)`. | [Strings and text](#strings-and-text) |
| `GetClass()`, `GetName()` | Get Class and Get Object Name, on this or any object. | [Working with other objects](#working-with-other-objects) |
| `GetOuter()` | The object's outer, read from memory. Not on a null object. | [Working with other objects](#working-with-other-objects) |
| `GetOutermostTypedOuter<T>(Obj)` | The farthest outer of Obj that is a T, or null. | [Working with other objects](#working-with-other-objects) |
| `GetParmClassName` | The method that `__ClassOf__` calls on the class that uses it. You define it, with exactly two parameters. | [Intrinsics](#intrinsics) |
| `GetSubsystem<T>()` | The same as `T::Get()`. | [Working with other objects](#working-with-other-objects) |
| `GetTypedOuter<T>(Obj)` | The nearest outer of Obj that is a T, or null. | [Working with other objects](#working-with-other-objects) |
| `if (Obj)`, `!Obj` | Is Valid: false for null and for an object that is being destroyed. | [Working with other objects](#working-with-other-objects) |
| `if (X)` on a value that is not a bool | An int32 goes through ToBool (integer), a float, int64 or byte is compared with zero, and an FName is tested against None. | [Statements and control flow](#statements-and-control-flow) |
| `InitCave`, `InitSpacerig` | The class names the game spawns from a mod: InitSpacerig in the Space Rig, InitCave in a mission. | [Running in the game](GUIDE.md#running-in-the-game) |
| `inline` functions and templates | Expanded at each call like an editor macro, with no Blueprint function of its own. A template is expanded, `inline` or not. | [Inline functions and templates](#inline-functions-and-templates) |
| Free function, `int32 Twice(int32 V) { ... }` | Without `inline`, a Blueprint function of `BPL_<source>`, the function library the compiler generates for the source. | [Free functions](#free-functions) |
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
| `[[gnu::always_inline]]`, `__attribute__((always_inline))` | On a function library's static: calls this source makes to it are copied in, the function still cooked. Elsewhere it changes nothing; those calls are copied in anyway. | [Calling your own functions](#calling-your-own-functions) |
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
| `T::StaticClass()` | The class as a constant, and as a default. On a mod class, that class, its name qualified or not (`Ns::T::StaticClass()`). | [Types](#types), [Creating objects](#creating-objects), [Classes and variables](#classes-and-variables) |
| `StaticClassOf<T>()` | `T::StaticClass()` on a class only declared so far. | [Creating objects](#creating-objects) |
| `TSubclassOf<T>`, `UClass *` | Class References. `TSubclassOf<T>` takes T and its subclasses. | [Types](#types) |
| `UE_ASSET_ALL(Class)` | Declares `All`: soft pointers to every UE_ASSET_AT of Class or a subclass in this namespace and the ones inside it, filled in at build time. | [Game assets](#game-assets) |
| `UE_ASSET_AT(Class, Name, Path)` | Names an asset that another package holds, the game's or another mod's, so that `&Name` points at it. | [Game assets](#game-assets) |
| `UE_ASSET_EDIT(Asset) { .Member = value };` | Edits a game asset that a UE_ASSET_AT names: the members the braces name, written into the asset's own package. | [Editing game packages](#editing-game-packages) |
| `UE_ASSET_EDIT__`, `UE_ASSET_EDIT__CAT`, `UE_ASSET_EDIT__CAT2` | Internal to UE_ASSET_EDIT and UE_ASSET_EDITS. Not written in a mod. | [Editing game packages](#editing-game-packages) |
| `UE_ASSET_EDITS { Asset.Member[1].Field = value; }` | Edits game assets by path: each statement changes one member, or one part of a member's value. | [Editing game packages](#editing-game-packages) |
| `UE_AUTHORITY_ONLY` | Authority Only (BlueprintAuthorityOnly): on an actor or its component, a call on a machine without authority is skipped; an engine or game static with this mark, on a client. | [RPCs](#rpcs) |
| `UE_AWAIT(Dispatcher)` | The rest of the function runs when the dispatcher next fires. Its value is the dispatcher's parameter when it has exactly one; two or more are bound one name each, `auto [Who, Points] = UE_AWAIT(OnScored);`. | [Waiting on events](#waiting-on-events) |
| `UE_CATEGORY("Setup\|Doors")` | The editor category of the functions and variables that follow it, `\|` starting a subcategory. It is written into the editor API stub only. | [Classes and variables](#classes-and-variables) |
| `UE_CATEGORY__JOIN`, `UE_CATEGORY__JOIN2` | Internal to UE_CATEGORY. Not written in a mod. | [Classes and variables](#classes-and-variables) |
| `UE_CLASS(Package, UeName)` | The package and UE name of a class. In a shared header it names the one source that cooks the class, the one whose UE_MOD_PACKAGE, namespaces and class name give exactly that path; every other source imports it. The SDK declares each engine and game class with it. | [Mod sources and packages](#mod-sources-and-packages) |
| `UE_CLIENT` | Run on owning Client RPC: called on the server, it runs on the client that owns the actor. | [RPCs](#rpcs) |
| `UE_COLOR_CURVE(Name, Keys...)` | A UCurveLinearColor asset of the mod's own, at namespace scope, keys `(time, r, g, b, a)`. | [Timelines](#timelines) |
| `UE_COLOR_TRACK(Timeline, Name, Keys...)` | A linear color track of a UE_TIMELINE: declares `FLinearColor Name`, which the timeline sets each frame it plays, and its curve, keys `(time, r, g, b, a)`. | [Timelines](#timelines) |
| `UE_COLOR_TRACK_CURVE(Timeline, Name, Curve)` | A color track of a UE_TIMELINE over a UCurveLinearColor asset, the editor's External Curve: declares `FLinearColor Name`. | [Timelines](#timelines) |
| `UE_COMPONENT(Type, Name)` | A component of a mod actor, of an engine, game or mod component class, as added in the Components panel. The first scene component is the root. | [Components](#components) |
| `UE_COOKED` | In a class of the mod's own folder: the class ships as the cooked package it was built from (bpbuild `kept:`), so it is declared for the rest of the mod to name, derive from and call, and imported, never cooked. bpdecomp marks a class AssetGen cannot write yet with it, and the reason. | [Mod sources and packages](#mod-sources-and-packages) |
| `UE_COSMETIC` | Cosmetic (BlueprintCosmetic): on an actor or its component, and for an engine or game static with this mark, a call on a dedicated server is skipped. | [RPCs](#rpcs) |
| `UE_DEFAULTS { ... }` | Defaults of components and of inherited properties, as the Details panel and Class Defaults set them. Read at build time, never run. | [Class defaults](#class-defaults) |
| `UE_DISPATCHER(Name, Params...)` | An event dispatcher on a mod class, with its parameter list. | [Event dispatchers](#event-dispatchers) |
| `UE_ENUM(Enum)` | Cooks an `enum class` based on `uint8`, `int32` or `int64` as a UserDefinedEnum (an Enumeration asset). | [Enums](#enums) |
| `UE_ENUM_IN(Enum, Package)` | UE_ENUM for an enum in a shared header: only the source whose UE_MOD_PACKAGE is exactly Package cooks it. | [Enums](#enums) |
| `UE_ENUM_MAP(Key, Value, Name)`, `UE_ENUM_MAP(Enum)` | A TMap member from each enumerator to its name, or back, filled in at build time: declared by the three-argument form, the default alone by the one-argument form. | [Enums](#enums) |
| `UE_FORCE_BROADCAST(Dispatcher)` | `.Broadcast(...)` on it broadcasts a native dispatcher that is not BlueprintCallable, which the editor refuses, with a warning. | [Event dispatchers](#event-dispatchers) |
| `UE_FINAL_AS(Base, Leaf)` | Declares `class Leaf final : public Base {}`, Base's one subclass: Base is compiled as final and cooked Abstract. | [Classes and variables](#classes-and-variables) |
| `UE_GAME_STRUCT(Package, Name)` | genueapi's marker on a game Blueprint's struct: imported from Package as the UserDefinedStruct Name, never cooked. | [Structs](#structs) |
| `UE_FLOAT_CURVE(Name, Keys...)` | A UCurveFloat asset of the mod's own, at namespace scope, keys `(time, value)` as a track's. `&Name` points at it. | [Timelines](#timelines) |
| `UE_FLOAT_TRACK(Timeline, Name, Keys...)` | A float track of a UE_TIMELINE: declares `float Name`, which the timeline sets each frame it plays, and its curve, keys `(time, value)` with an optional Cubic or Constant. | [Timelines](#timelines) |
| `UE_FLOAT_TRACK_CURVE(Timeline, Name, Curve)` | A float track of a UE_TIMELINE over a UCurveFloat asset, the editor's External Curve: declares `float Name`. | [Timelines](#timelines) |
| `UE_INPUT_ACTION("Action", Options...)` | Makes the method after it an InputAction event of an actor: the engine calls it on that action of the game's input settings. | [Timers and input](#timers-and-input) |
| `UE_INPUT_KEY("Key", Options...)` | Makes the method after it an InputKey event of an actor: the engine calls it on that key, pressed by default; the options name the edges, modifiers and the node's other settings. | [Timers and input](#timers-and-input) |
| `UE_INTERFACE` | Declares a mod interface, cooked as a Blueprint Interface asset. Its variables go to the classes that implement it. | [Interfaces](#interfaces) |
| `UE_KEYWORDS("Words")` | Search keywords of the method after it, written into the editor API stub only. | [Classes and variables](#classes-and-variables) |
| `UE_MOD_PACKAGE(Path)` | The /Game folder that a source's classes, structs, enums, interfaces and assets are cooked into. A namespace is a subfolder. | [Mod sources and packages](#mod-sources-and-packages) |
| `UE_MULTICAST` | Multicast RPC: called on the server, it runs on the server and on every client. | [RPCs](#rpcs) |
| `UE_NAME_CASE("Text")` | A case of UE_NAME_SWITCH. Every case of one must be written this way. | [Statements and control flow](#statements-and-control-flow) |
| `UE_NAME_SWITCH(Name)` | `switch` on an FName, the editor's Switch on Name. | [Statements and control flow](#statements-and-control-flow) |
| `UE_NO_OPTIMIZE` | Compiles the function as written: unused pure calls, unread locals and constant branches stay. | [The optimizer](#the-optimizer) |
| `UE_PATCH` | Makes the class a patch of the game Blueprint it derives from: its UE_DEFAULTS and methods are written into that Blueprint's own package. | [Editing game packages](#editing-game-packages) |
| `UE_PURE` | A pure function (BlueprintPure), drawn without exec pins. It must return a value. | [Functions](#functions) |
| `UE_RELIABLE` | Makes a UE_SERVER, UE_CLIENT or UE_MULTICAST RPC reliable. Refused on a method that is not an RPC. | [RPCs](#rpcs) |
| `UE_REPLICATED(Type, Name)` | A replicated variable. The initializer after the macro is its default, and the class gets bReplicates. | [Replication](#replication) |
| `UE_REPLICATED_IF(Type, Name, Cond)` | A replicated variable with a replication condition, an ELifetimeCondition name with or without `COND_`. | [Replication](#replication) |
| `UE_REPLICATED_USING(Type, Name, OnRep)` | A RepNotify variable. OnRep runs when a new value arrives, and right after the mod's own writes, as the Set w/ Notify node does. | [Replication](#replication) |
| `UE_REPLICATED_USING_IF(Type, Name, OnRep, Cond)` | RepNotify and a replication condition on one variable. | [Replication](#replication) |
| `UE_SERVER` | Run on Server RPC: called on the owning client, it runs on the server. | [RPCs](#rpcs) |
| `UE_STRUCT` | Cooks a struct as a UserDefinedStruct (a Structure asset). | [Structs](#structs) |
| `UE_TIMELINE(Name, Length, Options...)` | A Timeline of a mod actor: declares `UTimelineComponent *Name`, the component the engine makes from the template as the actor spawns. The options are the template's AutoPlay, Loop, Replicated, IgnoreTimeDilation and LastKeyframe. | [Timelines](#timelines) |
| `UE_TIMELINE_EVENT(Timeline, Times...)` | Makes the method after it an event track of the timeline: called each time the timeline passes one of the times. | [Timelines](#timelines) |
| `UE_TIMELINE_FINISHED(Timeline)` | Makes the method after it the timeline's Finished pin: called when it reaches its end, or its start in reverse. | [Timelines](#timelines) |
| `UE_TIMELINE_UPDATE(Timeline)` | Makes the method after it the timeline's Update pin: called each frame it plays, after its tracks are written. | [Timelines](#timelines) |
| `UE_TOOLTIP("Text")` | The tooltip of the method after it, written into the editor API stub only. | [Classes and variables](#classes-and-variables) |
| `UE_VECTOR_CURVE(Name, Keys...)` | A UCurveVector asset of the mod's own, at namespace scope, keys `(time, x, y, z)`. | [Timelines](#timelines) |
| `UE_VECTOR_TRACK(Timeline, Name, Keys...)` | A vector track of a UE_TIMELINE: declares `FVector Name`, which the timeline sets each frame it plays, and its curve, keys `(time, x, y, z)`. | [Timelines](#timelines) |
| `UE_VECTOR_TRACK_CURVE(Timeline, Name, Curve)` | A vector track of a UE_TIMELINE over a UCurveVector asset, the editor's External Curve: declares `FVector Name`. | [Timelines](#timelines) |
| `UE_CLASS_IN(ModPackage)` | UE_CLASS for a mod's own class, named by its owner's UE_MOD_PACKAGE: the package is ModPackage plus namespaces plus the class name, the UE name the class name plus `_C`. | [Mod sources and packages](#mod-sources-and-packages) |
| `UE_STRUCT_IN(Package)` | UE_STRUCT for a struct in a shared header: only the source whose UE_MOD_PACKAGE is exactly Package cooks it. | [Structs](#structs) |
| `UE_WIDGET(Type, Name)` | A widget of a UUserWidget class's designer tree and a variable of the class; the first is the root. The constructor's AddChild calls and setters lay the tree out. | [Widget layouts](#widget-layouts) |
| `UE_SLOT(Widget->Slot, Content)` | In a widget class's constructor: Content fills the named slot Slot of the nested user widget Widget. | [Widget layouts](#widget-layouts) |
| `UE_BIND(Widget, Property, &Class::Function)` | In a widget class's constructor: Widget's Property is read from Function each time it is drawn. | [Widget layouts](#widget-layouts) |
| `UeAssets::<Class>::All` | Every game asset of that class, as soft pointers. | [Game assets](#game-assets) |
| `UeAssets::<Class>::Game::...::<Name>` | A game asset by its content path, for `&` to point at. | [Game assets](#game-assets) |
| `TEnum<E>` | An enum that is E to the compiler, with `.Name()` and `.String()`. | [TEnum](#tenum) |
| `TEnumMask<E>` | An int32 bitmask over E, bit N for the enumerator of value N, with `.Has()`, `.Add()`, `.Remove()`, `.Clear()`. | [TEnumMask](#tenummask) |
| `using FTarget = AActor;` | An alias of a class. A variable of it is still an object reference. | [Classes and variables](#classes-and-variables) |
| `virtual` | Accepted and ignored: every mod method is called by name, so the most derived one runs. With `final`, see `final`; with `= 0`, see pure virtual. | [Functions](#functions) |
| `WorldContextObject` argument left out | Filled with self, as the editor's hidden pin is; in a static function, with its own world context parameter. An inline helper's own WorldContext parameter left at its default gets the same. | [Calling engine and game functions](#calling-engine-and-game-functions) |

## Diagnostics

When AssetGen refuses a source, it prints one line, and `assetgen compile` exits with code 1:

```text
  FAILED: <message>
```

A refusal raised inside a function starts with `<Class>::<Function>: `, and inside a free function that is not inline
with `BPL_<source>::<Function>: `. One raised inside an inline function's body adds `inline <Class>::<Method>: ` after
that, or `inline <Function>: ` for a free inline function, so the prefix names the function the problem is in. A refusal about a class's variables, or about the source as a whole, has no prefix.
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
- `the source declares no UE_STRUCT, UE_ENUM, class deriving from a UE class, asset or edit`: nothing in the source would be cooked,
  for example a file that holds only inline helper functions. Fix: declare a class that derives from a UE class, a
  UE_STRUCT or a UE_ENUM in it. A file of shared helpers belongs in a header that the mod's sources include, not in the
  mod's `sources` list in `mods.yaml`. See [Mod sources and packages](#mod-sources-and-packages).
- `` <Member>: a default is a value known when the mod is built - a literal, a constant expression over literals, enum
  constants and constexpr variables, a braced struct, an &Asset or a class, X::StaticClass(). A function call counts
  only through `constexpr T k = F();` with F consteval, which clang runs itself. Anything computed when the game runs
  belongs in ReceiveBeginPlay or UserConstructionScript ``: a member's initializer that AssetGen cannot work out when
  it builds the mod: a call such as `int32 Roll = UKismetMathLibrary::RandomInteger(3);`, a call to a plain
  `constexpr` function, `sizeof`, or a class read off an object, `GetClass()`. A value in UE_DEFAULTS gets the same
  message. Fix: use a literal or a constant expression, compute a constant with a `consteval` function through a
  `constexpr` variable, or set the value in ReceiveBeginPlay or UserConstructionScript. See
  [Classes and variables](#classes-and-variables).
- `<Member>: a TSubclassOf<<Class>> holds <Class> or a subclass of it, and <Other> is neither`: a class as the default
  of a `TSubclassOf` that does not take it, `TSubclassOf<APawn> Kind = AActor::StaticClass();`. C++ converts any class
  to a `TSubclassOf`, but the editor's class picker offers only those. Fix: name a class the member takes, or widen the
  member's type. See [Classes and variables](#classes-and-variables).
- `` <Member>: a container default is a braced list, `= { 1, 2 }`, of values a lone member could take ``: a TArray,
  TSet or TMap member initialized with something other than braces, such as a call, `TArray<int32> Ids = MakeIds();`.
  Fix: list the elements in braces, `TArray<int32> Ids = { 1, 2 };`, each a value that a single variable of the element
  type could take as its default, or fill the container in ReceiveBeginPlay. See
  [Classes and variables](#classes-and-variables).
- `<Name> is an asset, not a value: point at it with &<Name>`: a function uses a namespace-scope variable of a UObject
  class by value. That is a braced data asset of the mod (`CT_Hard.Waves`), a variable such as `UMoodDef GlobalDef;`,
  or a game asset named with UE_ASSET_AT used whole rather than through a member (a member of one is read when the mod
  is built, see [Game assets](#game-assets)). Fix: take the address, `(&CT_Hard)->Waves`, or keep `&CT_Hard` in a
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
  checked. A class the mod cooks under a root of its own (`namespace Game::...`, a second /Game folder) is not
  checked: that copy is the one the asset was saved with. Fix: add the UE_CLASS the message prints to the class's
  declaration in the shared header. Every mod but the owner then imports the class instead of cooking it. See [Mod sources and packages](#mod-sources-and-packages) and
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
- `warning: <Class>::<Function>: FRotator == compares rotations, not components: angles 360 degrees apart are equal,
  where C++ compares each component exactly` (for FQuat: `Q and -Q are equal`): `==` or `!=` on two rotators or two
  quaternions. Kismet's Equal node wraps each angle (a quaternion's takes either sign), so `FRotator(0, 360, 0)` equals
  `FRotator(0, 0, 0)`. Fix: nothing, when you mean the same rotation; to compare the numbers, compare the members. See
  [Struct operators](#struct-operators).
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
  container that a function call returns, as in `GetItems().Add(3)` or `GetItems()[0]` (a method that only reads,
  such as `GetItems().Num()`, compiles). `<Method>` is the Blueprint
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
- `<Member>: the engine reads a <Struct> value in its own binary form, which AssetGen does not write yet; leave the value out`:
  a default of an engine struct with a native serializer whose form AssetGen does not know (SoftObjectPath, PerPlatformFloat,
  RichCurveKey and the like). GameplayTagContainer and the Niagara variables are written. Fix: leave the value out.
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
  `UE_ENUM_MAP: <Member> is not a map over an enum` and `UE_ENUM_MAP makes a TMap between an enum and FName or
  FString, not a <Type>`: checks behind clang's and AssetGen's earlier ones, which a
  source rarely reaches. Fix: name the container type of a braced list, `TArray<int32> L = { 1, 2, 3 };`; include the
  header that declares a struct; give an inline constant its value where it is declared; use
  UE_ENUM_MAP only on a `TMap<E, FName>`, `TMap<FName, E>`, `TMap<E, FString>` or `TMap<FString, E>`. See
  [Containers](#containers), [Structs](#structs), [Constants](#constants) and [Enums](#enums).

### Operators, statements and loops

- `bit shift with a non-constant amount (Kismet has no shift op; use explicit multiply/divide)`: Blueprint has no shift
  node. AssetGen turns a shift into a multiply or divide only when the amount is a constant: a literal, a `constexpr`
  or `const` variable, an enumerator, a cast of one, `sizeof`, a template's value parameter. A variable amount, in
  `X << N` or `X <<= N`, gets this message. Fix: make the amount a constant, or multiply or divide by a power of two. See
  [Operators](#operators).
- `bit shift amount out of range: <N>`: a constant amount at or past the width of the left side: more than 31 on an
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
- `a TMap range-for's pair variable is read only as P.Key and P.Value`: a `for (auto &Pair : Scores)` body that uses
  Pair whole, such as passing it to a function or copying it. Fix: read `Pair.Key` and `Pair.Value`, or bind
  `auto& [Key, Value]`. See [Loops](#loops).
- `warning: <Class>::<Function>: unsafe to mutate iterated container (<What>)`: a range-for body adds to or removes
  from the container it walks, or may through a call (`<What>` names it: `Scores.Remove`, `Scores[] =`,
  `Prune(Scores)`, `Prune()`). It compiles as written, and the walk is undefined, as a C++ iterator's is. Fix: walk a
  copy you take first (`Scores.Keys(Keys)`, then `for (const FName &Key : Keys)`), or collect the changes in a local
  and apply them after the loop. See
  [Changing the container a loop walks](#changing-the-container-a-loop-walks).
- `TODO: a structured binding over a <Type>`: a structured binding over a TArray or TSet,
  `for (auto [X, Y] : Points)`. Fix: bind the element and read its members, `for (const FIntPoint &P : Points)` and
  `P.X`. See [Loops](#loops).
- `` a goto inside a TMap range-for that writes its value back would skip the write: bind `const auto& [Key, Value]`,
  or leave with break ``: a goto inside `for (auto& [Key, Value] : Map)` in the form that copies Value out and writes
  it back after each pass. AssetGen uses that form when Value is a container the body changes, and the map's key or
  value aligns past 8 bytes. Fix: bind `const auto&` when the body only reads Value, or leave with
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

- `call to an unknown function: <Name>`: a call to a function AssetGen has no body for. The usual cause is a free
  function declared in this source and defined in another, or a C runtime function such as `sqrtf` or
  `__builtin_sqrtf`, which `std::sqrt` calls too. The message names the caller, not the definition. Fix: define the
  free function in the source that calls it or in a header it includes; to share one between mods, make it a static
  method of a UBlueprintFunctionLibrary class. For math, call the UKismetMathLibrary statics. See
  [Free functions](#free-functions) and [Calling engine and game functions](#calling-engine-and-game-functions).
- `<Name> is overloaded, and a function library has one function per name: give each non-inline overload a name of
  its own, or make all but one inline`: a source calls two free functions of one name, neither `inline`, and each would
  be a Blueprint function of the source's library, which keeps one per name. Fix: rename one, or mark it `inline`. See
  [Free functions](#free-functions).
- `use of undeclared identifier 'FMath'`: clang's message. UE's native FMath is not part of the reflected API, and a
  mod has no C++ standard library to call. Fix: include `AssetGen/include/FMath.h`, whose FMath functions are the
  math nodes, or call UKismetMathLibrary, as in `UKismetMathLibrary::Clamp(V, 0, 10)`. See
  [Calling engine and game functions](#calling-engine-and-game-functions).
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
  `inline` from a class's method, so that it is a Blueprint function any object can run, or write a free function
  that takes the object or the struct as a parameter. See
  [Inline functions and templates](#inline-functions-and-templates).
- ``<Struct>::<Method>: a UE_STRUCT holds no functions, so nothing could run a call to it; write a free inline function that takes the struct``:
  a method of a UE_STRUCT that is not inline. A Blueprint struct has no functions, so a call to it named one that exists
  nowhere. Fix: `inline int32 SumOf(const FPair &P) { return P.A + P.B; }`. See [Functions](#functions).
- `` `Obj-><Class>::<Method>()` runs <Class>'s <Method> on another object without dispatch, which a Blueprint does only as self's parent call ``:
  a call on another object written with a qualifier, `Other->SuperBase::Bump(1)` or `GetOther()->SuperBase::Bump(1)`.
  C++ runs that class's own version;
  by name the call would reach the object's override. Fix: call it by name if the override is what you want, or move
  the code into a function of another name and call that. See [Overrides and parent calls](#overrides-and-parent-calls).
- `<Class>::<Function>: returns a reference to <Name>, a local` (or `a parameter`)`: a Blueprint variable has no
  address to return; ...`: a function returning `T&` whose `return` names a local or a parameter. C++ would dangle
  for a local or a by-value parameter; a `T&` parameter's address is not one a Blueprint can hand out. Fix: return a
  pointer, a value, or a reference into memory, a TArray element or an object's member. See [Functions](#functions).
- `<Class>::<Function>: a reference return gives the address of what it returns: <why>`: the returned expression has
  no address, such as a call's value. `<why>` says what was missing. Fix: as above.
- `warning: <Class>::<Function> returns const <Type>&: a Blueprint function returns a copy`: a `const T&` return. The
  value is right at the call; a caller keeping the reference does not see the original change. Fix: nothing, or
  return a non-const reference or a pointer if the caller must follow the original.
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
  free function. To call something later, bind a method to a dispatcher or a delegate parameter,
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

`UKismetSystemLibrary::PrintString` compiles with no message, but UE 4.27 keeps its body only outside shipping builds,
so the retail game prints nothing. See [Strings and text](#strings-and-text) and
[Calling engine and game functions](#calling-engine-and-game-functions).

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
- `warning: <Class>: a widget class AssetGen cooks has no widget tree unless it declares one with UE_WIDGET, so it
  shows nothing of its own, not even a widget Blueprint parent's layout`: a class that derives `UUserWidget`, or a game
  widget Blueprint, which declares no `UE_WIDGET`, nor does a mod class above it, has no `OnPaint` of its own and whose
  code never sets `WidgetTree->RootWidget`. The engine makes a tree only from a widget Blueprint's class. The build
  goes on. Fix: declare the layout with `UE_WIDGET`, build the tree in `OnInitialized`, or draw in `OnPaint`. See
  [Widget layouts](#widget-layouts).
- `warning: <Class>: the <Event> it inherits from <Parent> never runs, since a widget calls <Event> only for a class that
  overrides it itself, as the editor's widget compiler decides`: `<Event>` is `Tick` or `OnPaint`, which a Blueprint
  class above overrides and this widget class does not. The engine calls it only where the class's own override set
  its flag on the class default object. The build goes on. Fix: override `<Event>` in `<Class>` and call
  `<Parent>::<Event>`. See [Parent classes](#parent-classes).
- `warning: <Class>::UE_DEFAULTS: bHasScriptImplementedTick follows whether the class overrides Tick itself, as the
  editor's widget compiler sets it on every compile, so this line changes nothing; drop it`, and the same for
  `bHasScriptImplementedPaint` and `OnPaint`, or with `<Class>::<Class>()` for a constructor: a widget class's
  `UE_DEFAULTS` or constructor sets one of the two flags to the value AssetGen gives it anyway. The build goes on. Fix:
  drop the line.
- `<Class>::UE_DEFAULTS: bHasScriptImplementedTick follows whether the class overrides Tick itself, as the editor's
  widget compiler sets it on every compile; override Tick for it to run`, or `...; drop the Tick override to turn it
  off`, and the same for `bHasScriptImplementedPaint` and `OnPaint`: the line sets the flag to the other value than
  the class's own events give it, which the compile would set back. Fix: override the event for it to run, or drop the
  override for it not to.
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
- `` <Class>::UE_DEFAULTS: every statement is `Field = value;`, `Component->Field = value;` or `Component->SetupAttachment(Parent);`, on this object's members or the members of a struct they hold, `Field.X = value;` ``:
  a statement in UE_DEFAULTS is not a plain `=` onto a member of this object or a part of one, nor a SetupAttachment:
  a call such as `K2_DestroyActor();`, `Health += 5;`, a local, an `if`, or
  another object's member (`GetOwner()->InitialLifeSpan = 5.0f;`). The block never runs; AssetGen only reads its
  assignments and attachments. A value computed at run time is refused with the
  member-default message that starts `<Member>: a default is a value known when the mod is built`. Fix: keep only
  assignments of build-time values in UE_DEFAULTS, a native struct (Vector, LinearColor) given whole, and move the rest to ReceiveBeginPlay or
  UserConstructionScript. See [Class defaults](#class-defaults).
- `<Struct>: a UE_STRUCT has no UE_DEFAULTS, so its statements would be dropped; give each variable its initializer`,
  and the same for a UE_INTERFACE: neither cooks a default object of its own. Fix: give each variable its
  initializer; set a variable an interface inherits in the UE_DEFAULTS of a class that implements it. See
  [Class defaults](#class-defaults).
- `<Class>::<Class>(<Parameters>): the engine makes a Blueprint class's objects with no arguments, so this constructor
  is never called`: a mod class's constructor takes parameters. Fix: drop it; a constructor that takes none is read as
  the class's defaults. See [Constructors](#constructors).
- `<Class>::<Class>() has no body in the class`: the constructor is defined out of the class, or nowhere. AssetGen
  reads it in the class alone, as UE_DEFAULTS. Fix: write its body in the class. See [Constructors](#constructors).
- `<Class>::<Class>(): a constructor never runs in the game, so its statements are read as the class's defaults`: a
  statement that is not one UE_DEFAULTS takes, such as a call, a local or `Home.Z = 5.0f;`. Fix: move run-time setup
  to ReceiveBeginPlay, and set a struct whole. See [Constructors](#constructors).
- `<Class>::<Class>(): a UE_STRUCT's constructor sets its own members, the only defaults it has`, and the same for a
  UE_INTERFACE: the constructor sets something else, such as a variable of an interface the interface extends. Fix:
  set that in the UE_DEFAULTS of a class that implements it. See [Constructors](#constructors).
- `<Class>::~<Class>(): a destructor never runs in the game, which frees the object without it`: a destructor with a
  body, or one defined out of the class. Fix: drop it, and do teardown in ReceiveEndPlay. See
  [Constructors](#constructors).
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
  Fix: regenerate UeApi with genueapi from a dump
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
- `<Where>: an edit starts from the game's own package: pass the folder /Game is in (assetgen compile --game
  <extracted pak>/FSD/Content; bpbuild: game_content in mods.yaml)`: a `UE_ASSET_EDIT`, `UE_ASSET_EDITS` or
  `UE_PATCH` compiled without `--game`. Fix: pass the extracted pak's `Content` folder. See
  [Editing game packages](#editing-game-packages).
- `UE_ASSET_EDIT(<Asset>): the target is a UE_ASSET_AT (every asset in UeAssets/ is one); a mod's own asset takes its
  values where it is declared`, and `UE_ASSET_EDITS: <Asset> is not a UE_ASSET_AT ...`: an edit of an asset the mod
  cooks itself. Fix: set the value in that asset's own braces. See [Editing game packages](#editing-game-packages).
- `... <Array> has <N> elements`: a path's index past the end of the array the game's package holds. Fix: index
  an element the array has, or assign the whole array. See [Editing game packages](#editing-game-packages).
- `<Where>: <Member> is not set on <Object> (it takes the archetype's), and the path goes through a native struct or an
  element, whose other parts are unknown here: assign the whole of <Member>`: the object has no value of the member,
  so the rest of a native struct or an element around the path is unknown. Fix: assign the whole member. See
  [Editing game packages](#editing-game-packages).
- `<Where>: <Path>: only a TArray's element can be assigned on its own; assign the whole value`: a path step `[...]`
  into a value that is no array. Fix: assign the whole value. See [Editing game packages](#editing-game-packages).
- `<Where>: <Path>: a map's value is assigned whole, `` `Map[Key] = value;` ``: a path past a map's key,
  `Score["a"].X = 1`. Fix: assign the element's whole value. See [Editing game packages](#editing-game-packages).
- `<Where>: <Path>: an element of a map or set inside an array's element or a native struct cannot be changed on its
  own yet; assign the whole element`: `Arr[0].Score["a"] = 1`. Fix: assign the array's whole element. See
  [Editing game packages](#editing-game-packages).
- `<Where>: <Member>: the value the object holds does not read (<why>), so an element cannot be merged into it`: the
  game's map or set is in a form AssetGen's reader does not hold (a struct key written natively, ...). Fix: assign the
  whole container. See [Editing game packages](#editing-game-packages).
- `<Asset>.<Path>: the game's <Object> sets no value of its own, so it holds <Class>'s C++ default, which no package
  holds; read it when the game runs, through (&<Asset>)`: a member read when the mod is built that the asset, and its
  archetypes up to a native class, leave unset. Fix: read it at run time, `(&Asset)->Member`. See
  [Game assets](#game-assets).
- `<Asset>.<Path>: the game's package holds a <Type> there, where UeApi declares a <Type>: regenerate UeApi from the
  same game build as the extracted pak` (and `the game's value is <E::Name>, which UeApi's <E> does not declare`): the
  SDK and the extracted pak come from different game builds. Fix: regenerate UeApi, or extract the pak again. See
  [Game assets](#game-assets).
- `<Asset>.<Path>: only a number, a bool, an enum, a name or a string is read when the mod is built, and this is a
  <Type>; read it when the game runs, through (&<Asset>)`, `<Asset>.<Path>: the game's array has <N> elements`,
  `<Asset>.<Path>: <Package> is not in <folder> (an asset of a mod pak has no package there)`: a value that cannot be
  read when the mod is built. Fix: read it at run time through the pointer. See [Game assets](#game-assets).
- `<Asset>.<Path>: this mod's edit changes the value read when the mod is built, and the game loads the edited one:
  read it when the game runs, through the asset's pointer, or use the edit's value itself`. Fix: as it says. See
  [Game assets](#game-assets).
- `<Asset>.<Path>: a game asset's value is read off its package when the mod is built: pass the folder /Game is in
  (assetgen compile --game <extracted pak>/FSD/Content; bpbuild: game_content in mods.yaml)`. Fix: pass it. See
  [Game assets](#game-assets).
- `<Class> (UE_PATCH): a patch derives from the game Blueprint it edits (a class in a /Game package); a native class's
  defaults come from its C++ constructor, not from a package`: a patch of an engine or game C++ class. Fix: patch a
  game Blueprint, or derive a class of your own. See [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH): a patch edits its parent's defaults and replaces its functions; a member or an interface of its
  own is not built yet`: a patch that declares a variable or implements an interface. Fix: keep the state in another
  class of the mod. See [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH): nothing to edit - a patch's defaults go in UE_DEFAULTS { Field = value; }, and its methods
  replace the Blueprint's functions of the same name`: a patch with neither. See
  [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH): <Package> holds no <Class>:<Component>_GEN_VARIABLE - the class does not override that
  inherited component. Patch the Blueprint that declares it instead (every child that does not override it changes
  too); adding an override record here is not built yet`: a parent Blueprint's component this Blueprint never
  overrides. Fix: patch the parent that declares it. See [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH): <Component> is not one of <Blueprint>'s components (UeApi gives it no SCS node)`: a
  `Comp->Field` through a member that is not a component the Blueprint adds. Fix: set it on a component, or set the
  member itself. See [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH)::<Function>: its parameters <ours> are not the game function's <theirs>; a replacement takes
  the same ones, in the same order`: a replacement whose parameters differ in type, name or direction. Fix: copy them
  from the SDK header. See [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH)::<Function>: <Blueprint> has no function of that name of its own, though its declaration has
  one: the declaration is not the game's`: the SDK header and the extracted package come from different game builds.
  Fix: regenerate the SDK, or extract the pak again. See [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH)::<Member>: a patch's own member does not replicate: it would renumber the class's replicated
  properties against an unpatched client's or server's; leave out UE_REPLICATED`. Fix: keep the member unreplicated,
  or send it with an RPC of the game's. See [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH)::<Member>: a patch adds no component or widget to the game's class` / `a patch adds no
  dispatcher to the game's class`: the Blueprint's construction script and widget tree stay the game's. Fix: spawn the
  component at run time, or declare a class of your own. See [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH)::<Member>: <Blueprint> already has a variable <Name>; rename it`: the game's package has a
  variable of that name that the SDK header does not list. Fix: rename the member. See
  [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH): <Blueprint> has a function <Fn> already, which the interface the patch adds would replace with
  an empty one`: an interface function the patch leaves out. Fix: write the method in the patch; `Parent::<Fn>()` in it
  runs the game's. See [Editing game packages](#editing-game-packages).
- `<Class> (UE_PATCH): <Blueprint> implements <Interface> already`. Fix: leave the interface out of the patch's bases.

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
- `<Function>: K2_SetTimer by name <Name> names no function of the class` (or `names an inline method`, `names a
  function that takes parameters`, or `names a function that returns a value`)`, so the engine sets no timer`: the
  engine finds the function by name when the timer is set, and only calls one with no parameters and no return
  value. Fix: name a void method of the class that takes none, or pass a delegate with `K2_SetTimerDelegate`.
- `` a delegate cannot bind <Function>: an inline function is expanded where it is called, no UFunction (drop `inline`) ``:
  Add, Remove or a delegate value names an inline method. An inline method is copied into each caller and never
  becomes a function of the class. Fix: drop `inline` from the handler. See [Event dispatchers](#event-dispatchers).
- `<Dispatcher>.Broadcast: <Dispatcher> is a native dispatcher (<Class>::<Dispatcher>) that is not BlueprintCallable,
  and the editor's Call node refuses one ("Event Dispatcher is not 'BlueprintCallable'");
  UE_FORCE_BROADCAST(<Dispatcher>).Broadcast(...) broadcasts it anyway, with a warning`: `OnDestroyed.Broadcast(this)`
  and the like; none of the engine's dispatchers is BlueprintCallable, 41 of the game's are. Fix: call what the engine
  calls to make the event happen (`K2_DestroyActor()`), or, to run the bound handlers without the event, write
  `UE_FORCE_BROADCAST(<Dispatcher>).Broadcast(...)`. See [Broadcast](#broadcast).
- `UE_FORCE_BROADCAST(<Dispatcher>).<Method>: UE_FORCE_BROADCAST is for Broadcast only`: the marker on Add, Remove or
  Clear. Fix: drop it there.
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
- `no member named 'BindKey' in 'UInputComponent'`: clang's message. BindKey and BindAction are native C++ only and
  absent from the SDK. Fix: mark the method, `UE_INPUT_KEY("F5") void OnF5();`, or poll the player controller in
  ReceiveTick. See [Timers and input](#timers-and-input).
- `<Class>::<Method>: UE_INPUT_KEY("F5") on a class that is no actor`: only an actor gets input. Fix: mark a method
  of an actor, such as the component's owner, and have it call the component. See [Timers and input](#timers-and-input).
- `... on a static method`, `... on an inline method`, `... on a method with no body`: the engine calls the marked
  method on the actor by its name, so it must be a function of the object. Fix: drop `static` or `inline`, or give the
  method its body. See [Timers and input](#timers-and-input).
- `... on a method returning <Type>`, `... on a method taking (<Types>)`: the engine hands the method the key and
  nothing else, and would hand those bytes to any other parameter or result. Fix: return void and take nothing, or
  `FKey Key`. See [Timers and input](#timers-and-input).
- `... has an unknown option <Option>`, `UE_INPUT_ACTION("Use", Ctrl) ... takes no Ctrl`: an option the marker does
  not have (the message lists them), or a modifier on an action, whose keys and modifiers the game's input settings
  give. See [Timers and input](#timers-and-input).
- `<Class>::<Method>: UE_INPUT_KEY("Space") names no key the engine has, so it would never fire: the closest is
  SpaceBar`: the engine binds the key by its name, and a name it has no key of matches no input. Fix: write the name
  the message gives, or another of the engine's key names. See [Timers and input](#timers-and-input).
- `warning: <Class>::<Function>: FKey{"Space"} names no key the engine has, so no input matches it: the closest is
  SpaceBar`: the same name in a key value, in a method, a member's initializer (`<Class>::<Member>`), a UE_DEFAULTS or
  a constant (named alone). It compiles, as code may compare it with a key the engine makes up as it runs. Fix: write
  the name the message gives. See [Timers and input](#timers-and-input).
- `<Class>: UE_INPUT_KEY("F5") marks no method`: the marker is followed by a variable, another kind of declaration or
  the end of the class. Fix: write it right before the method the input calls. See [Timers and input](#timers-and-input).
- `<Class>: UE_TOOLTIP("...") marks no method`, and the same for UE_KEYWORDS: the marker is followed by a variable,
  another kind of declaration or the end of the class. Fix: write it right before the method it describes. See
  [Classes and variables](#classes-and-variables).
- `... on a UE_STRUCT`, `... on a UE_INTERFACE`, `... on a UE_PATCH`: input binds to a class of the mod's own. Fix:
  mark the method in the class that implements the interface; a patch cannot add input to a game class. See
  [Timers and input](#timers-and-input).

- `<Class>::<Timeline>: UE_TIMELINE on a class that is no actor`: the engine makes a timeline's component from the
  actor's class as the actor spawns, and no other object has one. `... on a UE_STRUCT`, `... on a UE_INTERFACE`,
  `... on a UE_PATCH` likewise. Fix: declare it on an actor class of the mod's own. See [Timelines](#timelines).
- `<Class>::<Timeline>: UE_TIMELINE(Fade, <Length>) has a length that is not a number of seconds above 0`, `... has an
  unknown option <Option>`: the length is a number literal, `2.0f`, and the options are AutoPlay, Loop, Replicated,
  IgnoreTimeDilation and LastKeyframe. See [Timelines](#timelines).
- `<Class>::<Name>: UE_FLOAT_TRACK names <Timeline>, which is no UE_TIMELINE of the class`, and the same for the other
  tracks and the method markers: the timeline is declared with UE_TIMELINE in the same class, and a subclass cannot add
  tracks to its parent's. Fix: name one of the class's own timelines, or declare one. See [Timelines](#timelines).
- `<Class>::<Name>: UE_FLOAT_TRACK has no keys`, `... has the key <Key>, which is not (time, value) of number
  literals`, and `(time, x, y, z)` or `(time, r, g, b, a)` for a vector or color track: each key is a parenthesised
  list of number literals, with Cubic or Constant after the values for a key that is not Linear. See
  [Timelines](#timelines).
- `<Class>::<Method>: UE_TIMELINE_EVENT(Fade) has no times`, `... has the time <Text>, which is not a number literal`:
  list the times after the timeline, `UE_TIMELINE_EVENT(Fade, 0.5, 1.5)`. See [Timelines](#timelines).
- `<Class>::<Method>: UE_TIMELINE_UPDATE(Fade) on a static method`, `... on an inline method`, `... on a method with
  no body`, `... on a method returning <Type>`, `... on a method taking (<Types>)`: the engine calls the method by its
  name on the actor, with nothing, and takes nothing back. Fix: a method of the object that returns void and takes
  nothing. See [Timelines](#timelines).
- `<Class>::<Method>: UE_TIMELINE_UPDATE(Fade) marks a second method of Fade, after <Other>: a timeline calls one`: a
  timeline calls one update method and one finished method. Fix: call the second from the first. See [Timelines](#timelines).
- `<Class>: UE_TIMELINE_UPDATE(Fade) marks no method`: the marker is followed by a variable, another kind of
  declaration or the end of the class. Fix: write it right before the method. See [Timelines](#timelines).
- `<Class>::<Name>: UE_FLOAT_TRACK_CURVE names no curve asset`: the curve is not an asset: a UE_ASSET_AT, a UeAssets
  curve or a UE_FLOAT_CURVE of this mod. `... names <Curve>, a UCurveVector: the track's float is played from a
  UCurveFloat`: a float track plays a UCurveFloat, a vector track a UCurveVector, a color track a UCurveLinearColor.
  See [Timelines](#timelines).
- `<Curve>: UE_FLOAT_CURVE has no keys`, `... has the key <Key>, which is not (time, value) of number literals`, and
  the same for UE_VECTOR_CURVE and UE_COLOR_CURVE: a curve's keys are a track's. See [Timelines](#timelines).

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
  [Latent calls](#latent-calls). In a free function that is not inline the message ends `, and a free function is one
  unless it is inline`. Fix: mark the helper `inline`, so that it expands into the method that waits.
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
- `local <Name>: UE_AWAIT gives a dispatcher's two or more values one name each`: `auto V = UE_AWAIT(OnScored);` on
  a dispatcher of two or more parameters. Fix: bind one name per value, `auto [Who, Points] = UE_AWAIT(OnScored);`.
  See [Waiting on events](#waiting-on-events).
- `<Class>::ExecuteUbergraph_<Name>: ExecuteUbergraph_<Class> is the name of a class's ubergraph, whose latent calls
  resume through it; rename it`: the engine resumes a class's waits through that function, found by name on the
  object, so a method of that name would catch them. `<Class> declares ExecuteUbergraph_<Class>, the ubergraph's own
  name` is the same for a parent from another mod. Fix: rename the method. See [Latent calls](#latent-calls).

### Pointers, memory and intrinsics

- `TODO: a whole <Type> through a pointer; P->Member reaches its members`: a whole container read, written or
  passed through a raw pointer or a reference that keeps an address, as in `TArray<int32> A = *Pa;` or `R.Num()` with
  `TArray<int32> &R = *Pa;`. Structs, integers, float, double, bool, enums, FName, FString, FText and pointers are
  read whole. Fix: reach the elements through the container's data, or work on the container where it is a variable.
  See [Pointers and memory](#pointers-and-memory).
- ``TODO: the address of <ExprKind>: a Blueprint variable has none the VM hands out; point into an object
  (`(uint8*)Obj`), a TArray element (`&Items[I]`) or memory through a pointer``: the address of a local, a parameter
  or a member of a local struct, as in `&Count` or `&V.X`. `<ExprKind>` is clang's name for the expression, such as
  DeclRefExpr. Fix: point into an object, an array element or memory through a pointer, as the message says. See
  [Pointers and memory](#pointers-and-memory).
- `TODO: the address of <Struct>::<Member>: <Struct>'s reflected members take <N> bytes and it takes <M> in the game,
  so something unreflected moves them; add the member's offset to the struct's address yourself`: `&P->Member` on an
  engine struct whose reflected members do not account for its whole layout, such as FRuntimeCurveLinearColor. Fix: take the
  offset from the game's SDK dump and add it, `(int64)H + 8`. See [Pointers and memory](#pointers-and-memory).
- `TODO: the address of a member of <Type>, which is not a struct`: `&X.M` where the compiler knows X's type as
  neither a struct nor an object class. Fix: give the pointer its real type.
- `` `&Obj->Member` finds the member at run time through ReadProperty::GetPropertyAddress: include ReadProperty.h ``:
  `&Obj->Member`, or `&Member` on the object itself, with no class named ReadProperty in scope. A cooked property
  carries no offset, so the address is looked up at run time by a function of that class, which AssetGen does not
  ship. A reference local bound to another object's member, `float &R = Other->CustomTimeDilation;`, gives the same
  message after `reference R: `. Fix: include a header that declares `class ReadProperty` with a static, non-inline
  `int64 GetPropertyAddress(UObject *Target, FName PropName, UObject *WorldContextObject = nullptr)`, and build the mod
  that defines it. For a reference, read the member into a local instead. See
  [Pointers and memory](#pointers-and-memory).
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
