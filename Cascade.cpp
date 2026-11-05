#include "Cascade.h"

#include "Package.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>

using namespace Uasset;

namespace
{
/* ERawDistributionOperation (Distributions.h 18-24). */
enum : uint8 { RDO_Uninitialized = 0, RDO_None = 1, RDO_Random = 2, RDO_Extreme = 3 };
/* EDistributionVectorLockFlags (DistributionVector.h 16-24). */
enum : uint8 { EDVLF_None = 0, EDVLF_XY = 1, EDVLF_XZ = 2, EDVLF_YZ = 3, EDVLF_XYZ = 4 };

struct FVec { float X = 0, Y = 0, Z = 0; bool operator==(const FVec& O) const { return X == O.X && Y == O.Y && Z == O.Z; } };

// ---- reading the lowered tags

std::vector<FPropertyDef>* MembersOf(FPropertyDef& P)
{
    if (P.Members) return P.Members.get();
    return P.Default.Members.get();
}

FPropertyDef* Find(std::vector<FPropertyDef>& Tags, const std::string& Name)
{
    for (FPropertyDef& T : Tags) if (Lower(T.Name) == Lower(Name)) return &T;
    return nullptr;
}

float FloatOf(const FPropertyDef* T)
{
    if (!T) return 0.0f;
    return float(T->Default.K == FDefaultValue::Float ? T->Default.F : double(T->Default.I));
}

FVec VecOf(FPropertyDef* T)
{
    FVec V;
    if (std::vector<FPropertyDef>* M = T ? MembersOf(*T) : nullptr)
    {
        V.X = FloatOf(Find(*M, "X"));
        V.Y = FloatOf(Find(*M, "Y"));
        V.Z = FloatOf(Find(*M, "Z"));
    }
    return V;
}

/* An enum member, written as its enumerator's name (EDVLF_XY, or EDistributionVectorLockFlags::EDVLF_XY) or a number. */
uint8 LockOf(const FPropertyDef* T)
{
    if (!T || T->Default.K == FDefaultValue::None) return EDVLF_None;
    if (T->Default.K != FDefaultValue::Str) return uint8(T->Default.I);
    const std::string S = T->Default.S.substr(T->Default.S.rfind(':') == std::string::npos ? 0 : T->Default.S.rfind(':') + 1);
    static const char* const Names[] = { "EDVLF_None", "EDVLF_XY", "EDVLF_XZ", "EDVLF_YZ", "EDVLF_XYZ" };
    for (uint8 I = 0; I < 5; ++I) if (S == Names[I]) return I;
    return EDVLF_None;
}

bool BoolOf(const FPropertyDef* T) { return T && T->Default.K != FDefaultValue::None && T->Default.I != 0; }

// ---- writing tags as the cook writes them

FPropertyDef FloatTag(const std::string& Name, float F)
{
    FPropertyDef P;
    P.Type = "FloatProperty";
    P.Name = Name;
    P.ElementSize = 4;
    P.Default.K = FDefaultValue::Float;
    P.Default.F = F;
    return P;
}

FPropertyDef ByteTag(const std::string& Name, uint8 B)
{
    FPropertyDef P;
    P.Type = "ByteProperty";
    P.Name = Name;
    P.ElementSize = 1;
    P.Default.K = FDefaultValue::Int;
    P.Default.I = B;
    return P;
}

FPropertyDef VecTag(const std::string& Name, const FVec& V)
{
    FPropertyDef P;
    P.Type = "StructProperty";
    P.Name = Name;
    P.StructName = "Vector";
    P.ElementSize = 12;
    P.Default.K = FDefaultValue::Struct;
    P.Members = std::make_shared<std::vector<FPropertyDef>>(std::vector<FPropertyDef>{ FloatTag("X", V.X), FloatTag("Y", V.Y), FloatTag("Z", V.Z) });
    return P;
}

FPropertyDef StructTag(const std::string& Name, const std::string& Struct, std::vector<FPropertyDef> Members)
{
    FPropertyDef P;
    P.Type = "StructProperty";
    P.Name = Name;
    P.StructName = Struct;
    P.Default.K = FDefaultValue::Struct;
    P.Members = std::make_shared<std::vector<FPropertyDef>>(std::move(Members));
    return P;
}

FPropertyDef NullObjectTag(const std::string& Name)
{
    FPropertyDef P;
    P.Type = "ObjectProperty";
    P.Name = Name;
    P.ElementSize = 8;
    P.Default.K = FDefaultValue::Obj;
    return P;
}

// ---- curves: FInterpCurve (Core/Public/Math/InterpCurve.h, InterpCurvePoint.h, Private/Math/UnrealMath.cpp), with
// up to six components a key (a FTwoVectors), each worked in float as the engine works it

/* EInterpCurveMode (InterpCurvePoint.h 18-30). */
enum : uint8 { CIM_Linear = 0, CIM_CurveAuto = 1, CIM_Constant = 2, CIM_CurveUser = 3, CIM_CurveBreak = 4, CIM_CurveAutoClamped = 5 };

using FVals = std::array<float, 6>;
struct FKey { float In = 0; FVals Out{}, Arrive{}, Leave{}; uint8 Mode = CIM_Linear; };
struct FCurve
{
    int32 Width = 1;
    std::vector<FKey> Keys;
    bool bLooped = false;
    float LoopOffset = 0;
};

bool IsCurveKey(const FKey& K) { return K.Mode == CIM_CurveAuto || K.Mode == CIM_CurveAutoClamped || K.Mode == CIM_CurveUser || K.Mode == CIM_CurveBreak; }

uint8 ModeOf(const FPropertyDef* T)
{
    if (!T || T->Default.K == FDefaultValue::None) return CIM_Linear;
    if (T->Default.K != FDefaultValue::Str) return uint8(T->Default.I);
    const std::string S = T->Default.S.substr(T->Default.S.rfind(':') == std::string::npos ? 0 : T->Default.S.rfind(':') + 1);
    static const char* const Names[] = { "CIM_Linear", "CIM_CurveAuto", "CIM_Constant", "CIM_CurveUser", "CIM_CurveBreak", "CIM_CurveAutoClamped" };
    for (uint8 I = 0; I < 6; ++I) if (S == Names[I]) return I;
    return CIM_Linear;
}

/* A key's value member as Width floats: a float, a Vector2D, a Vector, or a TwoVectors' v1 then v2. */
FVals ValsOf(FPropertyDef* T, int32 Width)
{
    FVals V{};
    if (Width == 1) { V[0] = FloatOf(T); return V; }
    if (Width == 2)
    {
        if (std::vector<FPropertyDef>* M = T ? MembersOf(*T) : nullptr) { V[0] = FloatOf(Find(*M, "X")); V[1] = FloatOf(Find(*M, "Y")); }
        return V;
    }
    if (Width == 3) { const FVec A = VecOf(T); return { A.X, A.Y, A.Z }; }
    if (std::vector<FPropertyDef>* M = T ? MembersOf(*T) : nullptr)
    {
        const FVec A = VecOf(Find(*M, "v1")), B = VecOf(Find(*M, "v2"));
        V = { A.X, A.Y, A.Z, B.X, B.Y, B.Z };
    }
    return V;
}

/* The ConstantCurve member of a curve distribution. False with Why when its keys are not in InVal order: the
   engine's binary search (GetPointIndexForInputValue) assumes they are, and the editor keeps them so. */
bool CurveOf(std::vector<FPropertyDef>& Tags, int32 Width, FCurve& C, std::string& Why)
{
    C.Width = Width;
    FPropertyDef* Curve = Find(Tags, "ConstantCurve");
    std::vector<FPropertyDef>* M = Curve ? MembersOf(*Curve) : nullptr;
    if (!M) return true;
    C.bLooped = BoolOf(Find(*M, "bIsLooped"));
    C.LoopOffset = FloatOf(Find(*M, "LoopKeyOffset"));
    if (FPropertyDef* Points = Find(*M, "Points"))
        for (FDefaultValue& Item : Points->Default.Items)
        {
            if (!Item.Members) continue;
            FKey K;
            K.In = FloatOf(Find(*Item.Members, "InVal"));
            K.Out = ValsOf(Find(*Item.Members, "OutVal"), Width);
            K.Arrive = ValsOf(Find(*Item.Members, "ArriveTangent"), Width);
            K.Leave = ValsOf(Find(*Item.Members, "LeaveTangent"), Width);
            K.Mode = ModeOf(Find(*Item.Members, "InterpMode"));
            if (!C.Keys.empty() && K.In < C.Keys.back().In)
            {
                Why = "its ConstantCurve's point " + std::to_string(C.Keys.size()) + " has a smaller InVal than point "
                    + std::to_string(C.Keys.size() - 1) + "; give the points in InVal order";
                return false;
            }
            C.Keys.push_back(K);
        }
    return true;
}

/* ClampFloatTangent (UnrealMath.cpp 2552-2620). */
float ClampTangent(float PrevVal, float PrevTime, float CurVal, float CurTime, float NextVal, float NextTime)
{
    const float PrevToNextTime = std::max(1.e-4f, NextTime - PrevTime);
    const float PrevToCurTime = std::max(1.e-4f, CurTime - PrevTime);
    const float CurToNextTime = std::max(1.e-4f, NextTime - CurTime);
    const float PrevToNext = NextVal - PrevVal, PrevToCur = CurVal - PrevVal, CurToNext = NextVal - CurVal;
    if ((PrevToCur >= 0.0f && CurToNext <= 0.0f) || (PrevToCur <= 0.0f && CurToNext >= 0.0f)) return 0.0f;
    const float Threshold = 0.333f;
    const float CurToNextTan = CurToNext / CurToNextTime, PrevToCurTan = PrevToCur / PrevToCurTime, PrevToNextTan = PrevToNext / PrevToNextTime;
    float Clamped = PrevToNextTan;
    const float Alpha = PrevToCur / PrevToNext;
    auto Lerp = [](float A, float B, float T) { return A + T * (B - A); };
    if (Alpha < Threshold)
    {
        const float Lower = Lerp(PrevToNextTan, PrevToCurTan, 1.0f - Alpha / Threshold);
        Clamped = PrevToNext > 0.0f ? std::min(Clamped, Lower) : std::max(Clamped, Lower);
    }
    if (Alpha > 1.0f - Threshold)
    {
        const float Upper = Lerp(PrevToNextTan, CurToNextTan, (Alpha - (1.0f - Threshold)) / Threshold);
        Clamped = PrevToNext > 0.0f ? std::min(Clamped, Upper) : std::max(Clamped, Upper);
    }
    return Clamped;
}

/* FInterpCurve::AutoSetTangents (InterpCurve.h 618-683), tension 0, stationary end points: what the editor stores
   after every edit of a curve, so a mod's auto keys get the tangents the editor would have given them. */
void AutoSetTangents(FCurve& C)
{
    const int32 N = int32(C.Keys.size()), Last = N - 1;
    for (int32 I = 0; I < N; ++I)
    {
        FKey& This = C.Keys[size_t(I)];
        const FKey& Prev = C.Keys[size_t(I == 0 ? (C.bLooped ? Last : 0) : I - 1)];
        const FKey& Next = C.Keys[size_t(I == Last ? (C.bLooped ? 0 : Last) : I + 1)];
        if (This.Mode == CIM_CurveAuto || This.Mode == CIM_CurveAutoClamped)
        {
            if (I == 0 || (I == Last && !C.bLooped))
                This.Arrive = This.Leave = FVals{};
            else if (IsCurveKey(Prev))
            {
                const float PrevTime = (C.bLooped && I == 0) ? This.In - C.LoopOffset : Prev.In;
                const float NextTime = (C.bLooped && I == Last) ? This.In + C.LoopOffset : Next.In;
                FVals T{};
                for (int32 K = 0; K < C.Width; ++K)
                    T[size_t(K)] = This.Mode == CIM_CurveAutoClamped
                        ? ClampTangent(Prev.Out[size_t(K)], PrevTime, This.Out[size_t(K)], This.In, Next.Out[size_t(K)], NextTime)
                        : ((This.Out[size_t(K)] - Prev.Out[size_t(K)]) + (Next.Out[size_t(K)] - This.Out[size_t(K)]))
                              / std::max(1.e-4f, NextTime - PrevTime);
                This.Arrive = This.Leave = T;
            }
            else
            {
                This.Arrive = Prev.Arrive;
                This.Leave = Prev.Leave;
            }
        }
        else if (This.Mode == CIM_Linear)
        {
            for (int32 K = 0; K < C.Width; ++K) This.Arrive[size_t(K)] = Next.Out[size_t(K)] - This.Out[size_t(K)];
            This.Leave = This.Arrive;
        }
        else if (This.Mode == CIM_Constant)
            This.Arrive = This.Leave = FVals{};
    }
}

float Cubic(float P0, float T0, float P1, float T1, float A)
{
    const float A2 = A * A, A3 = A2 * A;
    return (((2 * A3) - (3 * A2) + 1) * P0) + ((A3 - (2 * A2) + A) * T0) + ((A3 - A2) * T1) + (((-2 * A3) + (3 * A2)) * P1);
}

/* FInterpCurve::Eval (InterpCurve.h 289-352); zero for a curve without keys. */
FVals Eval(const FCurve& C, float In)
{
    const int32 N = int32(C.Keys.size()), Last = N - 1;
    if (N == 0) return FVals{};
    if (In < C.Keys[0].In) return C.Keys[0].Out;
    int32 Index = Last;
    if (In < C.Keys[size_t(Last)].In)
    {
        int32 Min = 0, Max = N;
        while (Max - Min > 1)
        {
            const int32 Mid = (Min + Max) / 2;
            if (C.Keys[size_t(Mid)].In <= In) Min = Mid; else Max = Mid;
        }
        Index = Min;
    }
    if (Index == Last)
    {
        if (!C.bLooped) return C.Keys[size_t(Last)].Out;
        if (In >= C.Keys[size_t(Last)].In + C.LoopOffset) return C.Keys[0].Out;
    }
    const bool bLoopSegment = C.bLooped && Index == Last;
    const FKey& Prev = C.Keys[size_t(Index)];
    const FKey& Next = C.Keys[bLoopSegment ? 0 : size_t(Index + 1)];
    const float Diff = bLoopSegment ? C.LoopOffset : Next.In - Prev.In;
    if (!(Diff > 0.0f && Prev.Mode != CIM_Constant)) return Prev.Out;
    const float Alpha = (In - Prev.In) / Diff;
    FVals V{};
    for (int32 K = 0; K < C.Width; ++K)
    {
        const size_t S = size_t(K);
        V[S] = Prev.Mode == CIM_Linear ? Prev.Out[S] + Alpha * (Next.Out[S] - Prev.Out[S])
                                       : Cubic(Prev.Out[S], Prev.Leave[S] * Diff, Next.Out[S], Next.Arrive[S] * Diff, Alpha);
    }
    return V;
}

/* FInterpCurve::CalcBounds (InterpCurve.h 686-712) through FindBounds (UnrealMath.cpp 1331-1375), per component. */
void CalcBounds(const FCurve& C, FVals& Lo, FVals& Hi)
{
    const int32 N = int32(C.Keys.size());
    Lo = Hi = N ? C.Keys[0].Out : FVals{};
    if (N < 2) return;
    for (int32 I = 0; I < (C.bLooped ? N : N - 1); ++I)
    {
        const FKey& A = C.Keys[size_t(I)];
        const FKey& B = C.Keys[size_t(I == N - 1 ? 0 : I + 1)];
        for (int32 K = 0; K < C.Width; ++K)
        {
            const size_t S = size_t(K);
            const float Start = A.Out[S], End = B.Out[S];
            float Min = std::min(Start, End), Max = std::max(Start, End);
            if (IsCurveKey(A))
            {
                const float Diff = B.In - A.In;
                const float StartTan = A.Leave[S] * Diff, EndTan = B.Arrive[S] * Diff;
                const float Qa = 6.f * Start + 3.f * StartTan + 3.f * EndTan - 6.f * End;
                const float Qb = -6.f * Start - 4.f * StartTan - 2.f * EndTan + 6.f * End;
                const float Disc = (Qb * Qb) - (4.f * Qa * StartTan);
                if (Disc > 0.f && std::fabs(Qa) > 1.e-8f)
                {
                    const float Root = std::sqrt(Disc);
                    for (const float X : { (-Qb + Root) / (2.f * Qa), (-Qb - Root) / (2.f * Qa) })
                    {
                        const float T = A.In + X * (B.In - A.In);
                        if (T > A.In && T < B.In)
                        {
                            const float Y = Cubic(Start, StartTan, End, EndTan, X);
                            Min = std::min(Min, Y);
                            Max = std::max(Max, Y);
                        }
                    }
                }
            }
            Lo[S] = std::min(Lo[S], Min);
            Hi[S] = std::max(Hi[S], Max);
        }
    }
}

void InRange(const FCurve& C, float& MinIn, float& MaxIn)
{
    MinIn = MaxIn = 0.0f;
    if (C.Keys.empty()) return;
    MinIn = 3.4e+38f;      // BIG_NUMBER
    MaxIn = -3.4e+38f;
    for (const FKey& K : C.Keys) { MinIn = std::min(MinIn, K.In); MaxIn = std::max(MaxIn, K.In); }
}

// ---- the distributions: what FRawDistribution::Initialize reads off each class (Distributions.cpp)

/* What one distribution object gives the bake: its operation, values per entry and lock flag, its input range, its
   InitializeRawEntry, its GetOutRange and a vector's GetRange. */
struct FBaked
{
    bool bVector = false;
    uint8 Op = RDO_None;
    uint8 Lock = 0;
    int32 PerEntry = 1;
    float MinIn = 0, MaxIn = 0;
    std::function<FVals(float)> Entry;
    float MinOut = 0, MaxOut = 0;
    FVec MinVec, MaxVec;
};

/* EDistributionVectorLockFlags applied to a vector (UDistributionVectorConstant::GetValue, 2493-2509). */
FVec Locked(const FVec& V, uint8 Lock)
{
    switch (Lock)
    {
    case EDVLF_XY: return { V.X, V.X, V.Z };
    case EDVLF_XZ: return { V.X, V.Y, V.X };
    case EDVLF_YZ: return { V.X, V.Y, V.Y };
    case EDVLF_XYZ: return { V.X, V.X, V.X };
    default: return V;
    }
}

float MinOf(const FVec& V) { return std::min(V.X, std::min(V.Y, V.Z)); }
float MaxOf(const FVec& V) { return std::max(V.X, std::max(V.Y, V.Z)); }
FVec VecAt(const FVals& V, size_t At) { return { V[At], V[At + 1], V[At + 2] }; }

/* The classes AssetGen bakes, from their members (unset ones their class defaults: zero, no keys, MirrorFlags
   Different and LockedAxes None, which UeApi cannot set). False for a class not baked yet; false with Why for one
   the engine would misread. */
bool Bake(const std::string& Class, std::vector<FPropertyDef>& Tags, FBaked& Out, std::string& Why)
{
    if (Class == "DistributionFloatConstant")                   // 1731-1735, 1771-1781; GetOperation RDO_None
    {
        const float C = FloatOf(Find(Tags, "Constant"));
        Out.Entry = [C](float) { return FVals{ C }; };
        Out.MinOut = Out.MaxOut = C;
        return true;
    }
    if (Class == "DistributionFloatUniform")                    // 2035-2050, 2123-2133
    {
        const float Min = FloatOf(Find(Tags, "Min")), Max = FloatOf(Find(Tags, "Max"));
        Out.Op = Min == Max ? RDO_None : RDO_Random;
        Out.Entry = [Min, Max](float) { return FVals{ Min, Max }; };
        Out.MinOut = Min;
        Out.MaxOut = Max;
        return true;
    }
    if (Class == "DistributionVectorConstant")                  // 2493-2509, 2631-2657, 2721-2725
    {
        const FVec C = VecOf(Find(Tags, "Constant"));
        const FVec L = Locked(C, LockOf(Find(Tags, "LockedAxes")));
        Out.bVector = true;
        Out.PerEntry = 3;
        Out.Entry = [L](float) { return FVals{ L.X, L.Y, L.Z }; };
        Out.MinOut = MinOf(L);
        Out.MaxOut = MaxOf(L);
        Out.MinVec = Out.MaxVec = C;
        return true;
    }
    if (Class == "DistributionVectorUniform")                   // 3260-3360, 3559-3600, 3673-3677
    {
        const FVec Min = VecOf(Find(Tags, "Min")), Max = VecOf(Find(Tags, "Max"));
        const uint8 Lock = LockOf(Find(Tags, "LockedAxes"));
        const FVec LMin = Locked(Min, Lock), LMax = Locked(Max, Lock);
        Out.bVector = true;
        Out.PerEntry = 3;
        Out.Op = Min == Max ? RDO_None : BoolOf(Find(Tags, "bUseExtremes")) ? RDO_Extreme : RDO_Random;
        Out.Lock = Lock;
        Out.Entry = [LMin, LMax](float) { return FVals{ LMin.X, LMin.Y, LMin.Z, LMax.X, LMax.Y, LMax.Z }; };
        Out.MinOut = MinOf(LMin);
        Out.MaxOut = MaxOf(LMax);
        Out.MinVec = Min;
        Out.MaxVec = Max;
        return true;
    }

    /* The curves: the keys' span is the input range, the curve's value at a time the entry, its bounds the out range. */
    const int32 Width = Class == "DistributionFloatConstantCurve" ? 1 : Class == "DistributionFloatUniformCurve" ? 2
                      : Class == "DistributionVectorConstantCurve" ? 3 : Class == "DistributionVectorUniformCurve" ? 6 : 0;
    if (!Width) return false;
    auto Curve = std::make_shared<FCurve>();
    if (!CurveOf(Tags, Width, *Curve, Why)) return false;
    AutoSetTangents(*Curve);
    InRange(*Curve, Out.MinIn, Out.MaxIn);
    FVals Lo, Hi;
    CalcBounds(*Curve, Lo, Hi);
    const bool bOneKey = Curve->Keys.size() == 1;
    if (Width == 1)                                             // 1844-1848, 1884-1915
    {
        Out.Entry = [Curve](float T) { return Eval(*Curve, T); };
        Out.MinOut = Lo[0];
        Out.MaxOut = Hi[0];
    }
    else if (Width == 2)                                        // 2212-2232, 2342-2348
    {
        Out.Op = bOneKey && Curve->Keys[0].Out[0] == Curve->Keys[0].Out[1] ? RDO_None : RDO_Random;
        Out.Entry = [Curve](float T) { return Eval(*Curve, T); };
        Out.MinOut = std::min(Lo[0], Lo[1]);
        Out.MaxOut = std::max(Hi[0], Hi[1]);
    }
    else if (Width == 3)                                        // 2732-2748, 2889-2920, 3048-3078; GetLockFlag 0
    {
        const uint8 Lock = LockOf(Find(Tags, "LockedAxes"));
        Out.bVector = true;
        Out.PerEntry = 3;
        Out.Entry = [Curve, Lock](float T) { const FVec V = Locked(VecAt(Eval(*Curve, T), 0), Lock); return FVals{ V.X, V.Y, V.Z }; };
        Out.MinVec = Locked(VecAt(Lo, 0), Lock);
        Out.MaxVec = Locked(VecAt(Hi, 0), Lock);
        Out.MinOut = MinOf(Out.MinVec);
        Out.MaxOut = MaxOf(Out.MaxVec);
    }
    else                                                        // 3728-3760, 3961-3970, 4225-4245; LockAndMirror a no-op
    {
        const FKey* One = bOneKey ? &Curve->Keys[0] : nullptr;
        Out.bVector = true;
        Out.PerEntry = 3;
        Out.Op = One && VecAt(One->Out, 0) == VecAt(One->Out, 3) ? RDO_None : BoolOf(Find(Tags, "bUseExtremes")) ? RDO_Extreme : RDO_Random;
        Out.Entry = [Curve](float T) { return Eval(*Curve, T); };
        Out.MinOut = std::min(*std::min_element(Lo.begin(), Lo.end()), *std::min_element(Hi.begin(), Hi.end()));
        Out.MaxOut = std::max(*std::max_element(Lo.begin(), Lo.end()), *std::max_element(Hi.begin(), Hi.end()));
        Out.MinVec = { std::min(Lo[0], Hi[0]), std::min(Lo[1], Hi[1]), std::min(Lo[2], Hi[2]) };
        Out.MaxVec = { std::max(Lo[3], Hi[3]), std::max(Lo[4], Hi[4]), std::max(Lo[5], Hi[5]) };
    }
    return true;
}

// ---- the lookup table: BuildLookupTable then OptimizeLookupTable (Distributions.cpp 109-160, 535-679)

struct FTable
{
    float TimeScale = 0, TimeBias = 0;
    std::vector<float> Values;
    uint32 EntryCount = 0, EntryStride = 0, SubEntryStride = 0;
};

constexpr uint32 kMaxSamples = 128;     // LOOKUP_TABLE_MAX_SAMPLES

/* FDistributionLookupTable::GetEntry (Distributions.h 106-122): the two entries around a time, and the lerp alpha. */
void GetEntry(const FTable& T, float Time, const float*& E1, const float*& E2, float& Alpha)
{
    Time -= T.TimeBias;
    Time *= T.TimeScale;
    Time = Time >= 0.0f ? Time : 0.0f;
    Alpha = Time - std::trunc(Time);
    const uint32 Index = uint32(int32(Time));
    E1 = &T.Values[std::min(Index, T.EntryCount - 1) * T.EntryStride];
    E2 = &T.Values[std::min(Index + 1, T.EntryCount - 1) * T.EntryStride];
}

FTable BuildTable(const FBaked& B)
{
    FTable T;
    const float Step = (B.MaxIn - B.MinIn) / float(kMaxSamples - 1);
    T.EntryCount = kMaxSamples;
    T.EntryStride = uint32((B.Op == RDO_None ? 1 : 2) * B.PerEntry);
    T.SubEntryStride = B.Op == RDO_None ? 0 : uint32(B.PerEntry);
    T.TimeScale = Step > 0.0f ? 1.0f / Step : 0.0f;
    T.TimeBias = B.MinIn;
    for (uint32 I = 0; I < kMaxSamples; ++I)
    {
        const FVals V = B.Entry(B.MinIn + float(I) * Step);
        T.Values.insert(T.Values.end(), V.begin(), V.begin() + T.EntryStride);
    }
    return T;
}

FTable Resample(const FTable& In, float MinIn, float MaxIn, uint32 Count)
{
    FTable T;
    const float Step = Count > 1 ? (MaxIn - MinIn) / float(Count - 1) : 0.0f;
    T.EntryCount = Count;
    T.EntryStride = In.EntryStride;
    T.SubEntryStride = In.SubEntryStride;
    T.TimeBias = MinIn;
    T.TimeScale = Step > 0.0f ? 1.0f / Step : 0.0f;
    T.Values.resize(size_t(Count) * In.EntryStride);
    for (uint32 I = 0; I < Count; ++I)
    {
        const float* E1;
        const float* E2;
        float Alpha;
        GetEntry(In, MinIn + Step * float(I), E1, E2, Alpha);
        for (uint32 V = 0; V < In.EntryStride; ++V) T.Values[I * In.EntryStride + V] = E1[V] + Alpha * (E2[V] - E1[V]);
    }
    return T;
}

/* ComputeLookupTableError: the largest distance between the two tables' entries over Count samples. */
float TableError(const FTable& A, const FTable& B, float MinIn, float MaxIn, uint32 Count)
{
    const FTable& T1 = B.EntryCount > A.EntryCount ? B : A;
    const FTable& T2 = &T1 == &A ? B : A;
    const uint32 Per = T1.EntryStride - T1.SubEntryStride;
    const float Step = (MaxIn - MinIn) / float(Count - 1);
    float Error = 0.0f, Time = MinIn;
    for (uint32 I = 0; I < Count; ++I, Time += Step)
    {
        const float *A1, *A2, *B1, *B2;
        float AlphaA, AlphaB;
        GetEntry(T1, Time, A1, A2, AlphaA);
        GetEntry(T2, Time, B1, B2, AlphaB);
        for (uint32 Sub = 0; Sub < (T1.SubEntryStride > 0 ? 2u : 1u); ++Sub)
        {
            const uint32 At = Sub * T1.SubEntryStride;
            float Dist = 0.0f;
            for (uint32 V = 0; V < Per; ++V)
            {
                const float D = (A1[At + V] + AlphaA * (A2[At + V] - A1[At + V])) - (B1[At + V] + AlphaB * (B2[At + V] - B1[At + V]));
                Dist += D * D;
            }
            Error = std::max(Error, std::sqrt(Dist));
        }
    }
    return Error;
}

/* OptimizeLookupTable: the fewest samples, a power of two, within 0.05 of the 128 (LOOKUP_TABLE_ERROR_THRESHOLD). */
void Optimize(FTable& T)
{
    const float OneOverScale = T.TimeScale == 0.0f ? 0.0f : 1.0f / T.TimeScale;
    const float MinIn = T.TimeBias, MaxIn = T.TimeBias + float(T.EntryCount - 1) * OneOverScale;
    for (uint32 Count = 1; Count < kMaxSamples; Count <<= 1)
    {
        FTable R = Resample(T, MinIn, MaxIn, Count);
        if (TableError(R, T, MinIn, MaxIn, kMaxSamples) < 0.05f)
        {
            T = std::move(R);
            return;
        }
    }
}

/* The raw distribution's tags as the cook writes a baked one (P_CAretakerAoE_ElectricutionAttack, P_electric_arc; the
   order and the left-out zeros across DRG's P_*): MinValue, MaxValue and a vector's MinValueVec / MaxValueVec where not
   zero, Distribution null, then the Table, its members at zero left out as a delta over the struct's default. */
std::vector<FPropertyDef> RawTags(const FBaked& B)
{
    FTable T = BuildTable(B);
    Optimize(T);
    std::vector<FPropertyDef> Table;
    if (T.TimeScale != 0.0f) Table.push_back(FloatTag("TimeScale", T.TimeScale));
    if (T.TimeBias != 0.0f) Table.push_back(FloatTag("TimeBias", T.TimeBias));
    FPropertyDef Values;
    Values.Type = "ArrayProperty";
    Values.Name = "Values";
    Values.StructName = "FloatProperty";
    Values.Inner = std::make_shared<FPropertyDef>(FloatTag("Values", 0.0f));
    Values.Default.K = FDefaultValue::Array;
    for (const float F : T.Values)
    {
        FDefaultValue V;
        V.K = FDefaultValue::Float;
        V.F = F;
        Values.Default.Items.push_back(V);
    }
    Table.push_back(Values);
    Table.push_back(ByteTag("Op", B.Op));
    Table.push_back(ByteTag("EntryCount", uint8(T.EntryCount)));
    Table.push_back(ByteTag("EntryStride", uint8(T.EntryStride)));
    if (T.SubEntryStride) Table.push_back(ByteTag("SubEntryStride", uint8(T.SubEntryStride)));
    if (B.Lock) Table.push_back(ByteTag("LockFlag", B.Lock));

    std::vector<FPropertyDef> Out;
    if (B.MinOut != 0.0f) Out.push_back(FloatTag("MinValue", B.MinOut));
    if (B.MaxOut != 0.0f) Out.push_back(FloatTag("MaxValue", B.MaxOut));
    if (B.bVector && !(B.MinVec == FVec{})) Out.push_back(VecTag("MinValueVec", B.MinVec));
    if (B.bVector && !(B.MaxVec == FVec{})) Out.push_back(VecTag("MaxValueVec", B.MaxVec));
    Out.push_back(NullObjectTag("Distribution"));
    Out.push_back(StructTag("Table", "DistributionLookupTable", std::move(Table)));
    return Out;
}

// ---- the modules' starting distributions (each class's InitializeDefaults)

struct FStart { const char* Class; const char* Member; const char* Dist; float A[3]; float B[3]; };
/* What Cascade's editor gives a new module (Cascade.cpp 984): InitializeDefaults, from Engine/Private/Particles:
   ParticleModules.cpp (Required, Lifetime, Light), ParticleModules_Spawn.cpp, _Size.cpp, _Color.cpp, _Velocity.cpp,
   ParticleBeamModules.cpp; then SetToSensibleDefaults, which only Lifetime (1 to 1, ParticleModules.cpp 3863-3871) and
   Light (ParticleModules.cpp 3242-3282) change here. A is a Constant's value or a Uniform's Min, B a Uniform's Max; a
   ConstantCurve starts with no keys. */
const FStart kStarts[] = {
    { "ParticleModuleRequired", "SpawnRate", "DistributionFloatConstant", { 0 }, { 0 } },
    { "ParticleModuleSpawn", "Rate", "DistributionFloatConstant", { 20 }, { 0 } },
    { "ParticleModuleSpawn", "RateScale", "DistributionFloatConstant", { 1 }, { 0 } },
    { "ParticleModuleSpawn", "BurstScale", "DistributionFloatConstant", { 1 }, { 0 } },
    { "ParticleModuleLifetime", "Lifetime", "DistributionFloatUniform", { 1 }, { 1 } },
    { "ParticleModuleSize", "StartSize", "DistributionVectorUniform", { 1, 1, 1 }, { 1, 1, 1 } },
    { "ParticleModuleColor", "StartColor", "DistributionVectorConstant", { 0, 0, 0 }, { 0 } },
    { "ParticleModuleColor", "StartAlpha", "DistributionFloatConstant", { 1 }, { 0 } },
    { "ParticleModuleLight", "ColorScaleOverLife", "DistributionVectorConstant", { 1, 1, 1 }, { 0 } },
    { "ParticleModuleLight", "BrightnessOverLife", "DistributionFloatConstant", { 32 }, { 0 } },
    { "ParticleModuleLight", "RadiusScale", "DistributionFloatConstant", { 15 }, { 0 } },
    { "ParticleModuleLight", "LightExponent", "DistributionFloatConstant", { 16 }, { 0 } },
    { "ParticleModuleVelocity", "StartVelocity", "DistributionVectorUniform", { 0, 0, 0 }, { 0, 0, 0 } },
    { "ParticleModuleVelocity", "StartVelocityRadial", "DistributionFloatUniform", { 0 }, { 0 } },
    { "ParticleModuleTypeDataBeam2", "Distance", "DistributionFloatConstant", { 25 }, { 0 } },
    { "ParticleModuleTypeDataBeam2", "TaperFactor", "DistributionFloatConstant", { 1 }, { 0 } },
    { "ParticleModuleTypeDataBeam2", "TaperScale", "DistributionFloatConstant", { 1 }, { 0 } },
    { "ParticleModuleBeamSource", "Source", "DistributionVectorConstant", { 50, 50, 50 }, { 0 } },
    { "ParticleModuleBeamSource", "SourceTangent", "DistributionVectorConstant", { 1, 0, 0 }, { 0 } },
    { "ParticleModuleBeamSource", "SourceStrength", "DistributionFloatConstant", { 25 }, { 0 } },
    { "ParticleModuleBeamTarget", "Target", "DistributionVectorConstant", { 50, 50, 50 }, { 0 } },
    { "ParticleModuleBeamTarget", "TargetTangent", "DistributionVectorConstant", { 1, 0, 0 }, { 0 } },
    { "ParticleModuleBeamTarget", "TargetStrength", "DistributionFloatConstant", { 25 }, { 0 } },
    { "ParticleModuleBeamNoise", "NoiseSpeed", "DistributionVectorConstant", { 50, 50, 50 }, { 0 } },
    { "ParticleModuleBeamNoise", "NoiseRange", "DistributionVectorConstant", { 50, 50, 50 }, { 0 } },
    { "ParticleModuleBeamNoise", "NoiseRangeScale", "DistributionFloatConstant", { 1 }, { 0 } },
    { "ParticleModuleBeamNoise", "NoiseTangentStrength", "DistributionFloatConstant", { 250 }, { 0 } },
    { "ParticleModuleBeamNoise", "NoiseScale", "DistributionFloatConstantCurve", { 0 }, { 0 } },
};

/* A starting value as the tags of its distribution object, to bake as the mod's own. */
std::vector<FPropertyDef> StartTags(const FStart& S)
{
    const std::string D = S.Dist;
    const FVec A{ S.A[0], S.A[1], S.A[2] }, B{ S.B[0], S.B[1], S.B[2] };
    if (D == "DistributionFloatConstant") return { FloatTag("Constant", S.A[0]) };
    if (D == "DistributionFloatUniform") return { FloatTag("Min", S.A[0]), FloatTag("Max", S.B[0]) };
    if (D == "DistributionVectorConstant") return { VecTag("Constant", A) };
    if (D == "DistributionFloatConstantCurve") return {};
    return { VecTag("Min", A), VecTag("Max", B) };
}

// ---- walking every tag

void ForEachTag(std::vector<FPropertyDef>& Tags, const std::function<void(FPropertyDef&)>& Fn)
{
    for (FPropertyDef& T : Tags)
    {
        Fn(T);
        if (std::vector<FPropertyDef>* M = MembersOf(T)) ForEachTag(*M, Fn);
        for (FDefaultValue& Item : T.Default.Items)
            if (Item.Members) ForEachTag(*Item.Members, Fn);
    }
}

void ForEachValue(FDefaultValue& D, const std::function<void(FDefaultValue&)>& Fn)
{
    Fn(D);
    for (FDefaultValue& I : D.Items) ForEachValue(I, Fn);
    for (FDefaultValue& I : D.Removed) ForEachValue(I, Fn);
    if (D.Members) for (FPropertyDef& M : *D.Members) ForEachValue(M.Default, Fn);
}

void ForEachRef(std::vector<FPropertyDef>& Tags, const std::function<void(FDefaultValue&)>& Fn)
{
    for (FPropertyDef& T : Tags)
    {
        ForEachValue(T.Default, Fn);
        if (T.Members) ForEachRef(*T.Members, Fn);
    }
}

/* The objects a tag points at, as indices into the list. */
std::vector<int32> RefsOf(FPropertyDef* T)
{
    std::vector<int32> Out;
    if (!T) return Out;
    std::vector<FPropertyDef> One{ *T };
    ForEachRef(One, [&](FDefaultValue& V) { if (V.Instanced > 0) Out.push_back(V.Instanced - 1); });
    return Out;
}

bool IsRawDist(const FPropertyDef& T)
{
    return T.Type == "StructProperty" && (T.StructName == "RawDistributionFloat" || T.StructName == "RawDistributionVector");
}
}   // namespace

bool CookCascade(std::vector<FPropertyDef>& SystemTags, std::vector<FCascadeObject>& Objects, std::vector<int32>& Kept,
                 std::vector<std::string>& Warnings, std::string* Err)
{
    const size_t N = Objects.size();
    auto Where = [&](int32 I) { return I < 0 ? std::string("the system") : Objects[size_t(I)].Class + " " + Objects[size_t(I)].Name; };

    /* The LOD levels, each with its emitter and its place in it; checked against the system's LOD count. */
    FPropertyDef* Distances = Find(SystemTags, "LODDistances");
    const size_t LodCount = Distances ? Distances->Default.Items.size() : 0;
    FPropertyDef* Settings = Find(SystemTags, "LODSettings");
    if (Settings && Settings->Default.Items.size() != LodCount)
    {
        *Err = "the system: LODSettings has " + std::to_string(Settings->Default.Items.size()) + " entries and LODDistances "
             + std::to_string(LodCount) + "; give both one per LOD level";
        return false;
    }
    std::map<int32, uint8> Validity;        // a module -> the LOD levels naming it, as bits
    for (size_t E = 0; E < N; ++E)
    {
        FPropertyDef* Levels = Find(Objects[E].Tags, "LODLevels");
        if (!Levels) continue;
        const std::vector<int32> Lods = RefsOf(Levels);
        if (Lods.size() != LodCount)
        {
            *Err = Where(int32(E)) + ": has " + std::to_string(Lods.size()) + " LOD levels and the system's LODDistances "
                 + std::to_string(LodCount) + "; every emitter has one LOD level per entry of LODDistances";
            return false;
        }
        for (size_t L = 0; L < Lods.size(); ++L)
        {
            FCascadeObject& Lod = Objects[size_t(Lods[L])];
            const std::string At = Where(int32(E)) + " LOD " + std::to_string(L);
            if (FPropertyDef* Level = Find(Lod.Tags, "Level"))
            {
                if (Level->Default.I != int64(L))
                { *Err = At + ": its Level is " + std::to_string(Level->Default.I) + "; a LOD level's Level is its place in LODLevels, " + std::to_string(L); return false; }
            }
            else if (L != 0)        // the editor sets it; 0 is the default
            {
                FPropertyDef Level;
                Level.Type = "IntProperty";
                Level.Name = "Level";
                Level.ElementSize = 4;
                Level.Default.K = FDefaultValue::Int;
                Level.Default.I = int64(L);
                Lod.Tags.push_back(Level);
            }
            for (const char* Slot : { "RequiredModule", "SpawnModule" })
                if (RefsOf(Find(Lod.Tags, Slot)).empty())
                {
                    *Err = At + ": has no " + Slot + "; give it a " + (std::string(Slot) == "SpawnModule" ? "UParticleModuleSpawn"
                                                                                                      : "UParticleModuleRequired");
                    return false;
                }
            for (const char* Slot : { "RequiredModule", "SpawnModule", "TypeDataModule", "EventGenerator", "Modules" })
                for (int32 M : RefsOf(Find(Lod.Tags, Slot))) Validity[M] |= uint8(1u << L);
        }
    }
    for (const auto& [M, Bits] : Validity)
    {
        std::vector<FPropertyDef>& Tags = Objects[size_t(M)].Tags;
        Tags.erase(std::remove_if(Tags.begin(), Tags.end(), [](const FPropertyDef& T) { return T.Name == "LODValidity"; }), Tags.end());
        Tags.push_back(ByteTag("LODValidity", Bits));
    }

    /* A GPU sprite emitter simulates off what the editor's UParticleModuleTypeDataGpu::Build works out from the emitter's
       modules (EmitterInfo, ResourceData); the game's Build is editor-only, so without it the emitter misrenders. */
    for (size_t I = 0; I < N; ++I)
        if (Objects[I].Class == "ParticleModuleTypeDataGpu" && !Find(Objects[I].Tags, "ResourceData"))
        {
            *Err = Where(int32(I)) + ": a GPU sprite emitter needs the EmitterInfo and ResourceData the editor builds from its "
                   "modules, which AssetGen does not build yet; give them (bpdecomp writes a vanilla P_*'s) or use a CPU emitter";
            return false;
        }

    /* The starting values first, so they bake with the mod's own distributions below. Each is a distribution object of
       the module's, appended to the list as the editor's NewObject in InitializeDefaults makes one. */
    for (size_t I = 0; I < N; ++I)
    {
        /* By index, never a reference: Objects grows below, which moves every element. */
        bool bKnown = false;
        for (const FStart& S : kStarts)
        {
            if (Objects[I].Class != S.Class) continue;
            bKnown = true;
            if (Find(Objects[I].Tags, S.Member)) continue;
            const std::vector<std::pair<std::string, bool>>& Dists = Objects[I].RawDists;
            const auto Raw = std::find_if(Dists.begin(), Dists.end(), [&](const auto& R) { return Lower(R.first) == Lower(S.Member); });
            if (Raw == Dists.end()) continue;
            const std::string Member = Raw->first;
            const bool bVector = Raw->second;
            FCascadeObject D;
            D.Class = S.Dist;
            D.Name = std::string("Distribution") + S.Member;
            D.Outer = int32(I);
            D.Tags = StartTags(S);
            Objects.push_back(std::move(D));
            FPropertyDef Ref;
            Ref.Type = "ObjectProperty";
            Ref.Name = "Distribution";
            Ref.Default.K = FDefaultValue::Obj;
            Ref.Default.Instanced = int32(Objects.size());
            Objects[I].Tags.push_back(StructTag(Member, bVector ? "RawDistributionVector" : "RawDistributionFloat", { Ref }));
        }
        if (!bKnown && Objects[I].Class.compare(0, 14, "ParticleModule") == 0)
            for (const auto& [Member, bVector] : Objects[I].RawDists)
                if (!Find(Objects[I].Tags, Member))
                    Warnings.push_back(Where(int32(I)) + ": " + Member + " is left unset and reads as 0 in game (Cascade's editor "
                                       "starts it at a value of its own); set it");
    }

    /* Bake every raw distribution that points at a distribution object; the object is then left out unless something
       else still points at it. */
    std::set<int32> Baked;
    bool bOk = true;
    auto BakeIn = [&](std::vector<FPropertyDef>& Tags, int32 Owner) {
        ForEachTag(Tags, [&](FPropertyDef& T) {
            if (!bOk || !IsRawDist(T)) return;
            std::vector<FPropertyDef>* M = MembersOf(T);
            FPropertyDef* D = M ? Find(*M, "Distribution") : nullptr;
            if (!D || D->Default.Instanced <= 0) return;
            const int32 Obj = D->Default.Instanced - 1;
            /* What CanBeBaked refuses stays live, pointed at, without a table (FRawDistribution*::Initialize returns
               first): a particle parameter (ParameterBase overrides it false) or an object whose bCanBeBaked is false. */
            const std::string& Class = Objects[size_t(Obj)].Class;
            const FPropertyDef* Can = Find(Objects[size_t(Obj)].Tags, "bCanBeBaked");
            if (Class == "DistributionFloatParticleParameter" || Class == "DistributionVectorParticleParameter" || (Can && !BoolOf(Can)))
                return;
            FBaked B;
            std::string Why;
            if (!Bake(Objects[size_t(Obj)].Class, Objects[size_t(Obj)].Tags, B, Why) && !Why.empty())
            {
                *Err = Where(Owner) + ": " + T.Name + " is " + Objects[size_t(Obj)].Name + ", and " + Why;
                bOk = false;
                return;
            }
            if (!B.Entry)
            {
                *Err = Where(Owner) + ": " + T.Name + " is a " + Objects[size_t(Obj)].Class + ", which AssetGen does not bake "
                       "yet; use a Constant, Uniform, ConstantCurve or UniformCurve distribution";
                bOk = false;
                return;
            }
            if (B.bVector != (T.StructName == "RawDistributionVector"))
            {
                *Err = Where(Owner) + ": " + T.Name + " is a " + T.StructName + ", and " + Objects[size_t(Obj)].Name + " a "
                       + Objects[size_t(Obj)].Class;
                bOk = false;
                return;
            }
            T.Members = std::make_shared<std::vector<FPropertyDef>>(RawTags(B));
            T.Default.K = FDefaultValue::Struct;
            T.Default.Members = nullptr;
            Baked.insert(Obj);
        });
    };
    BakeIn(SystemTags, -1);
    for (size_t I = 0; I < Objects.size() && bOk; ++I) BakeIn(Objects[I].Tags, int32(I));
    if (!bOk) return false;

    /* What still points at each object, or holds it as outer: a baked one with neither is left out. */
    std::set<int32> Live;
    ForEachRef(SystemTags, [&](FDefaultValue& V) { if (V.Instanced > 0) Live.insert(V.Instanced - 1); });
    for (FCascadeObject& O : Objects)
    {
        ForEachRef(O.Tags, [&](FDefaultValue& V) { if (V.Instanced > 0) Live.insert(V.Instanced - 1); });
        if (O.Outer >= 0) Live.insert(O.Outer);
    }
    std::vector<int32> NewId(Objects.size(), -1);
    std::vector<FCascadeObject> Out;
    Kept.clear();
    for (size_t I = 0; I < Objects.size(); ++I)
    {
        if (Baked.count(int32(I)) && !Live.count(int32(I))) continue;
        NewId[I] = int32(Out.size());
        Kept.push_back(int32(I));
        Out.push_back(std::move(Objects[I]));
    }
    auto Renumber = [&](FDefaultValue& V) { if (V.Instanced > 0) V.Instanced = NewId[size_t(V.Instanced - 1)] + 1; };
    ForEachRef(SystemTags, Renumber);
    for (FCascadeObject& O : Out)
    {
        ForEachRef(O.Tags, Renumber);
        if (O.Outer >= 0) O.Outer = NewId[size_t(O.Outer)];
    }
    Objects = std::move(Out);
    return true;
}
