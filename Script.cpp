#include "Script.h"

#include <algorithm>
#include <map>

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

/* Container elements carry no parm flags of their own; the element property is named like its container. */
FPropertyDef Element(const FPropertyDef& In)
{
    FPropertyDef E = In;
    E.PropertyFlags &= ~uint64(CPF_Parm | CPF_OutParm | CPF_ReferenceParm | CPF_BlueprintVisible
                               | CPF_BlueprintReadOnly | CPF_Edit | CPF_DisableEditOnInstance);
    return E;
}

FPropertyDef ArrayParam(const std::string& Name, const FPropertyDef& Inner, uint64 ExtraFlags)
{
    FPropertyDef P{ "ArrayProperty", Name, RF_Public, 1, 16,
                    CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, FIndex(), Inner.Type };
    P.Inner = std::make_shared<FPropertyDef>(Element(Inner));
    return P;
}

FPropertyDef SetParam(const std::string& Name, const FPropertyDef& Elem, uint64 ExtraFlags)
{
    FPropertyDef P{ "SetProperty", Name, RF_Public, 1, 80,
                    CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, FIndex(), Elem.Type };
    P.Inner = std::make_shared<FPropertyDef>(Element(Elem));
    return P;
}

FPropertyDef MapParam(const std::string& Name, const FPropertyDef& Key, const FPropertyDef& Value, uint64 ExtraFlags)
{
    FPropertyDef P{ "MapProperty", Name, RF_Public, 1, 80,
                    CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, FIndex(), Key.Type + "," + Value.Type };
    P.Inner = std::make_shared<FPropertyDef>(Element(Key));
    P.Value = std::make_shared<FPropertyDef>(Element(Value));
    return P;
}

FPropertyDef ClassParam(const std::string& Name, FIndex UClassImp, FIndex MetaClass, uint64 ExtraFlags)
{
    // Storage is a UClass* (8b). Extra = PropertyClass (always UClass); Extra2 = MetaClass filter.
    return FPropertyDef{ "ClassProperty", Name, RF_Public, 1, 8,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags,
                         UClassImp, "", MetaClass };
}

/* TSoftObjectPtr: FWeakObjectPtr + TagAtLastTest + FSoftObjectPath (FName + FString) = 40 bytes. */
FPropertyDef SoftObjectParam(const std::string& Name, FIndex Class, uint64 ExtraFlags)
{
    return FPropertyDef{ "SoftObjectProperty", Name, RF_Public, 1, 40,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Class };
}

FPropertyDef SoftClassParam(const std::string& Name, FIndex ClassClass, FIndex MetaClass, uint64 ExtraFlags)
{
    FPropertyDef P{ "SoftClassProperty", Name, RF_Public, 1, 40,
                    CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, ClassClass };
    P.Extra2 = MetaClass;
    return P;
}

FPropertyDef StructParam(const std::string& Name, FIndex Struct, const std::string& StructName,
                         int32 Size, uint64 ExtraFlags)
{
    return FPropertyDef{ "StructProperty", Name, RF_Public, 1, Size,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Struct, StructName };
}

/* Measured on DRG (BP_PassedOut, ENE_Flea): 16 bytes, the interface class as the tail. */
FPropertyDef InterfaceParam(const std::string& Name, FIndex InterfaceClass, uint64 ExtraFlags)
{
    return FPropertyDef{ "InterfaceProperty", Name, RF_Public, 1, 16,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, InterfaceClass };
}

/* Measured on MOD_Proxy_SpawnEnemy: 16 bytes, flags Edit | BlueprintVisible | DisableEditOnInstance |
   BlueprintAssignable | BlueprintCallable, the signature function as the tail. */
FPropertyDef DispatcherParam(const std::string& Name, FIndex Signature, uint64 ExtraFlags)
{
    return FPropertyDef{ "MulticastInlineDelegateProperty", Name, RF_Public, 1, 16,
                         CPF_Edit | CPF_BlueprintVisible | CPF_DisableEditOnInstance | CPF_BlueprintAssignable
                         | CPF_BlueprintCallable | ExtraFlags, Signature };
}

bool IsAscii(const std::string& S)
{
    return std::all_of(S.begin(), S.end(), [](char C) { return uint8(C) < 0x80; });
}

std::u16string Utf8To16(const std::string& Utf8)
{
    std::u16string W;
    for (size_t I = 0; I < Utf8.size();)
    {
        const uint8 C = uint8(Utf8[I]);
        const int32 Extra = C >= 0xF0 ? 3 : C >= 0xE0 ? 2 : C >= 0xC0 ? 1 : 0;
        uint32 Cp = Extra == 0 ? C : C & (0x3F >> Extra);
        for (int32 J = 1; J <= Extra && I + J < Utf8.size(); ++J) Cp = (Cp << 6) | (uint8(Utf8[I + J]) & 0x3F);
        I += size_t(Extra) + 1;
        if (Cp >= 0x10000)
        {
            Cp -= 0x10000;
            W.push_back(char16_t(0xD800 + (Cp >> 10)));
            W.push_back(char16_t(0xDC00 + (Cp & 0x3FF)));
        }
        else W.push_back(char16_t(Cp));
    }
    return W;
}

namespace
{
/* FString's operator<<: empty is length 0, ANSI a positive length, UTF-16 a negative one (both count the null). */
void WriteFStringValue(FArc& Ar, const std::string& Utf8)
{
    if (Utf8.empty()) { Ar.I32(0); return; }
    if (IsAscii(Utf8)) { Ar.Str(Utf8); return; }
    const std::u16string W = Utf8To16(Utf8);
    Ar.I32(-int32(W.size() + 1));
    Ar.Raw(W.data(), W.size() * 2);
    Ar.U16(0);
}
}   // namespace

void DefaultRefs(const FDefaultValue& D, std::vector<int32>& Out)
{
    if (D.K == FDefaultValue::Obj && D.Object.V != 0 && std::find(Out.begin(), Out.end(), D.Object.V) == Out.end())
        Out.push_back(D.Object.V);
    for (const FDefaultValue& Item : D.Items) DefaultRefs(Item, Out);
}

namespace
{
/* D as P's value, without a tag: a tag's payload, or one array element. */
void WriteValue(FArc& V, const FPropertyDef& P, const FDefaultValue& D);
void WriteDefaultTagInner(FArc& Ar, const FPropertyDef& P);

void WriteValue(FArc& V, const FPropertyDef& P, const FDefaultValue& D)
{
    const bool bSet = D.K != FDefaultValue::None;
    {
        if (P.Type == "IntProperty") V.I32(int32(D.I));
        else if (P.Type == "BoolProperty") V.U8(bSet && D.I != 0 ? 1 : 0);     // an element; a bool member is its tag
        else if (P.Type == "FloatProperty") { const float F = float(D.F); V.Raw(&F, 4); }
        else if (P.Type == "Int64Property") V.I64(D.I);
        else if (P.Type == "EnumProperty") V.Name(D.K == FDefaultValue::Str ? D.S : P.EnumZero);
        else if (P.Type == "ByteProperty") { if (P.StructName.empty()) V.U8(uint8(D.I)); else V.Name(D.K == FDefaultValue::Str ? D.S : P.EnumZero); }   // an enum byte is its enumerator's name
        else if (P.Type == "StrProperty") WriteFStringValue(V, D.S);
        else if (P.Type == "NameProperty") V.Name(bSet ? D.S : std::string("None"));
        else if (P.Type == "TextProperty")
        {
            /* A literal is a culture-invariant text, like EX_TextConst's LiteralString: Flags,
               ETextHistoryType::None, bHasCultureInvariantString, the string. */
            const bool bText = bSet && !D.S.empty();
            V.U32(bText ? 2 : 0);           // ETextFlag::CultureInvariant
            V.U8(0xFF);
            V.I32(bText ? 1 : 0);
            if (bText) WriteFStringValue(V, D.S);
        }
        else if (P.Type == "ObjectProperty") V.Idx(D.K == FDefaultValue::Obj ? D.Object : Null());
        else if (P.Type == "ClassProperty" || P.Type == "InterfaceProperty") V.I32(0);
        else if (P.Type == "SoftObjectProperty" || P.Type == "SoftClassProperty") { V.Name("None"); V.I32(0); }   // FSoftObjectPath: AssetPathName, SubPathString
        else if (P.Type == "SetProperty" || P.Type == "MapProperty")
        {
            /* Removed count, count, then the elements; a map's Items alternate key, value. */
            const bool bMap = P.Type == "MapProperty" && P.Value;
            V.I32(0);
            V.I32(int32(D.Items.size() / (bMap ? 2 : 1)));
            if (P.Inner)
                for (size_t I = 0; I < D.Items.size(); ++I)
                    WriteValue(V, bMap && I % 2 == 1 ? *P.Value : *P.Inner, D.Items[I]);
        }
        else if (P.Type == "ArrayProperty")
        {
            V.I32(int32(D.Items.size()));
            /* An array of structs carries an inner tag between the count and the elements, even when
               empty (FArrayProperty::SerializeItem; checked against an editor-saved array of two).
               Its Size is the elements' byte count, so they are measured first. */
            FArc Elements(V.Owner());
            if (P.Inner) for (const FDefaultValue& Item : D.Items) WriteValue(Elements, *P.Inner, Item);
            if (P.Inner && P.Inner->Type == "StructProperty")
            {
                V.Name(P.Name); V.Name("StructProperty"); V.I32(int32(Elements.B.size())); V.I32(0);
                V.Name(P.Inner->StructName);
                for (int32 I = 0; I < 4; ++I) V.U32(0);
                V.U8(0);
            }
            V.Append(Elements);
        }
        else if (P.Type == "StructProperty")
        {
            /* A struct with a native Serialize writes raw bytes, not tags; a zero of ElementSize
               bytes is its default. Box / Box2D serialize IsValid as one byte, so they are shorter. */
            static const std::map<std::string, int32> Native = {
                { "Vector", 12 }, { "Vector2D", 8 }, { "Vector4", 16 }, { "Rotator", 12 }, { "Quat", 16 },
                { "Plane", 16 }, { "Matrix", 64 }, { "Color", 4 }, { "LinearColor", 16 }, { "IntPoint", 8 },
                { "IntVector", 12 }, { "Guid", 16 }, { "DateTime", 8 }, { "Timespan", 8 }, { "Box", 25 },
                { "Box2D", 17 }, { "BoxSphereBounds", 28 }, { "FrameNumber", 4 } };
            auto N = Native.find(P.StructName);
            if (D.K == FDefaultValue::Struct && P.Members)
            {
                if (N == Native.end())
                {
                    for (const FPropertyDef& M : *P.Members) WriteDefaultTagInner(V, M);
                    TagEnd(V);
                }
                else
                {
                    /* Raw, in declaration order, then zero-padded to the size the engine reads -
                       which also supplies Box's trailing IsValid byte. */
                    FArc Raw(V.Owner());
                    for (const FPropertyDef& M : *P.Members) WriteValue(Raw, M, M.Default);
                    V.Append(Raw);
                    for (int32 i = int32(Raw.B.size()); i < N->second; ++i) V.U8(0);
                }
            }
            else if (N == Native.end()) TagEnd(V);
            else for (int32 i = 0; i < N->second; ++i) V.U8(0);
        }
    }
}

void WriteDefaultTagInner(FArc& Ar, const FPropertyDef& P)
{
    const FDefaultValue& D = P.Default;
    if (P.Type == "BoolProperty") { TagBool(Ar, P.Name, D.K != FDefaultValue::None && D.I != 0); return; }
    Tag(Ar, P.Name, P.Type, [&](FArc& V) { WriteValue(V, P, D); }, P.StructName);
}
}   // namespace

void WriteDefaultTag(FArc& Ar, const FPropertyDef& P)
{
    const FDefaultValue& D = P.Default;
    if (P.Type == "BoolProperty") { TagBool(Ar, P.Name, D.K != FDefaultValue::None && D.I != 0); return; }
    Tag(Ar, P.Name, P.Type, [&](FArc& V) { WriteValue(V, P, D); }, P.StructName);
}

void WriteProperty(FArc& Ar, const FPropertyDef& P, bool bUncooked)
{
    Ar.Name(P.Type);

    Ar.Name(P.Name);                    // FField::Serialize
    Ar.U32(P.ObjectFlags);
    // An uncooked (editor) package serializes a per-field metadata map; a cooked one strips it
    // (FField::Serialize gates it on !IsCooking). Every FField carries the flag, inner ones too.
    if (bUncooked) Ar.Bool(false);      // bHasMetaData

    Ar.I32(P.ArrayDim);                 // FProperty::Serialize
    Ar.I32(P.ElementSize);
    Ar.Raw(&P.PropertyFlags, 8);
    Ar.U16(0);                          // RepIndex: UClass::SetUpRuntimeReplicationData numbers CPF_Net properties at load
    Ar.Name(P.RepNotify.empty() ? std::string("None") : P.RepNotify);   // RepNotifyFunc
    Ar.U8(P.RepCondition);              // BlueprintReplicationCondition

    if (P.Type == "ObjectProperty" || P.Type == "SoftObjectProperty")
        Ar.Idx(P.Extra);                // PropertyClass
    else if (P.Type == "InterfaceProperty")
        Ar.Idx(P.Extra);                // InterfaceClass
    else if (P.Type == "MulticastInlineDelegateProperty")
        Ar.Idx(P.Extra);                // SignatureFunction
    else if (P.Type == "ClassProperty" || P.Type == "SoftClassProperty")
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
    else if (P.Type == "EnumProperty")
    {
        Ar.Idx(P.Extra);                // Enum
        WriteProperty(Ar, *P.Inner, bUncooked);    // UnderlyingProp, through SerializeSingleField
    }
    else if (P.Type == "ArrayProperty" || P.Type == "SetProperty")
        WriteProperty(Ar, *P.Inner, bUncooked);    // SerializeSingleField: type name then the field
    else if (P.Type == "MapProperty")
    {
        WriteProperty(Ar, *P.Inner, bUncooked);    // KeyProp
        WriteProperty(Ar, *P.Value, bUncooked);    // ValueProp
    }
    // TODO: unimplemented tail - DelegateProperty (SignatureFunction).
}

/* ---- bytecode ---- */

void FScript::ClassCast(EExprToken Token, FIndex Class, const std::function<void(FScript&)>& Expr)
{
    Op(Token);
    Ar.Idx(Class);
    Memory += kMemObjectRef;
    Expr(*this);
}

void FScript::InterfaceContext(const std::function<void(FScript&)>& InterfaceExpr)
{
    Op(EX_InterfaceContext);
    InterfaceExpr(*this);
}

void FScript::InstanceDelegate(const std::string& FunctionName)
{
    Op(EX_InstanceDelegate);
    Ar.Name(FunctionName);
    Memory += kFNameSize;
}

void FScript::AddMulticastDelegate(const std::function<void(FScript&)>& Dispatcher,
                                   const std::function<void(FScript&)>& Delegate)
{
    Op(EX_AddMulticastDelegate);
    Dispatcher(*this);
    Delegate(*this);
}

void FScript::RemoveMulticastDelegate(const std::function<void(FScript&)>& Dispatcher,
                                      const std::function<void(FScript&)>& Delegate)
{
    Op(EX_RemoveMulticastDelegate);
    Dispatcher(*this);
    Delegate(*this);
}

void FScript::ClearMulticastDelegate(const std::function<void(FScript&)>& Dispatcher)
{
    Op(EX_ClearMulticastDelegate);
    Dispatcher(*this);
}

void FScript::CallMulticastDelegate(FIndex Signature)
{
    Op(EX_CallMulticastDelegate);
    Ar.Idx(Signature);
    Memory += kMemObjectRef;
}

void FScript::Op(EExprToken Token)
{
    Ar.U8(uint8(Token));
    Memory += 1;
}

void FScript::Raw(const void* Bytes, size_t StorageBytes, int32 MemBytes)
{
    Ar.Raw(Bytes, StorageBytes);
    Memory += MemBytes;
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

void FScript::ComputedJump(const std::function<void(FScript&)>& OffsetExpr)
{
    Op(EX_ComputedJump);
    OffsetExpr(*this);
}

void FScript::RawInt32(int32 Value)
{
    Ar.U32(uint32(Value));
    Memory += 4;
}

int32 FScript::SkipOffsetConst(int32 MemTarget)
{
    Op(EX_SkipOffsetConst);
    const int32 PatchAt = int32(Ar.B.size());
    RawInt32(MemTarget);
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
    if (bWide || !IsAscii(Value)) UnicodeStringConst(Utf8To16(Value));
    else StringConst(Value);
}

void FScript::True() { Op(EX_True); }
void FScript::False() { Op(EX_False); }
void FScript::NoObject() { Op(EX_NoObject); }

void FScript::SoftObjectConst(const std::string& Path)
{
    Op(EX_SoftObjectConst);
    StringConst(Path);
}

void FScript::DynamicCast(FIndex Class, const std::function<void(FScript&)>& Expr)
{
    ClassCast(EX_DynamicCast, Class, Expr);
}

void FScript::ObjectConst(FIndex Object)
{
    Op(EX_ObjectConst);
    Ar.Idx(Object);
    Memory += kMemObjectRef;
}

void FScript::FieldPath(const std::string& PropertyName, FIndex Owner)
{
    FieldPath(std::vector<std::string>{ PropertyName }, Owner);
}

void FScript::FieldPath(const std::vector<std::string>& Path, FIndex Owner)
{
    // On disk: segment count, segments, owning object. In memory: one pointer.
    Ar.I32(int32(Path.size()));
    for (const std::string& Segment : Path) Ar.Name(Segment);
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
    LetPath(LetOp, { PropertyName }, Owner, Var, Value);
}

void FScript::LetPath(EExprToken LetOp, const std::vector<std::string>& Path, FIndex Owner,
                      const std::function<void(FScript&)>& Var,
                      const std::function<void(FScript&)>& Value)
{
    Op(LetOp);
    if (LetOp == EX_Let)
        FieldPath(Path, Owner);
    Var(*this);
    Value(*this);
}

void FScript::Context(const std::function<void(FScript&)>& ObjectExpr,
                      const std::function<void(FScript&)>& ContextExpr,
                      const std::string& RValue, FIndex RValueOwner)
{
    Op(EX_Context);
    ObjectExpr(*this);

    // Skip count is MEMORY bytes, so the inner expression is built and measured separately first.
    FScript Inner(Ar.Owner());
    ContextExpr(Inner);

    Ar.I32(Inner.MemorySize());
    Memory += 4;
    if (RValue.empty()) NullFieldPath();    // RValuePointer
    else FieldPath(RValue, RValueOwner);
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

void FScript::LetValueOnPersistentFrame(const std::string& PropertyName, FIndex UberGraph,
                                        const std::function<void(FScript&)>& Value)
{
    Op(EX_LetValueOnPersistentFrame);
    FieldPath(PropertyName, UberGraph);
    Value(*this);
}

void FScript::LocalVirtualFunction(const std::string& FunctionName)
{
    Op(EX_LocalVirtualFunction);
    Ar.Name(FunctionName);
    Memory += kFNameSize;
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
        if (Captured.FunctionFlags & 0x40) Ar.U16(0);   // FUNC_Net: the unused RepOffset
        Ar.Idx(Null());                 // EventGraphFunction
        Ar.I32(0);                      // EventGraphCallOffset
    };

    return P.AddExport(std::move(E));
}

}   // namespace Uasset
