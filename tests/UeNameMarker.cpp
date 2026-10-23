#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/UeNameMarker");

/* Functions an editor named with what C++ cannot spell (DRGLib's interface has `Compare Function`): a `__UeName` marker
   in the class gives the name they cook under, which callers and an interface's implementations use too. */

class IOrdering {
public:
  UE_INTERFACE;
  void Compare_Function(UObject *A, UObject *B, bool &A_B);

private:
  struct UeMarkers {
    static constexpr const char *Compare_Function__UeName = "Compare Function";
  };
};

class UeNameMarker : public AActor, public IOrdering {
public:
  void Compare_Function(UObject *A, UObject *B, bool &A_B) { A_B = A == nullptr; }
  bool AskThrough(TScriptInterface<IOrdering> I) {
    bool R = false;
    I->Compare_Function(nullptr, this, R);
    return R;
  }
  int32 Twice_Over(int32 X) { return 2 * X; }
  int32 Call() { return Twice_Over(3); }

private:
  struct UeMarkers {
    static constexpr const char *Twice_Over__UeName = "Twice Over";
  };
};
