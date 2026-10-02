#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/FuncDeclaredSig");

/*
FdsRoot declares Scale(int32) and never defines it, so it compiles no Scale and no caller lays out its parameters:
FuncDeclaredSig's Scale(float) replaces nothing, and may take parameters of its own (test_bytecode.py's
func_declared_sig).
*/
class FdsRoot : public AActor {
public:
  int32 Scale(int32 V);
};

class FuncDeclaredSig : public FdsRoot {
public:
  float Scale(float V) { return V * 3.0f; }
  float Use(int32 V) { return Scale(V * 1.0f); }
};
