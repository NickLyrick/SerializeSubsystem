// All Rights Reserved by TacoEaters

#pragma once

#include "CoreMinimal.h"

#include "SaveGameRecords.h"

class AActor;
class ULevel;
class UWorld;

/** An actor the plugin saves in the current world. */
struct FSaveGameTrackedActor final
{
	// Spawned by the game during this world (seen in OnActorPreSpawnInitialization); placed otherwise.
	bool bSpawned = false;
	// Its record was applied already: a second RestoreActor does nothing.
	bool bRestored = false;
};

/** Hash of the state an actor had when its level appeared; a capture equal to it drops the record. */
struct FSaveGamePristine final
{
	// CityHash64 of the state bytes.
	uint64 Hash = 0;
	// Length of the state bytes.
	int32 Size = 0;
};

/** Bookkeeping of the server game world the plugin works in (ADR-0060 §3). */
/**
 * A replicated placed actor a tombstone destroyed while a net driver was up. Iris scopes a destruction to its level's
 * replication group, which exists only once the level's first actor replicates: stored before that, the destruction
 * goes out unscoped and a client still loading the level drops it. It is stored again once the level has begun play.
 */
struct FSaveGamePendingNetDestruction final
{
	// Level the actor stood in.
	TWeakObjectPtr<ULevel> Level;
	// Object name of the destroyed actor inside Level.
	FName Name;
	// Where it stood, for distance-based prioritization.
	FVector Location = FVector::ZeroVector;
};

struct FSaveGameWorldState final
{
	// Every saveable actor of the world the plugin knows about.
	TMap<TWeakObjectPtr<AActor>, FSaveGameTrackedActor> Tracked;
	// Authored state of placed actors by save key.
	TMap<FName, FSaveGamePristine> Pristine;
#if !UE_BUILD_SHIPPING
	// Authored state bytes by save key, for SaveGame.Diff.
	TMap<FName, TArray<uint8>> PristineData;
#endif
	// Tracked actors of levels that left the world, by level: shown again with the same level object (hidden, or
	// unloaded and reused before garbage collection), they are tracked again as they were. Stale keys are dropped.
	TMap<TWeakObjectPtr<const ULevel>, TArray<TPair<TWeakObjectPtr<AActor>, FSaveGameTrackedActor>>> HiddenLevels;
	// Long package name of the map, without PIE prefix.
	FString MapPath;
	// The server game world.
	TWeakObjectPtr<UWorld> World;
	// Destructions to store again for clients, see FSaveGamePendingNetDestruction.
	TArray<FSaveGamePendingNetDestruction> PendingNetDestructions;
	// Handles of the per-world spawn, destroy and begin-play handlers.
	FDelegateHandle PreSpawnHandle;
	FDelegateHandle DestroyedHandle;
	FDelegateHandle BeginPlayHandle;
};

/** Every recorded state of the game: the global table, the current map's table decoded, other maps compressed. */
struct FSaveGameStore final
{
	// Versions of this build, shared by every record captured now.
	TSharedRef<FSaveGameVersions> CurrentVersions = FSaveGameVersions::MakeCurrent();
	// Players and singletons.
	FSaveGameTable GlobalTable;
	// Records of the map MapTablePath.
	FSaveGameTable MapTable;
	// Compressed tables of the other maps by map path.
	TMap<FString, TArray<uint8>> MapBlobs;
	// Map whose records MapTable holds; empty before the first map.
	FString MapTablePath;

	// Makes MapTable the table of MapPath, compressing the one it held into MapBlobs. False when the stored table
	// of MapPath is unreadable; MapTable is then empty.
	bool EnterMap(const FString& MapPath);

	// Serializes the whole store (global table, every map) into a save file body.
	TArray<uint8> WriteBody() const;

	// Inverse of WriteBody; false when the body or a table in it is unreadable.
	static bool ReadBody(const TArray<uint8>& Body, FSaveGameStore& OutStore);
};
