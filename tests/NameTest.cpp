#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NameTest");

/* GetOuter() below is a memory read; a mod reading memory declares the compiler's scratch (Intrin.h). */
struct FDeref {
  UE_STRUCT;
  int64 Data;
  int32 Num;
  int32 Max;
};

/*
Dumper-7 respells what C++ cannot say: UFSDSaveGame's `Index` and `Name` are Index_0 and Name_0 in the SDK, because
its UObject already has members of those names. The engine knows them as Index and Name only, and a header says so
beside the member (`Index_0__UeName = "Index"`). Everything cooked - the default tag, the property a body reads and
writes - goes by the engine's name; the source keeps the C++ one.
*/
class NameTest : public UFSDSaveGame {
  UE_DEFAULTS {
    Index_0 = 7;
    Name_0 = "Karl";
  }

public:
  int32 Next() {
    Index_0 = Index_0 + 1;
    return Index_0;
  }

  /* UObject's C++ helpers, forwarded to the Kismet statics: this or any object. The second GetName() here is
     UFSDSaveGame's own UFunction, which hides UObject's forwarder as it would in C++: the real function wins. */
  FString Whose() { return GetOuter()->GetName() + "/" + GetName(); }
  // Whether Other is an object of this one's class.
  bool SameKind(UObject *Other) { return Other->GetClass() == GetClass(); }

  /* UE_CATEGORY files what follows it under a category of the editor's menus, in the API stub. */
  UE_CATEGORY("Names|Test");
  UE_PURE int32 Peek() { return Index_0; }
  int32 Charges;
  UE_CATEGORY("");
  int32 Plain;

  /* An access specifier means in the editor what it means here: a protected function is this class's and its
     subclasses', a private one this class's alone. A private field is left out of the editor API stub. */
protected:
  int32 Step() { return 1; }

  const int32 Limit = 3; // a const member is BlueprintReadOnly: the editor offers a Get and no Set

  /* UE_READONLY is BlueprintReadOnly too, but still written from code - with a warning, as the VM allows it. */
  UE_READONLY int32 Cap = 5;

public:
  /* UE_TOOLTIP and UE_KEYWORDS: the function's tooltip and search words in the editor, carried by the API stub alone.
     Without UE_TOOLTIP this comment would be the tooltip, as Whose's is. */
  UE_TOOLTIP("Lifts the cap to nine.") UE_KEYWORDS("raise lift cap") void Raise() { Cap = 9; }

private:
  int32 Seed; // a comment trailing a member describes nothing below it
  int32 Twice() { return Step() + Step(); }
};
