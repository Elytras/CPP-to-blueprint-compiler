#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/StructCtorValues");

/*
`T()`, `T{}` and a declaration with no initializer of an engine struct hold what the engine's constructor makes:
FTransform's is the identity (TransformVectorized.h 108), so Scale3D is 1 and Rotation.W 1; FVector4's W is 1
(Vector4.h 59), though the engine makes a fresh FVector4 property or local as zeros (STRUCT_ZeroConstructor, Property.cpp
88-98). A function's FTransform() is the editor's Make Struct with nothing set: a temp the frame constructs
(ScriptCore.cpp 909-916); an FVector4 one is the literal (0, 0, 0, 1). A UE_STRUCT holding one, and a class's own
default, hold the same (test_bytecode.py's struct_ctor_values).
*/
struct FHoldsXf {
  UE_STRUCT;
  FTransform Xf;
  int32 N = 0;
};

struct FHoldsV4 {
  UE_STRUCT;
  FVector4 V;
  int32 N = 0;
};

class ScvParent : public AActor {
public:
  FVector4 Over = FVector4(1, 2, 3, 4);
};

class StructCtorValues : public ScvParent {
public:
  FVector4 Bare;
  FVector4 Ctor = FVector4();
  FHoldsV4 Held = {.N = 2};

  UE_DEFAULTS { Over = FVector4(); }

  float Echo(const FTransform& T) { return T.Scale3D.Z; }

  float XfLocal(int32 M) { FTransform T = FTransform(); return T.Scale3D.X + T.Rotation.W + M; }
  float XfTemp(int32 M) { return FTransform().Scale3D.Y + M; }
  float XfArg(int32 M) { return Echo(FTransform()) + M; }
  float XfAssign(int32 M) {
    FTransform T;
    T.Scale3D.X = 7.0f;
    T = FTransform();
    return T.Scale3D.X + M;
  }
  float XfLoop(int32 M) {
    float S = 0.0f;
    for (int32 I = 0; I < 3; ++I) {
      FTransform T = FTransform();
      T.Scale3D.X += 1.0f;
      S += T.Scale3D.X;
    }
    return S + M;
  }
  float XfBareLoop(int32 M) {
    float S = 0.0f;
    for (int32 I = 0; I < 3; ++I) {
      FTransform T;
      T.Scale3D.X += 1.0f;
      S += T.Scale3D.X;
    }
    return S + M;
  }
  float XfHeld(int32 M) { FHoldsXf H = {FTransform(), 3}; return H.Xf.Scale3D.X + H.N + M; }
  float V4(int32 M) { return FVector4().W + M; }
  float V4Decl(int32 M) { FVector4 V; return V.W + M; }
  float V4Braces(int32 M) { FVector4 V{}; return V.W + M; }
  float V4Loop(int32 M) {
    float S = 0.0f;
    for (int32 I = 0; I < 3; ++I) {
      FVector4 V;
      V.W += 1.0f;
      S += V.W;
    }
    return S + M;
  }
  float V4Held(int32 M) { FHoldsV4 H = FHoldsV4(); return H.V.W + H.N + M; }
};
