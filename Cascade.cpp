#include "Cascade.h"

#include "Package.h"

#include <algorithm>
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

// ---- the distributions: what FRawDistribution::Initialize reads off each class (Distributions.cpp)

/* What baking one distribution object gives: the table's one entry (InitializeRawEntry, cut to the stride), its
   operation and lock flag, the out range (GetOutRange) and a vector's GetRange. */
struct FBaked
{
    bool bVector = false;
    uint8 Op = RDO_None;
    uint8 Lock = 0;
    std::vector<float> Entry;
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

/* The four classes slice 1 bakes, from their members (unset ones their class defaults: zero, MirrorFlags Different,
   which UeApi cannot set). False for a class not baked yet. */
bool Bake(const std::string& Class, std::vector<FPropertyDef>& Tags, FBaked& Out)
{
    if (Class == "DistributionFloatConstant")                   // 1731-1735, 1771-1781; GetOperation RDO_None
    {
        const float C = FloatOf(Find(Tags, "Constant"));
        Out.Entry = { C };
        Out.MinOut = Out.MaxOut = C;
        return true;
    }
    if (Class == "DistributionFloatUniform")                    // 2035-2050, 2123-2133
    {
        const float Min = FloatOf(Find(Tags, "Min")), Max = FloatOf(Find(Tags, "Max"));
        Out.Op = Min == Max ? RDO_None : RDO_Random;
        Out.Entry = { Min, Max };
        Out.MinOut = Min;
        Out.MaxOut = Max;
        return true;
    }
    if (Class == "DistributionVectorConstant")                  // 2493-2509, 2631-2657, 2721-2725
    {
        const FVec C = VecOf(Find(Tags, "Constant"));
        const FVec L = Locked(C, LockOf(Find(Tags, "LockedAxes")));
        Out.bVector = true;
        Out.Entry = { L.X, L.Y, L.Z };
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
        Out.Op = Min == Max ? RDO_None : BoolOf(Find(Tags, "bUseExtremes")) ? RDO_Extreme : RDO_Random;
        Out.Lock = Lock;
        Out.Entry = { LMin.X, LMin.Y, LMin.Z, LMax.X, LMax.Y, LMax.Z };
        Out.MinOut = MinOf(LMin);
        Out.MaxOut = MaxOf(LMax);
        Out.MinVec = Min;
        Out.MaxVec = Max;
        return true;
    }
    return false;
}

/* The raw distribution's tags as the cook writes a baked one (P_CAretakerAoE_ElectricutionAttack, P_electric_arc):
   MinValue, MaxValue and a vector's MinValueVec / MaxValueVec where not zero, Distribution null, then the Table.

   The table is BuildLookupTable's (Distributions.cpp 109-160) after OptimizeLookupTable (613-679). Every class above
   has the input range [0, 0], so the 128 samples are equal and the first resample, to one entry, has no error: one
   entry, TimeScale and TimeBias 0, EntryStride ValuesPerEntry (twice that unless RDO_None), SubEntryStride
   ValuesPerEntry unless RDO_None, LockFlag kept. Members at their zero are left out, as a delta over the struct's
   default. */
std::vector<FPropertyDef> RawTags(const FBaked& B)
{
    const size_t PerEntry = B.bVector ? 3 : 1;
    const size_t Stride = (B.Op == RDO_None ? 1 : 2) * PerEntry;
    std::vector<FPropertyDef> Table;
    FPropertyDef Values;
    Values.Type = "ArrayProperty";
    Values.Name = "Values";
    Values.StructName = "FloatProperty";
    Values.Inner = std::make_shared<FPropertyDef>(FloatTag("Values", 0.0f));
    Values.Default.K = FDefaultValue::Array;
    for (size_t I = 0; I < Stride; ++I)
    {
        FDefaultValue V;
        V.K = FDefaultValue::Float;
        V.F = B.Entry[I];
        Values.Default.Items.push_back(V);
    }
    Table.push_back(Values);
    Table.push_back(ByteTag("Op", B.Op));
    Table.push_back(ByteTag("EntryCount", 1));
    Table.push_back(ByteTag("EntryStride", uint8(Stride)));
    if (B.Op != RDO_None) Table.push_back(ByteTag("SubEntryStride", uint8(PerEntry)));
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
/* Ported from InitializeDefaults in Engine/Private/Particles: ParticleModules.cpp (Required, Lifetime, Light),
   ParticleModules_Spawn.cpp, _Size.cpp, _Color.cpp, _Velocity.cpp, ParticleBeamModules.cpp. A is a Constant's value
   or a Uniform's Min, B a Uniform's Max. Dist "Curve" is a default AssetGen cannot bake yet. */
const FStart kStarts[] = {
    { "ParticleModuleRequired", "SpawnRate", "DistributionFloatConstant", { 0 }, { 0 } },
    { "ParticleModuleSpawn", "Rate", "DistributionFloatConstant", { 20 }, { 0 } },
    { "ParticleModuleSpawn", "RateScale", "DistributionFloatConstant", { 1 }, { 0 } },
    { "ParticleModuleSpawn", "BurstScale", "DistributionFloatConstant", { 1 }, { 0 } },
    { "ParticleModuleLifetime", "Lifetime", "DistributionFloatUniform", { 0 }, { 0 } },
    { "ParticleModuleSize", "StartSize", "DistributionVectorUniform", { 1, 1, 1 }, { 1, 1, 1 } },
    { "ParticleModuleColor", "StartColor", "DistributionVectorConstant", { 0, 0, 0 }, { 0 } },
    { "ParticleModuleColor", "StartAlpha", "DistributionFloatConstant", { 1 }, { 0 } },
    { "ParticleModuleLight", "ColorScaleOverLife", "DistributionVectorConstant", { 0, 0, 0 }, { 0 } },
    { "ParticleModuleLight", "BrightnessOverLife", "DistributionFloatConstant", { 0 }, { 0 } },
    { "ParticleModuleLight", "RadiusScale", "DistributionFloatConstant", { 0 }, { 0 } },
    { "ParticleModuleLight", "LightExponent", "DistributionFloatConstant", { 0 }, { 0 } },
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
    { "ParticleModuleBeamNoise", "NoiseScale", "Curve", { 0 }, { 0 } },
};

/* A starting value as the tags of its distribution object, to bake as the mod's own. */
std::vector<FPropertyDef> StartTags(const FStart& S)
{
    const std::string D = S.Dist;
    const FVec A{ S.A[0], S.A[1], S.A[2] }, B{ S.B[0], S.B[1], S.B[2] };
    if (D == "DistributionFloatConstant") return { FloatTag("Constant", S.A[0]) };
    if (D == "DistributionFloatUniform") return { FloatTag("Min", S.A[0]), FloatTag("Max", S.B[0]) };
    if (D == "DistributionVectorConstant") return { VecTag("Constant", A) };
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

    /* The starting values first, so they bake with the mod's own distributions below. Each is a distribution object of
       the module's, appended to the list as the editor's NewObject in InitializeDefaults makes one. */
    for (size_t I = 0; I < N; ++I)
    {
        FCascadeObject& O = Objects[I];
        bool bKnown = false;
        for (const FStart& S : kStarts)
        {
            if (O.Class != S.Class) continue;
            bKnown = true;
            if (Find(O.Tags, S.Member)) continue;
            const auto Raw = std::find_if(O.RawDists.begin(), O.RawDists.end(), [&](const auto& R) { return Lower(R.first) == Lower(S.Member); });
            if (Raw == O.RawDists.end()) continue;
            if (std::string(S.Dist) == "Curve")
            {
                Warnings.push_back(Where(int32(I)) + ": " + Raw->first + " is left unset and reads as 0 in game; Cascade's editor "
                                   "starts it as a curve, which AssetGen does not bake yet; set it");
                continue;
            }
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
            Objects[I].Tags.push_back(StructTag(Raw->first, Raw->second ? "RawDistributionVector" : "RawDistributionFloat", { Ref }));
        }
        if (!bKnown && O.Class.compare(0, 14, "ParticleModule") == 0)
            for (const auto& [Member, bVector] : O.RawDists)
                if (!Find(O.Tags, Member))
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
            FBaked B;
            if (!Bake(Objects[size_t(Obj)].Class, Objects[size_t(Obj)].Tags, B))
            {
                *Err = Where(Owner) + ": " + T.Name + " is a " + Objects[size_t(Obj)].Class + ", which AssetGen does not bake "
                       "yet; use a DistributionFloatConstant / DistributionFloatUniform (Vector for a vector one)";
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
