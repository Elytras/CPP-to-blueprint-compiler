// Locals of one name in different scopes are different variables, whatever their types: sibling blocks of two types,
// an inner block's X shadowing the outer X, names that differ only in case (an FName ignores case), and the same within
// an inline function's expansion (test_bytecode.py sibling_locals).
#include "UeApi/Types.h"
#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/SiblingLocals");

inline int32 TwoBlocks(int32 Which) {
    int32 R = 0;
    if (Which == 0) {
        TArray<FString> Keys = {"a", "bb"};
        R = Keys.Num() + Keys[1].Len() * 10;
    } else {
        TArray<float> Keys = {1.5f, 2.5f, 3.5f};
        R = Keys.Num();
        if (Keys[2] > 3.0f)
            R = R + 100;
    }
    return R;
}

class SiblingLocals : public AActor {
public:
    // A TArray<FString> and a TArray<float>, both Keys, in sibling blocks: 22 for 0, 103 otherwise.
    int32 Sibling(int32 Which) {
        int32 R = 0;
        if (Which == 0) {
            TArray<FString> Keys = {"a", "bb"};
            R = Keys.Num() + Keys[1].Len() * 10;
        } else {
            TArray<float> Keys = {1.5f, 2.5f, 3.5f};
            R = Keys.Num();
            if (Keys[2] > 3.0f)
                R = R + 100;
        }
        return R;
    }

    // The inner X is its own: the outer one still holds A.
    int32 Shadow(int32 A) {
        int32 X = A;
        {
            int32 X = A * 2;
            X = X + 1;
        }
        return X;
    }

    // A float v and an int32 V: A * 10, plus 1 when A is 2 or more. optnone keeps both as variables.
    [[clang::optnone]] int32 CaseOnly(int32 A) {
        int32 R = 0;
        {
            float v = A + 0.5f;
            if (v > 2.0f)
                R = R + 1;
        }
        {
            int32 V = A;
            R = R + V * 10;
        }
        return R;
    }

    int32 Inlined(int32 Which) { return TwoBlocks(Which); }
};
