// All Rights Reserved by TacoEaters

#pragma once

#include "CoreMinimal.h"

#include "Engine/World.h"
#include "SaveGameTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "SerializeSubsystem.generated.h"

class AGameModeBase;
class APlayerController;
class ULevel;
class ULevelStreaming;
struct FSaveGameRecord;
struct FSaveGameStore;
struct FSaveGameTable;
struct FSaveGameTrackedActor;
struct FSaveGameWorldState;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSaveGameLoadCompleted, ESaveGameLoadResult, Result);

/**
 * Saves the state of every ISaveGameObject actor of the server's game world as records keyed by the actor's save
 * key (ADR-0060): one mechanism for World Partition, embedded Level Instances and plain level streaming. A record
 * is applied before the actor's BeginPlay when its level appears and captured before EndPlay when the level goes;
 * only actors that differ from how the level authored them are recorded. Clients do nothing: they get the state
 * by replication.
 */
UCLASS(DisplayName = "Serialize Subsystem", Category = "Serialize Subsystem Plugin")
class SERIALIZESUBSYSTEM_API USerializeSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Captures the current world into a complete save file whose header carries UserMeta. Empty when this
	// instance cannot save now: it is not the server of a game world, or a load is in flight.
	TArray<uint8> CaptureSaveFile(const TArray<uint8>& UserMeta);

	// Checks File completely and travels to its map (ServerTravel: clients follow). Success = travel started,
	// the final verdict arrives through OnLoadCompleted. Any other verdict is a refusal: only returned, never
	// broadcast, and the current world is left untouched.
	ESaveGameLoadResult LoadSaveFile(const TArray<uint8>& File);

	// Forgets every recorded state: worlds entered from now on start as authored.
	UFUNCTION(BlueprintCallable, Category = "Serialize Subsystem")
	void StartNewGame();

	// Applies the pending record of Actor, once: for an actor whose save identity is known only after it spawned.
	UFUNCTION(BlueprintCallable, Category = "Serialize Subsystem")
	void RestoreActor(AActor* Actor);

	// True from an accepted LoadSaveFile until OnLoadCompleted.
	UFUNCTION(BlueprintPure, Category = "Serialize Subsystem")
	bool IsLoadingSaveGame() const;

	// Save key of Actor (ADR-0060 §1); empty when the actor is not saved here.
	FString GetActorSaveKey(const AActor* Actor) const;

	// Readable list of the recorded states: scope, key, flags and size of every record.
	FString DescribeRecords() const;

#if !UE_BUILD_SHIPPING
	// For every tracked placed actor whose state is no longer what its level authored: which parts differ and where.
	FString DescribeDifferences() const;
#endif

	// Fired when an accepted load ends, successfully or not.
	UPROPERTY(BlueprintAssignable, Category = "Serialize Subsystem")
	FOnSaveGameLoadCompleted OnLoadCompleted;

private:
	/** Where an actor's record lives and what it is called. */
	struct FActorKey final
	{
		FName Key;
		bool bGlobal  = false;
		bool bRespawn = false;
		// Spawned without an identity: the key names a singleton of its class.
		bool bClassKey = false;
	};

	// Starts tracking World when it is a server game world of this game instance; true when it is tracked now (false
	// for a world a load is leaving).
	bool EnsureWorld(UWorld* World);
	void ReleaseWorld();
	bool IsTrackedWorld(const UWorld* World) const;

	static FActorKey MakeKey(const AActor* Actor, bool bSpawned);
	// Live tracked actors of exactly Class.
	int32 CountTrackedOfClass(const UClass* Class) const;
	// The tracked actors and their entries, copied: game code run per actor may edit the map.
	TArray<TPair<AActor*, FSaveGameTrackedActor>> SnapshotTracked() const;
	// Save keys of the spawned actors tracked in the world now.
	TSet<FName> CollectAliveSpawnKeys() const;
	// The table Key's record lives in: global for players and singletons, the map's otherwise.
	FSaveGameTable& GetTable(const FActorKey& Key) const;

	// Records the pristine state of Level's saveable actors, applies their records and destroys tombstoned ones.
	void ApplyLevel(const ULevel* Level) const;
	// Applies global records to the tracked actors that have one (GameState, players already logged in).
	void ApplyGlobalRecords();
	// Spawns into Level every respawnable record of LevelKey (or of Keys, when given) whose actor is not alive.
	void RespawnRecords(ULevel* Level, FName LevelKey, const TArray<FName>* Keys = nullptr) const;
	void Respawn(ULevel* Level, FName Key, const FSaveGameRecord& Record) const;
	static void ApplyRecord(AActor* Actor, const FSaveGameRecord& Record);
	void RespawnPending();

	void CaptureActor(AActor* Actor, const FSaveGameTrackedActor& Tracked, const FActorKey& Key) const;
	void CaptureAll() const;

	void FinishLoad(ESaveGameLoadResult Result);

	void HandlePostWorldInitialization(UWorld* World, UWorld::InitializationValues Values);
	void HandleWorldInitializedActors(const FActorsInitializedParams& Params);
	void HandleWorldBeginTearDown(UWorld* World) const;
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void HandlePostLoadMap(UWorld* World);
	void HandleLevelBeginMakingVisible(UWorld* World, const ULevelStreaming* StreamingLevel, ULevel* Level);
	void HandleLevelAddedToWorld(ULevel* Level, UWorld* World) const;
	void HandleWorldBeginPlay() const;
	void HandlePreLevelRemoved(ULevel* Level, UWorld* World);
	void HandlePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer);
	void HandleActorPreSpawn(AActor* Actor) const;
	void HandleActorDestroyed(AActor* Actor) const;

	TSharedPtr<FSaveGameStore> Store;
	TSharedPtr<FSaveGameWorldState> WorldState;

	// Respawnable actors whose World Partition cell unloaded; spawned again in the persistent level next tick.
	TArray<FName> PendingRespawns;

	// Map a load travels to; empty when no load is in flight.
	FString LoadTargetMap;

	// The world running when a load was accepted: it is about to be thrown away and must not write records.
	bool bDiscardCurrentWorld = false;
};
