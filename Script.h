#pragma once
#include <memory>
#include <string>
#include <vector>

#include "Package.h"
#include "UeEnums.h"

namespace Uasset
{
bool IsAscii(const std::string& S);
std::u16string Utf8To16(const std::string& Utf8);

/* A member's literal initializer. None = the type's zero value. */
struct FDefaultValue
{
    enum EKind { None, Int, Float, Bool, Str } K = None;
    int64 I = 0;                        // Int, Bool
    double F = 0.0;                     // Float
    std::string S;                      // Str: UTF-8, for StrProperty / NameProperty / TextProperty
};

/* One ChildProperties entry. ElementSize must equal the type's runtime size; the engine lays the struct out from it. */
struct FPropertyDef
{
    std::string Type;
    std::string Name;
    uint32 ObjectFlags = RF_Public;
    int32 ArrayDim = 1;
    int32 ElementSize = 0;
    uint64 PropertyFlags = 0;
    FIndex Extra;                       // ObjectProperty / SoftObjectProperty: PropertyClass. StructProperty: Struct. ByteProperty: Enum.
    std::string StructName;             // StructProperty: the struct; ByteProperty: the enum; Array/Set/Map: inner type(s). For the default-value tag.
    FIndex Extra2;                      // ClassProperty / SoftClassProperty: MetaClass. Last, so the aggregate inits above it still line up.
    std::string EnumZero;               // ByteProperty with an enum: the enumerator the zero value is written as
    std::shared_ptr<FPropertyDef> Inner;    // ArrayProperty / SetProperty: element; MapProperty: key
    std::shared_ptr<FPropertyDef> Value;    // MapProperty: value
    FDefaultValue Default;              // written into the CDO / struct default instance
    std::string RepNotify;              // CPF_RepNotify: the function the client runs when the value arrives
    uint8 RepCondition = 0;             // CPF_Net: ELifetimeCondition
};

FPropertyDef FloatParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef IntParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef Int64Param(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef BoolParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef ByteParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef StringParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef NameParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef TextParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef ObjectParam(const std::string& Name, FIndex Class, uint64 ExtraFlags = 0);
/* PropertyClass = UClass, MetaClass = the subclass filter (`class UClass *X` -> UObject). */
FPropertyDef ClassParam(const std::string& Name, FIndex ClassClass, FIndex MetaClass, uint64 ExtraFlags = 0);
FPropertyDef SoftObjectParam(const std::string& Name, FIndex Class, uint64 ExtraFlags = 0);
FPropertyDef SoftClassParam(const std::string& Name, FIndex ClassClass, FIndex MetaClass, uint64 ExtraFlags = 0);
FPropertyDef ArrayParam(const std::string& Name, const FPropertyDef& Inner, uint64 ExtraFlags = 0);
FPropertyDef SetParam(const std::string& Name, const FPropertyDef& Element, uint64 ExtraFlags = 0);
FPropertyDef MapParam(const std::string& Name, const FPropertyDef& Key, const FPropertyDef& Value, uint64 ExtraFlags = 0);
FPropertyDef StructParam(const std::string& Name, FIndex Struct, const std::string& StructName,
                         int32 Size, uint64 ExtraFlags = 0);
/* TScriptInterface: object + interface pointer. Extra = the interface class. */
FPropertyDef InterfaceParam(const std::string& Name, FIndex InterfaceClass, uint64 ExtraFlags = 0);
/* An event dispatcher. Extra = its <Name>__DelegateSignature function. */
FPropertyDef DispatcherParam(const std::string& Name, FIndex Signature, uint64 ExtraFlags = 0);

void WriteProperty(FArc& Ar, const FPropertyDef& P);

/* The property as a tagged-property entry holding P.Default (its zero value when unset). */
void WriteDefaultTag(FArc& Ar, const FPropertyDef& P);

/*
Kismet bytecode buffer. MemorySize and StorageSize differ by design: a property reference is
a pointer in memory but a whole FFieldPath on disk. The package stores both.
*/
class FScript
{
public:
    explicit FScript(FPackage* InPkg) : Ar(InPkg) {}

    void Op(EExprToken Token);
    void Self();
    void Nothing();
    void Return();
    void Return(const std::function<void(FScript&)>& Value);
    void EndOfScript();
    void EndFunctionParms();

    void IntConst(int32 Value);
    void Int64Const(int64 Value);
    void ByteConst(uint8 Value);
    void FloatConst(float Value);
    void NameConst(const std::string& NameStr);
    void StringConst(const std::string& Value);             // ANSI
    void UnicodeStringConst(const std::u16string& Value);   // UCS-2
    void TextConst(const std::string& Value, bool bWide);   // EX_TextConst LiteralString
    void True();
    void False();
    void NoObject();
    void ObjectConst(FIndex Object);
    void SoftObjectConst(const std::string& Path);
    void DynamicCast(FIndex Class, const std::function<void(FScript&)>& Expr);
    /* EX_ObjToInterfaceCast / EX_CrossInterfaceCast / EX_InterfaceToObjCast / EX_DynamicCast: class, then the value. */
    void ClassCast(EExprToken Token, FIndex Class, const std::function<void(FScript&)>& Expr);
    /* The object an EX_Context runs against, read out of an FScriptInterface. */
    void InterfaceContext(const std::function<void(FScript&)>& InterfaceExpr);

    /* A delegate bound to self's function by name, with no local. */
    void InstanceDelegate(const std::string& FunctionName);
    /* Dispatcher must leave MostRecentProperty on a multicast delegate property: EX_InstanceVariable,
       or an EX_Context whose rvalue is that property. */
    void AddMulticastDelegate(const std::function<void(FScript&)>& Dispatcher,
                              const std::function<void(FScript&)>& Delegate);
    void RemoveMulticastDelegate(const std::function<void(FScript&)>& Dispatcher,
                                 const std::function<void(FScript&)>& Delegate);
    void ClearMulticastDelegate(const std::function<void(FScript&)>& Dispatcher);
    /* Follow with the dispatcher, one expression per signature parameter, then EndFunctionParms. */
    void CallMulticastDelegate(FIndex Signature);

    /*
    Jump targets are MEMORY offsets, not storage offsets. Forward jump:
        int32 Patch = Jump(0); ...body...; PatchJumpTarget(Patch, MemorySize());
    */
    int32 Jump(int32 MemTarget);
    int32 JumpIfNot(int32 MemTarget, const std::function<void(FScript&)>& Cond);
    void PatchJumpTarget(int32 StorageOffset, int32 MemTarget);
    /* EX_ComputedJump: OffsetExpr yields the int32 MEMORY offset to continue at. */
    void ComputedJump(const std::function<void(FScript&)>& OffsetExpr);
    /* A raw int32 operand after an Op, to patch later through PatchJumpTarget. */
    void RawInt32(int32 Value);
    /* EX_SkipOffsetConst: a MEMORY offset as a value (FLatentActionInfo::Linkage). Returns the patch position. */
    int32 SkipOffsetConst(int32 MemTarget);
    /* Patch positions of latent calls' Linkage whose resume point is the end of the statement being emitted. */
    std::vector<int32> LatentResumes;
    /* The same for an await: the event that resumes lives in another function, so the offset is stored, not patched. */
    std::vector<std::shared_ptr<int32>> ResumeSinks;

    void FieldPath(const std::string& PropertyName, FIndex Owner);
    void FieldPath(const std::vector<std::string>& Path, FIndex Owner);     // innermost first: {"Items", "Items"} is an array's element
    void NullFieldPath();
    void LocalVariable(const std::string& PropertyName, FIndex Owner);
    void LocalOutVariable(const std::string& PropertyName, FIndex Owner);
    void InstanceVariable(const std::string& PropertyName, FIndex Owner);

    /* Accesses (StructExpr address + Member.Offset_Internal) typed as Member; the VM never checks Member matches what lives there. */
    void StructMember(const std::string& MemberName, FIndex MemberOwner,
                      const std::function<void(FScript&)>& StructExpr);

    /* EX_Let names its property and copies ElementSize bytes; EX_LetBool / EX_LetObj carry no property. Wrong choice is silent offline. */
    void Let(EExprToken LetOp, const std::string& PropertyName, FIndex Owner,
             const std::function<void(FScript&)>& Var,
             const std::function<void(FScript&)>& Value);
    void LetPath(EExprToken LetOp, const std::vector<std::string>& Path, FIndex Owner,
                 const std::function<void(FScript&)>& Var,
                 const std::function<void(FScript&)>& Value);

    /* The skip count is MEMORY bytes of ContextExpr, measured here. RValue names the property
       ContextExpr reads, which the VM zeroes when the object is null; a call leaves it empty. */
    void Context(const std::function<void(FScript&)>& ObjectExpr,
                 const std::function<void(FScript&)>& ContextExpr,
                 const std::string& RValue = std::string(), FIndex RValueOwner = FIndex());

    void StructConst(FIndex Struct, int32 SerializedSize, const std::function<void(FScript&)>& Members);

    /* Bounds-checked array element deref, guard-free: writes InnerProp.ElementSize bytes from
       (*ArrayExpr).Data[Index] to the caller's dest. The outer must set MostRecentProperty to an
       FArrayProperty (StructMember of a TArray field, or LocalVariable of a TArray local). */
    void ArrayGetByRef(const std::function<void(FScript&)>& ArrayExpr,
                       const std::function<void(FScript&)>& IndexExpr);

    void IntZero();
    void IntOne();

    void CallMath(FIndex Function);
    void FinalFunction(FIndex Function);
    void LocalFinalFunction(FIndex Function);
    /* Writes Value into the ubergraph frame property PropertyName of UberGraph (an event stub copying a parm in). */
    void LetValueOnPersistentFrame(const std::string& PropertyName, FIndex UberGraph,
                                   const std::function<void(FScript&)>& Value);
    void VirtualFunction(const std::string& FunctionName);
    /* The same, through ProcessLocalFunction: no UObject::CallFunction, so no RPC routing. */
    void LocalVirtualFunction(const std::string& FunctionName);

    void Unimplemented(EExprToken Token, std::string* Err);

    const std::vector<uint8>& Bytes() const { return Ar.B; }
    int32 StorageSize() const { return int32(Ar.B.size()); }
    int32 MemorySize() const { return Memory; }

private:
    FArc Ar;
    int32 Memory = 0;
};

/* Super is the overridden engine UFunction (what makes e.g. ReceiveTick an event), or null. */
struct FFunctionDef
{
    std::string Name;
    FIndex Super;
    uint32 FunctionFlags = FUNC_BlueprintEvent | FUNC_Public | FUNC_Event;
    std::vector<FPropertyDef> Params;
};

/* Returns the export row. The caller must still list it in the owning class's FuncMap. */
int32 AddFunctionExport(FPackage& P, const FFunctionDef& Def, FIndex OwnerClass,
                        FIndex FunctionClass, FIndex FunctionTemplate,
                        const std::function<void(FScript&)>& Body,
                        const std::vector<int32>& BytecodeRefs);

}   // namespace Uasset
