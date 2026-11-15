#include "UeApi/Types.h"
#include "UeApi/Engine.h"
#include "UeApi/AnimGraphRuntime.h"

/* An Anim Blueprint: its anim nodes are members, wired in the constructor, their dynamic inputs written in
   BlueprintUpdateAnimation. A state machine of two states (one a blend space, one named apart from its node) and a
   conduit, cached and read back, a blend by bool, an additive, a layered blend per bone, a bone turned in component
   space and a slot, then the output pose; and an actor whose mesh plays it. */
UE_MOD_PACKAGE("/Game/_ElytrasMods/AnimBlueprint");

UE_ASSET_AT(UAnimSequence, Anim_Idle, "/Game/_MyMods/Critter/Animations/ANIM_Critter_Idle");
UE_ASSET_AT(UAnimSequence, Anim_Hurt, "/Game/_MyMods/Critter/Animations/ANIM_Critter_Hurt");
UE_ASSET_AT(UAnimSequence, Anim_Lean, "/Game/_MyMods/Critter/Animations/ANIM_Critter_Lean_Add");
UE_ASSET_AT(UAnimSequence, Anim_Bite, "/Game/_MyMods/Critter/Animations/ANIM_Critter_Bite");
UE_ASSET_AT(UBlendSpace1D, BS_Move, "/Game/_MyMods/Critter/Animations/BS_Critter_Move");
UE_ASSET_AT(USkeletalMesh, SK_Critter, "/Game/_MyMods/Critter/SK_Critter");

class ABP_Critter : public UAnimInstance {
public:
    UE_SKELETON("/Game/_MyMods/Critter/SK_Critter_Skeleton");

    bool bMoving = false;
    bool bHurt = false;
    float Speed = 0.0f;
    float TiltPitch = 0.0f;

    FAnimNode_SequencePlayer Idle;
    FAnimNode_BlendSpacePlayer Move;
    FAnimNode_StateResult IdleState;
    FAnimNode_StateResult MoveState;
    FAnimNode_TransitionResult ToMove;
    FAnimNode_TransitionResult ToIdle;
    FAnimNode_TransitionResult Pick;
    FAnimNode_TransitionResult ToPick;
    FAnimNode_TransitionResult PickToMove;
    FAnimNode_StateMachine Locomotion;
    FAnimNode_SaveCachedPose LocoCache;
    FAnimNode_UseCachedPose UseLoco;
    FAnimNode_SequencePlayer Hurt;
    FAnimNode_BlendListByBool HurtBlend;
    FAnimNode_SequencePlayer Lean;
    FAnimNode_ApplyAdditive Leaning;
    FAnimNode_SequencePlayer Bite;
    FAnimNode_LayeredBoneBlend Upper;
    FAnimNode_ConvertLocalToComponentSpace ToCs;
    FAnimNode_ModifyBone Tilt;
    FAnimNode_ConvertComponentToLocalSpace ToLocal;
    FAnimNode_Slot Slot;
    FAnimNode_Root Output;

    ABP_Critter() {
        Idle.Sequence = &Anim_Idle;
        Move.BlendSpace = &BS_Move;
        IdleState.Result = UE_LINK(Idle);
        MoveState.Result = UE_LINK(Move);
        UE_STATE(Locomotion, IdleState);
        UE_STATE_AS(Locomotion, MoveState, "Moving");
        UE_CONDUIT(Locomotion, Pick, "Pick Next");
        UE_TRANSITION(Locomotion, IdleState, MoveState, ToMove, 0.2f);
        UE_TRANSITION_AUTO(Locomotion, MoveState, IdleState, ToIdle, 0.3f);
        UE_TRANSITION(Locomotion, IdleState, Pick, ToPick, 0.1f);
        UE_TRANSITION(Locomotion, Pick, MoveState, PickToMove, 0.15f);
        LocoCache.pose = UE_LINK(Locomotion);
        LocoCache.CachePoseName = FName("Loco");
        UseLoco.LinkToCachingNode = UE_LINK(LocoCache);
        UseLoco.CachePoseName = FName("Loco");
        Hurt.Sequence = &Anim_Hurt;
        HurtBlend.BlendPose = {UE_LINK(UseLoco), UE_LINK(Hurt)};
        HurtBlend.BlendTime = {0.1f, 0.1f};
        Lean.Sequence = &Anim_Lean;
        Leaning.Base = UE_LINK(HurtBlend);
        Leaning.Additive = UE_LINK(Lean);
        Leaning.alpha = 1.0f;
        Bite.Sequence = &Anim_Bite;
        Upper.BasePose = UE_LINK(Leaning);
        Upper.BlendPoses = {UE_LINK(Bite)};
        Upper.LayerSetup = {FInputBlendPose({FBranchFilter(FName("spine_01"), 0)})};
        Upper.BlendWeights = {1.0f};
        Slot.slotName = FName("DefaultSlot");
        ToCs.LocalPose = UE_LINK(Upper);
        Tilt.ComponentPose = UE_LINK_CS(ToCs);
        Tilt.BoneToModify.BoneName = FName("spine_01");
        Tilt.RotationMode = EBoneModificationMode::BMM_Additive;
        ToLocal.ComponentPose = UE_LINK_CS(Tilt);
        Slot.Source = UE_LINK(ToLocal);
        Output.Result = UE_LINK(Slot);
    }

    void BlueprintUpdateAnimation(float DeltaTimeX) {
        ToMove.bCanEnterTransition = bMoving;
        Move.X = Speed;
        HurtBlend.bActiveValue = bHurt;
        Pick.bCanEnterTransition = bMoving;
        Tilt.Rotation = FRotator(TiltPitch, 0.0f, 0.0f);
    }
};

class AnimBlueprint : public AActor {
public:
    UE_COMPONENT(USkeletalMeshComponent, Mesh);
    UE_DEFAULTS {
        Mesh->SkeletalMesh = &SK_Critter;
        Mesh->AnimClass = ABP_Critter::StaticClass();
        Mesh->AnimationMode = EAnimationMode::AnimationBlueprint;
    }
};
