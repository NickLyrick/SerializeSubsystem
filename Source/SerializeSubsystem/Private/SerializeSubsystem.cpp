#include "SerializeSubsystem.h"

#include "SaveGameFunctionLibrary.h"
#include "SaveGameObject.h"
#include "SaveGameSerializer.h"

#include "EngineUtils.h"

void USerializeSubsystem::Initialize(FSubsystemCollectionBase &Collection) {
  // Persistent Levels
  FWorldDelegates::OnPostWorldInitialization.AddUObject(
      this, &ThisClass::OnWorldInitialized);
  FWorldDelegates::OnWorldInitializedActors.AddUObject(
      this, &ThisClass::OnActorsInitialized);
  FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::OnWorldCleanup);

  // Streaming Levels
  FWorldDelegates::LevelAddedToWorld.AddUObject(
      this, &ThisClass::OnLevelAddedToWorld);
  FWorldDelegates::PreLevelRemovedFromWorld.AddUObject(
      this, &ThisClass::OnLevelRemovedFromWorld);

  OnWorldInitialized(GetWorld(), UWorld::InitializationValues());
}

void USerializeSubsystem::Deinitialize() {
  FWorldDelegates::OnPostWorldInitialization.RemoveAll(this);
  FWorldDelegates::OnWorldInitializedActors.RemoveAll(this);
  FWorldDelegates::OnWorldCleanup.RemoveAll(this);

  FWorldDelegates::LevelAddedToWorld.RemoveAll(this);
  FWorldDelegates::PreLevelRemovedFromWorld.RemoveAll(this);
}

void USerializeSubsystem::Save(FSerializedData &Data) {
  if (!ensureMsgf(!IsLoadingSaveGame(),
                  TEXT("SerializeSubsystem: Save() called while a load is in "
                       "progress — ignoring."))) {
    return;
  }

  const TSoftObjectPtr<ULevel> Level = GetWorld()->GetCurrentLevel();

  { // Serialize Header Data
    TSaveGameSerializer<false> BinarySerializer(this);
    SerializedData->Header = BinarySerializer.SerializeHeaderData();
  }

  { // Serialize Level Data
    TSaveGameSerializer<false> BinarySerializer(this);

    SerializedData->CurrentLevel = Level->GetWorld()->GetCurrentLevel();

    SerializedData->Levels.FindOrAdd(Level.Get());
    SerializedData->Levels[Level.Get()].Data =
        BinarySerializer.SerializeLevelData(Level.Get());
  }

#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
  // This is for debug purposes only, we want to use binary serialization
  // for smallest file sizes
  {
    TSaveGameSerializer<false, true> TextSerializer(this);
    TextSerializer.SerializeHeaderData();
  }

  {
    TSaveGameSerializer<false, true> TextSerializer(this);

    TextSerializer.SerializeLevelData(Level.Get());
  }
#endif

  // Serialize Streaming Levels Data
  SaveStreamingLevels();

  Data = *SerializedData;
}

void USerializeSubsystem::Load(FSerializedData Data) {
  if (!ensureMsgf(!IsLoadingSaveGame(),
                  TEXT("SerializeSubsystem: Load() called while a load is in "
                       "progress — ignoring."))) {
    return;
  }

  *SerializedData = Data;

  {
    TSaveGameSerializer<true> BinarySerializer(this);
    BinarySerializer.DeserializeHeaderData(SerializedData->Header);
  }

  {
    // IMPORTANT:
    // This pointer is used to keep the serializer alive until the end of the
    // load. We use seamless travel to load the persistent level, that is kinda
    // asynchronous. We trigger loading here but the level will be serialized in
    // the OnMapLoad event.
    const TSharedRef<TSaveGameSerializer<true>> BinarySerializer =
        MakeShared<TSaveGameSerializer<true>>(this);
    CurrentSerializer = BinarySerializer.ToSharedPtr();

    BinarySerializer->DeserializeLevelData(
        SerializedData->CurrentLevel.Get(),
        SerializedData->Levels[SerializedData->CurrentLevel.Get()].Data);
  }

  // NOTICE:
  // Deserialization of Streaming Levels is done in OnMapLoad that will be
  // called after the persistent level will be loaded
}

bool USerializeSubsystem::IsLoadingSaveGame() const {
  return CurrentSerializer.IsValid();
}

#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
void USerializeSubsystem::LoadFromJson(TArray<uint8> JsonLevelData) {
  // FJsonArchiveInputFormatter parses JSON eagerly in its constructor, so the
  // data must be passed upfront rather than provided via DeserializeLevelData.
  const TSharedRef<TSaveGameSerializer<true, true>> JsonSerializer =
      MakeShared<TSaveGameSerializer<true, true>>(this,
                                                  MoveTemp(JsonLevelData));
  CurrentSerializer = JsonSerializer.ToSharedPtr();

  JsonSerializer->DeserializeLevelData(SerializedData->CurrentLevel.Get());
}
#endif

// Used to serialize the streaming levels data
void USerializeSubsystem::SaveStreamingLevels() {
  SerializedData->CurrentLevel = GetWorld()->GetCurrentLevel();

  // Serialize Streaming Levels Data
  for (TPair StreamingLevel : PersistentLevelRecord->StreamingLevels) {
    // Save the state of the streaming level
    SerializedData->Levels.FindOrAdd(SerializedData->CurrentLevel.Get())
        .StreamingLevels.FindOrAdd(StreamingLevel.Key)
        .SaveStreamingLevelState(StreamingLevel.Key);

    // We want only to serialize the streaming level if it is loaded
    // In other way we will overwrite the data of unloaded levels with empty
    // data
    if (!StreamingLevel.Key->IsLevelLoaded())
      continue;

    // Actually serialize the streaming level data
    TSaveGameSerializer<false> BinarySerializer(this);
    SerializedData->Levels.FindOrAdd(SerializedData->CurrentLevel.Get())
        .StreamingLevels.FindOrAdd(StreamingLevel.Key)
        .Data =
        BinarySerializer.SerializeStreamingLevelData(StreamingLevel.Key);

#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
    // And for Testing purposes, save some streaming levels data in text format
    TSaveGameSerializer<false, true> TextSerializer(this);
    TextSerializer.SerializeStreamingLevelData(StreamingLevel.Key);
#endif
  }
}

// Used to deserialize the streaming levels data
void USerializeSubsystem::LoadStreamingLevels() {
  if (!SerializedData.IsValid())
    return;

  for (TPair StreamingLevel : PersistentLevelRecord->StreamingLevels) {
    // Load State of Streaming Level (bIsVisible and bIsLoad)
    SerializedData->Levels.FindOrAdd(SerializedData->CurrentLevel.Get())
        .StreamingLevels.FindOrAdd(StreamingLevel.Key)
        .LoadStreamingLevelState(StreamingLevel.Key);

    // We can't deserialize the streaming level if it is not loaded
    if (!StreamingLevel.Key->IsLevelLoaded())
      continue;

    TSaveGameSerializer<true> BinarySerializer =
        TSaveGameSerializer<true>(this);
    BinarySerializer.DeserializeStreamingLevelData(
        StreamingLevel.Key.Get(),
        SerializedData->Levels.FindOrAdd(SerializedData->CurrentLevel.Get())
            .StreamingLevels.FindOrAdd(StreamingLevel.Key)
            .Data);
  }
}

// This is called after the persistent level is initialized
void USerializeSubsystem::OnWorldInitialized(
    UWorld *World, const UWorld::InitializationValues) {
  if (!IsValid(World) || GetWorld() != World) {
    return;
  }

  PersistentLevelRecord = MakeShared<FLevelStruct>(World);
  SerializedData->CurrentLevel = World->GetCurrentLevel();

  // Register for Actor Pre-Spawn and Destroyed handlers
  World->AddOnActorPreSpawnInitialization(
      FOnActorSpawned::FDelegate::CreateUObject(this,
                                                &ThisClass::OnActorPreSpawn));
  World->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateUObject(
      this, &ThisClass::OnActorDestroyed));
}

// This is called after the all actors of the level are initialized
void USerializeSubsystem::OnActorsInitialized(
    const FActorsInitializedParams &Params) {
  if (!IsValid(Params.World) || GetWorld() != Params.World) {
    return;
  }

  for (TActorIterator<AActor> It(Params.World); It; ++It) {
    AActor *Actor = *It;

    // If Actor is not valid or doesn't implement USaveGameObject, skip
    if (!(IsValid(Actor) && Actor->Implements<USaveGameObject>()))
      continue;

    TWeakObjectPtr Level = Actor->GetLevel();

    /* Check if the actor is in the persistent level or a streaming level and
     * add it to the appropriate list
     * */

    if (Level.Get() == Params.World->GetCurrentLevel()) {
      PersistentLevelRecord->Actors->SaveGame.Add(Actor);
    }

    for (TSoftObjectPtr<ULevelStreaming> StreamingLevel :
         Params.World->GetStreamingLevels()) {
      const bool bStreamingLevel = StreamingLevel->GetWorldAssetPackageName() ==
                                   Level->GetOutermost()->GetFName();
      if (StreamingLevel.IsValid() && bStreamingLevel) {
        PersistentLevelRecord->StreamingLevels[StreamingLevel]
            ->Actors->SaveGame.Add(Actor);
      }
    }
  }
}

// This is called when the world is cleaned up
void USerializeSubsystem::OnWorldCleanup(UWorld *World, bool, bool) {
  if (!IsValid(World) || GetWorld() != World) {
    return;
  }

  PersistentLevelRecord->ResetActors();
}

// This is called when a streaming level is added to the world
void USerializeSubsystem::OnLevelAddedToWorld(ULevel *Level, UWorld *World) {
  if (!IsValid(Level) || GetWorld() != World)
    return;

  const TSoftObjectPtr<ULevelStreaming> StreamingLevel =
      PersistentLevelRecord->FindStreamingLevel(Level);

  for (AActor *Actor : Level->Actors) {
    if (IsValid(Actor) && Actor->Implements<USaveGameObject>()) {
      PersistentLevelRecord->StreamingLevels[StreamingLevel]
          ->Actors->SaveGame.Add(Actor);
    }
  }

  if (SerializedData.IsValid()) {
    const bool bIsStreamingLevelDataExists =
        SerializedData->Levels.Contains(SerializedData->CurrentLevel) &&
        SerializedData->Levels[SerializedData->CurrentLevel]
            .StreamingLevels.Contains(StreamingLevel);

    if (bIsStreamingLevelDataExists) {
      // If we have data for the streaming level, deserialize it
      // One of the edge cases. When we already store streaming level state but
      // never serialize it
      if (SerializedData->Levels[SerializedData->CurrentLevel]
              .StreamingLevels[StreamingLevel]
              .Data.IsEmpty())
        return;

      TSaveGameSerializer<true> BinarySerializer =
          TSaveGameSerializer<true>(this);
      BinarySerializer.DeserializeStreamingLevelData(
          StreamingLevel, SerializedData->Levels[SerializedData->CurrentLevel]
                              .StreamingLevels[StreamingLevel]
                              .Data);
    }
  }
}

// This is called just before the moment when a streaming level is removed from
// the world (in this case, the level is still loaded)
void USerializeSubsystem::OnLevelRemovedFromWorld(ULevel *Level,
                                                  UWorld *World) {
  if (!IsValid(Level) || GetWorld() != World)
    return;

  const TSoftObjectPtr<ULevel> LevelCurrent = GetWorld()->GetCurrentLevel();
  const TSoftObjectPtr<ULevelStreaming> StreamingLevel =
      PersistentLevelRecord->FindStreamingLevel(Level);

  if (SerializedData.IsValid()) {
    TSaveGameSerializer<false> BinarySerializer(this);

    SerializedData->Levels.FindOrAdd(LevelCurrent)
        .StreamingLevels.FindOrAdd(StreamingLevel)
        .Data = BinarySerializer.SerializeStreamingLevelData(StreamingLevel);

#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
    TSaveGameSerializer<false, true> TextSerializer(this);
    TextSerializer.SerializeStreamingLevelData(StreamingLevel);
#endif
  }

  PersistentLevelRecord->StreamingLevels[StreamingLevel]
      ->Actors->SaveGame.Reset();
  PersistentLevelRecord->StreamingLevels[StreamingLevel]
      ->Actors->Destroyed.Reset();
}

// This is called just before an actor is spawned
void USerializeSubsystem::OnActorPreSpawn(AActor *Actor) {
  if (!IsValid(Actor))
    return;

  if (Actor->Implements<USaveGameObject>()) {
    const ULevel *Level = Actor->GetLevel();
    if (!IsValid(Level))
      return;

    if (Level == Actor->GetWorld()->GetCurrentLevel()) {
      PersistentLevelRecord->Actors->SaveGame.Add(Actor);
      return;
    }

    for (TSoftObjectPtr<ULevelStreaming> StreamingLevel :
         Actor->GetWorld()->GetStreamingLevels()) {
      if (StreamingLevel.IsValid() &&
          StreamingLevel->GetWorldAssetPackageName() ==
              Level->GetOutermost()->GetFName()) {
        PersistentLevelRecord->StreamingLevels[StreamingLevel]
            ->Actors->SaveGame.Add(Actor);
        return;
      }
    }
  }
}

// This is called just before an actor is destroyed
void USerializeSubsystem::OnActorDestroyed(AActor *Actor) {
  if (PersistentLevelRecord->Actors->SaveGame.Remove(Actor)) {
    if (USaveGameFunctionLibrary::WasObjectLoaded(Actor))
      PersistentLevelRecord->Actors->Destroyed.Add(Actor);
  }

  for (const TPair StreamingLevel : PersistentLevelRecord->StreamingLevels) {
    if (StreamingLevel.Value->Actors->SaveGame.Remove(Actor)) {
      if (USaveGameFunctionLibrary::WasObjectLoaded(Actor))
        StreamingLevel.Value->Actors->Destroyed.Add(Actor);
    }
  }
}

void USerializeSubsystem::FinalizeLoad(ESaveGameLoadResult Result) {
  CurrentSerializer = nullptr;

  OnLoadCompleted.Broadcast(Result);

  if (Result == ESaveGameLoadResult::Success && SerializedData.IsValid())
    LoadStreamingLevels();
}
