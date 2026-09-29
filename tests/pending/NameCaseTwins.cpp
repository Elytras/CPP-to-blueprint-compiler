#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/NameCaseTwins");

/*
C++ tells Bump from bump; FName does not - it compares case-insensitively, so both would be the one export name Bump
under the class. The loader finds exports by outer and name (AsyncLoading.cpp 2901-2941) and FuncMap keeps one entry
per name (Class.cpp 4413), so the second function would load as the first object or never load. The compiler must
refuse the pair, saying the names differ only in case, or cook two functions FName tells apart, each with its own
body. Pending: two Function exports named Bump are written.
*/
class NameCaseTwins : public AActor {
public:
  int32 Bump(int32 V) { return V + 1; }
  int32 bump(int32 V) { return V + 2; }
};
