#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SaveGameMigrationStep.h"
#include "Structs/SaveGameSturct.h"
#include "Structs/SerializationStructs.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "SerializeSubsystem.generated.h"

/** Result passed to the OnLoadCompleted delegate after every load attempt. */
UENUM(BlueprintType)
enum class ESaveGameLoadResult : uint8 {
  Success              UMETA(DisplayName = "Success"),
  CorruptedData        UMETA(DisplayName = "Corrupted Data"),
  IncompatibleVersion  UMETA(DisplayName = "Incompatible Version"),
  EngineVersionMismatch UMETA(DisplayName = "Engine Version Mismatch"),
  MapMissing           UMETA(DisplayName = "Map Missing"),
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSaveGameLoadCompleted,
                                            ESaveGameLoadResult, Result);

/**
 * The subsystem that serializes and deserializes the game world.
 */
UCLASS(DisplayName = "Serialize Subsystem",
       Category = "Serialize Subsystem Plugin")
class USerializeSubsystem : public UGameInstanceSubsystem {
  GENERATED_BODY()

public:
  /* Subsystem API */
  virtual void Initialize(FSubsystemCollectionBase &Collection) override;
  virtual void Deinitialize() override;
  /* End Subsystem API */

  UFUNCTION(BlueprintCallable, Category = "SaveGamePlugin|Save")
  void Save(UPARAM(DisplayName = "Serialized Data") FSerializedData &Data);

  UFUNCTION(BlueprintCallable, Category = "SaveGamePlugin|Load")
  void Load(FSerializedData Data);

  UFUNCTION(BlueprintCallable, Category = "SaveGamePlugin|Load")
  bool IsLoadingSaveGame() const;

  /**
   * Fired when a load attempt finishes — successfully or otherwise.
   * Check the Result parameter to distinguish success from each failure mode.
   */
  UPROPERTY(BlueprintAssignable, Category = "SaveGamePlugin|Load")
  FOnSaveGameLoadCompleted OnLoadCompleted;

#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
  /**
   * Loads level state from raw JSON bytes. Intended for automated tests:
   * pair with the JSON files produced by Save() in non-shipping builds.
   *
   * @param JsonLevelData Raw UTF-8 JSON bytes (output of SerializeLevelData
   *                      with TSaveGameSerializer<false, true>).
   */
  void LoadFromJson(TArray<uint8> JsonLevelData);
#endif

private:
  void SaveStreamingLevels();
  void LoadStreamingLevels();

protected:
  // World Event Handlers
  void OnWorldInitialized(UWorld *World, const UWorld::InitializationValues);
  void OnActorsInitialized(const FActorsInitializedParams &Params);
  void OnWorldCleanup(UWorld *World, bool, bool);

  // Streaming Level Event Handlers
  void OnLevelAddedToWorld(ULevel *Level, UWorld *World);
  void OnLevelRemovedFromWorld(ULevel *Level, UWorld *World);

  // Actor Event Handlers
  void OnActorPreSpawn(AActor *Actor);
  void OnActorDestroyed(AActor *Actor);

  // Deferred Event Handlers
  void FinalizeLoad(ESaveGameLoadResult Result);

private:
  template <bool, bool> friend class TSaveGameSerializer;
  TSharedPtr<class FSaveGameSerializer, ESPMode::ThreadSafe> CurrentSerializer;

  TSharedPtr<FLevelStruct> PersistentLevelRecord;
  TSharedPtr<FSerializedData> SerializedData = MakeShared<FSerializedData>();

  // SetDefaultValue migrations collected by the header serializer during
  // DeserializeHeaderData. Stored here so all level and streaming-level
  // serializer instances (which are separate objects) share the same queue.
  // Reset at the start of each Load() call.
  TArray<FMigration_SetDefaultValue> PendingDefaultMigrations;
  TArray<TObjectPtr<UClass>> PendingDefaultMigrationClasses;
};
