#include "Niagara.h"

#include "Package.h"

#include <map>
#include <memory>

using namespace Uasset;

namespace
{
// ---- reading the lowered tags

const std::vector<FPropertyDef>* MembersOf(const FPropertyDef& P)
{
    return P.Members ? P.Members.get() : P.Default.Members.get();
}

const FPropertyDef* Find(const std::vector<FPropertyDef>* Tags, const std::string& Name)
{
    if (Tags) for (const FPropertyDef& T : *Tags) if (Lower(T.Name) == Lower(Name)) return &T;
    return nullptr;
}

const FPropertyDef* Find(const FPropertyDef* Struct, const std::string& Name)
{
    return Struct ? Find(MembersOf(*Struct), Name) : nullptr;
}

const FPropertyDef* Find(const FDefaultValue& Element, const std::string& Name)
{
    return Find(Element.Members.get(), Name);
}

std::string StrOf(const FPropertyDef* T) { return T && T->Default.K == FDefaultValue::Str ? T->Default.S : std::string(); }

/* The object an ObjectProperty's value is instanced as, or null. */
const FNiagaraObject* ObjectOf(const FPropertyDef* T, const std::vector<FNiagaraObject>& Objects)
{
    if (!T || T->Default.K != FDefaultValue::Obj || T->Default.Instanced <= 0) return nullptr;
    return size_t(T->Default.Instanced) <= Objects.size() ? &Objects[size_t(T->Default.Instanced - 1)] : nullptr;
}

/* A variable's type as FNiagaraTypeDefinition compares it: the struct (class, enum) and UnderlyingType. */
struct FType
{
    std::string Name;
    int64 Underlying = 0;
    bool operator==(const FType& O) const { return Name == O.Name && Underlying == O.Underlying; }
};

FType TypeOf(const FDefaultValue& Var, const std::function<std::string(FIndex)>& TypeName)
{
    const FPropertyDef* Handle = Find(Var, "TypeDefHandle");
    const FPropertyDef* Cso = Find(Handle, "ClassStructOrEnum");
    const FPropertyDef* Ut = Find(Handle, "UnderlyingType");
    return { Cso && Cso->Default.K == FDefaultValue::Obj ? TypeName(Cso->Default.Object) : std::string(), Ut ? Ut->Default.I : 0 };
}

std::string VarName(const FDefaultValue& Var) { return StrOf(Find(Var, "Name")); }

/* A script's CachedScriptVM.Attributes, each element a FNiagaraVariable's value. */
const std::vector<FDefaultValue>* AttributesOf(const FNiagaraObject* Script)
{
    static const std::vector<FDefaultValue> None;
    if (!Script) return &None;
    const FPropertyDef* Attrs = Find(Find(&Script->Tags, "CachedScriptVM"), "Attributes");
    return Attrs ? &Attrs->Default.Items : &None;
}

// ---- building tags

FPropertyDef Def(const std::string& Type, const std::string& Name, const std::string& StructName = std::string())
{
    FPropertyDef P;
    P.Type = Type;
    P.Name = Name;
    P.StructName = StructName;
    return P;
}

FPropertyDef U32(const std::string& Name, uint32 V)
{
    FPropertyDef P = Def("UInt32Property", Name);
    P.Default.K = FDefaultValue::Int;
    P.Default.I = V;
    return P;
}

FPropertyDef Struct(const std::string& Name, const std::string& StructName, std::vector<FPropertyDef> Members)
{
    FPropertyDef P = Def("StructProperty", Name, StructName);
    P.Default.K = FDefaultValue::Struct;
    P.Members = P.Default.Members = std::make_shared<std::vector<FPropertyDef>>(std::move(Members));
    return P;
}

FPropertyDef Array(const std::string& Name, const FPropertyDef& Inner, std::vector<FDefaultValue> Items)
{
    FPropertyDef P = Def("ArrayProperty", Name, Inner.Type);
    P.Inner = std::make_shared<FPropertyDef>(Inner);
    P.Default.K = FDefaultValue::Array;
    P.Default.Items = std::move(Items);
    return P;
}

FPropertyDef U32Array(const std::string& Name, const std::vector<uint32>& Vs)
{
    std::vector<FDefaultValue> Items;
    for (uint32 V : Vs) { FDefaultValue D; D.K = FDefaultValue::Int; D.I = V; Items.push_back(D); }
    return Array(Name, Def("UInt32Property", Name), std::move(Items));
}

/* A FNiagaraVariable without data, of a /Script/Niagara struct type: the alias variables. */
FPropertyDef Variable(const std::string& Member, const std::string& Name, FIndex Type)
{
    FPropertyDef N = Def("NameProperty", "Name");
    N.Default.K = FDefaultValue::Str;
    N.Default.S = Name;
    FPropertyDef Cso = Def("ObjectProperty", "ClassStructOrEnum");
    Cso.Default.K = FDefaultValue::Obj;
    Cso.Default.Object = Type;
    FPropertyDef Ut = Def("UInt16Property", "UnderlyingType");
    Ut.Default.K = FDefaultValue::Int;
    Ut.Default.I = 2;                   // FNiagaraTypeDefinition::UT_Struct
    return Struct(Member, "NiagaraVariable", { N, Struct("TypeDefHandle", "NiagaraTypeDefinitionHandle", { Cso, Ut }) });
}

// ---- layouts

/* FNiagaraTypeLayoutInfo::GenerateLayoutInfo (NiagaraTypes.h 284-327): each float, int32 / bool and uint16 (half)
   member's byte offset, nested structs flattened. The member layouts of the types a particle data set holds, from
   their 4.27 declarations (NiagaraTypes.h; FVector and the like Core's). An enum's struct is FNiagaraInt32's. */
struct FLayout { std::vector<uint32> F, I, H; };

bool LayoutOf(const FType& T, FLayout& L)
{
    if (T.Underlying == 3) { L.I = { 0 }; return true; }      // UT_Enum: GetStruct() is IntStruct
    static const std::map<std::string, FLayout> Known = {
        { "NiagaraFloat", { { 0 }, {}, {} } },
        { "NiagaraInt32", { {}, { 0 }, {} } },
        { "NiagaraBool", { {}, { 0 }, {} } },
        { "NiagaraID", { {}, { 0, 4 }, {} } },
        { "NiagaraRandInfo", { {}, { 0, 4, 8 }, {} } },        // Seed1, Seed2, Seed3
        { "NiagaraSpawnInfo", { { 4, 8 }, { 0, 12 }, {} } },   // Count, InterpStartDt, IntervalDt, SpawnGroup
        { "Vector2D", { { 0, 4 }, {}, {} } },
        { "Vector", { { 0, 4, 8 }, {}, {} } },
        { "Vector4", { { 0, 4, 8, 12 }, {}, {} } },
        { "Quat", { { 0, 4, 8, 12 }, {}, {} } },
        { "LinearColor", { { 0, 4, 8, 12 }, {}, {} } },
        { "NiagaraMatrix", { { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60 }, {}, {} } },
        { "NiagaraHalf", { {}, {}, { 0 } } },
        { "NiagaraHalfVector2", { {}, {}, { 0, 2 } } },
        { "NiagaraHalfVector3", { {}, {}, { 0, 2, 4 } } },
        { "NiagaraHalfVector4", { {}, {}, { 0, 2, 4, 6 } } },
    };
    const auto K = Known.find(T.Name);
    if (K == Known.end()) return false;
    L = K->second;
    return true;
}

std::vector<uint32> Count(size_t N)
{
    std::vector<uint32> Out;
    for (uint32 I = 0; I < uint32(N); ++I) Out.push_back(I);
    return Out;
}

/* FString::Replace's default: every occurrence, matched ignoring case. */
std::string ReplaceIgnoreCase(const std::string& S, const std::string& From, const std::string& To)
{
    std::string Out;
    const std::string Ls = Lower(S), Lf = Lower(From);
    size_t At = 0;
    for (size_t Hit; (Hit = Ls.find(Lf, At)) != std::string::npos; At = Hit + From.size())
        Out += S.substr(At, Hit - At) + To;
    return Out + S.substr(At);
}
}   // namespace

namespace
{
bool CompiledData(const std::vector<FPropertyDef>& SystemTags, const std::vector<FNiagaraObject>& Objects,
                  const std::function<std::string(FIndex)>& TypeName, const std::function<FIndex(const std::string&)>& NiagaraStruct,
                  std::vector<std::vector<FPropertyDef>>& Out, std::string* Err)
{
    Out.clear();
    /* Only when both system scripts have compiled (FNiagaraVMExecutableData::IsValid: a LastCompileStatus but
       NCS_Unknown). */
    const FNiagaraObject* SysSpawn = ObjectOf(Find(&SystemTags, "SystemSpawnScript"), Objects);
    const FNiagaraObject* SysUpdate = ObjectOf(Find(&SystemTags, "SystemUpdateScript"), Objects);
    for (const FNiagaraObject* S : { SysSpawn, SysUpdate })
    {
        const std::string Status = S ? StrOf(Find(Find(&S->Tags, "CachedScriptVM"), "LastCompileStatus")) : std::string();
        if (Status.empty() || Status.find("NCS_Unknown") != std::string::npos) return true;
    }
    const FPropertyDef* Handles = Find(&SystemTags, "EmitterHandles");
    if (!Handles) return true;
    const FIndex FloatDef = NiagaraStruct("NiagaraFloat"), IntDef = NiagaraStruct("NiagaraInt32");
    for (size_t E = 0; E < Handles->Default.Items.size(); ++E)
    {
        const FDefaultValue& Handle = Handles->Default.Items[E];
        const FNiagaraObject* Emitter = ObjectOf(Find(Handle, "Instance"), Objects);
        /* A disabled emitter the cook leaves out (its handle's Instance null): its data was built while it was there,
           under the handle's name, from scripts the editor does not compile for it, so it holds no variables (DRG's
           NS_MarkedForDeath). An enabled handle without its emitter the editor could not build. */
        const FPropertyDef* Enabled = Find(Handle, "bIsEnabled");
        if (!Emitter && (!Enabled || Enabled->Default.I != 0))
        { *Err = "EmitterHandles[" + std::to_string(E) + "] names no emitter of the block, which the editor's compile needs"; return false; }
        static const std::vector<FPropertyDef> None;
        const std::vector<FPropertyDef>* ETags = Emitter ? &Emitter->Tags : &None;
        const std::string Unique = Emitter ? StrOf(Find(ETags, "UniqueEmitterName")) : StrOf(Find(Handle, "Name"));

        // SpawnAttributes: the system scripts' spawn infos of this emitter, in order, each once
        std::vector<FDefaultValue> Spawn;
        for (const FNiagaraObject* S : { SysSpawn, SysUpdate })
            for (const FDefaultValue& Var : *AttributesOf(S))
            {
                const std::string N = VarName(Var);
                if (!(TypeOf(Var, TypeName) == FType{ "NiagaraSpawnInfo", 2 }) || Lower(N).rfind(Lower(Unique + "."), 0) != 0) continue;
                bool bHave = false;
                for (const FDefaultValue& H : Spawn) bHave |= Lower(H.S) == Lower(N);
                if (bHave) continue;
                FDefaultValue D;
                D.K = FDefaultValue::Str;
                D.S = N;
                Spawn.push_back(D);
            }

        // the data set's variables
        const bool bGpu = StrOf(Find(ETags, "SimTarget")).find("GPUComputeSim") != std::string::npos;
        std::vector<FDefaultValue> Vars;
        auto AddUnique = [&](const FDefaultValue& V) {
            for (const FDefaultValue& H : Vars)
                if (Lower(VarName(H)) == Lower(VarName(V)) && TypeOf(H, TypeName) == TypeOf(V, TypeName)) return;
            Vars.push_back(V);
        };
        const std::vector<std::string> Scripts = bGpu ? std::vector<std::string>{ "GPUComputeScript" }
                                                      : std::vector<std::string>{ "UpdateScriptProps", "SpawnScriptProps" };
        for (const std::string& S : Scripts)
        {
            const FPropertyDef* Ref = Find(ETags, S);
            if (Ref && Ref->Type == "StructProperty") Ref = Find(Ref, "Script");
            for (const FDefaultValue& V : *AttributesOf(ObjectOf(Ref, Objects))) AddUnique(V);
        }

        // their layouts (FNiagaraDataSetCompiledData::BuildLayout, NiagaraDataSet.cpp 1401-1420)
        FPropertyDef LayoutElem = Def("StructProperty", "VariableLayouts", "NiagaraVariableLayoutInfo");
        std::vector<FDefaultValue> Layouts;
        uint32 Floats = 0, Ints = 0, Halves = 0;
        bool bPersistentIds = false;
        if (const FPropertyDef* B = Find(ETags, "bRequiresPersistentIDs")) bPersistentIds = B->Default.I != 0;
        for (const FDefaultValue& V : Vars)
        {
            const FType T = TypeOf(V, TypeName);
            if (Lower(VarName(V)) == "particles.id" && T == FType{ "NiagaraID", 2 }) bPersistentIds = true;
            FLayout L;
            if (!LayoutOf(T, L))
            {
                *Err = Unique + ": particle attribute " + VarName(V) + " is a " + (T.Name.empty() ? std::string("type it cannot name") : T.Name)
                     + ", whose register layout AssetGen does not know";
                return false;
            }
            FDefaultValue Elem;
            Elem.K = FDefaultValue::Struct;
            Elem.Members = std::make_shared<std::vector<FPropertyDef>>(std::vector<FPropertyDef>{
                U32("FloatComponentStart", Floats), U32("Int32ComponentStart", Ints), U32("HalfComponentStart", Halves),
                Struct("LayoutInfo", "NiagaraTypeLayoutInfo", {
                    U32Array("FloatComponentByteOffsets", L.F), U32Array("FloatComponentRegisterOffsets", Count(L.F.size())),
                    U32Array("Int32ComponentByteOffsets", L.I), U32Array("Int32ComponentRegisterOffsets", Count(L.I.size())),
                    U32Array("HalfComponentByteOffsets", L.H), U32Array("HalfComponentRegisterOffsets", Count(L.H.size())) }) });
            Layouts.push_back(Elem);
            Floats += uint32(L.F.size());
            Ints += uint32(L.I.size());
            Halves += uint32(L.H.size());
        }

        FPropertyDef IdName = Def("NameProperty", "Name");
        IdName.Default.K = FDefaultValue::Str;
        IdName.Default.S = StrOf(Find(Handle, "IdName"));
        if (IdName.Default.S.empty()) IdName.Default.S = "None";
        FPropertyDef IdType = Def("EnumProperty", "Type", "ENiagaraDataSetType");
        IdType.Default.K = FDefaultValue::Str;
        IdType.Default.S = "ENiagaraDataSetType::ParticleData";
        FPropertyDef Persistent = Def("BoolProperty", "bRequiresPersistentIDs");
        Persistent.Default.K = FDefaultValue::Bool;
        Persistent.Default.I = bPersistentIds;
        FPropertyDef Sim = Def("EnumProperty", "SimTarget", "ENiagaraSimTarget");
        Sim.Default.K = FDefaultValue::Str;
        Sim.Default.S = bGpu ? "ENiagaraSimTarget::GPUComputeSim" : "ENiagaraSimTarget::CPUSim";

        auto Alias = [&](const std::string& Member, const std::string& Var, FIndex Type) {
            return Variable(Member, ReplaceIgnoreCase(Var, "Emitter.", Unique + "."), Type);
        };
        FPropertyDef SpawnNames = Array("SpawnAttributes", Def("NameProperty", "SpawnAttributes"), std::move(Spawn));
        Out.push_back({
            SpawnNames,
            Alias("EmitterSpawnIntervalVar", "Emitter.SpawnInterval", FloatDef),
            Alias("EmitterInterpSpawnStartDTVar", "Emitter.InterpSpawnStartDt", FloatDef),
            Alias("EmitterSpawnGroupVar", "Emitter.SpawnGroup", IntDef),
            Alias("EmitterAgeVar", "Emitter.Age", FloatDef),
            Alias("EmitterRandomSeedVar", "Emitter.RandomSeed", IntDef),
            Alias("EmitterInstanceSeedVar", "Engine.Emitter.InstanceSeed", IntDef),
            Alias("EmitterTotalSpawnedParticlesVar", "Engine.Emitter.TotalSpawnedParticles", IntDef),
            Struct("DataSetCompiledData", "NiagaraDataSetCompiledData", {
                Array("Variables", Def("StructProperty", "Variables", "NiagaraVariable"), Vars),
                Array("VariableLayouts", LayoutElem, std::move(Layouts)),
                Struct("ID", "NiagaraDataSetID", { IdName, IdType }),
                U32("TotalFloatComponents", Floats), U32("TotalInt32Components", Ints), U32("TotalHalfComponents", Halves),
                Persistent, Sim }) });
    }
    return true;
}
}   // namespace

bool CookNiagara(std::vector<FPropertyDef>& SystemTags, std::vector<FNiagaraObject>& Objects, std::vector<int32>& Kept,
                 const std::function<std::string(FIndex)>& TypeName, const std::function<FIndex(const std::string&)>& NiagaraStruct,
                 std::vector<std::vector<FPropertyDef>>& Compiled, std::string* Err)
{
    if (!CompiledData(SystemTags, Objects, TypeName, NiagaraStruct, Compiled, Err)) return false;

    /* A disabled handle's emitter, and what it holds, is not cooked (UNiagaraEmitter::NeedsLoadForTargetPlatform): the
       handle's Instance loads null, as in DRG's NS_MarkedForDeath. */
    std::vector<bool> Drop(Objects.size(), false);
    if (const FPropertyDef* Handles = Find(&SystemTags, "EmitterHandles"))
        for (const FDefaultValue& Handle : Handles->Default.Items)
        {
            const FPropertyDef* Enabled = Find(Handle, "bIsEnabled");
            const FPropertyDef* Instance = Find(Handle, "Instance");
            if (Enabled && Enabled->Default.I == 0 && Instance && Instance->Default.Instanced > 0
                && size_t(Instance->Default.Instanced) <= Objects.size())
                Drop[size_t(Instance->Default.Instanced - 1)] = true;
        }
    for (size_t I = 0; I < Objects.size(); ++I)
        for (int32 O = Objects[I].Outer; O >= 0 && !Drop[I]; O = Objects[size_t(O)].Outer)
            if (Drop[size_t(O)]) Drop[I] = true;
    std::vector<int32> NewId(Objects.size(), -1);
    std::vector<FNiagaraObject> Out;
    Kept.clear();
    for (size_t I = 0; I < Objects.size(); ++I)
    {
        if (Drop[I]) continue;
        NewId[I] = int32(Out.size());
        Kept.push_back(int32(I));
        Out.push_back(std::move(Objects[I]));
    }
    auto Renumber = [&](FDefaultValue& V) {
        if (V.Instanced <= 0) return;
        const int32 New = NewId[size_t(V.Instanced - 1)];
        V.Instanced = New + 1;
        if (New < 0) V.Object = FIndex();
    };
    ForEachRef(SystemTags, Renumber);
    for (FNiagaraObject& O : Out)
    {
        ForEachRef(O.Tags, Renumber);
        if (O.Outer >= 0) O.Outer = NewId[size_t(O.Outer)];
    }
    Objects = std::move(Out);
    /* A cooked emitter's compiled GPU compute script ends in the shader maps the editor compiles from its HLSL
       (UNiagaraScript::SerializeNiagaraShaderMaps, read whenever CanBeRunOnGpu), which AssetGen does not build. */
    for (const FNiagaraObject& O : Objects)
    {
        if (O.Class != "NiagaraScript" || StrOf(Find(&O.Tags, "Usage")).find("ParticleGPUComputeScript") == std::string::npos) continue;
        const std::string Status = StrOf(Find(Find(&O.Tags, "CachedScriptVM"), "LastCompileStatus"));
        if (Status.empty() || Status.find("NCS_Unknown") != std::string::npos) continue;
        *Err = O.Name + ": a compiled GPU compute script carries the GPU shader maps the editor compiles from its HLSL, which "
               "AssetGen does not build yet; use a CPU emitter";
        return false;
    }
    /* An enabled GPU emitter runs nothing without them, whatever its script holds. */
    for (const FNiagaraObject& O : Objects)
        if (O.Class == "NiagaraEmitter" && StrOf(Find(&O.Tags, "SimTarget")).find("GPUComputeSim") != std::string::npos)
        {
            *Err = StrOf(Find(&O.Tags, "UniqueEmitterName")) + ": a GPU emitter runs on the GPU shader maps the editor compiles "
                   "from its scripts' HLSL, which AssetGen does not build yet; use a CPU emitter";
            return false;
        }
    return true;
}
