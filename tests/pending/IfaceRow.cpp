// A mod interface's AssetRegistry.bin row names its class, `<Package>.IRowMarked_C  BlueprintGeneratedClass`, as a
// class's row does and as the game's Blueprint interfaces' rows do (TempRocketInterface, RadarPointInterface): the
// package holds no object named IRowMarked (test_bytecode.py registry_rows_name_exports).
#include "UeApi/Types.h"
#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/IfaceRow");

class IRowMarked
{
public:
    UE_INTERFACE;
    void Mark();
};

class IfaceRow : public AActor, public IRowMarked
{
public:
    int32 Marks = 0;
    void Mark() { Marks = Marks + 1; }
};
