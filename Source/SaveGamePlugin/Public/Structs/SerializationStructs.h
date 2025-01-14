#pragma once

#include "SerializationStructs.generated.h"

// Struct to hold data for a single streaming level.
USTRUCT(BlueprintType, Blueprintable)
struct FStreamingLevelData {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, meta = (HideInDetailPanel))
  TArray<uint8> Data = {};
};

// Struct to hold data for a single Persistent Level and its streaming levels.
USTRUCT(BlueprintType, Blueprintable)
struct FLevelData {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, meta = (HideInDetailPanel))
  TArray<uint8> Data = {};

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TMap<TSoftObjectPtr<ULevelStreaming>, FStreamingLevelData> StreamingLevels =
      {};
};

// Struct to hold data for the entire game.
USTRUCT(BlueprintType, Blueprintable)
struct FSerializedData {
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite, meta = (HideInDetailPanel))
  TArray<uint8> Header = {};

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TSoftObjectPtr<ULevel> CurrentLevel;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TMap<TSoftObjectPtr<ULevel>, FLevelData> Levels = {};
};