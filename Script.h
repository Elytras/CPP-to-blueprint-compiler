#pragma once
/*
Script.h — FProperty and UFunction emission, plus a small Kismet assembler.

This is the layer above the raw package writer: it knows how UE lays out a reflected property,
how a UFunction export is shaped around its bytecode, and how to write the handful of Kismet
expressions a generated class needs. All three layouts were decoded from real cooked DRG
assets and cross-checked against UE 4.27's own serializers.

The assembler deliberately implements only the opcodes a generated class actually emits.
Every other opcode still exists in UeEnums.h and still has a name via ExprName(), so an
unsupported one fails loudly and by name rather than writing a plausible-looking wrong byte.
Search for "TODO: unimplemented" to see the boundary.
*/
#include <string>
#include <vector>

#include "Package.h"
#include "UeEnums.h"

namespace Uasset
{
/*
FPropertyDef — one entry of a UStruct's ChildProperties list.

`Type` is the FField class name as UE spells it ("FloatProperty", "ObjectProperty", ...);
`Extra` carries the type's own tail, which for the pointer-ish kinds is the class or struct
being pointed at. ElementSize must match the runtime size of the type, because the engine
trusts it when laying the struct out.
*/
struct FPropertyDef
{
    std::string Type;
    std::string Name;
    uint32 ObjectFlags = RF_Public;
    int32 ArrayDim = 1;
    int32 ElementSize = 0;
    uint64 PropertyFlags = 0;
    FIndex Extra;                       // ObjectProperty: PropertyClass. StructProperty: Struct.
};

/* Convenience builders for the property kinds the generator can currently emit. */
FPropertyDef FloatParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef IntParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef Int64Param(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef BoolParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef StringParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef NameParam(const std::string& Name, uint64 ExtraFlags = 0);
FPropertyDef ObjectParam(const std::string& Name, FIndex Class, uint64 ExtraFlags = 0);

/* Writes one ChildProperties entry: the field class name, then FField and FProperty. */
void WriteProperty(FArc& Ar, const FPropertyDef& P);

/*
FScript — a Kismet bytecode buffer.

Two sizes end up in the package and they are not the same number: the in-memory size the VM
will occupy and the on-disk storage size. They diverge because a property reference costs a
pointer in memory but a whole FFieldPath on disk, so both are tracked as the buffer is built.
*/
class FScript
{
public:
    explicit FScript(FPackage* InPkg) : Ar(InPkg) {}

    void Op(EExprToken Token);
    void Self();
    void Nothing();
    void Return();                                  // EX_Return + EX_Nothing, void tail
    void Return(const std::function<void(FScript&)>& Value);   // EX_Return + expression
    void EndOfScript();
    void EndFunctionParms();

    void IntConst(int32 Value);
    void Int64Const(int64 Value);
    void ByteConst(uint8 Value);
    void FloatConst(float Value);
    void StringConst(const std::string& Value);     // ANSI, one byte per char
    void UnicodeStringConst(const std::u16string& Value);   // UCS-2, two bytes per char
    void True();
    void False();

    /*
    A branch. Targets are MEMORY offsets into this same script - the VM does not know how big
    the on-disk form of a reference was. Because the target is usually a forward jump into
    bytecode not yet written, the sequence is:

        int32 Patch = Jump(0);              // or JumpIfNot(0, Cond)
        ... emit the body the jump skips ...
        PatchJumpTarget(Patch, MemorySize());   // now MemorySize is the target's memory address

    Backward jumps skip the patch: pass MemorySize() at the loop head straight in.
    */
    int32 Jump(int32 MemTarget);
    int32 JumpIfNot(int32 MemTarget, const std::function<void(FScript&)>& Cond);
    void PatchJumpTarget(int32 StorageOffset, int32 MemTarget);

    /* A reference to a property, as the on-disk FFieldPath: path names then owning export. */
    void FieldPath(const std::string& PropertyName, FIndex Owner);
    void NullFieldPath();
    void LocalVariable(const std::string& PropertyName, FIndex Owner);
    void InstanceVariable(const std::string& PropertyName, FIndex Owner);

    /*
    Addresses `Struct.Member`: the member's FFieldPath, then the expression that yields the
    struct's address. It reads or writes at (that address + Member.Offset_Internal), typed as
    Member. The VM's only guard is that the inner expression produced a non-null property, so a
    Member whose offset/type does not match what actually lives at that address reinterprets the
    bytes there. That is the point: it reaches an offset without naming the property that owns it,
    which is what a private/cross-package field read needs to avoid the load-time mismatch.
    */
    void StructMember(const std::string& MemberName, FIndex MemberOwner,
                      const std::function<void(FScript&)>& StructExpr);

    /*
    An assignment, `Var = Value`.

    Which opcode to use is a property of the destination's type, not a choice: EX_Let names the
    property it writes and then copies ElementSize bytes, while EX_LetBool and EX_LetObj know
    their own width and so carry no property at all. Passing EX_Let for a bool writes a byte
    over a bitfield's whole containing byte; passing it for an object ref skips the reference
    bookkeeping. Both are silent offline.
    */
    void Let(EExprToken LetOp, const std::string& PropertyName, FIndex Owner,
             const std::function<void(FScript&)>& Var,
             const std::function<void(FScript&)>& Value);

    /*
    Calls something on another object: `ObjectExpr` yields the target, `ContextExpr` is the
    call made on it. The skip count the VM needs is measured for you — and it counts MEMORY
    bytes, not the bytes written here, which is a difference an object reference makes every
    time it appears.
    */
    void Context(const std::function<void(FScript&)>& ObjectExpr,
                 const std::function<void(FScript&)>& ContextExpr);

    /* A struct literal: the members are written by `Members`, in the struct's own field order. */
    void StructConst(FIndex Struct, int32 SerializedSize, const std::function<void(FScript&)>& Members);

    /* Calls. `Function` is the import or export holding the UFunction. */
    void CallMath(FIndex Function);                 // static, final, no context
    void FinalFunction(FIndex Function);
    void LocalFinalFunction(FIndex Function);
    void VirtualFunction(const std::string& FunctionName);

    /* Refuses to guess: names the opcode it cannot write yet. */
    void Unimplemented(EExprToken Token, std::string* Err);

    const std::vector<uint8>& Bytes() const { return Ar.B; }
    int32 StorageSize() const { return int32(Ar.B.size()); }
    int32 MemorySize() const { return Memory; }

private:
    FArc Ar;
    int32 Memory = 0;      // what the same stream costs once loaded, tracked alongside the bytes
};

/*
FFunctionDef — a UFunction export.

`Super` points at the engine function being overridden, which is what makes a name like
ReceiveTick an actual event rather than a new method the engine never calls.
*/
struct FFunctionDef
{
    std::string Name;
    FIndex Super;                       // the overridden engine UFunction, or null
    uint32 FunctionFlags = FUNC_BlueprintEvent | FUNC_Public | FUNC_Event;
    std::vector<FPropertyDef> Params;   // in declaration order
};

/*
Adds a UFunction export to the package, with `Body` supplying its bytecode.

Returns the new export's row. The caller still has to list it in the owning class's FuncMap,
which is what makes the engine able to find it by name.
*/
int32 AddFunctionExport(FPackage& P, const FFunctionDef& Def, FIndex OwnerClass,
                        FIndex FunctionClass, FIndex FunctionTemplate,
                        const std::function<void(FScript&)>& Body,
                        const std::vector<int32>& BytecodeRefs);

}   // namespace Uasset
