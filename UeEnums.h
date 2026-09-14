#pragma once
/*
UeEnums.h — UE 4.27's serialization enums, generated from the engine headers.

These are the vocabularies a package writer has to speak exactly: Kismet opcodes, and the
flag words stored on functions, properties, classes and export rows. They are generated
rather than hand-copied (tools/genenums.py) because a single mistyped bit produces an
asset that loads and then misbehaves, which is far worse than one that fails outright.

Emitting an opcode is a separate question from naming it: the full EExprToken set is here so
the assembler can *name* anything it meets, while ExprName() plus a TODO marker tells you
which ones it cannot yet write.
*/
#include "SharedLib/core/Types.h"

namespace Uasset
{
/* Kismet bytecode opcodes (FBlueprintBytecode / Script.h). */
enum EExprToken : uint16
{
    EX_LocalVariable             = 0x00,   // A local variable.
    EX_InstanceVariable          = 0x01,   // An object variable.
    EX_DefaultVariable           = 0x02,   // Default variable for a class context.
    EX_Return                    = 0x04,   // Return from function.
    EX_Jump                      = 0x06,   // Goto a local address in code.
    EX_JumpIfNot                 = 0x07,   // Goto if not expression.
    EX_Assert                    = 0x09,   // Assertion.
    EX_Nothing                   = 0x0B,   // No operation.
    EX_Let                       = 0x0F,   // Assign an arbitrary size value to a variable.
    EX_ClassContext              = 0x12,   // Class default object context.
    EX_MetaCast                  = 0x13,   // Metaclass cast.
    EX_LetBool                   = 0x14,   // Let boolean variable.
    EX_EndParmValue              = 0x15,   // end of default value for optional function parameter
    EX_EndFunctionParms          = 0x16,   // End of function call parameters.
    EX_Self                      = 0x17,   // Self object.
    EX_Skip                      = 0x18,   // Skippable expression.
    EX_Context                   = 0x19,   // Call a function through an object context.
    EX_Context_FailSilent        = 0x1A,   // Call a function through an object context (can fail silently if the context is NULL; only generated for functions that don't have output or return values).
    EX_VirtualFunction           = 0x1B,   // A function call with parameters.
    EX_FinalFunction             = 0x1C,   // A prebound function call with parameters.
    EX_IntConst                  = 0x1D,   // Int constant.
    EX_FloatConst                = 0x1E,   // Floating point constant.
    EX_StringConst               = 0x1F,   // String constant.
    EX_ObjectConst               = 0x20,   // An object constant.
    EX_NameConst                 = 0x21,   // A name constant.
    EX_RotationConst             = 0x22,   // A rotation constant.
    EX_VectorConst               = 0x23,   // A vector constant.
    EX_ByteConst                 = 0x24,   // A byte constant.
    EX_IntZero                   = 0x25,   // Zero.
    EX_IntOne                    = 0x26,   // One.
    EX_True                      = 0x27,   // Bool True.
    EX_False                     = 0x28,   // Bool False.
    EX_TextConst                 = 0x29,   // FText constant
    EX_NoObject                  = 0x2A,   // NoObject.
    EX_TransformConst            = 0x2B,   // A transform constant
    EX_IntConstByte              = 0x2C,   // Int constant that requires 1 byte.
    EX_NoInterface               = 0x2D,   // A null interface (similar to EX_NoObject, but for interfaces)
    EX_DynamicCast               = 0x2E,   // Safe dynamic class casting.
    EX_StructConst               = 0x2F,   // An arbitrary UStruct constant
    EX_EndStructConst            = 0x30,   // End of UStruct constant
    EX_SetArray                  = 0x31,   // Set the value of arbitrary array
    EX_EndArray                  = 0x32,
    EX_PropertyConst             = 0x33,   // FProperty constant.
    EX_UnicodeStringConst        = 0x34,   // Unicode string constant.
    EX_Int64Const                = 0x35,   // 64-bit integer constant.
    EX_UInt64Const               = 0x36,   // 64-bit unsigned integer constant.
    EX_PrimitiveCast             = 0x38,   // A casting operator for primitives which reads the type as the subsequent byte
    EX_SetSet                    = 0x39,
    EX_EndSet                    = 0x3A,
    EX_SetMap                    = 0x3B,
    EX_EndMap                    = 0x3C,
    EX_SetConst                  = 0x3D,
    EX_EndSetConst               = 0x3E,
    EX_MapConst                  = 0x3F,
    EX_EndMapConst               = 0x40,
    EX_StructMemberContext       = 0x42,   // Context expression to address a property within a struct
    EX_LetMulticastDelegate      = 0x43,   // Assignment to a multi-cast delegate
    EX_LetDelegate               = 0x44,   // Assignment to a delegate
    EX_LocalVirtualFunction      = 0x45,   // Special instructions to quickly call a virtual function that we know is going to run only locally
    EX_LocalFinalFunction        = 0x46,   // Special instructions to quickly call a final function that we know is going to run only locally
    EX_LocalOutVariable          = 0x48,   // local out (pass by reference) function parameter
    EX_DeprecatedOp4A            = 0x4A,
    EX_InstanceDelegate          = 0x4B,   // const reference to a delegate or normal function object
    EX_PushExecutionFlow         = 0x4C,   // push an address on to the execution flow stack for future execution when a EX_PopExecutionFlow is executed.   Execution continues on normally and doesn't change to the pushed address.
    EX_PopExecutionFlow          = 0x4D,   // continue execution at the last address previously pushed onto the execution flow stack.
    EX_ComputedJump              = 0x4E,   // Goto a local address in code, specified by an integer value.
    EX_PopExecutionFlowIfNot     = 0x4F,   // continue execution at the last address previously pushed onto the execution flow stack, if the condition is not true.
    EX_Breakpoint                = 0x50,   // Breakpoint.  Only observed in the editor, otherwise it behaves like EX_Nothing.
    EX_InterfaceContext          = 0x51,   // Call a function through a native interface variable
    EX_ObjToInterfaceCast        = 0x52,   // Converting an object reference to native interface variable
    EX_EndOfScript               = 0x53,   // Last byte in script code
    EX_CrossInterfaceCast        = 0x54,   // Converting an interface variable reference to native interface variable
    EX_InterfaceToObjCast        = 0x55,   // Converting an interface variable reference to an object
    EX_WireTracepoint            = 0x5A,   // Trace point.  Only observed in the editor, otherwise it behaves like EX_Nothing.
    EX_SkipOffsetConst           = 0x5B,   // A CodeSizeSkipOffset constant
    EX_AddMulticastDelegate      = 0x5C,   // Adds a delegate to a multicast delegate's targets
    EX_ClearMulticastDelegate    = 0x5D,   // Clears all delegates in a multicast target
    EX_Tracepoint                = 0x5E,   // Trace point.  Only observed in the editor, otherwise it behaves like EX_Nothing.
    EX_LetObj                    = 0x5F,   // assign to any object ref pointer
    EX_LetWeakObjPtr             = 0x60,   // assign to a weak object pointer
    EX_BindDelegate              = 0x61,   // bind object and name to delegate
    EX_RemoveMulticastDelegate   = 0x62,   // Remove a delegate from a multicast delegate's targets
    EX_CallMulticastDelegate     = 0x63,   // Call multicast delegate
    EX_LetValueOnPersistentFrame = 0x64,
    EX_ArrayConst                = 0x65,
    EX_EndArrayConst             = 0x66,
    EX_SoftObjectConst           = 0x67,
    EX_CallMath                  = 0x68,   // static pure function from on local call space
    EX_SwitchValue               = 0x69,
    EX_InstrumentationEvent      = 0x6A,   // Instrumentation event
    EX_ArrayGetByRef             = 0x6B,
    EX_ClassSparseDataVariable   = 0x6C,   // Sparse data variable
    EX_FieldPathConst            = 0x6D,
    EX_Max                       = 0x100,
};

/* Operands of EX_PrimitiveCast. */
enum ECastToken : uint16
{
    CST_ObjectToInterface = 0x46,
    CST_ObjectToBool      = 0x47,
    CST_InterfaceToBool   = 0x49,
    CST_Max               = 0xFF,
};

/* UFunction::FunctionFlags. */
enum EFunctionFlags : uint32
{
    FUNC_None                   = 0x00000000,
    FUNC_Final                  = 0x00000001,   // Function is final (prebindable, non-overridable function).
    FUNC_RequiredAPI            = 0x00000002,   // Indicates this function is DLL exported/imported.
    FUNC_BlueprintAuthorityOnly = 0x00000004,   // Function will only run if the object has network authority
    FUNC_BlueprintCosmetic      = 0x00000008,   // Function is cosmetic in nature and should not be invoked on dedicated servers
    FUNC_Net                    = 0x00000040,   // Function is network-replicated.
    FUNC_NetReliable            = 0x00000080,   // Function should be sent reliably on the network.
    FUNC_NetRequest             = 0x00000100,   // Function is sent to a net service
    FUNC_Exec                   = 0x00000200,   // Executable from command line.
    FUNC_Native                 = 0x00000400,   // Native function.
    FUNC_Event                  = 0x00000800,   // Event function.
    FUNC_NetResponse            = 0x00001000,   // Function response from a net service
    FUNC_Static                 = 0x00002000,   // Static function.
    FUNC_NetMulticast           = 0x00004000,   // Function is networked multicast Server -> All Clients
    FUNC_UbergraphFunction      = 0x00008000,   // Function is used as the merge 'ubergraph' for a blueprint, only assigned when using the persistent 'ubergraph' frame
    FUNC_MulticastDelegate      = 0x00010000,   // Function is a multi-cast delegate signature (also requires FUNC_Delegate to be set!)
    FUNC_Public                 = 0x00020000,   // Function is accessible in all classes (if overridden, parameters must remain unchanged).
    FUNC_Private                = 0x00040000,   // Function is accessible only in the class it is defined in (cannot be overridden, but function name may be reused in subclasses.  IOW: if overridden, parameters don't need to match, and Super.Func() cannot be accessed since it's private.)
    FUNC_Protected              = 0x00080000,   // Function is accessible only in the class it is defined in and subclasses (if overridden, parameters much remain unchanged).
    FUNC_Delegate               = 0x00100000,   // Function is delegate signature (either single-cast or multi-cast, depending on whether FUNC_MulticastDelegate is set.)
    FUNC_NetServer              = 0x00200000,   // Function is executed on servers (set by replication code if passes check)
    FUNC_HasOutParms            = 0x00400000,   // function has out (pass by reference) parameters
    FUNC_HasDefaults            = 0x00800000,   // function has structs that contain defaults
    FUNC_NetClient              = 0x01000000,   // function is executed on clients
    FUNC_DLLImport              = 0x02000000,   // function is imported from a DLL
    FUNC_BlueprintCallable      = 0x04000000,   // function can be called from blueprint code
    FUNC_BlueprintEvent         = 0x08000000,   // function can be overridden/implemented from a blueprint
    FUNC_BlueprintPure          = 0x10000000,   // function can be called from blueprint code, and is also pure (produces no side effects). If you set this, you should set FUNC_BlueprintCallable as well.
    FUNC_EditorOnly             = 0x20000000,   // function can only be called from an editor scrippt.
    FUNC_Const                  = 0x40000000,   // function can be called from blueprint code, and only reads state (never writes state)
    FUNC_NetValidate            = 0x80000000,   // function must supply a _Validate implementation
    FUNC_AllFlags               = 0xFFFFFFFF,
};

/* FProperty::PropertyFlags. */
enum EPropertyFlags : uint64
{
    CPF_None                           = 0x0000000000000000,
    CPF_Edit                           = 0x0000000000000001,   // Property is user-settable in the editor.
    CPF_ConstParm                      = 0x0000000000000002,   // This is a constant function parameter
    CPF_BlueprintVisible               = 0x0000000000000004,   // This property can be read by blueprint code
    CPF_ExportObject                   = 0x0000000000000008,   // Object can be exported with actor.
    CPF_BlueprintReadOnly              = 0x0000000000000010,   // This property cannot be modified by blueprint code
    CPF_Net                            = 0x0000000000000020,   // Property is relevant to network replication.
    CPF_EditFixedSize                  = 0x0000000000000040,   // Indicates that elements of an array can be modified, but its size cannot be changed.
    CPF_Parm                           = 0x0000000000000080,   // Function/When call parameter.
    CPF_OutParm                        = 0x0000000000000100,   // Value is copied out after function call.
    CPF_ZeroConstructor                = 0x0000000000000200,   // memset is fine for construction
    CPF_ReturnParm                     = 0x0000000000000400,   // Return value.
    CPF_DisableEditOnTemplate          = 0x0000000000000800,   // Disable editing of this property on an archetype/sub-blueprint
    CPF_Transient                      = 0x0000000000002000,   // Property is transient: shouldn't be saved or loaded, except for Blueprint CDOs.
    CPF_Config                         = 0x0000000000004000,   // Property should be loaded/saved as permanent profile.
    CPF_DisableEditOnInstance          = 0x0000000000010000,   // Disable editing on an instance of this class
    CPF_EditConst                      = 0x0000000000020000,   // Property is uneditable in the editor.
    CPF_GlobalConfig                   = 0x0000000000040000,   // Load config from base class, not subclass.
    CPF_InstancedReference             = 0x0000000000080000,   // Property is a component references.
    CPF_DuplicateTransient             = 0x0000000000200000,   // Property should always be reset to the default value during any type of duplication (copy/paste, binary duplication, etc.)
    CPF_SaveGame                       = 0x0000000001000000,   // Property should be serialized for save games, this is only checked for game-specific archives with ArIsSaveGame
    CPF_NoClear                        = 0x0000000002000000,   // Hide clear (and browse) button.
    CPF_ReferenceParm                  = 0x0000000008000000,   // Value is passed by reference; CPF_OutParam and CPF_Param should also be set.
    CPF_BlueprintAssignable            = 0x0000000010000000,   // MC Delegates only.  Property should be exposed for assigning in blueprint code
    CPF_Deprecated                     = 0x0000000020000000,   // Property is deprecated.  Read it from an archive, but don't save it.
    CPF_IsPlainOldData                 = 0x0000000040000000,   // If this is set, then the property can be memcopied instead of CopyCompleteValue / CopySingleValue
    CPF_RepSkip                        = 0x0000000080000000,   // Not replicated. For non replicated properties in replicated structs
    CPF_RepNotify                      = 0x0000000100000000,   // Notify actors when a property is replicated
    CPF_Interp                         = 0x0000000200000000,   // interpolatable property for use with matinee
    CPF_NonTransactional               = 0x0000000400000000,   // Property isn't transacted
    CPF_EditorOnly                     = 0x0000000800000000,   // Property should only be loaded in the editor
    CPF_NoDestructor                   = 0x0000001000000000,   // No destructor
    CPF_AutoWeak                       = 0x0000004000000000,   // Only used for weak pointers, means the export type is autoweak
    CPF_ContainsInstancedReference     = 0x0000008000000000,   // Property contains component references.
    CPF_AssetRegistrySearchable        = 0x0000010000000000,   // asset instances will add properties with this flag to the asset registry automatically
    CPF_SimpleDisplay                  = 0x0000020000000000,   // The property is visible by default in the editor details view
    CPF_AdvancedDisplay                = 0x0000040000000000,   // The property is advanced and not visible by default in the editor details view
    CPF_Protected                      = 0x0000080000000000,   // property is protected from the perspective of script
    CPF_BlueprintCallable              = 0x0000100000000000,   // MC Delegates only.  Property should be exposed for calling in blueprint code
    CPF_BlueprintAuthorityOnly         = 0x0000200000000000,   // MC Delegates only.  This delegate accepts (only in blueprint) only events with BlueprintAuthorityOnly.
    CPF_TextExportTransient            = 0x0000400000000000,   // Property shouldn't be exported to text format (e.g. copy/paste)
    CPF_NonPIEDuplicateTransient       = 0x0000800000000000,   // Property should only be copied in PIE
    CPF_ExposeOnSpawn                  = 0x0001000000000000,   // Property is exposed on spawn
    CPF_PersistentInstance             = 0x0002000000000000,   // A object referenced by the property is duplicated like a component. (Each actor should have an own instance.)
    CPF_UObjectWrapper                 = 0x0004000000000000,   // Property was parsed as a wrapper class like TSubclassOf<T>, FScriptInterface etc., rather than a USomething*
    CPF_HasGetValueTypeHash            = 0x0008000000000000,   // This property can generate a meaningful hash value.
    CPF_NativeAccessSpecifierPublic    = 0x0010000000000000,   // Public native access specifier
    CPF_NativeAccessSpecifierProtected = 0x0020000000000000,   // Protected native access specifier
    CPF_NativeAccessSpecifierPrivate   = 0x0040000000000000,   // Private native access specifier
    CPF_SkipSerialization              = 0x0080000000000000,   // Property shouldn't be serialized, can still be exported to text
};

/* UClass::ClassFlags. */
enum EClassFlags : uint32
{
    CLASS_None                     = 0x00000000,
    CLASS_Abstract                 = 0x00000001,
    CLASS_DefaultConfig            = 0x00000002,
    CLASS_Config                   = 0x00000004,
    CLASS_Transient                = 0x00000008,
    CLASS_Parsed                   = 0x00000010,
    CLASS_MatchedSerializers       = 0x00000020,
    CLASS_ProjectUserConfig        = 0x00000040,
    CLASS_Native                   = 0x00000080,
    CLASS_NoExport                 = 0x00000100,
    CLASS_NotPlaceable             = 0x00000200,
    CLASS_PerObjectConfig          = 0x00000400,
    CLASS_ReplicationDataIsSetUp   = 0x00000800,
    CLASS_EditInlineNew            = 0x00001000,
    CLASS_CollapseCategories       = 0x00002000,
    CLASS_Interface                = 0x00004000,
    CLASS_CustomConstructor        = 0x00008000,
    CLASS_Const                    = 0x00010000,
    CLASS_LayoutChanging           = 0x00020000,
    CLASS_CompiledFromBlueprint    = 0x00040000,
    CLASS_MinimalAPI               = 0x00080000,
    CLASS_RequiredAPI              = 0x00100000,
    CLASS_DefaultToInstanced       = 0x00200000,
    CLASS_TokenStreamAssembled     = 0x00400000,
    CLASS_HasInstancedReference    = 0x00800000,
    CLASS_Hidden                   = 0x01000000,
    CLASS_Deprecated               = 0x02000000,
    CLASS_HideDropDown             = 0x04000000,
    CLASS_GlobalUserConfig         = 0x08000000,
    CLASS_Intrinsic                = 0x10000000,
    CLASS_Constructed              = 0x20000000,
    CLASS_ConfigDoNotCheckDefaults = 0x40000000,
    CLASS_NewerVersionExists       = 0x80000000,
};

/* Export-row object flags. */
enum EObjectFlags : uint32
{
    RF_NoFlags                      = 0x00000000,   // No flags, used to avoid a cast
    RF_Public                       = 0x00000001,   // Object is visible outside its package.
    RF_Standalone                   = 0x00000002,   // Keep object around for editing even if unreferenced.
    RF_MarkAsNative                 = 0x00000004,   // Object (UField) will be marked as native on construction (DO NOT USE THIS FLAG in HasAnyFlags() etc)
    RF_Transactional                = 0x00000008,   // Object is transactional.
    RF_ClassDefaultObject           = 0x00000010,   // This object is its class's default object
    RF_ArchetypeObject              = 0x00000020,   // This object is a template for another object - treat like a class default object
    RF_Transient                    = 0x00000040,   // Don't save object.
    RF_MarkAsRootSet                = 0x00000080,   // Object will be marked as root set on construction and not be garbage collected, even if unreferenced (DO NOT USE THIS FLAG in HasAnyFlags() etc)
    RF_TagGarbageTemp               = 0x00000100,   // This is a temp user flag for various utilities that need to use the garbage collector. The garbage collector itself does not interpret it.
    RF_NeedInitialization           = 0x00000200,   // This object has not completed its initialization process. Cleared when ~FObjectInitializer completes
    RF_NeedLoad                     = 0x00000400,   // During load, indicates object needs loading.
    RF_KeepForCooker                = 0x00000800,   // Keep this object during garbage collection because it's still being used by the cooker
    RF_NeedPostLoad                 = 0x00001000,   // Object needs to be postloaded.
    RF_NeedPostLoadSubobjects       = 0x00002000,   // During load, indicates that the object still needs to instance subobjects and fixup serialized component references
    RF_NewerVersionExists           = 0x00004000,   // Object has been consigned to oblivion due to its owner package being reloaded, and a newer version currently exists
    RF_BeginDestroyed               = 0x00008000,   // BeginDestroy has been called on the object.
    RF_FinishDestroyed              = 0x00010000,   // FinishDestroy has been called on the object.
    RF_BeingRegenerated             = 0x00020000,   // Flagged on UObjects that are used to create UClasses (e.g. Blueprints) while they are regenerating their UClass on load (See FLinkerLoad::CreateExport()), as well as UClass objects in the midst of being created
    RF_DefaultSubObject             = 0x00040000,   // Flagged on subobjects that are defaults
    RF_WasLoaded                    = 0x00080000,   // Flagged on UObjects that were loaded
    RF_TextExportTransient          = 0x00100000,   // Do not export object to text form (e.g. copy/paste). Generally used for sub-objects that can be regenerated from data in their parent object.
    RF_LoadCompleted                = 0x00200000,   // Object has been completely serialized by linkerload at least once. DO NOT USE THIS FLAG, It should be replaced with RF_WasLoaded.
    RF_InheritableComponentTemplate = 0x00400000,   // Archetype of the object can be in its super class
    RF_DuplicateTransient           = 0x00800000,   // Object should not be included in any type of duplication (copy/paste, binary duplication, etc.)
    RF_StrongRefOnFrame             = 0x01000000,   // References to this object from persistent function frame are handled as strong ones.
    RF_NonPIEDuplicateTransient     = 0x02000000,   // Object should not be included for duplication unless it's being duplicated for a PIE session
    RF_Dynamic                      = 0x04000000,   // Field Only. Dynamic field - doesn't get constructed during static initialization, can be constructed multiple times
    RF_WillBeLoaded                 = 0x08000000,   // This object was constructed during load and will be loaded shortly
    RF_HasExternalPackage           = 0x10000000,   // This object has an external package assigned and should look it up when getting the outermost package
};

/* Every opcode's spelling, for diagnostics and the assembler's unimplemented path. */
inline const char* ExprName(uint8 Op)
{
    switch (Op)
    {
    case 0x00: return "EX_LocalVariable";
    case 0x01: return "EX_InstanceVariable";
    case 0x02: return "EX_DefaultVariable";
    case 0x04: return "EX_Return";
    case 0x06: return "EX_Jump";
    case 0x07: return "EX_JumpIfNot";
    case 0x09: return "EX_Assert";
    case 0x0B: return "EX_Nothing";
    case 0x0F: return "EX_Let";
    case 0x12: return "EX_ClassContext";
    case 0x13: return "EX_MetaCast";
    case 0x14: return "EX_LetBool";
    case 0x15: return "EX_EndParmValue";
    case 0x16: return "EX_EndFunctionParms";
    case 0x17: return "EX_Self";
    case 0x18: return "EX_Skip";
    case 0x19: return "EX_Context";
    case 0x1A: return "EX_Context_FailSilent";
    case 0x1B: return "EX_VirtualFunction";
    case 0x1C: return "EX_FinalFunction";
    case 0x1D: return "EX_IntConst";
    case 0x1E: return "EX_FloatConst";
    case 0x1F: return "EX_StringConst";
    case 0x20: return "EX_ObjectConst";
    case 0x21: return "EX_NameConst";
    case 0x22: return "EX_RotationConst";
    case 0x23: return "EX_VectorConst";
    case 0x24: return "EX_ByteConst";
    case 0x25: return "EX_IntZero";
    case 0x26: return "EX_IntOne";
    case 0x27: return "EX_True";
    case 0x28: return "EX_False";
    case 0x29: return "EX_TextConst";
    case 0x2A: return "EX_NoObject";
    case 0x2B: return "EX_TransformConst";
    case 0x2C: return "EX_IntConstByte";
    case 0x2D: return "EX_NoInterface";
    case 0x2E: return "EX_DynamicCast";
    case 0x2F: return "EX_StructConst";
    case 0x30: return "EX_EndStructConst";
    case 0x31: return "EX_SetArray";
    case 0x32: return "EX_EndArray";
    case 0x33: return "EX_PropertyConst";
    case 0x34: return "EX_UnicodeStringConst";
    case 0x35: return "EX_Int64Const";
    case 0x36: return "EX_UInt64Const";
    case 0x38: return "EX_PrimitiveCast";
    case 0x39: return "EX_SetSet";
    case 0x3A: return "EX_EndSet";
    case 0x3B: return "EX_SetMap";
    case 0x3C: return "EX_EndMap";
    case 0x3D: return "EX_SetConst";
    case 0x3E: return "EX_EndSetConst";
    case 0x3F: return "EX_MapConst";
    case 0x40: return "EX_EndMapConst";
    case 0x42: return "EX_StructMemberContext";
    case 0x43: return "EX_LetMulticastDelegate";
    case 0x44: return "EX_LetDelegate";
    case 0x45: return "EX_LocalVirtualFunction";
    case 0x46: return "EX_LocalFinalFunction";
    case 0x48: return "EX_LocalOutVariable";
    case 0x4A: return "EX_DeprecatedOp4A";
    case 0x4B: return "EX_InstanceDelegate";
    case 0x4C: return "EX_PushExecutionFlow";
    case 0x4D: return "EX_PopExecutionFlow";
    case 0x4E: return "EX_ComputedJump";
    case 0x4F: return "EX_PopExecutionFlowIfNot";
    case 0x50: return "EX_Breakpoint";
    case 0x51: return "EX_InterfaceContext";
    case 0x52: return "EX_ObjToInterfaceCast";
    case 0x53: return "EX_EndOfScript";
    case 0x54: return "EX_CrossInterfaceCast";
    case 0x55: return "EX_InterfaceToObjCast";
    case 0x5A: return "EX_WireTracepoint";
    case 0x5B: return "EX_SkipOffsetConst";
    case 0x5C: return "EX_AddMulticastDelegate";
    case 0x5D: return "EX_ClearMulticastDelegate";
    case 0x5E: return "EX_Tracepoint";
    case 0x5F: return "EX_LetObj";
    case 0x60: return "EX_LetWeakObjPtr";
    case 0x61: return "EX_BindDelegate";
    case 0x62: return "EX_RemoveMulticastDelegate";
    case 0x63: return "EX_CallMulticastDelegate";
    case 0x64: return "EX_LetValueOnPersistentFrame";
    case 0x65: return "EX_ArrayConst";
    case 0x66: return "EX_EndArrayConst";
    case 0x67: return "EX_SoftObjectConst";
    case 0x68: return "EX_CallMath";
    case 0x69: return "EX_SwitchValue";
    case 0x6A: return "EX_InstrumentationEvent";
    case 0x6B: return "EX_ArrayGetByRef";
    case 0x6C: return "EX_ClassSparseDataVariable";
    case 0x6D: return "EX_FieldPathConst";
    case 0x100: return "EX_Max";
    default: return "EX_<unknown>";
    }
}

}   // namespace Uasset
