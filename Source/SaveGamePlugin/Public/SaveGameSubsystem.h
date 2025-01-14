#pragma once

#include "CoreMinimal.h"
#include "Structs/SaveGameSturct.h"
#include "Structs/SerializationStructs.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "SaveGameSubsystem.generated.h"

/**
 * The subsystem that manages the lifetime of a save game.
 */
UCLASS()
class SAVEGAMEPLUGIN_API USaveGameSubsystem : public UGameInstanceSubsystem {
  GENERATED_BODY()

public:
  virtual void Initialize(FSubsystemCollectionBase &Collection) override;
  virtual void Deinitialize() override;

  UFUNCTION(BlueprintCallable, Category = "SaveGamePlugin|Save")
  void Save(UPARAM(DisplayName = "Serialized Data") FSerializedData &Data);

  UFUNCTION(BlueprintCallable, Category = "SaveGamePlugin|Load")
  void Load(FSerializedData Data);

  UFUNCTION(BlueprintCallable, Category = "SaveGamePlugin|Load")
  bool IsLoadingSaveGame() const;

protected:
  void OnWorldInitialized(UWorld *World, const UWorld::InitializationValues);
  void OnActorsInitialized(const FActorsInitializedParams &Params);
  void OnWorldCleanup(UWorld *World, bool, bool);

  // Streaming Level Events
  void OnLevelAddedToWorld(ULevel *Level, UWorld *World);
  void OnLevelRemovedFromWorld(ULevel *Level, UWorld *World);

  void OnActorPreSpawn(AActor *Actor);
  void OnActorDestroyed(AActor *Actor);

  void OnLoadCompleted();

  UFUNCTION(BlueprintCallable, Category = "SaveGamePlugin|Load")
  void LoadStreamingLevels(FSerializedData Data);

  void SaveStreamingLevels(FSerializedData &Data);

private:
  template <bool, bool> friend class TSaveGameSerializer;
  TSharedPtr<class FSaveGameSerializer, ESPMode::ThreadSafe> CurrentSerializer;

  TSharedPtr<FLevelStruct> PersistentLevelRecord;
  TSharedPtr<FSerializedData> SerializedData = MakeShared<FSerializedData>();
};
