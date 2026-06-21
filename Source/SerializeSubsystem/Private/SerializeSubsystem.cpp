#include "SerializeSubsystem.h"

#include "EngineUtils.h"
#include "SaveGameFunctionLibrary.h"
#include "SaveGameObject.h"
#include "SaveGameSerializer.h"

void USerializeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	// Persistent Levels
	FWorldDelegates::OnPostWorldInitialization.AddUObject(this, &ThisClass::OnWorldInitialized);
	FWorldDelegates::OnWorldInitializedActors.AddUObject(this, &ThisClass::OnActorsInitialized);
	FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::OnWorldCleanup);

	// Streaming Levels
	FWorldDelegates::LevelAddedToWorld.AddUObject(this, &ThisClass::OnLevelAddedToWorld);
	FWorldDelegates::PreLevelRemovedFromWorld.AddUObject(this, &ThisClass::OnLevelRemovedFromWorld);

	OnWorldInitialized(GetWorld(), UWorld::InitializationValues());
}

void USerializeSubsystem::Deinitialize()
{
	FWorldDelegates::OnPostWorldInitialization.RemoveAll(this);
	FWorldDelegates::OnWorldInitializedActors.RemoveAll(this);
	FWorldDelegates::OnWorldCleanup.RemoveAll(this);

	FWorldDelegates::LevelAddedToWorld.RemoveAll(this);
	FWorldDelegates::PreLevelRemovedFromWorld.RemoveAll(this);
}

void USerializeSubsystem::Save(FSerializedData& Data)
{
	if (!ensureMsgf(!IsLoadingSaveGame(),
	                TEXT("SerializeSubsystem: Save() called while a load is in "
	                     "progress — ignoring.")))
	{
		return;
	}

	const bool bTextFormat = Data.bIsTextFormat;
	const TSoftObjectPtr<ULevel> Level = GetWorld()->GetCurrentLevel();

	SerializedData->LevelName = Level->GetOutermost()->GetLoadedPath().GetPackageName();
	SerializedData->Levels.FindOrAdd(SerializedData->LevelName);
	SerializedData->bIsTextFormat = bTextFormat;

#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
	if (bTextFormat)
	{
		{
			TSaveGameSerializer<false, true> S(this);
			SerializedData->Header = S.SerializeHeaderData();
		}
		{
			TSaveGameSerializer<false, true> S(this);
			SerializedData->Levels[SerializedData->LevelName].Data = S.SerializeLevelData(Level);
		}
	}
	else
#endif
	{
		{
			TSaveGameSerializer<false> BinarySerializer(this);
			SerializedData->Header = BinarySerializer.SerializeHeaderData();
		}
		{
			TSaveGameSerializer<false> BinarySerializer(this);
			SerializedData->Levels[SerializedData->LevelName].Data = BinarySerializer.SerializeLevelData(Level);
		}
	}

	// Serialize Streaming Levels Data
	SaveStreamingLevels();

	Data = *SerializedData;
}

void USerializeSubsystem::Load(const FSerializedData Data)
{
	if (!ensureMsgf(!IsLoadingSaveGame(),
	                TEXT("SerializeSubsystem: Load() called while a load is in "
	                     "progress — ignoring.")))
	{
		return;
	}

	*SerializedData = Data;
	PendingDefaultMigrations.Reset();
	PendingDefaultMigrationClasses.Reset();

#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
	if (Data.bIsTextFormat)
	{
		// Header: temporary serializer, applies custom versions; no need to keep alive.
		// FJsonArchiveInputFormatter parses JSON eagerly at construction, so each block
		// needs its own serializer instance with its own byte buffer.
		TSaveGameSerializer<true, true> HeaderSerializer(this, SerializedData->Header);
		HeaderSerializer.DeserializeHeaderData();

		// Level: must stay alive across SeamlessTravel until OnMapLoad fires.
		const TSharedRef<TSaveGameSerializer<true, true>> JsonSerializer =
		    MakeShared<TSaveGameSerializer<true, true>>(this, SerializedData->Levels[SerializedData->LevelName].Data);
		CurrentSerializer = JsonSerializer.ToSharedPtr();

		JsonSerializer->DeserializeLevelData(SerializedData->LevelName);
		return;
	}
#endif

	{
		TSaveGameSerializer<true> BinarySerializer(this);
		BinarySerializer.DeserializeHeaderData(SerializedData->Header);
	}

	{
		// CurrentSerializer keeps the serializer alive across the asynchronous SeamlessTravel:
		// we trigger level loading here, but OnMapLoad (where actors are actually deserialized)
		// fires on a future frame — the serializer must not be GC'd in between.
		const TSharedRef<TSaveGameSerializer<true>> BinarySerializer = MakeShared<TSaveGameSerializer<true>>(this);
		CurrentSerializer = BinarySerializer.ToSharedPtr();

		BinarySerializer->DeserializeLevelData(SerializedData->LevelName,
		                                       SerializedData->Levels[SerializedData->LevelName].Data);
	}

	// NOTICE:
	// Deserialization of Streaming Levels is done in OnMapLoad that will be
	// called after the persistent level will be loaded
}

bool USerializeSubsystem::IsLoadingSaveGame() const
{
	return CurrentSerializer.IsValid();
}

// Used to serialize the streaming levels data
void USerializeSubsystem::SaveStreamingLevels()
{
	const TSoftObjectPtr<ULevel> Level = GetWorld()->GetCurrentLevel();
	const FString LevelName = Level->GetOutermost()->GetLoadedPath().GetPackageName();

	// Serialize Streaming Levels Data
	for (TPair StreamingLevel : PersistentLevelRecord->StreamingLevels)
	{
		// Save the state of the streaming level
		SerializedData->Levels.FindOrAdd(LevelName)
		    .StreamingLevels.FindOrAdd(StreamingLevel.Key)
		    .SaveStreamingLevelState(StreamingLevel.Key);

		// We want only to serialize the streaming level if it is loaded
		// In other way we will overwrite the data of unloaded levels with empty
		// data
		if (!StreamingLevel.Key->IsLevelLoaded())
			continue;

		// Actually serialize the streaming level data
#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
		if (SerializedData->bIsTextFormat)
		{
			TSaveGameSerializer<false, true> S(this);
			SerializedData->Levels.FindOrAdd(LevelName).StreamingLevels.FindOrAdd(StreamingLevel.Key).Data =
			    S.SerializeStreamingLevelData(StreamingLevel.Key);
		}
		else
#endif
		{
			TSaveGameSerializer<false> BinarySerializer(this);
			SerializedData->Levels.FindOrAdd(LevelName).StreamingLevels.FindOrAdd(StreamingLevel.Key).Data =
			    BinarySerializer.SerializeStreamingLevelData(StreamingLevel.Key);
		}
	}
}

// Used to deserialize the streaming levels data
void USerializeSubsystem::LoadStreamingLevels()
{
	if (!SerializedData.IsValid())
		return;

	const FString LevelName = GetWorld()->GetCurrentLevel()->GetOutermost()->GetLoadedPath().GetPackageName();

	for (TPair StreamingLevel : PersistentLevelRecord->StreamingLevels)
	{
		// Load State of Streaming Level (bIsVisible and bIsLoad)
		SerializedData->Levels.FindOrAdd(LevelName)
		    .StreamingLevels.FindOrAdd(StreamingLevel.Key)
		    .LoadStreamingLevelState(StreamingLevel.Key);

		// We can't deserialize the streaming level if it is not loaded
		if (!StreamingLevel.Key->IsLevelLoaded())
			continue;

		TSaveGameSerializer<true> BinarySerializer = TSaveGameSerializer<true>(this);
		BinarySerializer.DeserializeStreamingLevelData(
		    StreamingLevel.Key.Get(),
		    SerializedData->Levels.FindOrAdd(LevelName).StreamingLevels.FindOrAdd(StreamingLevel.Key).Data);
	}
}

// This is called after the persistent level is initialized
void USerializeSubsystem::OnWorldInitialized(UWorld* World, const UWorld::InitializationValues)
{
	if (!IsValid(World) || GetWorld() != World)
	{
		return;
	}

	PersistentLevelRecord = MakeShared<FLevelStruct>(World);
	SerializedData->LevelName = GetWorld()->GetCurrentLevel()->GetOutermost()->GetLoadedPath().GetPackageName();

	// Register for Actor Pre-Spawn and Destroyed handlers
	World->AddOnActorPreSpawnInitialization(
	    FOnActorSpawned::FDelegate::CreateUObject(this, &ThisClass::OnActorPreSpawn));
	World->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateUObject(this, &ThisClass::OnActorDestroyed));
}

// This is called after the all actors of the level are initialized
void USerializeSubsystem::OnActorsInitialized(const FActorsInitializedParams& Params) const
{
	if (!IsValid(Params.World) || GetWorld() != Params.World)
	{
		return;
	}

	for (TActorIterator<AActor> It(Params.World); It; ++It)
	{
		AActor* Actor = *It;

		// If Actor is not valid or doesn't implement USaveGameObject, skip
		if (!(IsValid(Actor) && Actor->Implements<USaveGameObject>()))
			continue;

		TWeakObjectPtr Level = Actor->GetLevel();

		/* Check if the actor is in the persistent level or a streaming level and
		 * add it to the appropriate list
		 * */

		if (Level.Get() == Params.World->GetCurrentLevel())
		{
			PersistentLevelRecord->Actors->SaveGame.Add(Actor);
		}

		for (TSoftObjectPtr<ULevelStreaming> StreamingLevel : Params.World->GetStreamingLevels())
		{
			const bool bStreamingLevel =
			    StreamingLevel->GetWorldAssetPackageName() == Level->GetOutermost()->GetFName();
			if (StreamingLevel.IsValid() && bStreamingLevel)
			{
				PersistentLevelRecord->StreamingLevels[StreamingLevel]->Actors->SaveGame.Add(Actor);
			}
		}
	}
}

// This is called when the world is cleaned up
void USerializeSubsystem::OnWorldCleanup(UWorld* World, bool, bool) const
{
	if (!IsValid(World) || GetWorld() != World)
	{
		return;
	}

	PersistentLevelRecord->ResetActors();
}

// This is called when a streaming level is added to the world
void USerializeSubsystem::OnLevelAddedToWorld(ULevel* Level, UWorld* World)
{
	if (!IsValid(Level) || GetWorld() != World)
		return;

	const TSoftObjectPtr<ULevelStreaming> StreamingLevel = PersistentLevelRecord->FindStreamingLevel(Level);

	if (!StreamingLevel.IsValid())
		return;

	FString LevelName = GetWorld()->GetCurrentLevel()->GetOutermost()->GetLoadedPath().GetPackageName();

	for (AActor* Actor : Level->Actors)
	{
		if (IsValid(Actor) && Actor->Implements<USaveGameObject>())
		{
			PersistentLevelRecord->StreamingLevels[StreamingLevel]->Actors->SaveGame.Add(Actor);
		}
	}

	if (SerializedData.IsValid())
	{
		const bool bIsStreamingLevelDataExists =
		    SerializedData->Levels.Contains(LevelName) &&
		    SerializedData->Levels[LevelName].StreamingLevels.Contains(StreamingLevel);

		if (bIsStreamingLevelDataExists)
		{
			// If we have data for the streaming level, deserialize it
			// One of the edge cases. When we already store streaming level state but
			// never serialize it
			if (SerializedData->Levels[LevelName].StreamingLevels[StreamingLevel].Data.IsEmpty())
				return;

			TSaveGameSerializer<true> BinarySerializer = TSaveGameSerializer<true>(this);
			BinarySerializer.DeserializeStreamingLevelData(
			    StreamingLevel, SerializedData->Levels[LevelName].StreamingLevels[StreamingLevel].Data);
		}
	}
}

// This is called just before the moment when a streaming level is removed from
// the world (in this case, the level is still loaded)
void USerializeSubsystem::OnLevelRemovedFromWorld(ULevel* Level, UWorld* World)
{
	if (!IsValid(Level) || GetWorld() != World)
		return;

	// SeamlessTravel we initiated calls PreLevelRemovedFromWorld before the new map is ready.
	// Without this guard, unloading the old level would overwrite its serialized data with an empty snapshot.
	if (IsLoadingSaveGame())
		return;

	const TSoftObjectPtr<ULevel> LevelCurrent = GetWorld()->GetCurrentLevel();
	const FString LevelName = LevelCurrent->GetOutermost()->GetLoadedPath().GetPackageName();
	const TSoftObjectPtr<ULevelStreaming> StreamingLevel = PersistentLevelRecord->FindStreamingLevel(Level);

	// If there is no data for the streaming level return early
	if (!StreamingLevel.IsValid())
		return;

	if (SerializedData.IsValid())
	{
#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
		if (SerializedData->bIsTextFormat)
		{
			TSaveGameSerializer<false, true> S(this);
			SerializedData->Levels.FindOrAdd(LevelName).StreamingLevels.FindOrAdd(StreamingLevel).Data =
			    S.SerializeStreamingLevelData(StreamingLevel);
		}
		else
#endif
		{
			TSaveGameSerializer<false> BinarySerializer(this);
			SerializedData->Levels.FindOrAdd(LevelName).StreamingLevels.FindOrAdd(StreamingLevel).Data =
			    BinarySerializer.SerializeStreamingLevelData(StreamingLevel);
		}
	}

	PersistentLevelRecord->StreamingLevels[StreamingLevel]->Actors->SaveGame.Reset();
	PersistentLevelRecord->StreamingLevels[StreamingLevel]->Actors->Destroyed.Reset();
}

// This is called just before an actor is spawned
void USerializeSubsystem::OnActorPreSpawn(AActor* Actor) const
{
	if (!IsValid(Actor))
		return;

	if (Actor->Implements<USaveGameObject>())
	{
		const ULevel* Level = Actor->GetLevel();
		if (!IsValid(Level))
			return;

		if (Level == Actor->GetWorld()->GetCurrentLevel())
		{
			PersistentLevelRecord->Actors->SaveGame.Add(Actor);
			return;
		}

		for (TSoftObjectPtr<ULevelStreaming> StreamingLevel : Actor->GetWorld()->GetStreamingLevels())
		{
			if (StreamingLevel.IsValid() &&
			    StreamingLevel->GetWorldAssetPackageName() == Level->GetOutermost()->GetFName())
			{
				PersistentLevelRecord->StreamingLevels[StreamingLevel]->Actors->SaveGame.Add(Actor);
				return;
			}
		}
	}
}

// This is called just before an actor is destroyed
void USerializeSubsystem::OnActorDestroyed(AActor* Actor) const
{
	if (PersistentLevelRecord->Actors->SaveGame.Remove(Actor))
	{
		if (USaveGameFunctionLibrary::WasObjectLoaded(Actor))
			PersistentLevelRecord->Actors->Destroyed.Add(Actor);
	}

	for (const TPair StreamingLevel : PersistentLevelRecord->StreamingLevels)
	{
		if (StreamingLevel.Value->Actors->SaveGame.Remove(Actor))
		{
			if (USaveGameFunctionLibrary::WasObjectLoaded(Actor))
				StreamingLevel.Value->Actors->Destroyed.Add(Actor);
		}
	}
}

void USerializeSubsystem::FinalizeLoad(ESaveGameLoadResult Result)
{
	CurrentSerializer = nullptr;

	OnLoadCompleted.Broadcast(Result);

	if (Result == ESaveGameLoadResult::Success && SerializedData.IsValid())
		LoadStreamingLevels();
}
