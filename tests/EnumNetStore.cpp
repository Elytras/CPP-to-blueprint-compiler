/*
EnumNetStore.cpp - UE_ENUM values that cross the network.

A replicated variable of a UE_ENUM, and a server RPC's parameter of one, are sent with only as many bits as the enum's
largest value needs (FByteProperty::NetSerializeItem), so what a script stores or passes there stays within the enum.
Run as the server: the store wakes the object and lands, the RPC runs its body with the value it was given.
*/
#include "UeApi/Types.h"

#include "UeApi/FSD.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/EnumNetStore");

enum class ENetGear : uint8 { Park, Drive, Sport };
UE_ENUM(ENetGear);

class EnumNetStore : public AActor {
  UE_REPLICATED(ENetGear, Gear);

public:
  ENetGear Asked;
  UE_SERVER UE_RELIABLE void ServerShift(ENetGear To);
  void Shift() { Gear = ENetGear::Sport; }
  void Ask() { ServerShift(ENetGear::Drive); }
};

void EnumNetStore::ServerShift(ENetGear To) {
  Gear = To;
  Asked = To;
}
