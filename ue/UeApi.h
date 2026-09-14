#pragma once
/*
UeApi.h — the declarations a mod source compiles against.

This is the whole point of the compiler front end: a mod is ordinary C++, so the base class it
derives from and every function it calls must be *declared*, and clang rejects the file if they
are not. A typo that the old string-based builder would have happily written into an asset is
now a compile error.

Each class carries the two things the package writer cannot infer from a C++ name — the UE
package it lives in and its UE spelling (which drops the A/U prefix). UE_CLASS stores them as a
constexpr string because clang's JSON AST dump carries string literals but drops annotate
attributes, so a literal is the only metadata channel that survives to the generator.

Hand-written for now, and deliberately small. It is a slice of the real SDK, and the intent is
to emit it from the Dumper-7 dump rather than maintain it by hand.
*/
#define UE_CLASS(Package, UeName) static constexpr const char* UeClassMeta = Package ":" UeName

/* The /Game package the classes in a mod source are written into. */
#define UE_MOD_PACKAGE(Path) static constexpr const char* UeModPackage = Path

class AActor
{
public:
    UE_CLASS("/Script/Engine", "Actor");

    /* Blueprint events. Overriding one is what makes the engine call into a generated class. */
    void ReceiveBeginPlay();
    void ReceiveTick(float DeltaSeconds);
};

class AFSDGameState
{
public:
    UE_CLASS("/Script/FSD", "FSDGameState");

    /* The game's own on-screen message path — unlike PrintString it survives a shipping build. */
    void PostGameMessage(const char* Message);
};

class UGameFunctionLibrary
{
public:
    UE_CLASS("/Script/FSD", "GameFunctionLibrary");

    static AFSDGameState* GetFSDGameState(AActor* WorldContext);
};
