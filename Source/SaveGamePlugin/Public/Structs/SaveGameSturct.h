#pragma once

// Struct to hold data for a single streaming level.
struct FActorsStruct {
  TSet<TWeakObjectPtr<AActor>> SaveGame = {};
  TSet<FSoftObjectPath> Destroyed = {};
};

struct FStreamingLevelStruct {
  FString LevelName;

  bool bIsLevelLoaded = false;
  bool bIsLevelVisible = false;

  TSharedPtr<FActorsStruct> Actors = MakeShared<FActorsStruct>();
};

struct FLevelStruct {
  FString LevelName;

  TSharedPtr<FActorsStruct> Actors = MakeShared<FActorsStruct>();

  TMap<TSoftObjectPtr<ULevelStreaming>, TSharedPtr<FStreamingLevelStruct>>
      StreamingLevels = {};

  explicit FLevelStruct(const UWorld *World) {
    LevelName = FPackageName::GetShortName(World->GetOutermost()->GetName());

    for (TSoftObjectPtr StreamingLevel : World->GetStreamingLevels()) {
      StreamingLevels.Add(StreamingLevel, MakeShared<FStreamingLevelStruct>());
    }
  }

  void ResetActors() {
    Actors->SaveGame.Reset();
    Actors->Destroyed.Reset();

    for (const TPair StreamingLevel : StreamingLevels) {
      StreamingLevel.Value->Actors->SaveGame.Reset();
      StreamingLevel.Value->Actors->Destroyed.Reset();
    }
  }

  TSoftObjectPtr<ULevelStreaming>
  FindStreamingLevel(const ULevel *Level) const {
    for (const TPair StreamingLevel : StreamingLevels) {
      if (StreamingLevel.Key->GetWorldAssetPackageName() ==
          Level->GetOutermost()->GetFName()) {
        return StreamingLevel.Key;
      }
    }

    return nullptr;
  }
};