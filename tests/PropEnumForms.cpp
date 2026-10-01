#include "UeApi/Types.h"

#include "UeApi/AIModule.h"
#include "UeApi/Engine.h"
#include "UeApi/FSD.h"
#include "UeApi/OnlineSubsystem.h"
#include "UeApi/OnlineSubsystemUtils.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/PropEnumForms");

/*
Native enums whose form no member or function parameter of the headers shows. The first seven are only ever a native
delegate's parameter: the object dump lists each delegate signature's parameters with their field class (an
EnumProperty for an `enum class`, a ByteProperty for a TEnumAsByte), but nothing in the headers names the signature, so
the join is on the delegate's parameter names and kinds. The last two are only a container's element, whose field
class the dump does not give: the game's own packages do, a local array of ECleanedStatus in
BP_PlagueHeartShield_Base's ubergraph (a ByteProperty inner) and GD_TreasureSettings' tags of UTreasureSettings'
TArray<ETreasureType> members (EnumProperty inners). The editor makes a variable of each the same way.
*/
class PropEnumForms : public AActor {
public:
  ETemperatureSeverityType Severity;          // EnumProperty: ApplicationLifecycleComponent's OnTemperatureChangeDelegate
  EQuartzCommandDelegateSubType QuartzEvent;  // EnumProperty: OnQuartzCommandEvent
  EInAppPurchaseStatus PurchaseStatus;        // EnumProperty: InAppPurchaseResult2, not InAppPurchaseResult
  EApplicationState AppState;                 // ByteProperty: PlatformGameInstance's notification delegates
  ENavPathEvent PathEvent;                    // ByteProperty: OnNavigationPathUpdated
  EEnvQueryStatus QueryStatus;                // ByteProperty: EQSQueryDoneSignature
  EInAppPurchaseState PurchaseState;          // ByteProperty: InAppPurchaseResult
  ETreasureType Treasure;                     // EnumProperty: GD_TreasureSettings' CrateTreasureTypes
  ECleanedStatus Cleaned;                     // ByteProperty: BP_PlagueHeartShield_Base's K2Node_MakeArray_Array_2
};
