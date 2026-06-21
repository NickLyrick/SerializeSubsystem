#pragma once

#include "CoreMinimal.h"

#include "SaveGameMigrationStep.h"
#include "Structs/SaveGameStruct.h"
#include "Structs/SerializationStructs.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "SerializeSubsystem.generated.h"

/** Result passed to the OnLoadCompleted delegate after every load attempt. */
UENUM(BlueprintType)
enum class ESaveGameLoadResult : uint8
{
	Success UMETA(DisplayName = "Success"),
	CorruptedData UMETA(DisplayName = "Corrupted Data"),
	IncompatibleVersion UMETA(DisplayName = "Incompatible Version"),
	EngineVersionMismatch UMETA(DisplayName = "Engine Version Mismatch"),
	MapMissing UMETA(DisplayName = "Map Missing"),
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSaveGameLoadCompleted, ESaveGameLoadResult, Result);

/**
 * The subsystem that serializes and deserializes the game world.
 */
UCLASS(DisplayName = "Serialize Subsystem", Category = "Serialize Subsystem Plugin")
class SERIALIZESUBSYSTEM_API USerializeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/* Subsystem API */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	/* End Subsystem API */

	UFUNCTION(BlueprintCallable, Category = "Serialize Subsystem | Save")
	void Save(UPARAM(DisplayName = "Serialized Data") FSerializedData& Data);

	UFUNCTION(BlueprintCallable, Category = "Serialize Subsystem | Load")
	void Load(FSerializedData Data);

	UFUNCTION(BlueprintCallable, Category = "Serialize Subsystem | Load")
	bool IsLoadingSaveGame() const;

	/**
	 * Fired when a load attempt finishes — successfully or otherwise.
	 * Check the Result parameter to distinguish success from each failure mode.
	 */
	UPROPERTY(BlueprintAssignable, Category = "SaveGamePlugin|Load")
	FOnSaveGameLoadCompleted OnLoadCompleted;

private:
	void SaveStreamingLevels();
	void LoadStreamingLevels();

protected:
	// World Event Handlers
	void OnWorldInitialized(UWorld* World, const UWorld::InitializationValues);
	void OnActorsInitialized(const FActorsInitializedParams& Params) const;
	void OnWorldCleanup(UWorld* World, bool, bool) const;

	// Streaming Level Event Handlers
	void OnLevelAddedToWorld(ULevel* Level, UWorld* World);
	void OnLevelRemovedFromWorld(ULevel* Level, UWorld* World);

	// Actor Event Handlers
	void OnActorPreSpawn(AActor* Actor) const;
	void OnActorDestroyed(AActor* Actor) const;

	// Deferred Event Handlers
	void FinalizeLoad(ESaveGameLoadResult Result);

private:
	// TSaveGameSerializer writes directly into PersistentLevelRecord and SerializedData to avoid
	// extra copies — friend access is intentional, not an abstraction leak.
	template <bool, bool>
	friend class TSaveGameSerializer;

	// SharedPtr: SeamlessTravel is asynchronous; the serializer must outlive the current frame
	// until OnActorsInitialized fires in the new map.
	TSharedPtr<class FSaveGameSerializer, ESPMode::ThreadSafe> CurrentSerializer;

	TSharedPtr<FLevelStruct> PersistentLevelRecord;
	TSharedPtr<FSerializedData> SerializedData = MakeShared<FSerializedData>();

	// Parallel arrays (index N in both refers to the same migration step).
	// Populated in SerializeVersions; applied per-actor in SerializeActorData after
	// deserialization, because ImportText_Direct requires a fully constructed UObject.
	TArray<FMigration_SetDefaultValue> PendingDefaultMigrations;
	TArray<TObjectPtr<UClass>> PendingDefaultMigrationClasses;
};
