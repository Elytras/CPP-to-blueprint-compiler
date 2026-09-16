#include "Script.h"

#include <cstring>

namespace Uasset
{
namespace
{
constexpr int32 kDiskObjectRef = 4;
constexpr int32 kMemObjectRef = 8;
constexpr int32 kFNameSize = 12;    // sizeof(FScriptName): the in-memory bytecode form of a name (8 on disk)
}   // namespace

/* ---- properties ---- */

FPropertyDef FloatParam(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "FloatProperty", Name, RF_Public, 1, 4,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef DoubleParam(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "DoubleProperty", Name, RF_Public, 1, 8,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef IntParam(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "IntProperty", Name, RF_Public, 1, 4,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef Int64Param(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "Int64Property", Name, RF_Public, 1, 8,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef Int8Param(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "Int8Property", Name, RF_Public, 1, 1,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef UInt32Param(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "UInt32Property", Name, RF_Public, 1, 4,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef UInt64Param(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "UInt64Property", Name, RF_Public, 1, 8,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef StringParam(const std::string& Name, uint64 ExtraFlags)
{
    // ElementSize 16 = the TArray<TCHAR> header, not any string's length.
    return FPropertyDef{ "StrProperty", Name, RF_Public, 1, 16,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef NameParam(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "NameProperty", Name, RF_Public, 1, 8,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef TextParam(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "TextProperty", Name, RF_Public, 1, 24,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef BoolParam(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "BoolProperty", Name, RF_Public, 1, 1,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef ByteParam(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "ByteProperty", Name, RF_Public, 1, 1,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef ObjectParam(const std::string& Name, FIndex Class, uint64 ExtraFlags)
{
    return FPropertyDef{ "ObjectProperty", Name, RF_Public, 1, 8,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Class };
}

FPropertyDef ClassParam(const std::string& Name, FIndex UClassImp, FIndex MetaClass, uint64 ExtraFlags)
{
    // Storage is a UClass* (8b). Extra = PropertyClass (always UClass); Extra2 = MetaClass filter.
    return FPropertyDef{ "ClassProperty", Name, RF_Public, 1, 8,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags,
                         UClassImp, "", MetaClass };
}

FPropertyDef StructParam(const std::string& Name, FIndex Struct, const std::string& StructName,
                         int32 Size, uint64 ExtraFlags)
{
    return FPropertyDef{ "StructProperty", Name, RF_Public, 1, Size,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Struct, StructName };
}

void WriteZeroValueTag(FArc& Ar, const FPropertyDef& P)
{
    if (P.Type == "BoolProperty") { TagBool(Ar, P.Name, false); return; }
    Tag(Ar, P.Name, P.Type, [&](FArc& V) {
        if (P.Type == "IntProperty" || P.Type == "FloatProperty" || P.Type == "StrProperty"
            || P.Type == "ObjectProperty" || P.Type == "ClassProperty" || P.Type == "UInt32Property")
            V.I32(0);
        else if (P.Type == "Int64Property" || P.Type == "UInt64Property") V.I64(0);
        else if (P.Type == "DoubleProperty") { double Z = 0.0; V.Raw(&Z, 8); }
        else if (P.Type == "ByteProperty" || P.Type == "Int8Property") V.U8(0);
        else if (P.Type == "NameProperty") V.Name("None");
        else if (P.Type == "TextProperty") { V.U32(0); V.U8(0xFF); V.I32(0); }   // flags, ETextHistoryType::None, no invariant string
        else if (P.Type == "StructProperty") TagEnd(V);
    }, P.StructName);
}

void WriteProperty(FArc& Ar, const FPropertyDef& P)
{
    Ar.Name(P.Type);

    Ar.Name(P.Name);                    // FField::Serialize
    Ar.U32(P.ObjectFlags);

    Ar.I32(P.ArrayDim);                 // FProperty::Serialize
    Ar.I32(P.ElementSize);
    Ar.Raw(&P.PropertyFlags, 8);
    Ar.U16(0);                          // RepIndex
    Ar.Name("None");                    // RepNotifyFunc
    Ar.U8(0);                           // BlueprintReplicationCondition

    if (P.Type == "ObjectProperty")
        Ar.Idx(P.Extra);                // PropertyClass
    else if (P.Type == "ClassProperty")
    {
        Ar.Idx(P.Extra);                // PropertyClass = UClass
        Ar.Idx(P.Extra2);               // MetaClass (the subclass filter)
    }
    else if (P.Type == "StructProperty")
        Ar.Idx(P.Extra);                // Struct
    else if (P.Type == "BoolProperty")
    {
        // Whole-byte bool, not a bitfield.
        Ar.U8(1);                       // FieldSize
        Ar.U8(0);                       // ByteOffset
        Ar.U8(1);                       // ByteMask
        Ar.U8(0xFF);                    // FieldMask
        Ar.U8(1);                       // BoolSize
        Ar.U8(0);                       // NativeBool
    }
    else if (P.Type == "ByteProperty")
        Ar.Idx(P.Extra);                // Enum
    // TODO: unimplemented tails - ArrayProperty (Inner), MapProperty, SetProperty,
    // EnumProperty (Enum + UnderlyingProp), DelegateProperty (SignatureFunction).
}

/* ---- bytecode ---- */

void FScript::Op(EExprToken Token)
{
    Ar.U8(uint8(Token));
    Memory += 1;
}

void FScript::Self() { Op(EX_Self); }
void FScript::Nothing() { Op(EX_Nothing); }
void FScript::EndOfScript() { Op(EX_EndOfScript); }
void FScript::EndFunctionParms() { Op(EX_EndFunctionParms); }

void FScript::Return()
{
    Op(EX_Return);
    Op(EX_Nothing);
}

void FScript::Return(const std::function<void(FScript&)>& Value)
{
    // The parm chain must carry a "ReturnValue" entry of the matching type; the VM finds it by name.
    Op(EX_Return);
    Value(*this);
}

void FScript::IntConst(int32 Value)
{
    Op(EX_IntConst);
    Ar.I32(Value);
    Memory += 4;
}

void FScript::Int64Const(int64 Value)
{
    Op(EX_Int64Const);
    Ar.Raw(&Value, 8);
    Memory += 8;
}

void FScript::ByteConst(uint8 Value)
{
    Op(EX_ByteConst);
    Ar.U8(Value);
    Memory += 1;
}

int32 FScript::Jump(int32 MemTarget)
{
    Op(EX_Jump);
    const int32 PatchAt = int32(Ar.B.size());
    Ar.U32(uint32(MemTarget));
    Memory += 4;
    return PatchAt;
}

int32 FScript::JumpIfNot(int32 MemTarget, const std::function<void(FScript&)>& Cond)
{
    Op(EX_JumpIfNot);
    const int32 PatchAt = int32(Ar.B.size());
    Ar.U32(uint32(MemTarget));
    Memory += 4;
    Cond(*this);
    return PatchAt;
}

void FScript::PatchJumpTarget(int32 StorageOffset, int32 MemTarget)
{
    const uint32 V = uint32(MemTarget);
    Ar.B[StorageOffset + 0] = uint8(V);
    Ar.B[StorageOffset + 1] = uint8(V >> 8);
    Ar.B[StorageOffset + 2] = uint8(V >> 16);
    Ar.B[StorageOffset + 3] = uint8(V >> 24);
}

void FScript::FloatConst(float Value)
{
    Op(EX_FloatConst);
    Ar.Raw(&Value, 4);
    Memory += 4;
}

void FScript::NameConst(const std::string& NameStr)
{
    Op(EX_NameConst);
    Ar.Name(NameStr);
    Memory += kFNameSize;
}

void FScript::StringConst(const std::string& Value)
{
    Op(EX_StringConst);
    Ar.Raw(Value.data(), Value.size());
    Ar.U8(0);
    Memory += int32(Value.size()) + 1;
}

void FScript::UnicodeStringConst(const std::u16string& Value)
{
    Op(EX_UnicodeStringConst);
    Ar.Raw(Value.data(), Value.size() * 2);
    Ar.U16(0);
    Memory += int32(Value.size() * 2) + 2;
}

void FScript::TextConst(const std::string& Value, bool bWide)
{
    Op(EX_TextConst);
    Ar.U8(3);                           // EBlueprintTextLiteralType::LiteralString
    Memory += 1;
    if (bWide)
    {
        std::u16string W;
        for (unsigned char C : Value) W.push_back(char16_t(C));
        UnicodeStringConst(W);
    }
    else StringConst(Value);
}

void FScript::True() { Op(EX_True); }
void FScript::False() { Op(EX_False); }
void FScript::NoObject() { Op(EX_NoObject); }

void FScript::ObjectConst(FIndex Object)
{
    Op(EX_ObjectConst);
    Ar.Idx(Object);
    Memory += kMemObjectRef;
}

void FScript::FieldPath(const std::string& PropertyName, FIndex Owner)
{
    // On disk: segment count, segments, owning object. In memory: one pointer.
    Ar.I32(1);
    Ar.Name(PropertyName);
    Ar.Idx(Owner);
    Memory += kMemObjectRef;
}

void FScript::NullFieldPath()
{
    Ar.I32(0);
    Ar.Idx(Null());
    Memory += kMemObjectRef;
}

void FScript::LocalVariable(const std::string& PropertyName, FIndex Owner)
{
    Op(EX_LocalVariable);
    FieldPath(PropertyName, Owner);
}

void FScript::LocalOutVariable(const std::string& PropertyName, FIndex Owner)
{
    Op(EX_LocalOutVariable);
    FieldPath(PropertyName, Owner);
}

void FScript::InstanceVariable(const std::string& PropertyName, FIndex Owner)
{
    Op(EX_InstanceVariable);
    FieldPath(PropertyName, Owner);
}

void FScript::StructMember(const std::string& MemberName, FIndex MemberOwner,
                           const std::function<void(FScript&)>& StructExpr)
{
    Op(EX_StructMemberContext);
    FieldPath(MemberName, MemberOwner);
    StructExpr(*this);
}

void FScript::Let(EExprToken LetOp, const std::string& PropertyName, FIndex Owner,
                  const std::function<void(FScript&)>& Var,
                  const std::function<void(FScript&)>& Value)
{
    Op(LetOp);
    if (LetOp == EX_Let)
        FieldPath(PropertyName, Owner);
    Var(*this);
    Value(*this);
}

void FScript::Context(const std::function<void(FScript&)>& ObjectExpr,
                      const std::function<void(FScript&)>& ContextExpr)
{
    Op(EX_Context);
    ObjectExpr(*this);

    // Skip count is MEMORY bytes, so the inner expression is built and measured separately first.
    FScript Inner(Ar.Owner());
    ContextExpr(Inner);

    Ar.I32(Inner.MemorySize());
    Memory += 4;
    NullFieldPath();                    // RValuePointer
    Ar.Raw(Inner.Bytes().data(), Inner.Bytes().size());
    Memory += Inner.MemorySize();
}

void FScript::StructConst(FIndex Struct, int32 SerializedSize,
                          const std::function<void(FScript&)>& Members)
{
    Op(EX_StructConst);
    Ar.Idx(Struct);
    Memory += kMemObjectRef;
    Ar.I32(SerializedSize);
    Memory += 4;
    Members(*this);
    Op(EX_EndStructConst);
}

void FScript::IntZero() { Op(EX_IntZero); }
void FScript::IntOne() { Op(EX_IntOne); }

void FScript::ArrayGetByRef(const std::function<void(FScript&)>& ArrayExpr,
                            const std::function<void(FScript&)>& IndexExpr)
{
    Op(EX_ArrayGetByRef);
    ArrayExpr(*this);
    IndexExpr(*this);
}

void FScript::CallMath(FIndex Function)
{
    Op(EX_CallMath);
    Ar.Idx(Function);
    Memory += kMemObjectRef;
}

void FScript::FinalFunction(FIndex Function)
{
    Op(EX_FinalFunction);
    Ar.Idx(Function);
    Memory += kMemObjectRef;
}

void FScript::LocalFinalFunction(FIndex Function)
{
    Op(EX_LocalFinalFunction);
    Ar.Idx(Function);
    Memory += kMemObjectRef;
}

void FScript::VirtualFunction(const std::string& FunctionName)
{
    Op(EX_VirtualFunction);
    Ar.Name(FunctionName);
    Memory += kFNameSize;
}

void FScript::Unimplemented(EExprToken Token, std::string* Err)
{
    if (Err)
        *Err = std::string("TODO: unimplemented ") + ExprName(uint8(Token));
}

/* ---- functions ---- */

int32 AddFunctionExport(FPackage& P, const FFunctionDef& Def, FIndex OwnerClass,
                        FIndex FunctionClass, FIndex FunctionTemplate,
                        const std::function<void(FScript&)>& Body,
                        const std::vector<int32>& BytecodeRefs)
{
    FExport E;
    E.ClassIndex = FunctionClass;
    E.SuperIndex = Def.Super;
    E.TemplateIndex = FunctionTemplate;
    E.OuterIndex = OwnerClass;
    E.ObjectName = Def.Name;
    E.ObjectFlags = RF_Public;

    // As cooked: bytecode refs are CreateBeforeSer, the owning class CreateBeforeCreate,
    // and no SerBeforeCreate for class/template (unlike every other export).
    E.CreateBeforeSer = BytecodeRefs;
    E.CreateBeforeCreate = { OwnerClass.V };

    const FFunctionDef Captured = Def;
    E.Serialize = [Captured, Body](FArc& Ar) {
        TagEnd(Ar);
        Ar.Bool(false);                 // lazy-object guid

        Ar.Idx(Captured.Super);         // UStruct::SuperStruct
        Ar.I32(0);                      // Children

        Ar.I32(int32(Captured.Params.size()));
        for (const FPropertyDef& Prop : Captured.Params)
            WriteProperty(Ar, Prop);

        FScript Script(Ar.Owner());
        Body(Script);
        Ar.I32(Script.MemorySize());
        Ar.I32(Script.StorageSize());
        Ar.Raw(Script.Bytes().data(), Script.Bytes().size());

        Ar.U32(Captured.FunctionFlags);
        Ar.Idx(Null());                 // EventGraphFunction
        Ar.I32(0);                      // EventGraphCallOffset
    };

    return P.AddExport(std::move(E));
}

}   // namespace Uasset
