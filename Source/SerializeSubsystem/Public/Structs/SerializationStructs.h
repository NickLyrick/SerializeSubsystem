#pragma once

#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "UObject/SoftObjectPtr.h"

#include "SerializationStructs.generated.h"

// Per-streaming-level serialized state: actor binary blob + visibility flags.
USTRUCT(BlueprintType, Blueprintable)
struct FStreamingLevelData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Serialization Data")
	bool bIsLoaded = false;

	UPROPERTY(BlueprintReadOnly, Category = "Serialization Data")
	bool bIsVisible = false;

	UPROPERTY(BlueprintReadWrite, meta = (HideInDetailPanel), Category = "Serialization Data")
	TArray<uint8> Data = {};

	void SaveStreamingLevelState(const TSoftObjectPtr<ULevelStreaming>& StreamingLevel)
	{
		bIsLoaded = StreamingLevel->IsLevelLoaded();
		bIsVisible = StreamingLevel->IsLevelVisible();
	}

	void LoadStreamingLevelState(const TSoftObjectPtr<ULevelStreaming>& StreamingLevel) const
	{
		StreamingLevel->SetShouldBeLoaded(bIsLoaded);
		StreamingLevel->SetShouldBeVisible(bIsVisible);
	}
};

// Struct to hold data for a single Persistent Level and its streaming levels.
USTRUCT(BlueprintType, Blueprintable)
struct FLevelData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, meta = (HideInDetailPanel), Category = "Serialization Data")
	TArray<uint8> Data = {};

	// TSoftObjectPtr as map key: stores a path string, so equality is by path — stable across frames
	// and safe to use as a TMap key (unlike raw UObject* which can be GC'd between frames).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Serialization Data")
	TMap<TSoftObjectPtr<ULevelStreaming>, FStreamingLevelData> StreamingLevels = {};
};

// Struct to hold data for the entire game.
USTRUCT(BlueprintType, Blueprintable)
struct FSerializedData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, meta = (HideInDetailPanel), Category = "Serialization Data")
	TArray<uint8> Header = {};

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Serialization Data")
	FString LevelName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Serialization Data")
	TMap<FString, FLevelData> Levels = {};
};