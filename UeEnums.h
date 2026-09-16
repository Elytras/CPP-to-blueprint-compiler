#pragma once
#include "SharedLib/core/Types.h"

namespace Uasset
{
/* UE4.27 Script.h */
enum EExprToken : uint16
{
    EX_LocalVariable             = 0x00,
    EX_InstanceVariable          = 0x01,
    EX_DefaultVariable           = 0x02,
    EX_Return                    = 0x04,
    EX_Jump                      = 0x06,
    EX_JumpIfNot                 = 0x07,
    EX_Assert                    = 0x09,
    EX_Nothing                   = 0x0B,
    EX_Let                       = 0x0F,
    EX_ClassContext              = 0x12,
    EX_MetaCast                  = 0x13,
    EX_LetBool                   = 0x14,
    EX_EndParmValue              = 0x15,
    EX_EndFunctionParms          = 0x16,
    EX_Self                      = 0x17,
    EX_Skip                      = 0x18,
    EX_Context                   = 0x19,
    EX_Context_FailSilent        = 0x1A,
    EX_VirtualFunction           = 0x1B,
    EX_FinalFunction             = 0x1C,
    EX_IntConst                  = 0x1D,
    EX_FloatConst                = 0x1E,
    EX_StringConst               = 0x1F,
    EX_ObjectConst               = 0x20,
    EX_NameConst                 = 0x21,
    EX_RotationConst             = 0x22,
    EX_VectorConst               = 0x23,
    EX_ByteConst                 = 0x24,
    EX_IntZero                   = 0x25,
    EX_IntOne                    = 0x26,
    EX_True                      = 0x27,
    EX_False                     = 0x28,
    EX_TextConst                 = 0x29,
    EX_NoObject                  = 0x2A,
    EX_TransformConst            = 0x2B,
    EX_IntConstByte              = 0x2C,
    EX_NoInterface               = 0x2D,
    EX_DynamicCast               = 0x2E,
    EX_StructConst               = 0x2F,
    EX_EndStructConst            = 0x30,
    EX_SetArray                  = 0x31,
    EX_EndArray                  = 0x32,
    EX_PropertyConst             = 0x33,
    EX_UnicodeStringConst        = 0x34,
    EX_Int64Const                = 0x35,
    EX_UInt64Const               = 0x36,
    EX_PrimitiveCast             = 0x38,   // next byte is the ECastToken
    EX_SetSet                    = 0x39,
    EX_EndSet                    = 0x3A,
    EX_SetMap                    = 0x3B,
    EX_EndMap                    = 0x3C,
    EX_SetConst                  = 0x3D,
    EX_EndSetConst               = 0x3E,
    EX_MapConst                  = 0x3F,
    EX_EndMapConst               = 0x40,
    EX_StructMemberContext       = 0x42,
    EX_LetMulticastDelegate      = 0x43,
    EX_LetDelegate               = 0x44,
    EX_LocalVirtualFunction      = 0x45,
    EX_LocalFinalFunction        = 0x46,
    EX_LocalOutVariable          = 0x48,
    EX_DeprecatedOp4A            = 0x4A,
    EX_InstanceDelegate          = 0x4B,
    EX_PushExecutionFlow         = 0x4C,
    EX_PopExecutionFlow          = 0x4D,
    EX_ComputedJump              = 0x4E,
    EX_PopExecutionFlowIfNot     = 0x4F,
    EX_Breakpoint                = 0x50,
    EX_InterfaceContext          = 0x51,
    EX_ObjToInterfaceCast        = 0x52,
    EX_EndOfScript               = 0x53,
    EX_CrossInterfaceCast        = 0x54,
    EX_InterfaceToObjCast        = 0x55,
    EX_WireTracepoint            = 0x5A,
    EX_SkipOffsetConst           = 0x5B,
    EX_AddMulticastDelegate      = 0x5C,
    EX_ClearMulticastDelegate    = 0x5D,
    EX_Tracepoint                = 0x5E,
    EX_LetObj                    = 0x5F,
    EX_LetWeakObjPtr             = 0x60,
    EX_BindDelegate              = 0x61,
    EX_RemoveMulticastDelegate   = 0x62,
    EX_CallMulticastDelegate     = 0x63,
    EX_LetValueOnPersistentFrame = 0x64,
    EX_ArrayConst                = 0x65,
    EX_EndArrayConst             = 0x66,
    EX_SoftObjectConst           = 0x67,
    EX_CallMath                  = 0x68,
    EX_SwitchValue               = 0x69,
    EX_InstrumentationEvent      = 0x6A,
    EX_ArrayGetByRef             = 0x6B,
    EX_ClassSparseDataVariable   = 0x6C,
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
    FUNC_Final                  = 0x00000001,
    FUNC_RequiredAPI            = 0x00000002,
    FUNC_BlueprintAuthorityOnly = 0x00000004,
    FUNC_BlueprintCosmetic      = 0x00000008,
    FUNC_Net                    = 0x00000040,
    FUNC_NetReliable            = 0x00000080,
    FUNC_NetRequest             = 0x00000100,
    FUNC_Exec                   = 0x00000200,
    FUNC_Native                 = 0x00000400,
    FUNC_Event                  = 0x00000800,
    FUNC_NetResponse            = 0x00001000,
    FUNC_Static                 = 0x00002000,
    FUNC_NetMulticast           = 0x00004000,
    FUNC_UbergraphFunction      = 0x00008000,
    FUNC_MulticastDelegate      = 0x00010000,
    FUNC_Public                 = 0x00020000,
    FUNC_Private                = 0x00040000,
    FUNC_Protected              = 0x00080000,
    FUNC_Delegate               = 0x00100000,
    FUNC_NetServer              = 0x00200000,
    FUNC_HasOutParms            = 0x00400000,
    FUNC_HasDefaults            = 0x00800000,
    FUNC_NetClient              = 0x01000000,
    FUNC_DLLImport              = 0x02000000,
    FUNC_BlueprintCallable      = 0x04000000,
    FUNC_BlueprintEvent         = 0x08000000,
    FUNC_BlueprintPure          = 0x10000000,
    FUNC_EditorOnly             = 0x20000000,
    FUNC_Const                  = 0x40000000,
    FUNC_NetValidate            = 0x80000000,
    FUNC_AllFlags               = 0xFFFFFFFF,
};

/* FProperty::PropertyFlags. */
enum EPropertyFlags : uint64
{
    CPF_None                           = 0x0000000000000000,
    CPF_Edit                           = 0x0000000000000001,
    CPF_ConstParm                      = 0x0000000000000002,
    CPF_BlueprintVisible               = 0x0000000000000004,
    CPF_ExportObject                   = 0x0000000000000008,
    CPF_BlueprintReadOnly              = 0x0000000000000010,
    CPF_Net                            = 0x0000000000000020,
    CPF_EditFixedSize                  = 0x0000000000000040,
    CPF_Parm                           = 0x0000000000000080,
    CPF_OutParm                        = 0x0000000000000100,
    CPF_ZeroConstructor                = 0x0000000000000200,
    CPF_ReturnParm                     = 0x0000000000000400,
    CPF_DisableEditOnTemplate          = 0x0000000000000800,
    CPF_Transient                      = 0x0000000000002000,
    CPF_Config                         = 0x0000000000004000,
    CPF_DisableEditOnInstance          = 0x0000000000010000,
    CPF_EditConst                      = 0x0000000000020000,
    CPF_GlobalConfig                   = 0x0000000000040000,
    CPF_InstancedReference             = 0x0000000000080000,
    CPF_DuplicateTransient             = 0x0000000000200000,
    CPF_SaveGame                       = 0x0000000001000000,
    CPF_NoClear                        = 0x0000000002000000,
    CPF_ReferenceParm                  = 0x0000000008000000,
    CPF_BlueprintAssignable            = 0x0000000010000000,
    CPF_Deprecated                     = 0x0000000020000000,
    CPF_IsPlainOldData                 = 0x0000000040000000,
    CPF_RepSkip                        = 0x0000000080000000,
    CPF_RepNotify                      = 0x0000000100000000,
    CPF_Interp                         = 0x0000000200000000,
    CPF_NonTransactional               = 0x0000000400000000,
    CPF_EditorOnly                     = 0x0000000800000000,
    CPF_NoDestructor                   = 0x0000001000000000,
    CPF_AutoWeak                       = 0x0000004000000000,
    CPF_ContainsInstancedReference     = 0x0000008000000000,
    CPF_AssetRegistrySearchable        = 0x0000010000000000,
    CPF_SimpleDisplay                  = 0x0000020000000000,
    CPF_AdvancedDisplay                = 0x0000040000000000,
    CPF_Protected                      = 0x0000080000000000,
    CPF_BlueprintCallable              = 0x0000100000000000,
    CPF_BlueprintAuthorityOnly         = 0x0000200000000000,
    CPF_TextExportTransient            = 0x0000400000000000,
    CPF_NonPIEDuplicateTransient       = 0x0000800000000000,
    CPF_ExposeOnSpawn                  = 0x0001000000000000,
    CPF_PersistentInstance             = 0x0002000000000000,
    CPF_UObjectWrapper                 = 0x0004000000000000,
    CPF_HasGetValueTypeHash            = 0x0008000000000000,
    CPF_NativeAccessSpecifierPublic    = 0x0010000000000000,
    CPF_NativeAccessSpecifierProtected = 0x0020000000000000,
    CPF_NativeAccessSpecifierPrivate   = 0x0040000000000000,
    CPF_SkipSerialization              = 0x0080000000000000,
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
    RF_NoFlags                      = 0x00000000,
    RF_Public                       = 0x00000001,
    RF_Standalone                   = 0x00000002,
    RF_MarkAsNative                 = 0x00000004,
    RF_Transactional                = 0x00000008,
    RF_ClassDefaultObject           = 0x00000010,
    RF_ArchetypeObject              = 0x00000020,
    RF_Transient                    = 0x00000040,
    RF_MarkAsRootSet                = 0x00000080,
    RF_TagGarbageTemp               = 0x00000100,
    RF_NeedInitialization           = 0x00000200,
    RF_NeedLoad                     = 0x00000400,
    RF_KeepForCooker                = 0x00000800,
    RF_NeedPostLoad                 = 0x00001000,
    RF_NeedPostLoadSubobjects       = 0x00002000,
    RF_NewerVersionExists           = 0x00004000,
    RF_BeginDestroyed               = 0x00008000,
    RF_FinishDestroyed              = 0x00010000,
    RF_BeingRegenerated             = 0x00020000,
    RF_DefaultSubObject             = 0x00040000,
    RF_WasLoaded                    = 0x00080000,
    RF_TextExportTransient          = 0x00100000,
    RF_LoadCompleted                = 0x00200000,
    RF_InheritableComponentTemplate = 0x00400000,
    RF_DuplicateTransient           = 0x00800000,
    RF_StrongRefOnFrame             = 0x01000000,
    RF_NonPIEDuplicateTransient     = 0x02000000,
    RF_Dynamic                      = 0x04000000,
    RF_WillBeLoaded                 = 0x08000000,
    RF_HasExternalPackage           = 0x10000000,
};

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
