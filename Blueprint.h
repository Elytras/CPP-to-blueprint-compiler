#pragma once
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Package.h"
#include "Script.h"

namespace Uasset
{
/* The VariableGuid an SCS node gets, by class and component name. An override record has to name the
   parent node's GUID exactly (FComponentKey::Match compares only OwnerClass and AssociatedGuid), and
   both sides derive it from the same seed rather than reading the parent asset back. */
void ScsNodeGuid(const std::string& ClassName, const std::string& ComponentName, uint32 (&Out)[4]);

/* A timeline's TimelineGuid, by class and timeline name, and its direction variable's name: the editor's
   `<Timeline>__Direction_<guid>` (UTimelineTemplate::UpdateCachedNames, TimelineTemplate.cpp 55-67). The editor's GUID
   is random; this one is seeded like ScsNodeGuid, so a recompile writes the same names. */
std::string TimelineGuid(const std::string& ClassName, const std::string& Timeline, uint32 (&Out)[4]);

class FBlueprintClass
{
public:
    FBlueprintClass(FPackage& InPkg, std::string InClassName,
                    std::string ParentPackage, std::string ParentClass, bool bParentIsBlueprint);

    FIndex PackageImport(const std::string& PackageName);
    /* A subobject of some other class, e.g. an inherited component's <Name>_GEN_VARIABLE archetype:
       an import whose outer is that class's import rather than a package. */
    FIndex Subobject(const std::string& ClassPackage, const std::string& ClassName_, FIndex Outer,
                     const std::string& ObjectName);
    FIndex EngineClass(const std::string& PackageName, const std::string& ClassName);
    /* What a game Blueprint's package exports it as, where that is not a BlueprintGeneratedClass: "<package>.<class>"
       -> (class package, class name), such as a widget Blueprint's (/Script/UMG, WidgetBlueprintGeneratedClass).
       EngineClass names it as the import row's class. Null, or a class it does not hold: BlueprintGeneratedClass. */
    const std::map<std::string, std::pair<std::string, std::string>>* GameClassKinds = nullptr;
    FIndex ScriptStruct(const std::string& PackageName, const std::string& StructName);
    FIndex Enum(const std::string& PackageName, const std::string& EnumName);
    FIndex EngineFunction(const std::string& PackageName, const std::string& OwningClass,
                          const std::string& FunctionName);

    /* Same import as EngineClass, but also recorded as a load dependency of every function body. */
    FIndex PropertyOwner(const std::string& PackageName, const std::string& ClassName);

    /* The CDO as an EX_Context literal; recorded as a load dependency like a called function. */
    FIndex ClassDefaultObject(const std::string& PackageName, const std::string& ClassName);

    /* An asset in another package (a data asset instance), as an object literal or a default's value. Recorded as a
       load dependency of every function body, like the CDO. */
    FIndex Asset(const std::string& ClassPackage, const std::string& ClassName_,
                 const std::string& AssetPackage, const std::string& AssetName);

    /* Only an actor may have the SCS trio: USimpleConstructionScript casts the owner CDO to AActor. */
    void SetIsActor(bool bValue) { bIsActor = bValue; }
    /* An actor component: its tick function is PrimaryComponentTick, not an actor's PrimaryActorTick. */
    void SetIsComponent(bool bValue) { bIsComponent = bValue; }
    /* A flag the class's own members call for on top of what SetClassFlags gave (CLASS_HasInstancedReference). */
    void AddClassFlags(uint32 Flags) { ClassFlags |= Flags; }

    /* A cooked class stores the FULL EClassFlags set, mostly inherited from the native parent. */
    void SetClassFlags(uint32 Flags) { ClassFlags = Flags; }
    /* ClassWithin and ClassConfigName, which the editor copies from the parent (KismetCompiler.cpp:320-321, 2453). */
    void SetClassTail(std::string WithinPackage_, std::string WithinClass_, std::string ConfigName_)
    { WithinPackage = std::move(WithinPackage_); WithinClass = std::move(WithinClass_); ConfigName = std::move(ConfigName_); }

    /* Replication: the CDO's bReplicates, set when the class replicates a variable or declares an RPC. The class's
       NumReplicatedProperties tag is counted from the CPF_Net variables. */
    void SetReplicates(bool bValue) { bReplicates = bValue; }
    /* ExecuteUbergraph_<Class>: written as the class's UberGraphFunction tag. */
    void SetUberGraphFunction(FIndex Function) { UberGraphFunction = Function; }

    /*
    `Super` is the engine UFunction being overridden, or null for a new method. Body receives the
    function's own export index (the FFieldPath owner for its params), filled in by Finish().
    Returns that export index.
    */
    /* UE_CATEGORY: a function's or variable's editor category. Editor metadata, so only the API stub carries it. */
    std::map<std::string, std::string> ApiCategory;
    /* UE_TOOLTIP / UE_KEYWORDS: a function's (tooltip, keywords), editor metadata likewise. */
    std::map<std::string, std::pair<std::string, std::string>> ApiDocs;

    FIndex AddFunction(const std::string& Name, FIndex Super,
                       const std::vector<FPropertyDef>& Params,
                       const std::function<void(FScript&, FIndex)>& Body,
                       uint32 FunctionFlags = 0);

    /* One ChildProperties entry on the class; no CDO default is written. */
    void AddVariable(const FPropertyDef& Var);

    /* An implemented interface: a UClass::Interfaces entry. Its functions are ordinary AddFunction()s. */
    void AddInterface(FIndex InterfaceClass) { Interfaces.push_back(InterfaceClass); }

    /*
    One UE_COMPONENT: an SCS node plus the <Name>_GEN_VARIABLE archetype it instantiates. `Defaults`
    are written as tagged properties on that archetype, so they delta against the component CDO -
    the editor's per-component defaults, not the actor CDO's. The class variable of the same name is
    an ordinary AddVariable(); USCS_Node::ExecuteNodeOnActor assigns the instance to it by name.
    `NativeTail` is what the class's native Serialize reads after UObject's part (FCompiler::NativeTail);
    the component overrides below take the same.
    */
    void AddComponent(const std::string& Name, FIndex ComponentClass, FIndex ComponentCdo,
                      bool bIsSceneComponent, const std::vector<FPropertyDef>& Defaults,
                      const std::vector<uint8>& NativeTail = {});

    /*
    Where UE_DEFAULTS' SetupAttachment puts a component added above, by the fields of its SCS node: under another of
    this class's components, as one of that node's ChildNodes (bOwn); or, as a root node, under an inherited one the
    node names - an ancestor Blueprint's node by its variable and OwnerClass, that Blueprint's class, which
    FixupRootNodeParentReferences looks it up in, or a native default subobject by its object name (bNative), which
    ExecuteScriptOnActor matches among the actor's native scene components. Or under the actor's root, whichever
    component that is (bRoot, `SetupAttachment(RootComponent)`): an inherited root takes a root node naming no parent
    (SimpleConstructionScript.cpp 686); without one, the component is a child of the class's own root, or of the
    DefaultSceneRoot node when no component of its own is left to be the root. Socket is AttachToName. A component
    given none is placed as before: the first such scene component is the root and the others attach to it.
    */
    struct FAttachment
    {
        std::string Parent, OwnerClass, Socket;
        bool bOwn = false, bNative = false, bRoot = false;
    };
    void AttachComponent(const std::string& Name, const FAttachment& Attachment);

    /* The SCS node's VariableGuid for a component added above, as the class's `<Comp>__UeScsNode` gives it: a class
       rebuilt from a cooked one keeps the guid its subclasses' override records name (FComponentKey's AssociatedGuid),
       which a subclass still shipped as cooked matches the node by. Without one the guid is ScsNodeGuid's. */
    void SetComponentGuid(const std::string& Name, const uint32 (&Guid)[4]);

    /* Whether the SCS lists its DefaultSceneRoot node, in RootNodes and AllNodes: the actor has no root before it runs
       and no scene component of the class's own is left to be the root - none, or each attached somewhere. The editor
       keeps the node there exactly then (ValidateSceneRootNodes, SimpleConstructionScript.cpp 1132-1150), and gives the
       class a variable of its name, which Generate adds. Known once every component is added and attached. */
    bool ListsDefaultRoot() const;

    /*
    An inherited component's defaults: one UInheritableComponentHandler record, which is how the
    editor stores a child class's override of a parent's SCS component. The template is a fresh
    component export archetyped on the parent's, so `Defaults` are its deltas.
    USCS_Node::GetActualComponentTemplate finds it by FComponentKey, which matches on OwnerClass and
    AssociatedGuid alone - so the GUID must be the parent SCS node's VariableGuid.
    */
    void AddComponentOverride(const std::string& Name, FIndex ComponentClass, FIndex ParentTemplate,
                              FIndex OwnerClass, const uint32 (&AssociatedGuid)[4],
                              const std::vector<FPropertyDef>& Defaults, const std::vector<uint8>& NativeTail = {});

    /*
    An override of a NATIVE parent's default subobject - its components, which are not SCS nodes and
    so have nothing to do with the handler above. Measured on Ene_Butterfly: one export named exactly
    as the subobject, outered to THIS class's CDO, flags Public|Transactional|ArchetypeObject|
    DefaultSubObject, archetyped on the parent CDO's subobject of the same name (Finish imports it),
    plus an ObjectProperty tag of that name on the CDO.
    */
    void AddSubobjectOverride(const std::string& Name, const std::string& Property, FIndex ComponentClass,
                              const std::vector<FPropertyDef>& Defaults, const std::vector<uint8>& NativeTail = {});

    /* A default subobject the Blueprint parent's CDO exports (its own AddSubobjectOverride): imported under the parent
       CDO and serialized before this class, which builds its own CDO's copy from it. A nested one is named by its path
       under the CDO, `Damage:BreakIceBonus_0`, and imported under its outer's import, which must be added too. */
    void AddParentSubobject(const std::string& Name, const std::string& ClassPackage, const std::string& ClassName_)
    {
        ParentSubobjects.push_back(FParentSubobject{ Name, ClassPackage, ClassName_ });
    }

    /* A tag on this class's CDO for a property an ancestor declares, which a member initializer
       cannot express: declaring the name again would shadow it with a second property. */
    void AddCdoDefault(const FPropertyDef& Var) { CdoDefaults.push_back(Var); }

    /* One editor InputKey or InputAction node: an entry of the class's InputKeyDelegateBinding or
       InputActionDelegateBinding object, which UInputDelegateBinding::BindInputDelegates binds to the actor's input
       component, calling Function on the actor by name (InputKeyDelegateBinding.cpp 12-45). Name is the key or the
       action. Event is an EInputEvent: 0 pressed, 1 released, 2 repeat, 3 double click. */
    struct FInputBinding
    {
        bool bAction = false;
        std::string Name, Function;
        uint8 Event = 0;
        bool bShift = false, bCtrl = false, bAlt = false, bCmd = false;
        bool bConsumeInput = true, bExecuteWhenPaused = false, bOverrideParentBinding = true;
    };
    void AddInputBinding(const FInputBinding& Binding) { InputBindings.push_back(Binding); }

    /* One editor Timeline node: a TimelineTemplate in the class's Timelines and a curve object per track, of which
       UBlueprintGeneratedClass::CreateTimelineComponent makes a UTimelineComponent on each actor, stores it in the
       variable Name and binds Update, Finished and each event track's function by name (BlueprintGeneratedClass.cpp
       1077-1195). A track's Name is its TrackName and the variable it sets each frame, or an event track's function.
       A key holds its time and one value per channel (an event's none, a float's one, a vector's three, a color's
       four); Interp is an ERichCurveInterpMode, 0 linear, 1 constant, 2 cubic. Kind: 'E' event, 'F' float, 'V' vector,
       'C' linear color. */
    struct FTimelineKey
    {
        float Time = 0.0f;
        float Values[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        uint8 Interp = 0;
    };
    struct FTimelineTrack
    {
        char Kind = 'F';
        std::string Name;
        std::vector<FTimelineKey> Keys;
        FIndex External;            // a curve asset the track plays, the editor's External Curve; null: its own Keys
    };
    struct FTimeline
    {
        std::string Name, DirectionProperty, Update, Finished;
        float Length = 5.0f;
        bool bAutoPlay = false, bLoop = false, bReplicated = false, bIgnoreTimeDilation = false, bLastKeyframe = false;
        uint32 Guid[4] = { 0, 0, 0, 0 };
        std::vector<FTimelineTrack> Tracks;
    };
    void AddTimeline(const FTimeline& Timeline) { Timelines.push_back(Timeline); }
    /* S39: a widget class with a designer tree, or below a mod class that has one. It is exported as a
       WidgetBlueprintGeneratedClass, the only kind UUserWidget::Initialize builds a tree for (UserWidget.cpp 125-143). */
    void SetWidgetClass(bool bValue) { bWidgetClass = bValue; }
    /* One UE_WIDGET: an export under the class's WidgetTree, archetyped on its class's CDO, its Defaults as tags. The
       first one added is the tree's RootWidget. InitializeWidgetStatic finds the class variable of the same name. */
    void AddWidget(const std::string& Name, FIndex Class, FIndex Cdo, const std::vector<FPropertyDef>& Defaults);
    /* Child in Panel, as `Panel->AddChild(Child)` makes it: a slot export of Class under the panel, holding Parent and
       Content, listed in the panel's Slots and named by the child's Slot. Both widgets are added with AddWidget. */
    void AddWidgetSlot(const std::string& Panel, const std::string& Child, FIndex Class, FIndex Cdo,
                       const std::vector<FPropertyDef>& Defaults);
    /* Content, a widget of this tree in no panel, fills the named slot Slot of the nested user widget Host: an entry of
       Host's NamedSlotBindings, which UUserWidget::InitializeNamedSlots adds to the UNamedSlot variable of that name. */
    void AddWidgetSlotFill(const std::string& Host, const std::string& Slot, const std::string& Content)
    {
        WidgetSlotFills.push_back({ Host, Slot, Content });
    }
    /* The class's NamedSlots: the names of its tree's UNamedSlot widgets, which UUserWidget::GetSlotNames answers. */
    void SetNamedSlots(const std::vector<std::string>& Names) { NamedSlots = Names; }
    /* An entry of the class's ComponentDelegateBinding: Function on the widget object bound to the multicast delegate
       Delegate of the widget the variable Component holds (ComponentDelegateBinding.cpp 12-38), as the designer's
       "On Clicked (Play)" node is. */
    struct FComponentBinding { std::string Component, Delegate, Function; };
    void AddComponentBinding(const FComponentBinding& Binding) { ComponentBindings.push_back(Binding); }
    /* An entry of the class's Bindings: the property Property of the widget variable Widget, read from Function each
       time it is drawn (InitializeBindingsStatic binds the widget's <Property>Delegate). */
    struct FPropertyBinding { std::string Widget, Property, Function; };
    void AddPropertyBinding(const FPropertyBinding& Binding) { PropertyBindings.push_back(Binding); }
    /* One UE_ANIMATION: a WidgetAnimation of the class, in its Animations, with its MovieScene, which binds each widget it
       animates by name (AnimationBindings) to a RenderTransform track of one section keying the channels given. Times
       are frames at the MovieScene's tick resolution, 60000 a second; EndFrame is the playback range's exclusive end.
       Channels: Translation X and Y, Rotation, Scale X and Y, Shear X and Y, the section's FMovieSceneFloatChannels.
       A key's tangents are value per frame; Interp an ERichCurveInterpMode, TangentMode an ERichCurveTangentMode. */
    struct FAnimKey
    {
        int32 Frame = 0;
        float Value = 0.0f, Arrive = 0.0f, Leave = 0.0f;
        uint8 Interp = 0, TangentMode = 0;
    };
    struct FAnimTrack
    {
        std::string Widget;
        FIndex WidgetClass;
        std::vector<FAnimKey> Channels[7];
    };
    struct FAnimation
    {
        std::string Name;
        int32 EndFrame = 0;
        std::vector<FAnimTrack> Tracks;
    };
    void AddAnimation(const FAnimation& Animation) { Animations.push_back(Animation); }

    /* The actor has a root before this class's SCS runs: a Blueprint parent's SCS leaves one, and a native parent's
       constructor sets one or ExecuteConstruction takes its first native scene component. Then the DefaultSceneRoot
       node is in neither RootNodes nor AllNodes, as the editor saves it (ValidateSceneRootNodes). */
    void SetRootInherited(bool bInherited) { bRootInherited = bInherited; }

    void Finish();

    /* The editor-side stub of this class: <OutDir>/<asset>.uasset, signatures only (Uncooked.h).
       Call after Finish(), while the cooked package still holds the imports the parameters name. */
    bool WriteApi(const std::string& OutDir, std::string* Err) const;

    /* Writes the package as one UserDefinedStruct export whose members are the AddVariable()s. */
    void FinishStruct(const uint32 (&Guid)[4]);

    /* The editor-side stub of this struct: an uncooked UserDefinedStruct with the same GUID-suffixed
       member names. Same Guid as FinishStruct so the cooked and editor assets share identity. */
    bool WriteApiStruct(const std::string& OutDir, const uint32 (&Guid)[4], std::string* Err) const;

    /* Writes the package as one UserDefinedEnum export: <ClassName>::<Enumerator> = value, then <ClassName>_MAX. */
    void FinishEnum(const std::vector<std::pair<std::string, int64>>& Enumerators,
                    const std::vector<std::pair<std::string, std::string>>& DisplayNames = {});

    /* The editor-side stub of this enum: an uncooked UserDefinedEnum with the same "<Enum>::<Entry>" names. */
    bool WriteApiEnum(const std::string& OutDir, const std::vector<std::pair<std::string, int64>>& Enumerators,
                      std::string* Err) const;

    /* Writes the package as one instance of Class (a data asset): a tag per AddVariable(), each holding its Default. */
    void FinishAsset(FIndex Class, FIndex ClassCdo);

    /* Writes the package as one curve asset of Class (CurveFloat, CurveVector, CurveLinearColor) holding Curve's keys,
       the body a timeline track's own curve has. */
    void FinishCurve(FIndex Class, FIndex ClassCdo, const FTimelineTrack& Curve);

    FIndex ClassIndex() const { return Exp(ClassRow); }

private:
    struct FPending
    {
        FFunctionDef Def;
        std::function<void(FScript&, FIndex)> Body;
    };

    FPackage& P;
    std::string ClassName;
    std::string ParentPackage, ParentClass;
    bool bParentIsBlueprint = false;
    bool bRootInherited = false;
    bool bIsActor = true;
    bool bIsComponent = false;
    bool bReplicates = false;
    FIndex UberGraphFunction;
    uint32 ClassFlags = 0x00840814;
    std::string WithinPackage = "/Script/CoreUObject", WithinClass = "Object", ConfigName = "Engine";

    std::unordered_map<std::string, int32> ImportCache;

    /*
    Union of every import a body may reach; each function export declares all of them as
    create-before-serialize edges. Under-declaring crashes the async loader.
    */
    std::vector<int32> CallImports;
    std::vector<FPending> Functions;
    std::vector<FPropertyDef> Vars;
    std::vector<FIndex> Interfaces;

    struct FComponent
    {
        std::string Name;
        FIndex Class, Cdo;
        bool bIsScene = false;
        std::vector<FPropertyDef> Defaults;
        std::vector<uint8> NativeTail;
        FAttachment Attachment;
        bool bGuid = false;             // SetComponentGuid: the node's VariableGuid as given, not ScsNodeGuid's
        uint32 Guid[4] = {};
    };
    std::vector<FComponent> Components;

    struct FComponentOverride
    {
        std::string Name;
        FIndex Class, ParentTemplate, OwnerClass;
        uint32 Guid[4] = { 0, 0, 0, 0 };
        std::vector<FPropertyDef> Defaults;
        std::vector<uint8> NativeTail;
    };
    std::vector<FComponentOverride> ComponentOverrides;
    std::vector<FPropertyDef> CdoDefaults;
    std::vector<FInputBinding> InputBindings;
    std::vector<FTimeline> Timelines;

    struct FSubobjectOverride
    {
        std::string Name;          // the subobject's, which the export takes
        std::string Property;      // the CDO's property that points at it
        FIndex Class;
        std::vector<FPropertyDef> Defaults;
        std::vector<uint8> NativeTail;
    };
    std::vector<FSubobjectOverride> SubobjectOverrides;

    struct FParentSubobject
    {
        std::string Name, ClassPackage, ClassName;
    };
    std::vector<FParentSubobject> ParentSubobjects;

    bool bWidgetClass = false;
    struct FWidget
    {
        std::string Name;
        FIndex Class, Cdo;
        std::vector<FPropertyDef> Defaults;
    };
    std::vector<FWidget> Widgets;
    struct FWidgetSlot
    {
        std::string Panel, Child;
        FIndex Class, Cdo;
        std::vector<FPropertyDef> Defaults;
    };
    std::vector<FWidgetSlot> WidgetSlots;
    struct FWidgetSlotFill { std::string Host, Slot, Content; };
    std::vector<FWidgetSlotFill> WidgetSlotFills;
    std::vector<std::string> NamedSlots;
    std::vector<FComponentBinding> ComponentBindings;
    std::vector<FPropertyBinding> PropertyBindings;
    std::vector<FAnimation> Animations;

    int32 ClassRow = 0;
};

}   // namespace Uasset
