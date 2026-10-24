/* IfaceStaticClass: `IIscMark::StaticClass()` on a mod's own interface names the interface's class, as a native
   interface's does: Which returns it as a constant and Has passes it to DoesImplementInterface. UE_INTERFACE's
   StaticClass is no function of the interface, so its asset has none. */
#include "UeApi/Types.h"
#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/IfaceStaticClass");

class IIscMark {
public:
  UE_INTERFACE;
  void Mark();
};

class IfaceStaticClass : public AActor, public IIscMark {
public:
  void Mark() {}
  UClass *Which() { return IIscMark::StaticClass(); }
  bool Has(AActor *A) { return UKismetSystemLibrary::DoesImplementInterface(A, IIscMark::StaticClass()); }
};
