#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CtxNullRead");

/*
A member read through another object. When the object is None, ProcessContextOpcode skips the read and clears the
context's r-value property in the result slot (ScriptCore.cpp 2940-2953): the destination reads zero, Blueprint's
"Accessed None", where C++ would crash. The r-value names the member read, so each read below is 0 on None.
A call whose int result a statement throws away lands in the VM's 64-byte statement buffer, which is fine for an int.
*/
class CtxNullRead : public AActor {
public:
  CtxNullRead *Peer;
  int32 Score = 5;
  int32 Calls;
  int32 Seen;

  int32 FieldOf(CtxNullRead *P) {
    int32 V = 7;
    V = P->Score;
    return V;
  }
  /* A chain: either link None reads zero. */
  int32 ChainOf(CtxNullRead *P) {
    int32 V = 7;
    V = P->Peer->Score;
    return V;
  }
  /* Into a member: Seen is cleared, not left as it was. */
  void Remember() { Seen = Peer->Score; }

  /* An object a call through None would return, stored: EX_LetObj steps it into a local that starts NULL and then
     stores that (ScriptCore.cpp 2713-2715), so Kept reads None whether or not the context names an r-value. */
  CtxNullRead *Kept;
  [[gnu::noinline]] CtxNullRead *Me() { return this; }
  void KeepPeer() { Kept = Peer->Me(); }

  [[gnu::noinline]] int32 Bump() {
    Calls += 1;
    return Calls;
  }
  void Touch() {
    Bump();
    Bump();
  }
};
