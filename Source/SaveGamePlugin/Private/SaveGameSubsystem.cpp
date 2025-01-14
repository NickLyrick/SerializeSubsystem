#include "SaveGameSubsystem.h"

#include "SaveGameFunctionLibrary.h"
#include "SaveGameObject.h"
#include "SaveGameSerializer.h"

#include "EngineUtils.h"

void USaveGameSubsystem::Initialize(FSubsystemCollectionBase &Collection) {
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

void USaveGameSubsystem::Deinitialize() {
  FWorldDelegates::OnPostWorldInitialization.RemoveAll(this);
  FWorldDelegates::OnWorldInitializedActors.RemoveAll(this);
  FWorldDelegates::OnWorldCleanup.RemoveAll(this);

  FWorldDelegates::LevelAddedToWorld.RemoveAll(this);
  FWorldDelegates::PreLevelRemovedFromWorld.RemoveAll(this);
}

void USaveGameSubsystem::Save(FSerializedData &Data) {
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
  SaveStreamingLevels(*SerializedData);

  Data = *SerializedData;
}

void USaveGameSubsystem::Load(FSerializedData Data) {
  const TSoftObjectPtr<ULevel> Level = GetWorld()->GetCurrentLevel();
  *SerializedData = Data;

  {
    TSaveGameSerializer<true> BinarySerializer(this);
    BinarySerializer.DeserializeHeaderData(SerializedData->Header);
  }

  {
    const TSharedRef<TSaveGameSerializer<true>> BinarySerializer =
        MakeShared<TSaveGameSerializer<true>>(this);
    CurrentSerializer = BinarySerializer.ToSharedPtr();

    BinarySerializer->DeserializeLevelData(
        Level.Get(), SerializedData->Levels[Level.Get()].Data);
  }

  // Deserialization of Streaming Levels is done in OnMapLoad that will be
  // called after the persistent level will be loaded
}

bool USaveGameSubsystem::IsLoadingSaveGame() const {
  return CurrentSerializer.IsValid();
}

// This is called after the persistent level is loaded
void USaveGameSubsystem::OnWorldInitialized(
    UWorld *World, const UWorld::InitializationValues) {
  if (!IsValid(World) || GetWorld() != World) {
    return;
  }

  PersistentLevelRecord = MakeShared<FLevelStruct>(World);

  // Register for Actor Pre-Spawn and Destroyed handlers
  World->AddOnActorPreSpawnInitialization(
      FOnActorSpawned::FDelegate::CreateUObject(this,
                                                &ThisClass::OnActorPreSpawn));
  World->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateUObject(
      this, &ThisClass::OnActorDestroyed));
}

// This is called after the all actors of the level are initialized
void USaveGameSubsystem::OnActorsInitialized(
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

    // Check if the actor is in the persistent level or a streaming level and
    // add it to the appropriate list

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
void USaveGameSubsystem::OnWorldCleanup(UWorld *World, bool, bool) {
  if (!IsValid(World) || GetWorld() != World) {
    return;
  }

  PersistentLevelRecord->ResetActors();
}

// This is called when a streaming level is added to the world
void USaveGameSubsystem::OnLevelAddedToWorld(ULevel *Level, UWorld *World) {
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
    const TSoftObjectPtr<ULevel> CurrentLevel = GetWorld()->GetCurrentLevel();

    if (!SerializedData->Levels.Contains(CurrentLevel) ||
        !SerializedData->Levels[CurrentLevel].StreamingLevels.Contains(
            StreamingLevel)) {
      TSaveGameSerializer<false> BinarySerializer(this);
      SerializedData->Levels.FindOrAdd(CurrentLevel)
          .StreamingLevels.FindOrAdd(StreamingLevel)
          .Data = BinarySerializer.SerializeStreamingLevelData(StreamingLevel);

#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
      TSaveGameSerializer<false, true> TextSerializer(this);
      TextSerializer.SerializeStreamingLevelData(StreamingLevel);
#endif
    }

    TSharedPtr<TSaveGameSerializer<true>> BinarySerializer =
        MakeShared<TSaveGameSerializer<true>>(this);

    BinarySerializer->DeserializeStreamingLevelData(
        StreamingLevel, SerializedData->Levels[CurrentLevel]
                            .StreamingLevels[StreamingLevel]
                            .Data);
  }
}

// This is called just before the moment when a streaming level is removed from
// the world (in this case, the level is still loaded)
void USaveGameSubsystem::OnLevelRemovedFromWorld(ULevel *Level, UWorld *World) {
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
void USaveGameSubsystem::OnActorPreSpawn(AActor *Actor) {
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
void USaveGameSubsystem::OnActorDestroyed(AActor *Actor) {
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

// This is called after the persistent level is loaded and all data on the
// persistent level is deserialized
void USaveGameSubsystem::OnLoadCompleted() {
  CurrentSerializer = nullptr;

  // On this point, we have all the data from the persistent level loaded, and
  // we can load the streaming levels
  LoadStreamingLevels(*SerializedData);
}

void USaveGameSubsystem::LoadStreamingLevels(FSerializedData Data) {
  if (!SerializedData)
    return;

  const TSoftObjectPtr<ULevel> Level = GetWorld()->GetCurrentLevel();

  for (TPair StreamingLevel : PersistentLevelRecord->StreamingLevels) {
    TSharedPtr<TSaveGameSerializer<true>> BinarySerializer =
        MakeShared<TSaveGameSerializer<true>>(this);

    BinarySerializer->DeserializeStreamingLevelData(
        StreamingLevel.Key.Get(),
        Data.Levels.FindOrAdd(Level)
            .StreamingLevels.FindOrAdd(StreamingLevel.Key)
            .Data);
  }
}

void USaveGameSubsystem::SaveStreamingLevels(FSerializedData &Data) {
  const TSoftObjectPtr<ULevel> Level = GetWorld()->GetCurrentLevel();

  // Serialize Streaming Levels Data
  for (TPair StreamingLevel : PersistentLevelRecord->StreamingLevels) {
    TSaveGameSerializer<false> BinarySerializer(this);
    if (!StreamingLevel.Key->IsLevelLoaded())
      continue;

    Data.Levels.FindOrAdd(Level.Get())
        .StreamingLevels.FindOrAdd(StreamingLevel.Key)
        .Data =
        BinarySerializer.SerializeStreamingLevelData(StreamingLevel.Key);

#if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT
    TSaveGameSerializer<false, true> TextSerializer(this);
    TextSerializer.SerializeStreamingLevelData(StreamingLevel.Key);
#endif
  }
}