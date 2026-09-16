#pragma once
#include <string>
#include <vector>

#include "Package.h"
#include "UeEnums.h"

namespace Uasset
{
/* One ChildProperties entry. ElementSize must equal the type's runtime size; the engine lays the struct out from it. */
struct FPropertyDef
{
    std::string Type;
    std::string Name;
    uint32 ObjectFlags = RF_Public;
    int32 ArrayDim = 1;
    int32 ElementSize = 0;
    uint64 PropertyFlags = 0;
    FIndex Extra;                       // ObjectProperty: PropertyClass. StructProperty: Struct. ByteProperty: Enum.
    std::string StructName;             // StructProperty: the struct's name, for the default-value tag
};

FPropertyDef FloatParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef IntParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef Int64Param(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef BoolParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef ByteParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef StringParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef NameParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef ObjectParam(const std::string& Name, FIndex Class, uint64 ExtraFlags = 0);
FPropertyDef StructParam(const std::string& Name, FIndex Struct, const std::string& StructName,
                         int32 Size, uint64 ExtraFlags = 0);

void WriteProperty(FArc& Ar, const FPropertyDef& P);

/* The property as a tagged-property entry holding its zero value. */
void WriteZeroValueTag(FArc& Ar, const FPropertyDef& P);

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
    void True();
    void False();
    void NoObject();
    void ObjectConst(FIndex Object);

    /*
    Jump targets are MEMORY offsets, not storage offsets. Forward jump:
        int32 Patch = Jump(0); ...body...; PatchJumpTarget(Patch, MemorySize());
    */
    int32 Jump(int32 MemTarget);
    int32 JumpIfNot(int32 MemTarget, const std::function<void(FScript&)>& Cond);
    void PatchJumpTarget(int32 StorageOffset, int32 MemTarget);

    void FieldPath(const std::string& PropertyName, FIndex Owner);
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

    /* The skip count is MEMORY bytes of ContextExpr, measured here. */
    void Context(const std::function<void(FScript&)>& ObjectExpr,
                 const std::function<void(FScript&)>& ContextExpr);

    void StructConst(FIndex Struct, int32 SerializedSize, const std::function<void(FScript&)>& Members);

    void CallMath(FIndex Function);
    void FinalFunction(FIndex Function);
    void LocalFinalFunction(FIndex Function);
    void VirtualFunction(const std::string& FunctionName);

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
