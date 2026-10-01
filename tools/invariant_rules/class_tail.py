import sys
from invariants import *
import re

# Work package CLASS: the class tail (ClassFlags, ClassWithin, ClassConfigName, Interfaces, bCooked, the ubergraph it
# names), what a class inherits from its parent, the instanced / config flags that tie properties to their class,
# member names, NumReplicatedProperties and the parameter block's width. Flag values: ObjectMacros.h 180-262 (class),
# 429-503 (CPF_*); Class.h 619 (STRUCT_HasInstancedReference).

SCRIPT_INHERIT = 0x4AA1364E         # CLASS_ScriptInherit, ObjectMacros.h:249-259
OBJECT = '/Script/CoreUObject.Object'
CLASS_Config, CLASS_NotPlaceable, CLASS_Interface = 0x4, 0x200, 0x4000
CLASS_DefaultToInstanced, CLASS_HasInstancedReference = 0x200000, 0x800000
CPF_Net, CPF_Config, CPF_InstancedReference, CPF_RepNotify = 0x20, 0x4000, 0x80000, 0x100000000
CPF_ContainsInstancedReference = 0x8000000000
CPF_Instanced = CPF_InstancedReference | CPF_ContainsInstancedReference
FUNC_UbergraphFunction = 0x8000
OBJECT_PROPS = ('ObjectProperty', 'WeakObjectProperty', 'SoftObjectProperty')


def _i32(tag):
    return struct.unpack_from('<i', tag['value'])[0] if tag and len(tag['value']) >= 4 else 0


def _same(a, b):
    """Two FNames / object paths as the engine compares them: case-insensitively."""
    return (a or '').lower() == (b or '').lower()


# ---- UeApi: the native classes' supers, members and interfaces

def _ueapi_dir():
    here = os.path.dirname(os.path.abspath(sys.modules['invariants'].__file__))
    for d in (UEAPI_DIR,):
        if os.path.exists(os.path.join(d, 'Engine.h')): return os.path.normpath(d)
    return None


_API = None


def _flat(s):
    """s with everything inside <...> dropped: a delegate member's parentheses sit inside its template argument."""
    out, depth = [], 0
    for ch in s:
        if ch == '<': depth += 1
        elif ch == '>': depth -= 1
        elif depth == 0: out.append(ch)
    return ''.join(out)


def ueapi():
    """{'/Script/Engine.Character': {'parent': '/Script/Engine.Pawn', 'props': {engine names, lower}, 'funcs': {...},
    'interface': bool}} for every class UeApi declares (UE_CLASS names its package and engine name, and a member
    Dumper-7 respelled carries its engine name as <Member>__UeName). {} without UeApi."""
    global _API
    if _API is not None: return _API
    _API, folder = {}, _ueapi_dir()
    if not folder: return _API
    decl = re.compile(r'^class (\w+)(?: : public (\w+))?\s*\{\s*public:\s*UE_CLASS\("(/Script/[^"]+)", "([^"]+)"\);(.*?)^\};',
                      re.M | re.S)
    cpp = {}
    for f in sorted(os.listdir(folder)):
        if not f.endswith('.h'): continue
        for m in decl.finditer(open(os.path.join(folder, f), encoding='utf-8-sig', errors='replace').read()):
            name, base, package, ue, body = m.groups()
            props, funcs, renames = set(), set(), {}
            for line in body.split('\n'):
                s = line.strip()
                if not s.endswith(';'): continue
                r = re.match(r'static constexpr const char\* (\w+)__UeName = "([^"]+)";', s)
                if r: renames[r.group(1)] = r.group(2); continue
                if s.startswith(('static constexpr', 'UE_CLASS', 'using ', 'typedef ', 'friend ')): continue
                flat = _flat(s)
                if '(' in flat:
                    f_ = re.search(r'(\w+)\s*\(', flat)
                    if f_: funcs.add(f_.group(1))
                    continue
                v = re.search(r'(\w+)\s*(?:\[[^\]]*\])?\s*(?::\s*\d+)?\s*;$', flat)
                if v: props.add(v.group(1))
            cpp[name] = (base, package + '.' + ue, {renames.get(p, p).lower() for p in props}, {renames.get(x, x) for x in funcs})
    for name, (base, path, props, funcs) in cpp.items():
        funcs = funcs if path != OBJECT else set()                      # UObject's are C++ helpers, not UFunctions
        _API[path] = dict(parent=cpp[base][1] if base in cpp else None, props=props, funcs={x.lower() for x in funcs},
                          interface=name.startswith('I') and base is None)
    return _API


def native_chain(path):
    """path, then each native super UeApi names."""
    seen = 0
    while path and seen < 64:
        yield path
        path = ueapi().get(path, {}).get('parent'); seen += 1


def ancestors(pkg, idx):
    """The class an FPackageIndex names and its supers: (package, export index, None) for each Blueprint class the
    resolver finds (Package.resolve: this Content folder, then GAME_CONTENT), then (None, None, path) for the first
    native class and each native super UeApi names. Stops early at a /Game class it cannot find."""
    for _ in range(64):
        if not idx: return
        r = pkg.resolve(idx)
        if r:
            P, k = r
            st = P.struct(k)
            if st is None or not hasattr(st, 'class_flags'): return
            yield P, k, None
            pkg, idx = P, st.super
            continue
        path = pkg.path(idx)
        if path.startswith('/Script/'):
            for p in native_chain(path): yield None, None, p
        return


# ---- what a native parent passes on

def _rows(groups):
    return {path: (flags, within or OBJECT, config) for (flags, within, config), paths in groups.items() for path in paths}


# Measured on EVERY package of the game's content (51,579 packages, 4570 classes, 559 native parents): for each /Script
# class some game Blueprint derives from, the ScriptInherit bits one of its children carries that the child cannot have
# made itself (own instanced / config properties, its interfaces' EditInlineNew|CollapseCategories, bDeprecate / a
# const Blueprint, a macro library's NotPlaceable) and every sibling carries too - so bits of the parent's own
# ClassFlags - and the one ClassWithin and ClassConfigName all its children share (KismetCompiler.cpp:320-321, 2453).
# Key: (required bits, ClassWithin or None for UObject, ClassConfigName).
NATIVE_TAILS = _rows({
    (0x00000004, None, 'Engine'): (
        '/Script/Engine.SkeletalMeshActor', '/Script/FSD.AdicPuddle', '/Script/FSD.AmberEvent',
        '/Script/FSD.AmberExcavation', '/Script/FSD.AmmoDrivenWeapon', '/Script/FSD.ArmorPiece',
        '/Script/FSD.AssaultRifle', '/Script/FSD.AutoCannon', '/Script/FSD.AutoShotgun', '/Script/FSD.BarrelSpawner',
        '/Script/FSD.BasicDepositableItem', '/Script/FSD.BasicPistol', '/Script/FSD.BhaBarnacle', '/Script/FSD.Boil',
        '/Script/FSD.BoltActionWeapon', '/Script/FSD.BouncyBoomerang', '/Script/FSD.BurstWeapon',
        '/Script/FSD.CarriableItem', '/Script/FSD.CaveVine', '/Script/FSD.CharacterSelectionSwitcher',
        '/Script/FSD.ChargedProjectile', '/Script/FSD.ChargedWeapon', '/Script/FSD.CleanupPod',
        '/Script/FSD.CleanupPodItem', '/Script/FSD.CoilGun', '/Script/FSD.CoilGunTrailSegment',
        '/Script/FSD.CoreCorruptionPillar', '/Script/FSD.CoreCorruptionRift', '/Script/FSD.CoreRift',
        '/Script/FSD.Crossbow', '/Script/FSD.CrossbowProjectileBase', '/Script/FSD.CrossbowProjectileStuck',
        '/Script/FSD.CryosprayItem', '/Script/FSD.DamageEnhancer', '/Script/FSD.DebrisDataActor',
        '/Script/FSD.DeepScanHiddenCrystal', '/Script/FSD.DefensePointActor', '/Script/FSD.DetPack',
        '/Script/FSD.DisplayCase', '/Script/FSD.DorrettaHead', '/Script/FSD.DoubleDrillItem',
        '/Script/FSD.Drillevator', '/Script/FSD.DrillevatorEngine', '/Script/FSD.DrinkableActor',
        '/Script/FSD.DrinkableItem', '/Script/FSD.DroneVacuumStream', '/Script/FSD.DropPod',
        '/Script/FSD.DroppableOutpost', '/Script/FSD.DualMachinePistols', '/Script/FSD.ElectricalSMG',
        '/Script/FSD.ElevatorPlant', '/Script/FSD.EnemyShowroomItem', '/Script/FSD.EscortDestination',
        '/Script/FSD.EscortMuleTrack', '/Script/FSD.EventRewardDispenser', '/Script/FSD.EventRewardFrame',
        '/Script/FSD.EventSpawnTimer', '/Script/FSD.EventStarterButton', '/Script/FSD.ExpeniteSamplePod',
        '/Script/FSD.ExplosiveBarrelEvent', '/Script/FSD.ExtractorItem', '/Script/FSD.FSDMiningHead',
        '/Script/FSD.FSDPhysicsActor', '/Script/FSD.FSDPlayerStart', '/Script/FSD.FSDPostProcessingActor',
        '/Script/FSD.FSDRefinery', '/Script/FSD.FacilityGeneratorLine', '/Script/FSD.FacilityHackingPod',
        '/Script/FSD.FacilityPowerStation', '/Script/FSD.FlameThrowerItem', '/Script/FSD.FlameWallProjectile',
        '/Script/FSD.FlameWallSegment', '/Script/FSD.Flare', '/Script/FSD.FlareGun',
        '/Script/FSD.FlareGunProjectile', '/Script/FSD.FoamPuddle', '/Script/FSD.FoamPuddle_WalkingPlagueheart',
        '/Script/FSD.FuelLineBuilderItem', '/Script/FSD.FuelLineEndPoint', '/Script/FSD.FuelLineSegment',
        '/Script/FSD.FuelLineStart', '/Script/FSD.GameEvent', '/Script/FSD.GasCloud', '/Script/FSD.GatlingGun',
        '/Script/FSD.Gem', '/Script/FSD.GlowPlant', '/Script/FSD.GooGun', '/Script/FSD.GooGunProjectile',
        '/Script/FSD.GooGunPuddle', '/Script/FSD.GrapplingHookGun', '/Script/FSD.Grenade',
        '/Script/FSD.GuntowerActivationPlatform', '/Script/FSD.GuntowerEvent', '/Script/FSD.GuntowerLineProjectile',
        '/Script/FSD.HackingToolItem', '/Script/FSD.HangingFireCracker', '/Script/FSD.HeartstoneTrap',
        '/Script/FSD.HeavyParticleCannon', '/Script/FSD.HeliumTank', '/Script/FSD.HomingDroneBomb',
        '/Script/FSD.HoopsGame', '/Script/FSD.HydraWeedSpawnProjectile', '/Script/FSD.IconGenerationCharacter',
        '/Script/FSD.IconGenerationPickaxe', '/Script/FSD.IconGenerationWeapon', '/Script/FSD.ImpactIndicator',
        '/Script/FSD.ItemMarker', '/Script/FSD.ItemPreviewActor', '/Script/FSD.JetBootsBox',
        '/Script/FSD.JetBootsBoxSpawner', '/Script/FSD.JetPackItem', '/Script/FSD.JettyBootsArcadeActor',
        '/Script/FSD.LaserPointerItem', '/Script/FSD.LaserPointerMarker', '/Script/FSD.LaserPointerWaypoint',
        '/Script/FSD.LineCutter', '/Script/FSD.LineCutterProjectile', '/Script/FSD.LoadoutItemProxy',
        '/Script/FSD.LockOnBeam', '/Script/FSD.LockOnWeapon', '/Script/FSD.Magazine',
        '/Script/FSD.MeteorDefenseEvent', '/Script/FSD.MicroMissileLauncher', '/Script/FSD.MicrowaveWeapon',
        '/Script/FSD.MorkiteSeedNut', '/Script/FSD.NeedleSprayer', '/Script/FSD.PatrolBotDecorative',
        '/Script/FSD.PhysicsBarrel', '/Script/FSD.PickaxeItem', '/Script/FSD.PipelineBuilderItem',
        '/Script/FSD.PipelineExtractorPod', '/Script/FSD.PipelineFinish', '/Script/FSD.PipelineSegment',
        '/Script/FSD.PipelineStart', '/Script/FSD.PlagueControlActor', '/Script/FSD.PlagueInfectionNode',
        '/Script/FSD.PlagueMeteor', '/Script/FSD.PlagueMeteorSpawner', '/Script/FSD.PlaguePuddle',
        '/Script/FSD.PlagueSoaperItem', '/Script/FSD.PlasmaBoomerang', '/Script/FSD.PlasmaCarbine',
        '/Script/FSD.PlatformProjectile', '/Script/FSD.ProceduralSetup', '/Script/FSD.PropHuntDisguiseActor',
        '/Script/FSD.PropHuntHunterItem', '/Script/FSD.RedeployableSentryGun', '/Script/FSD.RefineryExtractorPod',
        '/Script/FSD.ResonanceScannerPod', '/Script/FSD.ResourceChunk', '/Script/FSD.ResourcePouch',
        '/Script/FSD.RessuplyPod', '/Script/FSD.RessuplyPodItem', '/Script/FSD.RessuplyPodSpawn',
        '/Script/FSD.Revoler', '/Script/FSD.RivalBomb', '/Script/FSD.RivalBombNode', '/Script/FSD.RockCrackerDrill',
        '/Script/FSD.RockCrackerPod', '/Script/FSD.RockEnemiesEvent', '/Script/FSD.RocketAttachment',
        '/Script/FSD.SalvageFuelPod', '/Script/FSD.SawedOffShotgun', '/Script/FSD.ScrabTank',
        '/Script/FSD.SentryGun', '/Script/FSD.ShieldGeneratorActor', '/Script/FSD.ShieldGeneratorItem',
        '/Script/FSD.ShowroomStage', '/Script/FSD.SoapVacuumItem', '/Script/FSD.SpaceBall',
        '/Script/FSD.SpaceRigBar', '/Script/FSD.SpawnTrigger', '/Script/FSD.SplineCableActor',
        '/Script/FSD.SplinePlant', '/Script/FSD.StickyFlame', '/Script/FSD.StickyIce', '/Script/FSD.TaggedActor',
        '/Script/FSD.TargetDummyPawn', '/Script/FSD.TerrainScannerItem', '/Script/FSD.Tether',
        '/Script/FSD.TetherStation', '/Script/FSD.ThrowableActor', '/Script/FSD.ThrownGrenadeItem',
        '/Script/FSD.TreasureBox', '/Script/FSD.TreasureContainer', '/Script/FSD.TunnelEventBase',
        '/Script/FSD.TutorialManager', '/Script/FSD.WallSaw', '/Script/FSD.WormPod', '/Script/FSD.ZipLineConnector',
        '/Script/FSD.ZipLineItem', '/Script/FSD.ZipLineProjectile', '/Script/FSDEngine.CSGBuilder',),
    (0x00a00004, None, 'Engine'): (
        '/Script/Engine.ActorComponent', '/Script/Engine.SceneComponent', '/Script/FSD.AttachedStateComponent',
        '/Script/FSD.AttackBaseComponent', '/Script/FSD.BPInstantUsable', '/Script/FSD.BatchObjective',
        '/Script/FSD.BurrowComponent', '/Script/FSD.CaretakerActionComponent', '/Script/FSD.CaveScriptComponent',
        '/Script/FSD.CaveScriptExtraStationaryEnemies', '/Script/FSD.CharacterIntoxicationComponent',
        '/Script/FSD.CharacterShowroomController', '/Script/FSD.CoreCorruptionObjective',
        '/Script/FSD.CrossbowProjectileMagnetic', '/Script/FSD.CrossbowProjectileRecallable',
        '/Script/FSD.CrossbowProjectileRicochet', '/Script/FSD.CrossbowStuckProjectileEffectBanshee',
        '/Script/FSD.DamageAttackComponent', '/Script/FSD.DebrisItemComponent', '/Script/FSD.DeepScanObjective',
        '/Script/FSD.DeepScanSecondaryObjective', '/Script/FSD.DefenseObjective', '/Script/FSD.DownedStateComponent',
        '/Script/FSD.DrinkEffectComponent', '/Script/FSD.DynamicReverbComponent', '/Script/FSD.EliminationObjective',
        '/Script/FSD.EnemyAttackerPositioningComponent', '/Script/FSD.EnemyControlStateComponent',
        '/Script/FSD.EnemyShowroomController', '/Script/FSD.EscortObjective',
        '/Script/FSD.EventRewarderUsableComponent', '/Script/FSD.EyeForEyePerkComponent',
        '/Script/FSD.FacilityObjective', '/Script/FSD.FallingStateComponent', '/Script/FSD.FloatPerkComponent',
        '/Script/FSD.FlyingStateComponent', '/Script/FSD.FrozenStateComponent', '/Script/FSD.GatheItemsObjective',
        '/Script/FSD.GatherGemsObjective', '/Script/FSD.GrabbedStateComponent',
        '/Script/FSD.HeightenedSenseComponent', '/Script/FSD.ImpactAudioComponent',
        '/Script/FSD.InfectedStateComponent', '/Script/FSD.ItemPlacerAggregator', '/Script/FSD.KillEnemiesObjective',
        '/Script/FSD.KillNearbyCreaturePerkComponent', '/Script/FSD.MOD_MakeEliteEnemy',
        '/Script/FSD.NoMovementStateComponent', '/Script/FSD.OxygenComponent', '/Script/FSD.ParalyzedStateComponent',
        '/Script/FSD.PassedOutStateComponent', '/Script/FSD.PerkLogic', '/Script/FSD.PhotographyStateComponent',
        '/Script/FSD.PilotingStateComponent', '/Script/FSD.PlagueObjective', '/Script/FSD.ProceduralController',
        '/Script/FSD.ProjectileAttackBaseComponent', '/Script/FSD.ProjectileAttackComponent',
        '/Script/FSD.ProjectileLauncherComponent', '/Script/FSD.PropHuntDrinkEffect', '/Script/FSD.PushingState',
        '/Script/FSD.RefineryObjective', '/Script/FSD.RefinerySecondaryObjective', '/Script/FSD.RepairObjective',
        '/Script/FSD.ResourceObjective', '/Script/FSD.SalvageObjective', '/Script/FSD.ShieldLinkComponent',
        '/Script/FSD.SkinTreasureRewarder', '/Script/FSD.SpecialDamageAttackComponent',
        '/Script/FSD.TerrainPlacementComponent', '/Script/FSD.ThornsPerkComponent',
        '/Script/FSD.TrackMovementStateComponent', '/Script/FSD.TutorialHintComponent',
        '/Script/FSD.UsingStateComponent', '/Script/FSD.WalkingStateComponent',
        '/Script/FSD.WeaponHitCounterComponent', '/Script/FSD.WeaponHitEffectComponent',
        '/Script/FSD.ZipLineStateComponent',),
    (0x00800008, '/Script/Engine.SkeletalMeshComponent', 'Engine'): (
        '/Script/AnimationSharing.AnimSharingStateInstance', '/Script/Engine.AnimInstance',
        '/Script/FSD.AmmoDrivenWeaponAnimInstance', '/Script/FSD.AnchorTurnerAnimInstance',
        '/Script/FSD.BeltDrivenAnimInstance', '/Script/FSD.BomberAnimInstance', '/Script/FSD.BoscoAnimInstance',
        '/Script/FSD.CaretagerBodyAnimInstance', '/Script/FSD.CaretakerEyeAnimInstance',
        '/Script/FSD.CaveLeechAnimInstance', '/Script/FSD.ChargedWeaponAnimInstance',
        '/Script/FSD.CleanupToolAnimInstance', '/Script/FSD.CoilGunAnimInstance',
        '/Script/FSD.ConvertedRobotAnimInstance', '/Script/FSD.ConvertedRobotTurretAnimInstance',
        '/Script/FSD.CoreInfuserAnimInstance', '/Script/FSD.CoreInfuserPlatformAnimInstance',
        '/Script/FSD.CrawlerAnimInstance', '/Script/FSD.CryoSprayAnimInstance',
        '/Script/FSD.DisplayCaseAnimInstance', '/Script/FSD.DoubleDrillAnimInstance',
        '/Script/FSD.DrillAnimInstance', '/Script/FSD.EasterBunnyAnimInstance', '/Script/FSD.EnemyAnimInstance',
        '/Script/FSD.EscortMuleAnimInstance', '/Script/FSD.EventStarterAnimInstance',
        '/Script/FSD.FSDRefineryAnimInstance', '/Script/FSD.FacilityHackingPodAnimInstance',
        '/Script/FSD.FacilityTentacleHeadAnimInstance', '/Script/FSD.FacilityTetherDispenserAnimInstnace',
        '/Script/FSD.FacilityTurretAnimInstance', '/Script/FSD.FlyingBugAnimInstance',
        '/Script/FSD.FlyingLifterAnimInstance', '/Script/FSD.FriendlyParasiteAnimInstance',
        '/Script/FSD.FuelCannisterItemAnimInstance', '/Script/FSD.GameEventAnimInstance',
        '/Script/FSD.GliderBeastAnimInstance', '/Script/FSD.GooGunAnimInstance',
        '/Script/FSD.GunTowerModuleAnimInstance', '/Script/FSD.HalloweenSkullAnimInstance',
        '/Script/FSD.ItemDispenserAnimInstance', '/Script/FSD.JellyBreederAnimInstance',
        '/Script/FSD.JellyFishAnimInstance', '/Script/FSD.MicroMissileLauncherAnimInstance',
        '/Script/FSD.MiningPodAnimInstance', '/Script/FSD.NisseAnimInstance', '/Script/FSD.PatrolBotAnimInstance',
        '/Script/FSD.PipelineExtractorPodAnimInstance', '/Script/FSD.PipelineSegmentEndPostAnimInstance',
        '/Script/FSD.PlatformGunAnimInstance', '/Script/FSD.PlayerFPAnimInstance',
        '/Script/FSD.PlayerTPAnimInstance', '/Script/FSD.ProspectorAnimInstance',
        '/Script/FSD.RandomFireModuleAnimInstance', '/Script/FSD.RefineryExtractorPodAnimInstance',
        '/Script/FSD.RivalBombNodeAniminstance', '/Script/FSD.RockCrackedAnimInstance',
        '/Script/FSD.ScrabAnimInstance', '/Script/FSD.SentryGunAnimInstance', '/Script/FSD.SharkAnimInstance',
        '/Script/FSD.ShootingPlantAnimInstance', '/Script/FSD.ShootingSpiderAnimInstance',
        '/Script/FSD.ShredderAnimInstance', '/Script/FSD.SimpleMovingEnemyAnimInstance',
        '/Script/FSD.SpiderAnimInstance', '/Script/FSD.SpiderLobberAnimInstance', '/Script/FSD.TentacleAnimInstance',
        '/Script/FSD.TerminatorAnimInstance', '/Script/FSD.TerminatorTentacleHeadAnimInstance',
        '/Script/FSD.TestAnimInstance', '/Script/FSD.TetherAnimInstance', '/Script/FSD.TreasureBoxAnimInstance',
        '/Script/FSD.VacuumAnimInstance', '/Script/FSD.VanityAnimInstance', '/Script/FSD.WoodLouseAnimInstance',),
    (0x00201000, None, 'Engine'): (
        '/Script/FSD.ActorContextWidget', '/Script/FSD.ActorTrackingWidget', '/Script/FSD.AmmoCountWidget',
        '/Script/FSD.BarMenuWidget', '/Script/FSD.BossFightWidget', '/Script/FSD.ClaimableRewardEntryWidget',
        '/Script/FSD.ClaimableRewardViewWidget', '/Script/FSD.CoolDownProgressWidget', '/Script/FSD.CrosshairWidget',
        '/Script/FSD.CustomAmmoCountWidget', '/Script/FSD.DealWidget', '/Script/FSD.EscapeMenuWindow',
        '/Script/FSD.FSDAdvancedLabel', '/Script/FSD.FSDEventPopupWidget',
        '/Script/FSD.FSDLevelLoadingPersistentWidget', '/Script/FSD.FSDMainHUDWidget',
        '/Script/FSD.HUDWarningWidget', '/Script/FSD.HackingToolWidget', '/Script/FSD.HoopScoreWidget',
        '/Script/FSD.InputCaptureWidget', '/Script/FSD.ItemsBar', '/Script/FSD.ItemsBarIcon',
        '/Script/FSD.JetBootsFuelWidget', '/Script/FSD.JettyBootsArcadeWidget', '/Script/FSD.LockOnTrackingWidget',
        '/Script/FSD.LookingAtContentWidget', '/Script/FSD.LookingAtWidget', '/Script/FSD.MasteryIconWidget',
        '/Script/FSD.MediaPlayerWidget', '/Script/FSD.MissionPlayerAndCharacterWidget',
        '/Script/FSD.MouseCursorWidget', '/Script/FSD.ObjectiveWidget', '/Script/FSD.OptionalObjectiveWidget',
        '/Script/FSD.PerkHUDActivationWidget', '/Script/FSD.PerkHUDIconWidget',
        '/Script/FSD.PlayerAfflictionOverlayWidget', '/Script/FSD.PropHuntEndScreenWidget',
        '/Script/FSD.PropHuntOverlayWidget', '/Script/FSD.RefineryExtractorPodWidget', '/Script/FSD.RewardWidget',
        '/Script/FSD.RichTextInputWidget', '/Script/FSD.SeasonInfinityLevelWidget', '/Script/FSD.SeasonLevelWidget',
        '/Script/FSD.ShoutWidget', '/Script/FSD.SoundClassWidget', '/Script/FSD.SpaceRigBarMenuItem',
        '/Script/FSD.SpriteRectWidget', '/Script/FSD.TreeOfVanityItemWidget', '/Script/FSD.TutorialContentWidget',
        '/Script/FSD.TutorialLevelWidget', '/Script/FSD.TutorialWidget', '/Script/FSD.UIHoopHistory',
        '/Script/FSD.WeaponMaintenanceItemWidget', '/Script/FSD.WeaponMaintenanceTabWidget',
        '/Script/FSD.YesNoPromptWidget',),
    (0x00000004, None, 'Game'): (
        '/Script/Engine.Pawn', '/Script/FSD.AFlyingBug', '/Script/FSD.AimingFacilityTurret', '/Script/FSD.Bomber',
        '/Script/FSD.Bosco', '/Script/FSD.Caretaker', '/Script/FSD.CaveLeech', '/Script/FSD.CaveWorm',
        '/Script/FSD.ConvertedRobot', '/Script/FSD.CoreCorruptionCrystal', '/Script/FSD.DeepPathfinderCharacter',
        '/Script/FSD.EnemyDeepPathfinderCharacter', '/Script/FSD.EnemyPawn', '/Script/FSD.EscortMule',
        '/Script/FSD.FSDPawn', '/Script/FSD.FacilityTentacle', '/Script/FSD.FlyingEnemyDeepPathfinderCharacter',
        '/Script/FSD.FlyingLifter', '/Script/FSD.GuntowerWeakPoint', '/Script/FSD.HalloweenSkull',
        '/Script/FSD.HomingFireModule', '/Script/FSD.HydraWeedCore', '/Script/FSD.HydraWeedHealer',
        '/Script/FSD.HydraWeedShooter', '/Script/FSD.InsectSwarmEnemy', '/Script/FSD.InsectSwarmSpawner',
        '/Script/FSD.JellyBreeder', '/Script/FSD.LMGGuntoweModule', '/Script/FSD.Maggot', '/Script/FSD.MiniMule',
        '/Script/FSD.Molly', '/Script/FSD.ParasiteEnemy', '/Script/FSD.PatrolBot', '/Script/FSD.PitJaw',
        '/Script/FSD.PlayerCameraDrone', '/Script/FSD.PlayerCharacter', '/Script/FSD.ProspectorRobot',
        '/Script/FSD.RadialFireModule', '/Script/FSD.RandomFiringGuntowerModule', '/Script/FSD.RecallableSentryGun',
        '/Script/FSD.RiftCrystal', '/Script/FSD.Scrab', '/Script/FSD.SharkEnemy', '/Script/FSD.ShootingPlant',
        '/Script/FSD.Shredder', '/Script/FSD.SpiderEnemy', '/Script/FSD.SpinningFacilityturret',
        '/Script/FSD.StabberVine', '/Script/FSD.TentaclePlant', '/Script/FSD.TentaclePlantNode',
        '/Script/FSD.TerminatorEnemy', '/Script/FSD.TerminatorTentacle', '/Script/FSD.WoodLouse',),
    (0x00000000, None, 'Engine'): (
        '/Script/CoreUObject.Interface', '/Script/CoreUObject.Object', '/Script/Engine.BlueprintFunctionLibrary',
        '/Script/FSD.AfflictionEffect', '/Script/FSD.AttachMeshesAfflictionEffect',
        '/Script/FSD.AttachedParticlesAfflictionEffect', '/Script/FSD.BloodSugarBP',
        '/Script/FSD.BoneParticlesAfflictionEffect', '/Script/FSD.BurningAfflictionEffect',
        '/Script/FSD.CameraParticleAfflictionEffect', '/Script/FSD.EnemyScaleAfflictionEffect',
        '/Script/FSD.EnemyWaveController', '/Script/FSD.EscortMissionDNA', '/Script/FSD.ExterminationReward',
        '/Script/FSD.GameData', '/Script/FSD.HeroEnemyAfflictionEffect', '/Script/FSD.IconGenerationManager',
        '/Script/FSD.MissionDNA', '/Script/FSD.PawnBurningUniqueAfflictionEffect',
        '/Script/FSD.PlayerCharacterMontageAfflictionEffect', '/Script/FSD.RunningMissionBP',
        '/Script/FSD.ScalingMeshAfflictionEffect', '/Script/FSD.ShieldLinkedAfflictionEffect',
        '/Script/FSD.SoundAfflictionEffect', '/Script/FSD.SpawnAtLocationParticleAfflictionEffect',
        '/Script/FSD.StaggeredAfflictionEffect', '/Script/FSD.StatusEffectEnemies',
        '/Script/FSD.StatusEffectMissionBP', '/Script/FSD.StatusEffectReactiveTerrain',
        '/Script/FSD.SubsystemLoader', '/Script/FSD.TracerManager', '/Script/FSD.TreasureBeacon',
        '/Script/UMG.RichTextBlockImageDecorator',),
    (0x00800004, None, 'Engine'): (
        '/Script/Engine.Actor', '/Script/Engine.StaticMeshActor', '/Script/FSD.CoilgunWeaponTrail',
        '/Script/FSD.CrossbowElectroBeam', '/Script/FSD.DeepCSGWorld', '/Script/FSD.DetPackItem',
        '/Script/FSD.ElectroBeam', '/Script/FSD.FSDReverbVolume', '/Script/FSD.HolidayThrowableItem',
        '/Script/FSD.ItemDispenser', '/Script/FSD.PickaxePreviewActor', '/Script/FSD.Projectile',
        '/Script/FSD.RecallableSentryGunItem', '/Script/FSD.SentryElectroBeam',
        '/Script/FSD.SpawnActorGenerationItem', '/Script/FSD.TunnelEventEnemySpawner', '/Script/FSD.VanityCharacter',),
    (0x00800204, None, 'Engine'): (
        '/Script/FSD.BoscoController', '/Script/FSD.ConvertedRobotController', '/Script/FSD.EnemyAIController',
        '/Script/FSD.EscortMuleAIController', '/Script/FSD.FSDAIController', '/Script/FSD.FSDFlyingBugController',
        '/Script/FSD.FSDGroundToAirEnemyController', '/Script/FSD.FacilityTurretController',
        '/Script/FSD.HostileGuntowerModuleController',),
    (0x00200004, None, 'Engine'): (
        '/Script/FSD.BeastMasterComponent', '/Script/FSD.DeadStateComponent', '/Script/FSD.DeepScanPlayerComponent',
        '/Script/FSD.JetBootsMovementComponent', '/Script/FSD.PetComponent', '/Script/FSD.SpecialAttackComponent',
        '/Script/FSD.TentacleGrabAttack', '/Script/FSD.TrackBuilderMovement',),
    (0x0000020c, None, 'Game'): (
        '/Script/Engine.GameMode', '/Script/Engine.GameModeBase', '/Script/FSD.FSDGameMode',
        '/Script/FSD.FSDGameModeSpaceRig', '/Script/FSD.FSDHUD', '/Script/FSD.SpaceRigHUD',),
    (0x00a01000, None, 'Engine'): (
        '/Script/FSD.AngleIndicatorWidget', '/Script/FSD.FSDInWorldWidget', '/Script/FSD.FSDUserWidget',
        '/Script/FSD.LoreScreenMasterWidget', '/Script/FSD.WindowWidget', '/Script/UMG.UserWidget',),
    (0x00a01004, None, 'Engine'): (
        '/Script/Engine.StaticMeshComponent', '/Script/FSD.BeastMasterUseSphere', '/Script/FSD.DamageComponent',
        '/Script/FSD.FSDSkeletalMeshComponent',),
    (0x00000000, None, 'Game'): (
        '/Script/AIModule.BTDecorator_BlueprintBase', '/Script/AIModule.BTService_BlueprintBase',
        '/Script/AIModule.BTTask_BlueprintBase',),
    (0x00000204, None, 'Game'): (
        '/Script/FSD.FSDGameState', '/Script/FSD.FSDPlayerController', '/Script/FSD.FSDPlayerControllerBase',),
    (0x00800000, None, 'Engine'): (
        '/Script/FSD.Campaign', '/Script/FSD.CampaignManager', '/Script/FSD.RichTextInputDecorator',),
    (0x00001000, None, 'Engine'): (
        '/Script/FSD.FloatPerkActivation', '/Script/FSD.SetCooldownPerkActivation',),
    (0x00800004, None, 'Game'): (
        '/Script/FSD.CoreSpawnEnemyBase', '/Script/FSD.FriendlyParasite',),
    (0x00800204, None, 'Game'): (
        '/Script/Engine.PlayerController', '/Script/Engine.SpectatorPawn',),
    (0x00801000, None, 'Engine'): (
        '/Script/FSD.StatusEffect', '/Script/GameplayCameras.MatineeCameraShake',),
    (0x00000008, '/Script/Engine.SkeletalMeshComponent', 'Engine'): (
        '/Script/AnimationSharing.AnimSharingTransitionInstance',),
    (0x00000008, None, 'Game'): (
        '/Script/FSD.FSDGameInstance',),
    (0x00000204, None, 'Engine'): (
        '/Script/FSD.FSDPlayerState',),
    (0x0000020c, None, 'Engine'): (
        '/Script/FSD.FSDPlayerCameraManager',),
    (0x00800000, '/Script/Engine.GameInstance', 'Engine'): (
        '/Script/FSD.FadeScreenSubSystem',),
})

# From the UCLASS specifiers in UE 4.27's headers, for native parents a mod can name that no game Blueprint derives from
# (and a few that one does, cross-checked against NATIVE_TAILS by the calibration): (bits, Within, ConfigName). A native
# class's config name and Within are its nearest declaring ancestor's (Object.h:57-60 StaticConfigName Engine;
# HeaderParser.cpp:10374-10380; ClassDeclarationMetaData.cpp:520-526); CLASS_Config comes from config members
# (HeaderParser.cpp:6298-6304).
SOURCE_TAILS = {
    '/Script/Engine.Actor': (0, OBJECT, 'Engine'),                                           # Actor.h:131
    '/Script/Engine.ActorComponent': (CLASS_DefaultToInstanced, OBJECT, 'Engine'),           # ActorComponent.h:115
    '/Script/Engine.Pawn': (0, OBJECT, 'Game'),                                              # Pawn.h:36
    '/Script/Engine.Character': (0, OBJECT, 'Game'),                                         # Character.h:213
    '/Script/Engine.PlayerController': (CLASS_NotPlaceable, OBJECT, 'Game'),                 # PlayerController.h:222, Controller.h:39
    '/Script/Engine.GameModeBase': (0x208, OBJECT, 'Game'),                                  # GameModeBase.h:45 notplaceable, Transient
    '/Script/Engine.HUD': (0x20c, OBJECT, 'Game'),                                           # HUD.h:35; globalconfig members HUD.h:98,102
    '/Script/Engine.CheatManager': (0, '/Script/Engine.PlayerController', 'Engine'),        # CheatManager.h:87 Within=PlayerController
    '/Script/Engine.PlayerInput': (0xc, '/Script/Engine.PlayerController', 'Input'),        # PlayerInput.h:333; config members :35ff
    '/Script/Engine.GameUserSettings': (0x40000004, OBJECT, 'GameUserSettings'),            # GameUserSettings.h:37; config members :349ff
    '/Script/Engine.DamageType': (0x10000, OBJECT, 'Engine'),                                # DamageType.h:19 const
    '/Script/Engine.AnimInstance': (0x8, '/Script/Engine.SkeletalMeshComponent', 'Engine'), # AnimInstance.h:361 transient
    '/Script/Engine.GameInstance': (0x8, OBJECT, 'Game'),                                    # GameInstance.h:149 config=Game, transient
    '/Script/AIModule.BTNode': (0, OBJECT, 'Game'),                                          # BTNode.h:36 config=Game
    '/Script/UMG.Visual': (CLASS_DefaultToInstanced, OBJECT, 'Engine'),                      # Visual.h:11
    '/Script/UMG.UserWidget': (0x1000, OBJECT, 'Engine'),                                    # UserWidget.h:204 editinlinenew
}


# The ScriptInherit bits a native class can drop although its super has them: `placeable`, `NonTransient`,
# `NotEditInlineNew` and `DontCollapseCategories` clear NotPlaceable, Transient, EditInlineNew and CollapseCategories
# (ClassDeclarationMetaData.cpp:81-90, 157-160, 287-290, 400-407).
UHT_CLEARABLE = CLASS_NotPlaceable | 0x8 | 0x1000 | 0x2000


def native_tail(path):
    """(bits the class must pass on, its ClassWithin or None if unknown, its ClassConfigName or None if unknown) for a
    native class: its measured row and / or its UCLASS specifiers, plus the bits its native supers carry that no
    specifier of an in-between class can take away - UHT gives a class its super's ScriptInherit bits
    (HeaderParser.cpp:10374) and only the four specifiers of UHT_CLEARABLE drop one."""
    bits, within, config = 0, None, None
    for k, p in enumerate(native_chain(path)):
        row, src = NATIVE_TAILS.get(p), SOURCE_TAILS.get(p)
        mine = (row[0] if row else 0) | (src[0] if src else 0)
        bits |= mine if k == 0 else mine & ~UHT_CLEARABLE
        if k == 0:
            within = (row or src or (0, None, None))[1]
            config = (row or src or (0, None, None))[2]
    return bits, within, config


def parent_tail(pkg, idx):
    """(bits, Within, ConfigName, where from) of the parent class an FPackageIndex names: a Blueprint parent read from its
    own package, exactly; a native one from native_tail. None when nothing is known of it."""
    r = pkg.resolve(idx)
    if r:
        P, k = r
        st = P.struct(k)
        if st is None or not hasattr(st, 'class_flags'): return None
        return st.class_flags & SCRIPT_INHERIT, P.path(st.within) if st.within else OBJECT, st.config, 'its package'
    path = pkg.path(idx)
    if not path or not path.startswith('/Script/'): return None
    bits, within, config = native_tail(path)
    if not bits and within is None and config is None: return None
    return bits, within, config, 'native'


@rule
def class_tail_follows_parent(pkg):
    """A Blueprint class carries its parent's ScriptInherit ClassFlags, its parent's ClassWithin (UObject when the
    parent has none) and its parent's ClassConfigName, and the ScriptInherit ClassFlags of each interface it
    implements. This is what the editor's compiler always writes - it copies them (KismetCompiler.cpp:320-321 in
    CleanAndSanitizeClass, 2450-2453 in FinishCompilingClass, 2430 for interfaces; ScriptInherit =
    ObjectMacros.h:249-259) - and the game keeps it; nothing at load re-derives them (Class.cpp:4416-4450 reads them as
    saved), so what the cook writes is what runs. What that guards at run time: the Config family (a missing Config bit
    makes LoadConfig return at once, Obj.cpp:2077-2080; PerObjectConfig, ConfigDoNotCheckDefaults, Obj.cpp:332, 2555),
    the ConfigName (which ini LoadConfig reads), Transient (instances made RF_Transient, UObjectGlobals.cpp:2526-2531),
    HasInstancedReference (subobject instancing, UObjectGlobals.cpp:2875), DefaultToInstanced (CoreNative.cpp:271),
    Deprecated (CreateWidget refuses the class, UserWidget.cpp:2030); and ClassWithin - a narrower one is a Fatal
    wherever the object is created (StaticAllocateObject, UObjectGlobals.cpp:2309-2311), a wider one lets a child be
    created where native code's GetOuter<Within>() casts its outer unchecked (DECLARE_WITHIN, ObjectMacros.h:1703-1706).
    EditInlineNew is read only by a property-access permission check (PropertyAccessUtil.cpp:447), and NotPlaceable,
    CollapseCategories, Const and the Default/GlobalUser/ProjectUser config bits by nothing at run time (Const only
    shapes exported C++ text, Property.cpp:718-728): for them this is the editor's habit, which the game keeps.
    ClassWithin is never null (check, Class.cpp:4066), and a CLASS_Config class never has ConfigName None (Fatal in
    GetConfigName, Class.cpp:5385-5388). A parent or interface the resolver cannot find, or a native parent neither the
    game's Blueprints nor a UCLASS in NATIVE_TAILS / SOURCE_TAILS describes, is skipped (a native interface's flags
    are not known here)."""
    for i, st in classes(pkg):
        if not st.within: yield i, 'ClassWithin is null'
        if st.class_flags & CLASS_Config and st.config == 'None':
            yield i, 'ClassFlags %#x has CLASS_Config, ClassConfigName is None' % st.class_flags
        for idx, _, _ in st.interfaces:
            r = pkg.resolve(idx) if idx else None
            ist = r[0].struct(r[1]) if r else None
            if ist is not None and hasattr(ist, 'class_flags') and ist.class_flags & SCRIPT_INHERIT & ~st.class_flags:
                yield i, 'ClassFlags %#x lack %#x of interface %s' % (st.class_flags, ist.class_flags & SCRIPT_INHERIT
                                                                      & ~st.class_flags, pkg.path(idx))
        if not st.super: continue
        t = parent_tail(pkg, st.super)
        if not t: continue
        bits, within, config, where = t
        parent = pkg.path(st.super)
        if bits & ~st.class_flags:
            yield i, 'ClassFlags %#x lack %#x of parent %s (%s)' % (st.class_flags, bits & ~st.class_flags, parent, where)
        if within and st.within and not _same(pkg.path(st.within), within):
            yield i, 'ClassWithin %s, parent %s is within %s' % (pkg.path(st.within), parent, within)
        if config and not _same(st.config, config):
            yield i, 'ClassConfigName %s, parent %s has %s' % (st.config, parent, config)


@rule
def class_flags_compiled_bp(pkg):
    """A Blueprint class never carries LayoutChanging 0x20000 (set only during a compile, KismetCompiler.cpp:314-327)
    or NewerVersionExists 0x80000000 (a replaced class): neither is masked on load (Class.cpp:4425), and they trip the
    CreateDefaultObject ensure (Class.cpp:3713), the linker's re-resolve (LinkerLoad.cpp:4592-4601), IsAsset
    (BlueprintGeneratedClass.cpp:1624) and CreateWidget (UserWidget.cpp:2030). The rest is what the editor always
    writes and the game keeps, with no known run-time reader: CLASS_Parsed 0x10 beside CompiledFromBlueprint
    (KismetCompiler.cpp:2517; the only run-time test of Parsed, Obj.cpp:2869, lists native classes), and never
    NoExport 0x100 or TokenStreamAssembled 0x400000 (CLASS_RecompilerClear, ObjectMacros.h:253, cleared at
    KismetCompiler.cpp:2450; TokenStreamAssembled is masked on load anyway). CompiledFromBlueprint itself, Native and
    Intrinsic: class_flags_loadable."""
    for i, st in classes(pkg):
        f = st.class_flags
        if f & 0x40010 != 0x40010: yield i, 'ClassFlags %#x lack Parsed|CompiledFromBlueprint 0x40010' % f
        if f & 0x80420100: yield i, 'ClassFlags %#x carry %#x (NoExport/LayoutChanging/TokenStreamAssembled/NewerVersionExists)' % (f, f & 0x80420100)


@rule
def class_tail_cooked(pkg):
    """The cooked class tail: bCooked 1 and ClassGeneratedBy null (Class.cpp:4479-4482, 4533-4545 - the cook saves
    Ar.IsCooking(); with 0 the linker takes the uncooked Blueprint regeneration path, LinkerLoad.cpp:4874-4891, and a
    UBlueprint is not in a cooked game to be generated by), and each Interfaces entry is {an interface class,
    PointerOffset 0, bImplementedByK2 1} as the compiler writes it (KismetCompiler.cpp:2420-2433; read as is,
    Class.cpp:4730-4737). A native interface entry with bImplementedByK2 0 makes GetInterfaceAddress return
    this + PointerOffset (UObjectBaseUtility.cpp:432-442), so Cast<IFoo> hands native code the object as the interface;
    an entry whose class is not the interface or a child of it answers no to ImplementsInterface (Class.cpp:4649-4668).
    For a Blueprint interface nothing reads PointerOffset or bImplementedByK2 (UObjectBaseUtility.cpp:422-428): there
    the pair is what the editor always writes and the game keeps."""
    for i, st in classes(pkg):
        if st.cooked != 1: yield i, 'bCooked %d' % st.cooked
        if st.generated_by != 0: yield i, 'ClassGeneratedBy %s' % pkg.path(st.generated_by)
        for idx, po, k2 in st.interfaces:
            if (po, k2) != (0, 1): yield i, 'Interfaces entry %s: PointerOffset %d bImplementedByK2 %d' % (pkg.path(idx), po, k2)
            if not idx: yield i, 'Interfaces entry with a null class'; continue
            r = pkg.resolve(idx)
            if r:
                P, k = r
                ist = P.struct(k)
                if not ist or not hasattr(ist, 'class_flags') or not ist.class_flags & CLASS_Interface:
                    yield i, 'Interfaces entry %s is not an interface class' % pkg.path(idx)
            elif idx < 0 and pkg.obj(idx)['class_name'] != 'Class' and not pkg.path(idx).startswith('/Game/'):
                yield i, 'Interfaces entry %s is a %s' % (pkg.path(idx), pkg.obj(idx)['class_name'])
            elif idx < 0 and pkg.path(idx) in ueapi() and not ueapi()[pkg.path(idx)]['interface']:
                yield i, 'Interfaces entry %s is not a native interface' % pkg.path(idx)


@rule
def class_ubergraph_named(pkg):
    """A class whose UberGraphFunction tag is set declares its own StructProperty UberGraphFrame (a
    PointerToUberGraphFrame), and the tag names a function of this class - in Children and FuncMap - flagged
    FUNC_UbergraphFunction (KismetCompiler.cpp:1887-1895). Link finds the frame by name only
    (BlueprintGeneratedClass.cpp:1627-1652), and without it every event runs the ubergraph on a fresh zeroed frame;
    ProcessEvent runs a function on the persistent frame only if it has the flag (ScriptCore.cpp:1943-1955)."""
    for i, st in classes(pkg):
        t = pkg.tag(i, 'UberGraphFunction')
        if not t: continue
        u = _i32(t)
        if u <= 0 or pkg.exports[u - 1]['outer'] != i + 1:
            yield i, 'UberGraphFunction %s is not a function of this class' % pkg.path(u); continue
        name = pkg.exports[u - 1]['name']
        if u not in st.children or name not in dict(st.func_map): yield i, 'ubergraph %s not in Children / FuncMap' % name
        f = pkg.struct(u - 1)
        if not f or not getattr(f, 'function_flags', 0) & FUNC_UbergraphFunction:
            yield i, 'ubergraph %s lacks FUNC_UbergraphFunction' % name
        frame = [p for p in st.props if p.name == 'UberGraphFrame']
        if len(frame) != 1 or frame[0].type != 'StructProperty' or not (pkg.path(frame[0].ref) or '').endswith('PointerToUberGraphFrame'):
            yield i, 'UberGraphFrame property %s' % [(p.type, pkg.path(p.ref)) for p in frame]


def _notify_ok(pkg, i, p):
    """Whether FinishCompilingClass's RepNotify `continue` (KismetCompiler.cpp:2532-2542) skips property p: CPF_Net |
    CPF_RepNotify and a function of that name on the class or a Blueprint super (parameters not checked: a looser
    exception, never a false finding)."""
    if p.flags & (CPF_Net | CPF_RepNotify) != CPF_Net | CPF_RepNotify or p.notify == 'None': return False
    for P, k, native in ancestors(pkg, i + 1):
        if P and any(e['name'] == p.notify and e['outer'] == k + 1 for e in P.exports): return True
        if native and p.notify.lower() in ueapi().get(native, {}).get('funcs', ()): return True
    return False


@rule
def class_flags_follow_props(pkg):
    """A class that declares a property with CPF_InstancedReference or CPF_ContainsInstancedReference has
    CLASS_HasInstancedReference, and one that declares a CPF_Config property has CLASS_Config and a ConfigName
    (KismetCompiler.cpp:2521-2551, the property's own flags as ContainsInstancedObjectProperty reads them,
    UnrealType.h:752-755). Without them per-instance subobject instancing never runs (UObjectGlobals.cpp:2875) and
    LoadConfig returns at once (Obj.cpp:2077-2080). The compiler's own exception: a replicated RepNotify property with a
    valid notify function is skipped before the Config test (KismetCompiler.cpp:2537-2542)."""
    for i, st in classes(pkg):
        inst = [p.name for p in st.props if p.flags & CPF_Instanced]
        if inst and not st.class_flags & CLASS_HasInstancedReference:
            yield i, 'ClassFlags %#x lack HasInstancedReference; instanced %s' % (st.class_flags, inst)
        cfg = [p.name for p in st.props if p.flags & CPF_Config and not _notify_ok(pkg, i, p)]
        if cfg and (not st.class_flags & CLASS_Config or st.config == 'None'):
            yield i, 'config properties %s, ClassFlags %#x ConfigName %s' % (cfg, st.class_flags, st.config)


# Native classes declared DefaultToInstanced (CLASS_Inherit carries it to every native subclass), UE 4.27 UCLASS
# specifiers, and two FSD classes the game shows are (every game property of the type carries CPF_InstancedReference).
DTI_ROOTS = {
    '/Script/Engine.ActorComponent',                    # ActorComponent.h:115
    '/Script/UMG.Visual',                               # Visual.h:11
    '/Script/UMG.SlateAccessibleWidgetData',            # SlateWrapperTypes.h:60
    '/Script/Engine.NavAreaBase',                       # NavAreaBase.h:11
    '/Script/NavigationSystem.NavArea',                 # NavArea.h:13
    '/Script/Engine.Distribution',                      # Distribution.h:64
    '/Script/Engine.AssetUserData',                     # AssetUserData.h:13
    '/Script/Engine.LevelActorContainer',               # LevelActorContainer.h:16
    '/Script/LevelSequence.LevelSequenceBurnInInitSettings', '/Script/LevelSequence.LevelSequenceBurnInOptions',  # LevelSequenceActor.h:20, 26
    '/Script/MovieScene.MovieScene', '/Script/MovieScene.MovieSceneBindingOverrides',                  # MovieScene.h:376, MovieSceneBindingOverrides.h:41
    '/Script/MovieScene.MovieSceneFolder', '/Script/MovieScene.MovieSceneSection', '/Script/MovieScene.MovieSceneTrack',  # :15, :150, :133
    '/Script/FSD.CarvedResourceCreator', '/Script/FSD.ProjectileAttack',                              # measured
}
# Native structs with STRUCT_HasInstancedReference: every game property of these types carries
# CPF_ContainsInstancedReference, and none of another native struct type does (measured, every package).
INSTANCED_STRUCTS = {
    '/Script/Engine.HitResult', '/Script/AnimGraphRuntime.AnimNode_CopyPoseFromMesh', '/Script/FSD.BossFight',
    '/Script/FSD.ClaimableRewardEntry', '/Script/FSD.ClaimableRewardView', '/Script/FSD.DamageData',
    '/Script/FSD.EscortMuleExtractorSlot', '/Script/FSD.HolidayMeshItems', '/Script/FSD.MasteryItem',
    '/Script/FSD.SeasonLevel', '/Script/FSD.TextCounterEntry', '/Script/FSD.VanityNode',
}


def native_default_to_instanced(path):
    return any(p in DTI_ROOTS for p in native_chain(path))


def _imports_package(other, name):
    return any(e['outer'] == 0 and e['name'].lower() == name.lower() for e in other.imports)


def _instancing_wants(pkg, p):
    """The instancing flag the compiler gives property p, or 0 when it gives none or it cannot be told here. A
    Blueprint property class is read from its own package; it is not judged when it is this package's own class, or
    when its package imports this one - a load cycle, where the compiler sees a placeholder class without its flags
    (KismetCompilerMisc.cpp:954-957; the game's one unflagged case, UI_ClassInfo_CharacterIcon's Selector, is one)."""
    if p.type in OBJECT_PROPS and p.ref < 0:
        path = pkg.path(p.ref)
        if path.startswith('/Script/') and native_default_to_instanced(path): return CPF_InstancedReference
        r = pkg.resolve(p.ref) if path.startswith('/Game/') else None
        if r and r[0] is not pkg and not _imports_package(r[0], pkg.package_name()):
            c = r[0].struct(r[1])
            if c is not None and getattr(c, 'class_flags', 0) & CLASS_DefaultToInstanced: return CPF_InstancedReference
    if p.type == 'StructProperty' and p.ref:
        path = pkg.path(p.ref)
        if path in INSTANCED_STRUCTS: return CPF_ContainsInstancedReference
        r = pkg.resolve(p.ref)
        if r:
            s = r[0].struct(r[1])
            if s is not None and getattr(s, 'struct_flags', 0) & 0x4: return CPF_ContainsInstancedReference
    if p.subs and any(q.flags & CPF_Instanced or _instancing_wants(pkg, q) for q in p.subs):
        return CPF_ContainsInstancedReference
    return 0


@rule
def instanced_refs_flagged(pkg):
    """Every member variable of a class is flagged for instancing as its type needs: an object / weak / soft object
    property of a DefaultToInstanced class (every ActorComponent and widget, native or Blueprint) carries
    CPF_InstancedReference; a struct property of a STRUCT_HasInstancedReference struct, and an array / set / map whose
    inner, key or value carries either instanced flag, carries CPF_ContainsInstancedReference
    (KismetCompilerMisc.cpp:948-952, 974-977, 1215-1218, 1254-1257). Instancing (UObjectGlobals.cpp:2874-2886 ->
    Class.cpp:2152-2163) walks only flagged properties, so a spawn from a template or a duplicate leaves an unflagged
    member pointing at the archetype's component or instanced subobject instead of its own copy. Class members only:
    the compiler flags a function's parameters and locals the same way, and the game keeps that, but instancing never
    walks a function's properties, so there it is the editor's habit, not a requirement; and the game's user-defined
    structs do not follow it (BeamVThree's Collider and BeamEffect are unflagged). A Blueprint property class in a load
    cycle with this package is not judged (see _instancing_wants)."""
    for i, st in classes(pkg):
        todo = [(p, p.name) for p in st.props]
        while todo:
            p, where = todo.pop()
            want = _instancing_wants(pkg, p)
            if want and not p.flags & want:
                yield i, '%s %s (%s) flags %#x lack %#x' % (p.type, where, pkg.path(p.ref) if p.ref else
                                                           [q.type for q in p.subs], p.flags, want)
            todo += [(q, where + '.' + q.type) for q in p.subs]


def _super_properties(pkg, i):
    """Property names, lower-cased, of every super of class export i the resolver and UeApi know."""
    props = set()
    for P, k, native in ancestors(pkg, pkg.struct(i).super):
        props |= {p.name.lower() for p in P.struct(k).props} if P else ueapi().get(native, {}).get('props', set())
    return props


@rule
def member_names_distinct(pkg):
    """Member names that are one FName (FNames compare case-insensitively, so Score and score are one name):
    - two functions of one class: two exports of one name in one outer, and the linker creates the second over the
      first (StaticAllocateObject finds and reuses the object of that name and outer, UObjectGlobals.cpp:2394-2396);
      FuncMap (Class.cpp:4413) answers one of them. The editor refuses it (KismetCompiler.cpp:1737-1747).
    - two own properties, or an own property named like a super's ('UberGraphFrame' excepted, which every class with an
      ubergraph declares anew): tagged CDO values (Class.cpp:1355-1397) and by-name lookups (FindFProperty,
      UnrealType.h:5774-5796) reach the first of the chain, the class's own, so the other never gets its saved value
      and native code that finds a property by name gets the wrong storage. The editor renames it (ValidateVariableNames,
      KismetCompiler.cpp:570-616; CheckPropertyNameOnScope, KismetCompilerMisc.cpp:1127-1153).
    Not checked: a function spelled like a super's function in another case. It is one FName, so the engine takes it
    as an override (FindFunctionByName through FuncMap), and the game's own overrides are sometimes spelled so
    (PRJ_FlareGun_Projectile01_C's OnDropPodImpact over OnDroppodImpact, BP_SupplyPod_Ammo_C's OnTunnelBlocked over
    OnTunnelBLocked): whether the source meant an override is not in the package.
    Not checked either: a function and a property of one name, whichever class declares which. Functions and properties are
    looked up apart (FuncMap / Children against ChildProperties); the one run-time lookup that sees both,
    FindUFieldOrFProperty (UnrealType.h:5809-5818, for property paths, PropertyPathHelpers.cpp:570), takes the property
    either way - and the game ships that case (Basic_RadioButton_C's widget variable Tick beside UUserWidget's Tick
    event, ENE_Spider_Spawn_C's Spawn beside a super's Spawn). The editor refuses a new function named like a property
    (KismetCompiler.cpp:1748-1757), but that is its rule, not the engine's."""
    for i, st in classes(pkg):
        funcs = [e['name'] for n, e in enumerate(pkg.exports)
                 if e['outer'] == i + 1 and pkg.class_of(n + 1) in Package.FUNCTION_CLASSES]
        low = [f.lower() for f in funcs]
        for f in sorted({f for f in low if low.count(f) > 1}): yield i, 'two functions named %s' % f
        mine = [p.name.lower() for p in st.props]
        for p in sorted({p for p in mine if mine.count(p) > 1}): yield i, 'two properties named %s' % p
        sprops = _super_properties(pkg, i)
        for p in st.props:
            if p.name.lower() in sprops - {'ubergraphframe'}: yield i, "property %s shadows a super's property" % p.name


@rule
def class_replicated_count_not_short(pkg):
    """NumReplicatedProperties (an absent tag is 0) is at least the number of CPF_Net properties the class declares
    itself: GetLifetimeBlueprintReplicationList registers that many of them, in ChildProperties order, and stops
    (BlueprintGeneratedClass.cpp:1786-1799), so any past the count never replicate. The compiler writes the exact count
    (KismetCompiler.cpp:744, 787-790); a higher one is harmless, the loop ending with the fields."""
    for i, st in classes(pkg):
        n, want = _i32(pkg.tag(i, 'NumReplicatedProperties')), sum(1 for p in st.props if p.flags & CPF_Net)
        if n < want: yield i, 'NumReplicatedProperties %d, the class declares %d CPF_Net properties' % (n, want)


@rule
def function_parms_fit(pkg):
    """A function has at most 255 CPF_Parm properties (its return value included) and its parameters fit 65535 bytes:
    UFunction::NumParms is a uint8 and ParmsSize / ReturnValueOffset uint16s (Class.h:1800-1805), computed by
    InitializeDerivedMembers (Class.cpp:5638-5651) with no check, so a wider block wraps and ProcessEvent copies and
    zeroes the wrong byte range (ScriptCore.cpp:1952-1958, 2014-2016). The sum of the parameters' sizes is a lower bound
    of the linked ParmsSize."""
    for i, st in functions(pkg):
        parms = [p for p in st.props if p.flags & CPF_Parm]
        size = sum(p.elem_size * p.dim for p in parms)
        if len(parms) > 255: yield i, '%d parameters, NumParms is a uint8' % len(parms)
        if size > 65535: yield i, 'parameters take at least %d bytes, ParmsSize is a uint16' % size
