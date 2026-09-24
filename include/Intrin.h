#pragma once
/*
Intrin.h - AssetGen compiler intrinsics.

Each `__NAME__` here is a name Cpp.cpp recognises and lowers to a fixed Kismet expression
shape; none of them corresponds to a real UFunction, so their bodies do not exist. The
signatures are only what clang wants under -fsyntax-only, so a mod source that uses one
still parses. Include this header from any mod .cpp that reaches for one.

The Read<N>__ / __ReadObject__ / __ReadName__ family arbitrary-derefs the given address.
AssetGen's hoist pass rewrites each call into two statements against a per-function FDeref
scratch the compiler synthesizes: write the address into Data, then read element[0]
of an engine view struct's TArray<T> via EX_ArrayGetByRef. Both hops (StructMemberContext
and ArrayGetByRef) are guard-free, so the resulting pak carries the read without any
native patch loaded.

Raw pointers need none of them: a pointer to anything but a UObject (void*, int32*, FName*,
FVector*, UObject**) is an int64 address in the Blueprint, and `*P`, `P[I]`, `P + N`, `P - Q`,
`P->Member`, `&Items[I]` and `T& R = *P` lower to the same reads and writes. A mod using them needs
no declaration of its own. A T& parameter stays an out-parm; a T& return value is refused.
*/
#include "UeApi/Types.h"

class UObject;
class UClass;

int64          __AddrOf__(class UObject *Ref); // Ref reinterpreted as int64 (its address)
int64          __Read64__(int64 Addr);         // *(int64*)Addr        - 8-byte deref
int32          __Read32__(int64 Addr);         // *(int32*)Addr        - 4-byte int deref
float          __ReadFloat__(int64 Addr);      // *(float*)Addr        - 4-byte float deref
uint8          __ReadByte__(int64 Addr);       // *(uint8*)Addr        - 1-byte deref
class UObject *__ReadObject__(int64 Addr);     // *(UObject**)Addr     - 8-byte pointer deref
FName          __ReadName__(int64 Addr);       // *(FName*)Addr        - 8-byte FName deref
class UClass  *__ReadClass__(int64 Addr);      // *(UClass**)Addr      - 8-byte UClass pointer deref
FText          __ReadText__(int64 Addr);       // *(FText*)Addr        - 24-byte FText value deref (deep-copied)
FString        __ReadString__(int64 Addr);     // *(FString*)Addr      - 16-byte FString value deref (deep-copied)
int32          __NameIndex__(FName Name);      // Name.ComparisonIndex - the first 4 bytes of the FName slot
/* A by-reference expression AT Addr (MostRecentPropertyAddress == Addr), never copied into a
   temp. Only meaningful as the wildcard argument of a CustomThunk native that steps it with a
   null result; read as a value it is 8 bytes of int64. */
int64 __RefAt__(int64 Addr);

/* The same, given a pointer: its address. */
template <class T> int64          __Read64__(const T *Addr);
template <class T> int32          __Read32__(const T *Addr);
template <class T> float          __ReadFloat__(const T *Addr);
template <class T> uint8          __ReadByte__(const T *Addr);
template <class T> class UObject *__ReadObject__(const T *Addr);
template <class T> FName          __ReadName__(const T *Addr);
template <class T> class UClass  *__ReadClass__(const T *Addr);
template <class T> FText          __ReadText__(const T *Addr);
template <class T> FString        __ReadString__(const T *Addr);
template <class T> int64          __RefAt__(const T *Addr);

/*
Pointer, reference and int64 conversions, resolved at compile time:
  __PtrCast__<int64>(Obj)     the object's address         __PtrCast__<AActor*>(Addr)  the object at an address
  __PtrCast__<int32*>(Addr)   an address as a pointer      __PtrCast__<int32&>(Addr)   the int32 at Addr, an lvalue
  __PtrCast__<int64>(R)       the address that `int32& R = *P` keeps
A reference to a Blueprint variable has no address to give, so it is refused.
*/
template <class To, class From> To __PtrCast__(From &&Value);

/*
Raw Kismet bytecode, appended verbatim to the current script stream. Bytes is a hex string:
whitespace and commas are ignored, case does not matter, and the digit count must be even.
AssetGen advances the VM's memory offset (what jumps step over) by MemBytes; the one-arg form
uses the storage length, which is right for tokens whose disk and memory forms match, and
wrong for anything carrying an FName, an object reference or a jump target. No token is
inspected: garbage bytes produce a package the VM will execute anyway. An escape hatch for
ops the DSL cannot yet express.
*/
void __Asm__(const char *Bytes);
void __Asm__(const char *Bytes, int32 MemBytes);

/*
The bytes of a file, read at build time and baked into the class default:

    TArray<uint8> Payload = __EmbedFile__("data/blob.bin");

Only valid as the initialiser of a TArray<uint8> member - the file has no runtime existence, the
CDO carries one array element per byte. The path is relative to the mod .cpp being compiled, and
a missing or empty file fails the build.
*/
TArray<uint8> __EmbedFile__(const char *Path);

/*
Runtime lookup: at build time `__ClassOf__(x)` is rewritten into a call to
`GetParmClassName(<current UFunction*>, FName("x"))` on the enclosing class - where "x" is
the argument's own spelling captured at build time and the UFunction* is an ObjectConst
pointing at the export being emitted. At runtime the helper walks that function's
ChildProperties for a parm named "x" and returns its FField class name.

Slower than a compile-time literal (a chain walk per call) but tracks the parm's real
declared type - rename or retype a parm and the class-check follows without re-baking any
string. The DSL class that USES __ClassOf__ must declare a matching GetParmClassName method;
ReadProperty is the reference.
*/
template <class T> FName __ClassOf__(const T &Ref);
