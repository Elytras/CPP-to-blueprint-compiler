/*
HelloWorld.cpp — a mod, written as C++ and compiled into cooked Blueprint assets.

This file is not linked into anything. It is compiled for its diagnostics and its AST: clang
proves that AActor exists, that ReceiveBeginPlay is a real event on it and that
AFSDGameState::PostGameMessage takes a string, and AssetGen then turns the same AST into
Test.uasset, InitCave.uasset and InitSpacerig.uasset.

InitCave and InitSpacerig are named that way because DRG's mod loader spawns assets by those
names; a mod's entry point is its asset name, not a registration call.

    assetgen compile AssetGen/mods/HelloWorld.cpp <out-dir>
*/
#include "UeApi.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/CppTest");

class Test : public AActor
{
public:
    void ReceiveBeginPlay()
    {
        UGameFunctionLibrary::GetFSDGameState(this)->PostGameMessage("Hello from a C++ generated Blueprint");
    }
};

class InitCave : public Test
{
};

class InitSpacerig : public Test
{
};
