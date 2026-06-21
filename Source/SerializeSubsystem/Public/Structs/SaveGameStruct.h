#pragma once

#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

struct FActorsStruct
{
	TSet<TWeakObjectPtr<AActor>> SaveGame   = {};
	TSet<FSoftObjectPath>        Destroyed  = {};
};

struct FStreamingLevelStruct
{
	FString LevelName;

	// Both flags are persisted: a level can be loaded-but-not-visible (e.g., pre-warming).
	// Restoring both independently recreates the exact visibility state from the save.
	bool bIsLevelLoaded  = false;
	bool bIsLevelVisible = false;

	TSharedPtr<FActorsStruct> Actors = MakeShared<FActorsStruct>();
};

struct FLevelStruct
{
	FString LevelName;

	TSharedPtr<FActorsStruct> Actors = MakeShared<FActorsStruct>();

	TMap<TSoftObjectPtr<ULevelStreaming>, TSharedPtr<FStreamingLevelStruct>> StreamingLevels = {};

	explicit FLevelStruct(const UWorld* World)
	{
		LevelName = FPackageName::GetShortName(World->GetOutermost()->GetName());

		for (TSoftObjectPtr StreamingLevel : World->GetStreamingLevels())
		{
			StreamingLevels.Add(StreamingLevel, MakeShared<FStreamingLevelStruct>());
		}
	}

	void ResetActors()
	{
		Actors->SaveGame.Reset();
		Actors->Destroyed.Reset();

		for (const TPair StreamingLevel : StreamingLevels)
		{
			StreamingLevel.Value->Actors->SaveGame.Reset();
			StreamingLevel.Value->Actors->Destroyed.Reset();
		}
	}

	TSoftObjectPtr<ULevelStreaming> FindStreamingLevel(const ULevel* Level) const
	{
		for (const TPair StreamingLevel : StreamingLevels)
		{
			// GetWorldAssetPackageName() returns FString; GetFName() returns FName.
			// Explicit FName() conversion is required — FString == FName does not compile.
			if (FName(*StreamingLevel.Key->GetWorldAssetPackageName()) == Level->GetOutermost()->GetFName())
			{
				return StreamingLevel.Key;
			}
		}

		return nullptr;
	}
};