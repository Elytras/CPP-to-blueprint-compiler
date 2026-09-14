/*
Script.cpp — property, function and bytecode emission.

The layouts here were read out of cooked DRG assets byte by byte and then checked against UE
4.27's serializers, because every one of them has a detail that guessing gets wrong: a
property carries ElementSize as well as ArrayDim, a UStruct's Children is an array rather
than the linked-list head the runtime type suggests, and a property referenced from bytecode
is a whole FFieldPath on disk even though it is one pointer in memory.

That last point is why FScript tracks two sizes. The engine stores both and they disagree by
design; writing the same number twice produces an asset that loads and then reads past the
end of its own script.
*/
#include "Script.h"

#include <cstring>

namespace Uasset
{
namespace
{
/* On disk an object reference is a 4-byte FPackageIndex; once loaded it is a pointer. */
constexpr int32 kDiskObjectRef = 4;
constexpr int32 kMemObjectRef = 8;
constexpr int32 kFNameSize = 8;
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

FPropertyDef BoolParam(const std::string& Name, uint64 ExtraFlags)
{
    return FPropertyDef{ "BoolProperty", Name, RF_Public, 1, 1,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Null() };
}

FPropertyDef ObjectParam(const std::string& Name, FIndex Class, uint64 ExtraFlags)
{
    return FPropertyDef{ "ObjectProperty", Name, RF_Public, 1, 8,
                         CPF_Parm | CPF_BlueprintVisible | CPF_BlueprintReadOnly | ExtraFlags, Class };
}

void WriteProperty(FArc& Ar, const FPropertyDef& P)
{
    Ar.Name(P.Type);                    // the FField class, e.g. "FloatProperty"

    Ar.Name(P.Name);                    // FField::Serialize
    Ar.U32(P.ObjectFlags);

    Ar.I32(P.ArrayDim);                 // FProperty::Serialize
    Ar.I32(P.ElementSize);
    Ar.Raw(&P.PropertyFlags, 8);
    Ar.U16(0);                          // RepIndex
    Ar.Name("None");                    // RepNotifyFunc
    Ar.U8(0);                           // BlueprintReplicationCondition

    /*
    The type's own tail. Only the kinds the generator emits are handled; anything else would
    have to invent bytes it cannot know, so it is left to fail at the call site instead.
    */
    if (P.Type == "ObjectProperty" || P.Type == "ClassProperty")
        Ar.Idx(P.Extra);                // PropertyClass
    else if (P.Type == "StructProperty")
        Ar.Idx(P.Extra);                // the UScriptStruct
    else if (P.Type == "BoolProperty")
    {
        // A whole-byte bool rather than a bitfield: it owns the byte, so the field mask is full.
        Ar.U8(1);                       // FieldSize
        Ar.U8(0);                       // ByteOffset
        Ar.U8(1);                       // ByteMask
        Ar.U8(0xFF);                    // FieldMask
        Ar.U8(1);                       // BoolSize
        Ar.U8(0);                       // NativeBool - declared by a Blueprint, not by C++
    }
    // FloatProperty, IntProperty, NameProperty, StrProperty: no tail.
    // TODO: unimplemented tails - ArrayProperty (Inner), MapProperty, SetProperty,
    // EnumProperty (Enum + UnderlyingProp), ByteProperty (Enum), DelegateProperty (SignatureFunction).
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
    Op(EX_Nothing);                     // the return expression: this function returns nothing
}

void FScript::IntConst(int32 Value)
{
    Op(EX_IntConst);
    Ar.I32(Value);
    Memory += 4;
}

void FScript::FloatConst(float Value)
{
    Op(EX_FloatConst);
    Ar.Raw(&Value, 4);
    Memory += 4;
}

void FScript::StringConst(const std::string& Value)
{
    Op(EX_StringConst);
    Ar.Raw(Value.data(), Value.size());
    Ar.U8(0);
    Memory += int32(Value.size()) + 1;
}

void FScript::True() { Op(EX_True); }
void FScript::False() { Op(EX_False); }

void FScript::FieldPath(const std::string& PropertyName, FIndex Owner)
{
    /*
    A property reference serializes as the path that finds it again: how many name segments,
    the segments themselves, then the object that owns the outermost one. In memory the same
    reference collapses to a single pointer, which is the entire reason MemorySize exists.
    */
    Ar.I32(1);
    Ar.Name(PropertyName);
    Ar.Idx(Owner);
    Memory += kMemObjectRef;
}

void FScript::NullFieldPath()
{
    Ar.I32(0);                          // an empty path
    Ar.Idx(Null());
    Memory += kMemObjectRef;
}

void FScript::LocalVariable(const std::string& PropertyName, FIndex Owner)
{
    Op(EX_LocalVariable);
    FieldPath(PropertyName, Owner);
}

void FScript::InstanceVariable(const std::string& PropertyName, FIndex Owner)
{
    Op(EX_InstanceVariable);
    FieldPath(PropertyName, Owner);
}

void FScript::Let(EExprToken LetOp, const std::string& PropertyName, FIndex Owner,
                  const std::function<void(FScript&)>& Var,
                  const std::function<void(FScript&)>& Value)
{
    Op(LetOp);
    if (LetOp == EX_Let)
        FieldPath(PropertyName, Owner);     // only the general form names its destination
    Var(*this);
    Value(*this);
}

void FScript::Context(const std::function<void(FScript&)>& ObjectExpr,
                      const std::function<void(FScript&)>& ContextExpr)
{
    Op(EX_Context);
    ObjectExpr(*this);

    /*
    The skip count lets the VM jump the whole call when the target turns out to be null, and
    it is expressed in loaded bytes. So the context expression is built separately, measured,
    and only then spliced in behind its own length.
    */
    FScript Inner(Ar.Owner());
    ContextExpr(Inner);

    Ar.I32(Inner.MemorySize());
    Memory += 4;
    NullFieldPath();                    // RValuePointer: nothing is assigned from the call
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

    /*
    A function's preload dependencies, as a real cooked class states them: everything the
    bytecode reaches must be CREATED before this function is serialized, and the class it lives
    in must be created before it is. Note what is absent - a function declares no
    serialize-before-create for its own class or template, unlike every other export here.
    */
    E.CreateBeforeSer = BytecodeRefs;
    E.CreateBeforeCreate = { OwnerClass.V };

    const FFunctionDef Captured = Def;
    E.Serialize = [Captured, Body](FArc& Ar) {
        TagEnd(Ar);                     // a UFunction has no reflected properties of its own
        Ar.Bool(false);                 // no lazy-object guid

        Ar.Idx(Captured.Super);         // UStruct::SuperStruct
        Ar.I32(0);                      // Children: a function owns no UFields

        Ar.I32(int32(Captured.Params.size()));
        for (const FPropertyDef& Prop : Captured.Params)
            WriteProperty(Ar, Prop);

        /*
        The script is built against the same package so its FNames land in the one name table,
        then written behind its two sizes: what it costs loaded, and what it costs here.
        */
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
