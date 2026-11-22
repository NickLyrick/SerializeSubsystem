// All Rights Reserved by TacoEaters

#pragma once

#include "CoreMinimal.h"

#include "Misc/EngineVersion.h"
#include "Serialization/CustomVersion.h"
#include "UObject/SoftObjectPath.h"

class AActor;

/** Engine, package and custom versions a group of records was written with (ADR-0060 §2). */
struct FSaveGameVersions final
{
	// Engine custom versions the records used, plus every project version of USaveGameSettings::Versions.
	FCustomVersionContainer CustomVersions;
	// Engine that wrote the records.
	FEngineVersion EngineVersion;
	// Package file version the tagged properties were written with.
	FPackageFileVersion PackageVersion;

	// Versions of this build, every project version stamped (a record that never asks for one still counts as
	// current, so migrations do not run on it); engine custom versions grow as records using them are written.
	static TSharedRef<FSaveGameVersions> MakeCurrent();
};

namespace SaveGameRecordFlags
{
	// A placed actor the game destroyed: destroyed again when its level appears.
	constexpr uint8 Destroyed = 1 << 0;
	// An actor the game spawned: Class, Transform and LevelKey are set.
	constexpr uint8 Spawned = 1 << 1;
	// A spawned actor the plugin spawns again on load (ISaveGameSpawnActor::ShouldRespawnOnLoad).
	constexpr uint8 Respawn = 1 << 2;
} // namespace SaveGameRecordFlags

/** State of one actor under its save key. */
struct FSaveGameRecord final
{
	// Where a spawned actor stood.
	FTransform Transform;
	// Class of a spawned actor.
	FSoftClassPath Class;
	// Versions the data was written with, shared between records.
	TSharedPtr<const FSaveGameVersions> Versions;
	// State of the actor and its components (FSaveGameActorSerializer); empty for a destroyed actor.
	TArray<uint8> Data;
	// Plain streaming sublevel the spawned actor lived in; None = the persistent level.
	FName LevelKey;
	uint8 Flags = 0;

	bool HasFlag(const uint8 Flag) const
	{
		return (Flags & Flag) != 0;
	}
};

/** Records of one scope (the global one or one map), keyed by actor save key. */
struct FSaveGameTable final
{
	TMap<FName, FSaveGameRecord> Records;

	// Serializes and compresses the table, every record with its own CRC.
	TArray<uint8> Compress() const;

	// Inverse of Compress; a record whose CRC fails is dropped alone. False when the block itself is unreadable.
	static bool Decompress(const TArray<uint8>& Blob, FSaveGameTable& OutTable);
};

/** Reads and writes the state of one actor and its ISaveGameObject components. */
class FSaveGameActorSerializer final
{
public:
	// Writes Actor's state with Versions, whose custom versions grow by any the actor uses. False when the
	// actor's OnSerialize excluded it from the save.
	static bool Write(AActor* Actor, FSaveGameVersions& Versions, TArray<uint8>& OutData);

	// Restores Actor from Data written with Versions, applying the migrations those versions are older than
	// (USaveGameSettings::Migrations). False when Data is malformed; the actor then keeps what was read so far.
	static bool Read(AActor* Actor, const TArray<uint8>& Data, const FSaveGameVersions& Versions);

#if !UE_BUILD_SHIPPING
	// Which parts of two states of one actor differ (actor properties, a component's properties or data, actor
	// data), each with the first differing offset and the readable bytes around it. Empty when they are equal.
	static FString DescribeDifference(const TArray<uint8>& Before, const TArray<uint8>& After);
#endif
};
