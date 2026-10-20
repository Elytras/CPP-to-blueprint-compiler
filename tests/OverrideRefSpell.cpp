/* OverrideRefSpell: an override's reference parameter spelled another way is the same type. OrsBase declares
   Check(EOrsMode&) short, inside the enum's namespace; OrsKid, in another, overrides it with
   `::OrsA::EOrsMode&` and a const struct reference spelled `const ::FVector&`. clang desugars neither (a reference
   is no sugar), so the two spellings reach SignatureOf as written: one signature, and the override links to its
   parent's function. */
#include "UeApi/Types.h"
#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/OverrideRefSpell");

namespace OrsA {
enum class EOrsMode : uint8 { Cold, Hot };
UE_ENUM(EOrsMode);

class OrsBase : public AActor {
public:
  int32 Seen = 0;
  void Check(EOrsMode &M, const FVector &V) { Seen = 1; }
};
} // namespace OrsA

namespace OrsB {
class OrsKid : public ::OrsA::OrsBase {
public:
  void Check(::OrsA::EOrsMode &M, const ::FVector &V) { Seen = 2; M = ::OrsA::EOrsMode::Hot; }
};
} // namespace OrsB

class OverrideRefSpell : public AActor {
public:
  int32 Got = 0;
  void BeginPlay() {}
};
