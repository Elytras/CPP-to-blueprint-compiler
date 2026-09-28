/* MemoryRead: reading the game's memory through raw pointers.

   The mod waits for your dwarf, then reads the header that every UObject starts with: the object's name, its index
   in the engine's object list, its class and the object that contains it. In game you see one game message of the
   form "<name> is object #<index>, a <class> in <outer>. Offsets check out: true". The game spawns InitSpacerig in
   the Space Rig and InitCave in a mission; both are MemoryRead under another name.

   Nothing checks an address. A wrong offset, or a read through a null or destroyed object, returns garbage or
   crashes the game, as it would in native C++. Read only fields whose offsets you know, of objects you know are
   alive. */
#include "UeApi/Types.h"

#include "UeApi/Engine.h"
#include "UeApi/FSD.h"

#include "../include/Intrin.h" // AssetGen's include/ folder, by its path from this file

UE_MOD_PACKAGE("/Game/_AssetGenExamples/MemoryRead");

/* The first 40 bytes of every UObject, as the SDK's CoreUObject.h states them for UE 4.27: the vtable at 0x0,
   ObjectFlags at 0x8, InternalIndex at 0xC, ClassPrivate at 0x10, NamePrivate at 0x18 and OuterPrivate at 0x20.
   Each member sits at its natural alignment, as in C++, so the offsets match. The code never fills one in: it
   points at an object's memory as if it were one. Like any UE_STRUCT, it is cooked beside the class as
   FObjectHeader.uasset. */
struct FObjectHeader {
  UE_STRUCT;
  int64 VTable;
  int32 ObjectFlags;
  int32 InternalIndex;
  UClass *ClassPrivate;
  FName NamePrivate;
  UObject *OuterPrivate;
};

/* The mod's actor. The game starts it through InitSpacerig and InitCave below; it reads one header and is done. */
class MemoryRead : public AActor {
public:
  void ReceiveBeginPlay() {
    /* First we wait for your dwarf: the game can start the mod before the character exists. Delay is the latent
       Delay node, so each round of the loop hands control back to the engine and resumes here a second later. */
    APlayerCharacter *Dwarf = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    while (Dwarf == nullptr) {
      UKismetSystemLibrary::Delay(1.0f);
      Dwarf = UGameFunctionLibrary::GetLocalPlayerCharacter(this);
    }

    /* A function that makes a latent call runs in the event graph, where a memory read is not built. So the reads
       live in ReadHeader, an ordinary function that never waits, and this one only calls it. */
    UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage(ReadHeader(Dwarf));
  }

private:
  /* ReceiveBeginPlay calls this once the dwarf exists and posts what it returns. Every read below goes through the
     dwarf's own address, so it is only as safe as Dwarf: a live object, which the loop above made sure of. It is not
     inline: an inline body is pasted into its caller, where its reads would sit in the latent ReceiveBeginPlay and
     be refused. */
  FString ReadHeader(APlayerCharacter *Dwarf) {
    /* First we turn the object into an address. An object pointer is a reference the garbage collector tracks, not
       an address, so * and [] are refused on it. __PtrCast__<int64> gives its 8 bytes as a number, and __PtrCast__
       to a pointer type makes that number something to read through. Neither is a function call: __PtrCast__ is
       resolved when the mod compiles. A C-style cast, (int64)Dwarf or (uint8 *)Dwarf, does the same, and the byte
       reads further down use that spelling. */
    int64 Addr = __PtrCast__<int64>(Dwarf);
    FObjectHeader *Header = __PtrCast__<FObjectHeader *>(Addr);

    /* Header->Member reads one member in place, at Addr plus the member's offset; the struct is never copied. Every
       read goes through FDeref, a 16-byte scratch struct the compiler creates and cooks as FDeref.uasset beside the
       class. You declare nothing, but FDeref.uasset is part of the mod and ships with it. */
    FName Name = Header->NamePrivate;
    int32 Index = Header->InternalIndex;

    /* The same memory as int32 words. P[I] reads at P + I * sizeof(int32), and sizeof is the size in the game, so
       the byte offset 0xC is word 0xC / sizeof(int32). Reading InternalIndex both ways checks the struct's layout. */
    int32 *Words = __PtrCast__<int32 *>(Addr);
    bool bLayoutOk = Words[0xC / sizeof(int32)] == Index;

    /* And as bytes: a byte pointer moves by bytes, so Bytes + 0x10 is ClassPrivate. Reading through a UClass** gives
       the 8 bytes there back as a class reference, which we check against the engine's own GetClass. */
    uint8 *Bytes = (uint8 *)Dwarf;
    UClass *Cls = *(UClass **)(Bytes + 0x10);
    bool bClassOk = Cls == Dwarf->GetClass();

    /* An object pointer stored in memory is 8 bytes of address. Read as an int64, __PtrCast__ turns it back into an
       object; *(UObject **)(Bytes + 0x20) does both steps at once. */
    int64 OuterAddr = *(int64 *)(Bytes + 0x20);
    UObject *Outer = __PtrCast__<UObject *>(OuterAddr);

    return FString(Name) + " is object #" + Index + ", a " + Cls + " in " + Outer +
           ". Offsets check out: " + (bLayoutOk && bClassOk);
  }
};

/* DRG's mod support spawns every mounted mod's InitSpacerig in the Space Rig and its InitCave in a mission. Both are
   empty, so whichever one the game spawns runs MemoryRead's ReceiveBeginPlay. */
class InitSpacerig : public MemoryRead {};
class InitCave : public MemoryRead {};
