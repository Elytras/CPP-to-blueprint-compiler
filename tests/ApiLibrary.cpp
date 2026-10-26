#include "Types.h"
#include "UeMeta.h"
#include "Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ApiLibrary");

class IApiProbe {
public:
    UE_INTERFACE;
    int32 Ping();
};

class ApiActor : public AActor, public IApiProbe {
public:
    bool Ready = true;
    TArray<FName> Dependencies;
    int32 Ping() { return 7; }
};

class ApiHidden : public UObject {};

class ApiLibrary : public UBlueprintFunctionLibrary {
public:
    static TScriptInterface<IApiProbe> GetComponent(UObject* WorldContextObject) { return nullptr; }
protected:
    static ApiHidden* Internal() { return nullptr; }
private:
    static UObject* PrivateHelper() { return nullptr; }
};
