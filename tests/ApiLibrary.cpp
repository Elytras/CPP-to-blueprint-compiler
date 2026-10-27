#include "Types.h"
#include "UeMeta.h"
#include "Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ApiLibrary");

class IApiProbe {
public:
    UE_INTERFACE;
    int32 Ping();
    int32 Level() const;
    void Later(TDelegate<void()> Call);
};

/* Level is pure through the interface alone. WhenDone's parameter and Pending are of delegate types of the class's
   own, so their stub pins name signature functions the stub makes. */
class ApiActor : public AActor, public IApiProbe {
public:
    bool Ready = true;
    TArray<FName> Dependencies;
    TArray<TDelegate<void()>> Pending;
    int32 Ping() { return 7; }
    int32 Level() { return 3; }
    void Later(TDelegate<void()> Call) { Pending.Add(Call); }
    void WhenDone(TDelegate<void(int32 Code)> Call) { Call.ExecuteIfBound(1); }
};

class ApiHidden : public UObject {};

class ApiLibrary : public UBlueprintFunctionLibrary {
public:
    static TScriptInterface<IApiProbe> GetComponent(UObject* WorldContextObject) { return nullptr; }
    UE_OUT(Interface);
    static void GetInterface(UObject* WorldContextObject, TScriptInterface<IApiProbe>& Interface) {
        Interface = TScriptInterface<IApiProbe>(WorldContextObject);
    }
    static void KeepReference(TScriptInterface<IApiProbe>& Interface) { Interface = nullptr; }
    UE_OUT(Value);
    static void WriteNumber(int32& Value);
    static int32 CheckOutput() {
        int32 Value = 7;
        WriteNumber(Value);
        return Value;
    }
protected:
    static ApiHidden* Internal() { return nullptr; }
private:
    static UObject* PrivateHelper() { return nullptr; }
};

void ApiLibrary::WriteNumber(int32& Output) { Output = 42; }
