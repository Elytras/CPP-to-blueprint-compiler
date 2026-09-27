#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SoftTest");

/*
Kismet's soft reference conversions, spelled as C++ ones: an object or a class becomes a soft pointer where one is
wanted (Conv_ObjectToSoftObjectReference, Conv_ClassToSoftClassReference), a soft pointer becomes its path as a
string, and goes back to the object or class through an explicit cast, null unless it is loaded. Two soft pointers
compare by path.
*/
class SoftTest : public AActor {
public:
  TArray<TSoftClassPtr<AActor>> Kinds = {"/Game/A/BP_A.BP_A_C", "/Game/B/BP_B.BP_B_C"};
  TSoftClassPtr<AActor>         Kind  = "/Game/A/BP_A.BP_A_C";
  TSoftObjectPtr<AActor>        Seen;

  /* GetClass is a call, and Conv_ClassToSoftClassReference reads its argument where the argument left it: the class
     goes into a variable first, or the conversion would read what GetObjectClass's own argument left. */
  bool KnowsClassOf(AActor *A) { return Kinds.Contains(A->GetClass()); }

  void    Remember(AActor *A) { Seen = A; }
  AActor *Recall() { return (AActor *)Seen; }
  UClass *KindClass() { return (UClass *)Kind; }
  FString SeenPath() { return Seen; }

  /* A soft pointer made on the way is kept: the path of A's class, where the class itself would print its name. */
  FString ClassPathOf(AActor *A) { return FString(TSoftClassPtr<AActor>(A->GetClass())); }

  bool SameKind(TSoftClassPtr<AActor> Other) { return Kind == Other; }
  bool OtherKind(TSoftClassPtr<AActor> Other) { return Kind != Other; }

  /* Contains on an inline list of constants makes no array: the item goes into a variable once, then one == per
     element. Draw counts how often the item runs. */
  static inline const TArray<TSoftClassPtr<AActor>> kKinds  = {"/Game/A/BP_A.BP_A_C", "/Game/B/BP_B.BP_B_C"};
  static inline const TArray<int32>                 kPrimes = {2, 3, 5, 7};
  int32                                             Draws   = 0;

  bool  KnowsInline(AActor *A) { return kKinds.Contains(A->GetClass()); }
  int32 Draw() {
    Draws += 1;
    return Draws;
  }
  bool DrawIsPrime() { return kPrimes.Contains(Draw()); }
};
