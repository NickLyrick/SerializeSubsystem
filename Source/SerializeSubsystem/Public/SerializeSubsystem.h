#pragma once

#include "CoreMinimal.h"

#include "GameFramework/Actor.h"
#include "Structs/SaveGameSturct.h"
#include "Structs/SerializationStructs.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "SerializeSubsystem.generated.h"

/**
 * The subsystem that serializes and deserializes the game world.
 */
UCLASS(DisplayName = "Serialize Subsystem", Category = "Serialize Subsystem Plugin")
class USerializeSubsystem : public UGameInstanceSubsystem
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
	void OnWorldInitialized(UWorld* World, const UWorld::InitializationValues);
	void OnActorsInitialized(const FActorsInitializedParams& Params);
	void OnWorldCleanup(UWorld* World, bool, bool);

	// Streaming Level Event Handlers
	void OnLevelAddedToWorld(ULevel* Level, UWorld* World);
	void OnLevelRemovedFromWorld(ULevel* Level, UWorld* World);

	// Actor Event Handlers
	void OnActorPreSpawn(AActor* Actor);
	void OnActorDestroyed(AActor* Actor);

	// Deferred Event Handlers
	void OnLoadCompleted();

private:
	template <bool, bool>
	friend class TSaveGameSerializer;
	TSharedPtr<class FSaveGameSerializer, ESPMode::ThreadSafe> CurrentSerializer;

	TSharedPtr<FLevelStruct> PersistentLevelRecord;
	TSharedPtr<FSerializedData> SerializedData = MakeShared<FSerializedData>();
};
