#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ClassCtx");

/*
GetDefault<T>(Class): a class's default object, the editor's Get Class Defaults. A member read and a call on it run in
EX_ClassContext(Class), which steps them with the default object as the context (execClassContext, ScriptCore.cpp
2209-2256); the object itself is EX_Self run there. A None class is "Accessed None reading a Class": the read is zero.
*/
class ClassCtx : public AActor {
public:
  TSubclassOf<ClassCtx> Kind;
  int32 Score = 5;
  int32 Calls;

  int32 ScoreOf(TSubclassOf<ClassCtx> C) { return GetDefault<ClassCtx>(C)->Score; }
  int32 OwnScore() { return GetDefault<ClassCtx>(ClassCtx::StaticClass())->Score; }
  /* A bare class, said to hold one. */
  int32 RawScore(UClass *C) { return GetDefault<ClassCtx>(TSubclassOf<ClassCtx>(C))->Score; }
  /* Into a member, through the class member. */
  int32 Seen = 3;
  void Remember() { Seen = GetDefault<ClassCtx>(Kind)->Score; }

  [[gnu::noinline]] int32 Bump() {
    Calls += 1;
    return Calls;
  }
  int32 BumpDefaults(TSubclassOf<ClassCtx> C) { return GetMutableDefault<ClassCtx>(C)->Bump(); }

  /* The default object itself, of any class. */
  UObject *DefaultsOf(UClass *C) { return GetMutableDefault<UObject>(C); }
  ClassCtx *Held;
  void Hold() { Held = GetMutableDefault<ClassCtx>(Kind); }
};

/* A class whose default object is not the one a call runs on: what reads it reads another object. */
class ClassCtxKid : public ClassCtx {
public:
  UE_DEFAULTS { Score = 9; }
};
