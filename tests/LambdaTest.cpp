#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/LambdaTest");

/*
Lambdas are expanded where they are called, as inline functions are: no closure object, no property for a variable
that holds one. A by-reference capture names the variable itself; a by-value capture of a variable written after the
lambda is declared, an init-capture and a mutable lambda's state are snapshot locals, assigned where the lambda is
declared; `this` is the object. A lambda handed to a template expands at each call of its parameter.
*/
class LambdaTest : public AActor {
public:
  int32 M = 7;
  int32 Hits;

  template <class F> int32 Apply(int32 V, F Fn) { return Fn(V) + 1; }
  template <class F> void  Thrice(F Fn) {
    Fn(1);
    Fn(2);
    Fn(3);
  }
  template <class F> int32 Twice(F &Fn, int32 V) { return Fn(Fn(V)); }
  template <class F> int32 Pass(F Fn, int32 V) { return Apply(V, Fn); }
  template <class F> int32 Keep(int32 P, F Fn) {
    Fn();
    return P;
  }

  /* [&]: the lambda writes the function's own variable */
  int32 ByRef(int32 V) {
    int32 X = V;
    auto  L = [&](int32 A) {
      X += A;
      return X;
    };
    int32 R = L(3);
    R += L(4);
    return R * 100 + X;
  }

  /* [=]: a copy as of the declaration, X written after */
  int32 ByValue(int32 V) {
    int32 X = V;
    auto  L = [=](int32 A) { return X + A; };
    X       = 100;
    return L(1) + X;
  }

  /* [=] of a variable nothing writes is the variable */
  int32 ByValueRead(int32 V) {
    int32 X = V * 2;
    auto  L = [=](int32 A) { return X + A; };
    return L(1) + L(2);
  }

  /* mutable: the copy is the lambda's own state, kept from call to call, and the variable is untouched */
  int32 Mutable(int32 V) {
    int32 C = V;
    auto  L = [C](int32 A) mutable {
      C += A;
      return C;
    };
    int32 R = L(1);
    R += L(2);
    return R * 100 + C;
  }

  /* init-captures */
  int32 InitCapture(int32 V) {
    int32 X = V;
    auto  L = [Z = X + 1](int32 A) { return Z * A; };
    X       = 0;
    return L(3) + L(4) + X;
  }
  int32 InitCaptures(int32 V) {
    int32 X = V;
    auto  L = [A = X, B = X * 2, &R = X](int32 K) {
      R += 1;
      return A * K + B;
    };
    int32 S = L(10);
    S += L(1);
    return S * 100 + X;
  }
  int32 InitUnused(int32 V) {
    auto L = [A = V + 1, B = V * 3](int32 K) { return B + K; };
    return L(1);
  }

  /* [this] and a member */
  int32 ThisMember(int32 V) {
    auto L = [this](int32 A) {
      M += A;
      return M;
    };
    int32 R = L(V);
    return R * 100 + L(1);
  }

  /* called where it is written */
  int32 Immediate(int32 V) {
    const int32 R = [&] { return V * 2; }();
    return R + [](int32 A, int32 B) { return A * B; }(V, 3);
  }
  int32 ImmediateInCondition(int32 V) {
    if ([&] { return V > 3; }()) return 1;
    return 0;
  }

  /* handed to a template */
  int32 ToTemplate(int32 V) {
    int32 X = V;
    int32 R = Apply(5, [&](int32 A) {
      X += A;
      return X * 2;
    });
    return R + X;
  }
  int32 ToTemplateVar(int32 V) {
    int32 X = V;
    auto  L = [&](int32 A) {
      X += A;
      return X;
    };
    int32 R = Apply(2, L);
    R += Pass(L, 3);
    return R * 100 + X;
  }
  int32 ToTemplateRef(int32 V) {
    auto L = [](int32 A) { return A * 3 + 1; };
    return Twice(L, V);
  }
  int32 EachOf(int32 V) {
    int32 S = 0;
    Thrice([&](int32 A) { S = S * 10 + A + V; });
    return S;
  }

  /* a parameter that stands for the caller's variable must not see what the lambda writes to it */
  int32 VarsInPlace(int32 V) {
    int32 A = V;
    return Keep(A, [&] { A = 5; });
  }
  int32 ArgWritten(int32 V) {
    int32 A = V;
    auto  W = [&](int32 P) {
      A = 9;
      return P;
    };
    return W(A) * 100 + A;
  }

  /* generic */
  int32 Generic(int32 V) {
    auto Add = [](auto A, auto B) { return A + B; };
    return Add(V, 2) * 10 + Add(1, 1);
  }
  float GenericFloat(float V) {
    auto Add = [](auto A, auto B) { return A + B; };
    return Add(V, 0.5f) + Add(1, 1);
  }

  /* lambdas in lambdas */
  int32 Nested(int32 V) {
    auto A = [](int32 X) { return X + 1; };
    auto B = [&](int32 X) { return A(X) * 2; };
    auto C = [=](int32 X) { return B(X) + A(X); };
    return C(V);
  }

  /* a void body, a loop and an early return in it */
  int32 Body(int32 V) {
    int32 Sum     = 0;
    auto  AddUpTo = [&](int32 Limit) {
      for (int32 I = 0; I < 10; I++) {
        if (I >= Limit) return;
        Sum += I;
      }
    };
    AddUpTo(V);
    AddUpTo(2);
    return Sum;
  }
  int32 Returns(int32 V) {
    auto Sign = [](int32 A) {
      if (A < 0) return -1;
      if (A > 0) return 1;
      return 0;
    };
    return Sign(V) * 10 + Sign(V - 5);
  }

  /* a parameter, written or not */
  int32 WithParam(int32 V) {
    auto L = [V](int32 A) { return V * A; };
    return L(3);
  }
  int32 ParamWritten(int32 V) {
    auto L = [V](int32 A) { return V * A; };
    V += 100;
    return L(3) + V;
  }

  /* two lambdas copying the same variable, as it was at each declaration */
  int32 TwoShadows(int32 V) {
    int32 X = V;
    auto  A = [=](int32 K) { return X + K; };
    X       = V * 2;
    auto B  = [=](int32 K) { return X + K; };
    X       = 0;
    return A(1) * 1000 + B(1);
  }

  /* declared in a loop: the copy is as of that trip */
  int32 InLoop(int32 V) {
    int32 S = 0;
    for (int32 I = 0; I < 3; I++) {
      int32 K = I * 10;
      auto  L = [=](int32 A) { return K + A; };
      K       = 0;
      S += L(V);
    }
    return S;
  }

  /* a constant is no capture */
  int32 ConstCapture(int32 V) {
    const int32 K = 5;
    auto        L = [=](int32 A) { return A * K; };
    return L(V);
  }

  /* a default argument, a struct, an array, a reference variable, a reference parameter */
  int32 Defaults(int32 V) {
    auto L = [](int32 A, int32 B = 10) { return A * B; };
    return L(V) + L(V, 2);
  }
  float StructCapture(FVector P) {
    FVector Q = P;
    auto    L = [=](float K) { return Q.X * K + Q.Y; };
    Q.X       = 100.0f;
    return L(2.0f);
  }
  int32 ArrayRef(int32 V) {
    TArray<int32> A;
    auto          Add = [&](int32 X) { A.Add(X); };
    Add(V);
    Add(V + 1);
    return A.Num() * 100 + A[1];
  }
  int32 ReferenceVar(int32 V) {
    int32  X = V;
    int32 &R = X;
    auto   L = [=](int32 A) { return R + A; };
    R        = 50;
    return L(1) * 100 + X;
  }
  int32 RefParam(int32 V, int32 &Out) {
    auto L = [&](int32 A) { Out += A; };
    L(V);
    L(1);
    return Out;
  }
  int32 CommaArg(int32 V) {
    int32 X = V;
    auto  L = [](int32 A, int32 B) { return A * 10 + B; };
    return L((X += 1, X), 3);
  }
  int32 InCondition(int32 V) {
    int32 I    = 0;
    auto  Less = [&](int32 Limit) { return I < Limit; };
    while (Less(V)) I += 2;
    return I;
  }
  int32 RefToLambda(int32 V) {
    auto  L = [](int32 A) { return A + 1; };
    auto &R = L;
    return R(V);
  }

  /* reference parameters of a lambda, generic too */
  int32 RefArg(int32 V) {
    int32 X   = V;
    auto  Inc = [](int32 &R) { R += 2; };
    Inc(X);
    auto Bump = [](auto &R) { R += 1; };
    Bump(X);
    return X;
  }

  /* an inline method's lambda, written in the class that has it and run from a subclass's function */
  inline int32 FromInline(int32 V) {
    auto L = [this](int32 A) { return A + M; };
    return L(V);
  }

  /* a wait in the body: the function resumes after it, with the snapshot it took */
  int32         Stage;
  TArray<int32> Log;
  void          Waits(int32 Tag) {
    int32 X = Tag;
    auto  L = [=, this](int32 A) {
      UKismetSystemLibrary::Delay(0.5f);
      Log.Add(X + A);
    };
    L(1);
    X = 0;
    L(2);
    Stage = 5;
  }
};

class LambdaKid : public LambdaTest {
public:
  int32 Use(int32 V) { return FromInline(V) * 100 + Apply(V, [this](int32 A) { return A + M; }); }
};
