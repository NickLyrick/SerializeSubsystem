#include "SaveGameSerializer.h"

#include "Engine/Level.h"
#include "Misc/EngineVersion.h"
#include "Serialization/CustomVersion.h"
#include "UObject/Package.h"

#include "SaveGameFunctionLibrary.h"
#include "SaveGameObject.h"
#include "SaveGameVersion.h"
#include "SerializeSubsystem.h"

#include "PlatformFeatures.h"
#include "SaveGameSystem.h"

#define LEVEL_SUBPATH_PREFIX TEXT("PersistentLevel.")

// Template function to serialize or deserialize compressed data.
// The `bLoading` parameter determines whether the operation is for loading
// (deserialization) or saving (serialization).
template <bool bLoading>
FORCEINLINE_DEBUGGABLE void SerializeCompressedData(FArchive &Ar,
                                                    TArray<uint8> &Data) {
  // Ensure that the archive's current mode matches the operation type specified
  // by `bLoading`. If `bLoading` is true, the archive must be in loading mode
  // (`Ar.IsLoading()` must return true), otherwise, it must be in saving mode.
  check(Ar.IsLoading() == bLoading);

  // Declare a variable to hold the size of the uncompressed data.
  int64 UncompressedSize;

  // If we are saving data (not loading), calculate the size of the uncompressed
  // data.
  if (!bLoading) {
    UncompressedSize =
        Data.Num(); // Get the number of elements in the `Data` array.
  }

  // Serialize the `UncompressedSize` to or from the archive.
  // During saving, this writes the size into the archive.
  // During loading, this reads the size from the archive.
  Ar << UncompressedSize;

  // If we are in loading mode, allocate enough space in `Data` to hold the
  // uncompressed data.
  if (bLoading) {
    Data.SetNumUninitialized(UncompressedSize);
  }

  // Serialize the compressed data to or from the archive.
  // `Data.GetData()` returns a pointer to the array's internal storage.
  // `UncompressedSize` is the size of the uncompressed data.
  // `NAME_Zlib` specifies the compression format used (Zlib in this case).
  Ar.SerializeCompressed(Data.GetData(), UncompressedSize, NAME_Zlib);
}

// Template constructor for the `TSaveGameSerializer` class.
// This constructor initializes various members to facilitate structured
// serialization or deserialization. The template parameters:
// - `bIsLoading`: Indicates whether this serializer is for loading (true) or
// saving (false).
// - `bIsTextFormat`: Specifies if the format used is text-based (true) or
// binary (false).
template <bool bIsLoading, bool bIsTextFormat>
TSaveGameSerializer<bIsLoading, bIsTextFormat>::TSaveGameSerializer(
    USerializeSubsystem *InSerializeSubsystem, TArray<uint8> InInitialData)
    : SerializeSubsystem(InSerializeSubsystem),
      Data(MoveTemp(InInitialData)), // For JSON loading: JSON bytes must be
                                     // here before Formatter is constructed,
                                     // because FJsonArchiveInputFormatter
                                     // parses the JSON eagerly in its ctor.
      Archive(Data),
      ProxyArchive(Archive),
      Formatter(ProxyArchive),
      StructuredArchive(Formatter),
      RootSlot(StructuredArchive.Open()),
      RootRecord(RootSlot.EnterRecord()),
      VersionOffset(0),
      HeaderOffset(0)
{
  static_cast<FArchive &>(ProxyArchive).SetIsTextFormat(bIsTextFormat);

  // TODO: Look
  Archive.UsingCustomVersion(FSaveGameVersion::GUID);
}

template <bool bIsLoading, bool bIsTextFormat>
TArray<uint8>
TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeHeaderData() {
  check(!bIsLoading);

  TRACE_BOOKMARK(TEXT("Begin: SerializeHeaderData[%s]"),
                 bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

  ON_SCOPE_EXIT {
    TRACE_BOOKMARK(TEXT("End: SerializeHeaderData[%s]"),
                   bIsTextFormat ? TEXT("Text") : TEXT("Binary"));
  };

  check(SerializeSubsystem.IsValid());

  SerializeHeader();

  // Be sure to close this, as you'll be missing closed braces for JSON
  // archives
  StructuredArchive.Close();

  if (!bIsTextFormat && !bIsLoading) {
    // Compress the save game data
    TArray<uint8> CompressedData;
    FSaveGameMemoryArchive CompressorArchive(CompressedData);
    SerializeCompressedData<false>(CompressorArchive, Data);

    return CompressedData;
  }

  ISaveGameSystem *SaveSystem =
      IPlatformFeaturesModule::Get().GetSaveGameSystem();
  if (bIsTextFormat && SaveSystem) {
    SaveSystem->SaveGame(false, TEXT("Header.json"), 0, Data);
  }

  return Data;
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeHeaderData(
    TArray<uint8> &HeaderData) {
  check(bIsLoading);

  TRACE_BOOKMARK(TEXT("Begin: DeserializeHeaderData[%s]"),
                 bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

  if (!bIsTextFormat) {
    FSaveGameMemoryArchive CompressorArchive(HeaderData);
    SerializeCompressedData<true>(CompressorArchive, Data);
  }
  // JSON: data was provided at construction, nothing to decompress.

  SerializeHeader();
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeHeaderData() {
  static_assert(bIsLoading && bIsTextFormat,
                "No-param DeserializeHeaderData is for JSON loading only. "
                "For binary loading use DeserializeHeaderData(TArray<uint8>&).");
  SerializeHeader();
}

template <bool bIsLoading, bool bIsTextFormat>
TArray<uint8>
TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeLevelData(
    TSoftObjectPtr<ULevel> Level) {
  check(!bIsLoading);

  TRACE_BOOKMARK(TEXT("Begin: SerializeLevelData[%s]"),
                 bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

  ON_SCOPE_EXIT {
    TRACE_BOOKMARK(TEXT("End: SerializeLevelData[%s]"),
                   bIsTextFormat ? TEXT("Text") : TEXT("Binary"));
  };

  check(SerializeSubsystem.IsValid());

  SerializeLevel(Level);

  // Be sure to close this, as you'll be missing closed braces for JSON
  // archives
  StructuredArchive.Close();

  if (!bIsTextFormat && !bIsLoading) {
    // Compress the save game data
    TArray<uint8> CompressedData;
    FSaveGameMemoryArchive CompressorArchive(CompressedData);
    SerializeCompressedData<false>(CompressorArchive, Data);

    return CompressedData;
  }

  ISaveGameSystem *SaveSystem =
      IPlatformFeaturesModule::Get().GetSaveGameSystem();
  if (bIsTextFormat && SaveSystem) {
    const FString SaveName =
        FPackageName::GetShortName(Level->GetOutermost()->GetName()) + ".json";
    SaveSystem->SaveGame(false, *SaveName, 0, Data);
  }

  return Data;
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::InitiateLevelLoad(
    const TSoftObjectPtr<ULevel> &Level) {
  const FString MapName =
      Level->GetOutermost()->GetLoadedPath().GetPackageName();

  if (MapName.IsEmpty()) {
    UE_LOG(LogTemp, Error,
           TEXT("SerializeSubsystem: Cannot load level — map name is empty."));
    return;
  }

  check(SerializeSubsystem.IsValid());
  UWorld *World = SerializeSubsystem->GetWorld();

  if (World->IsInSeamlessTravel()) {
    return;
  }

  FCoreUObjectDelegates::PostLoadMapWithWorld.AddThreadSafeSP(
      this, &TSaveGameSerializer::OnMapLoad);

  World->SeamlessTravel(MapName, true);
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeLevelData(
    TSoftObjectPtr<ULevel> Level, TArray<uint8> &LevelData) {
  if (!bIsTextFormat) {
    FSaveGameMemoryArchive CompressorArchive(LevelData);
    SerializeCompressedData<true>(CompressorArchive, Data);
  }
  // JSON: data was provided at construction.

  InitiateLevelLoad(Level);
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeLevelData(
    TSoftObjectPtr<ULevel> Level) {
  static_assert(bIsLoading && bIsTextFormat,
                "No-param DeserializeLevelData is for JSON loading only. "
                "For binary loading use DeserializeLevelData(Level, TArray&).");
  InitiateLevelLoad(Level);
}

template <bool bIsLoading, bool bIsTextFormat>
TArray<uint8>
TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeStreamingLevelData(
    const TSoftObjectPtr<ULevelStreaming> &StreamingLevel) {
  check(!bIsLoading);

  TRACE_BOOKMARK(TEXT("Begin: SerializeStreamingLevel[%s]"),
                 bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

  ON_SCOPE_EXIT {
    TRACE_BOOKMARK(TEXT("End: SerializeStreamingLevel[%s]"),
                   bIsTextFormat ? TEXT("Text") : TEXT("Binary"));
  };

  check(SerializeSubsystem.IsValid());

  // Double check that the level is loaded
  // This is to ensure that the level is loaded before we serialize the actors
  if (StreamingLevel->IsLevelLoaded())
    SerializeStreamingLevel(StreamingLevel);

  // Be sure to close this, as you'll be missing closed braces for JSON
  // archives
  StructuredArchive.Close();

  if (!bIsTextFormat && !bIsLoading) {
    // Compress the save game data
    TArray<uint8> CompressedData;
    FSaveGameMemoryArchive CompressorArchive(CompressedData);
    SerializeCompressedData<false>(CompressorArchive, Data);

    return CompressedData;
  }

  ISaveGameSystem *SaveSystem =
      IPlatformFeaturesModule::Get().GetSaveGameSystem();
  if (bIsTextFormat && SaveSystem) {
    const FString SaveName =
        FPackageName::GetShortName(StreamingLevel->GetWorldAssetPackageName()) +
        ".json";
    SaveSystem->SaveGame(false, *SaveName, 0, Data);
  }

  return Data;
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::
    DeserializeStreamingLevelData(
        const TSoftObjectPtr<ULevelStreaming> &StreamingLevel,
        TArray<uint8> &StreamingLevelData) {
  check(bIsLoading);

  TRACE_BOOKMARK(TEXT("Begin: DeserializeStreamingLevelData[%s]"),
                 bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

  if (!bIsTextFormat) {
    FSaveGameMemoryArchive CompressorArchive(StreamingLevelData);
    SerializeCompressedData<true>(CompressorArchive, Data);
  }
  // JSON: data was provided at construction.

  if (StreamingLevel->IsLevelLoaded()) {
    SerializeStreamingLevel(StreamingLevel);
  }
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::
    DeserializeStreamingLevelData(
        const TSoftObjectPtr<ULevelStreaming> &StreamingLevel) {
  static_assert(
      bIsLoading && bIsTextFormat,
      "No-param DeserializeStreamingLevelData is for JSON loading only. "
      "For binary use DeserializeStreamingLevelData(Level, TArray&).");

  if (StreamingLevel->IsLevelLoaded()) {
    SerializeStreamingLevel(StreamingLevel);
  }
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::OnMapLoad(UWorld *World) {
  FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
  check(SerializeSubsystem->GetWorld() == World);

  SerializeLevel(World->GetCurrentLevel());

  SerializeSubsystem->OnLoadCompleted();

  TRACE_BOOKMARK(TEXT("End: LoadSaveGame[%s]"),
                 bIsTextFormat ? TEXT("Text") : TEXT("Binary"));
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeHeader() {
  QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeHeader);

  if (!bIsTextFormat) {
    // Store the version position so that we can serialize it in the header
    HeaderOffset = Archive.Tell();
  }

  FEngineVersion EngineVersion;
  FPackageFileVersion PackageVersion;

  if (!bIsLoading) {
    EngineVersion = FEngineVersion::Current();
    PackageVersion = GPackageFileUEVersion;
  }

  RootRecord << SA_VALUE(TEXT("EngineVersion"), EngineVersion);

  if (!bIsTextFormat) {
    // This doesn't have a structured archive serialize method
    Archive << PackageVersion;

    // We're a binary archive, so let's serialize where the version is
    // so that we can read it before loading anything
    RootRecord << SA_VALUE(TEXT("VersionsOffset"), VersionOffset);
  }

  if (bIsLoading) {
    Archive.SetEngineVer(EngineVersion);
    Archive.SetUEVer(PackageVersion);
  }
}

// SERIALIZE PERSISTENT LEVEL
template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeLevel(
    const TSoftObjectPtr<ULevel> &Level) {

  if (Level.IsValid()) {
    FStructuredArchive::FSlot ActorsSlot =
        RootRecord.EnterField(TEXT("Actors"));

    SerializeActors(Level.Get(),
                    SerializeSubsystem->PersistentLevelRecord->Actors->SaveGame,
                    ActorsSlot);

    FStructuredArchive::FSlot DestroyedActorsSlot =
        RootRecord.EnterField(TEXT("DestroyedActors"));
    SerializeDestroyedActors(Level.Get(), DestroyedActorsSlot);
  }
}

// SERIALIZE STREAMING LEVEL
template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeStreamingLevel(
    const TSoftObjectPtr<ULevelStreaming> &StreamingLevel) {

  if (StreamingLevel.IsValid()) {
    FStructuredArchive::FSlot ActorsSlot =
        RootRecord.EnterField(TEXT("Actors"));

    SerializeActors(StreamingLevel->GetLoadedLevel(),
                    SerializeSubsystem->PersistentLevelRecord
                        ->StreamingLevels[StreamingLevel]
                        ->Actors->SaveGame,
                    ActorsSlot);

    // TODO: Ensure that is working
    FStructuredArchive::FSlot DestroyedActorsSlot =
        RootRecord.EnterField(TEXT("DestroyedActors"));
    SerializeDestroyedActors(StreamingLevel->GetLoadedLevel(),
                             DestroyedActorsSlot);
  }
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActors(
    ULevel *Level, TSet<TWeakObjectPtr<AActor>> &SaveGameActors,
    FStructuredArchive::FSlot &ActorsSlot) {
  QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeActors);
  check(SerializeSubsystem.IsValid());

  UWorld *World = SerializeSubsystem->GetWorld();
  if (!IsValid(World))
    return;

  if (!IsValid(Level))
    return;

  TArray<AActor *> Actors;
  int32 NumActors = SaveGameActors.Num();

  FStructuredArchive::FMap ActorsMap = ActorsSlot.EnterMap(NumActors);

  const uint64 ActorsPosition = Archive.Tell();

  if (bIsLoading) {
    TMap<FGuid, AActor *> SpawnIDs;
    QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_InitializeActors);

    for (const TWeakObjectPtr<AActor> &ActorPtr : SaveGameActors) {
      AActor *Actor = ActorPtr.Get();
      if (IsValid(Actor) && Actor->Implements<USaveGameSpawnActor>()) {
        const FGuid SpawnID = ISaveGameSpawnActor::Execute_GetSpawnID(Actor);
        if (SpawnID.IsValid()) {
          SpawnIDs.Add(SpawnID, Actor);
        }
      }
    }

    Actors.SetNumZeroed(NumActors);

    for (int32 ActorIdx = 0; ActorIdx < NumActors; ++ActorIdx) {
      AActor *&Actor = Actors[ActorIdx];

      SerializeActor(
          ActorsMap, Actor,
          [&](const FString &ActorName, const FSoftClassPath &Class,
              const FGuid &SpawnID, FStructuredArchive::FSlot &ActorSlot) {
            ensureAlways(!ActorName.IsEmpty());

            if (Class.IsNull()) {
              Actor = FindObjectFast<AActor>(Level, *ActorName);
            } else if (SpawnID.IsValid() && SpawnIDs.Contains(SpawnID)) {
              Actor = SpawnIDs[SpawnID];
            } else {
              UClass *ActorClass = Class.TryLoadClass<AActor>();

              if (!ActorClass) {
                UE_LOG(LogTemp, Error,
                       TEXT("SerializeSubsystem: Failed to load class '%s' for "
                            "actor '%s' — actor will be skipped."),
                       *Class.ToString(), *ActorName);
                return;
              }

              FActorSpawnParameters SpawnParameters;
              SpawnParameters.OverrideLevel = Level;
              SpawnParameters.Name = *ActorName;
              SpawnParameters.bNoFail = true;

              Actor = Level->GetWorld()->SpawnActor(ActorClass, nullptr,
                                                    nullptr, SpawnParameters);

              if (IsValid(Actor) && SpawnID.IsValid() &&
                  Actor->Implements<USaveGameSpawnActor>()) {
                ISaveGameSpawnActor::Execute_SetSpawnID(Actor, SpawnID);
              }
            }

            if (IsValid(Actor) && SpawnID.IsValid()) {
              const FTopLevelAssetPath LevelAssetPath(
                  Level->GetPackage()->GetFName(),
                  Level->GetOuter()->GetFName());
              const FString ActorSubPath = LEVEL_SUBPATH_PREFIX + ActorName;
              ProxyArchive.AddRedirect(
                  FSoftObjectPath(LevelAssetPath, ActorSubPath),
                  FSoftObjectPath(Actor));
            }

            // JSON loading uses a single-pass approach: FStructuredArchive
            // with JSON is forward-only and cannot seek back for a second pass,
            // so we serialize properties immediately after spawning/finding
            // the actor, while ActorSlot is still open.
            if constexpr (bIsTextFormat) {
              if (!IsValid(Actor)) {
                UE_LOG(LogTemp, Warning,
                       TEXT("SerializeSubsystem: Actor '%s' is invalid after "
                            "spawn/find — skipping property deserialization."),
                       *ActorName);
                return;
              }

              SerializeActorData(Actor, ActorSlot);
            }
          });
    }

    // JSON loading is fully handled above in the single-pass lambda.
    // Skip the binary second pass (which seeks back and re-reads the map).
    if constexpr (bIsTextFormat) {
      return;
    }
  }

  {
    QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeActorData);

    // Binary loading: seek back to re-read actor data in a second pass.
    // (First pass only spawned/found actors; this pass serializes properties.)
    if (bIsLoading && !bIsTextFormat) {
      Archive.Seek(ActorsPosition);
    }

    TSet<TWeakObjectPtr<AActor>>::TConstIterator ActorsIt =
        SaveGameActors.CreateConstIterator();

    for (int32 ActorIdx = 0; ActorIdx < NumActors; ++ActorIdx) {
      AActor *Actor;

      if (bIsLoading) {
        Actor = Actors[ActorIdx];
      } else {
        Actor = ActorsIt->Get();
        ++ActorsIt;
      }

      if (!IsValid(Actor))
        continue;

      SerializeActor(
          ActorsMap, Actor,
          [&](const FString &, const FSoftClassPath &, const FGuid &,
              FStructuredArchive::FSlot &ActorSlot) {
            SerializeActorData(Actor, ActorSlot);
          });
    }
  }
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActorData(
    AActor *Actor, FStructuredArchive::FSlot &ActorSlot) {
  Actor->SerializeScriptProperties(ActorSlot.EnterAttribute(TEXT("Properties")));
  SerializeActorComponents(Actor, ActorSlot);

  FStructuredArchive::FSlot CustomDataSlot = ActorSlot.EnterAttribute(TEXT("Data"));
  FStructuredArchive::FRecord CustomDataRecord = CustomDataSlot.EnterRecord();
  FSaveGameArchive SaveGameArchive(CustomDataRecord, Actor);
  ISaveGameObject::Execute_OnSerialize(Actor, SaveGameArchive, bIsLoading);
}

// Serialize the actor's components
template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActorComponents(
    AActor *&Actor, FStructuredArchive::FSlot &ActorSlot) {
  if (!Actor)
    return;

  TArray<UActorComponent *> Components =
      Actor->GetComponentsByInterface(USaveGameObject::StaticClass());

  if (Components.IsEmpty())
    return;

  int32 NumComponents = Components.Num();

  FStructuredArchive::FSlot ActorComponentsSlot =
      ActorSlot.EnterAttribute(TEXT("Components"));
  FStructuredArchive::FMap ComponentsMap =
      ActorComponentsSlot.EnterMap(NumComponents);

  for (TSoftObjectPtr<UActorComponent> ActorComponent : Components) {
    if (ActorComponent.IsValid()) {
      SerializeActorComponent(ComponentsMap, ActorComponent);
    }
  }
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeDestroyedActors(
    ULevel *Level, FStructuredArchive::FSlot &DestroyedActorsSlot) {
  QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeDestroyedActors);
  check(SerializeSubsystem.IsValid());
  check(IsValid(Level));

  const TSharedPtr<FLevelStruct> LevelRecord =
      SerializeSubsystem->PersistentLevelRecord;

  int32 NumDestroyedActors;

  if (!bIsLoading) {
    NumDestroyedActors = LevelRecord->Actors->Destroyed.Num();
  }

  FStructuredArchive::FArray DestroyedActorsArray =
      DestroyedActorsSlot.EnterArray(NumDestroyedActors);

  if (LevelRecord->Actors->Destroyed.IsEmpty())
    return;

  if (bIsLoading) {
    // Allocate our expected number of actors
    LevelRecord->Actors->Destroyed.Reset();
    LevelRecord->Actors->Destroyed.Reserve(NumDestroyedActors);
  }

  TSet<FSoftObjectPath>::TConstIterator DestroyedActorsIt =
      LevelRecord->Actors->Destroyed.CreateConstIterator();

  for (int32 ActorIdx = 0; ActorIdx < NumDestroyedActors; ++ActorIdx) {
    FName ActorName;

    if (!bIsLoading) {
      // Only store the object name without the prefix and full path
      FString ActorSubPath = DestroyedActorsIt->GetSubPathString();
      ActorSubPath.RemoveFromStart(LEVEL_SUBPATH_PREFIX);
      ActorName = *ActorSubPath;

      ++DestroyedActorsIt;
    }

    DestroyedActorsArray.EnterElement() << ActorName;

    if (bIsLoading) {
      // Find the live actor in the level
      if (AActor *DestroyedActor = FindObjectFast<AActor>(Level, ActorName)) {
        // Be sure to add any valid destroyed actors back into the array for
        // saving later!
        LevelRecord->Actors->Destroyed.Add(DestroyedActor);

        DestroyedActor->Destroy();
      }
    }
  }
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeVersions() {
  QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeVersions);

  if (!bIsTextFormat) {
    // Store the version position so that we can serialize it in the header
    VersionOffset = Archive.Tell();
  }

  FCustomVersionContainer VersionContainer;

  if (!bIsLoading) {
    // Grab a copy of our archive's current versions
    VersionContainer = Archive.GetCustomVersions();
  }

  VersionContainer.Serialize(RootRecord.EnterField(TEXT("Versions")));

  if (bIsLoading) {
    // Assign our serialized versions
    Archive.SetCustomVersions(VersionContainer);
  }

  if (!bIsTextFormat) {
    uint64 CurrentOffset = Archive.Tell();

    // We've updated the VersionOffset, let's go back to the start and rewrite
    // the header
    Archive.Seek(HeaderOffset);
    SerializeHeader();

    Archive.Seek(CurrentOffset);
  }
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActor(
    FStructuredArchive::FMap &ActorMap, AActor *&Actor,
    TFunction<void(const FString &, const FSoftClassPath &, const FGuid &,
                   FStructuredArchive::FSlot &)> &&BodyFunction) {
  QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeActor);

  FString ActorName;
  FSoftClassPath Class;
  FGuid SpawnID;

  if (!bIsLoading) {
    ActorName = Actor->GetName();

    if (!USaveGameFunctionLibrary::WasObjectLoaded(Actor)) {
      // We're a spawned actor, stash the class
      Class = Actor->GetClass();
    }

    if (Actor->Implements<USaveGameSpawnActor>()) {
      SpawnID = ISaveGameSpawnActor::Execute_GetSpawnID(Actor);
    }
  }

  FStructuredArchive::FSlot ActorSlot = ActorMap.EnterElement(ActorName);

  // If we have a class, we're a spawned actor
  if (TOptional<FStructuredArchive::FSlot> ClassSlot =
          ActorSlot.TryEnterAttribute(TEXT("Class"), !Class.IsNull())) {
    ClassSlot.GetValue() << Class;
  }

  // If we have a GUID, we're a spawn actor that needs to be mapped by GUID
  TOptional<FStructuredArchive::FSlot> GuidSlot =
      ActorSlot.TryEnterAttribute(TEXT("GUID"), SpawnID.IsValid());
  if (GuidSlot.IsSet()) {
    GuidSlot.GetValue() << SpawnID;
  }

  uint64 DataSize;

  if (!bIsTextFormat) {
    // Pre-write how much data (in bytes) was serialized for this actor
    Archive << DataSize;
  }

  const uint64 BeginDataPosition = Archive.Tell();

  BodyFunction(ActorName, Class, SpawnID, ActorSlot);

  if (!bIsTextFormat) {
    if (bIsLoading) {
      // Skip our data and onto the next actor
      Archive.Seek(BeginDataPosition + DataSize);
    } else {
      const uint64 EndDataPosition = Archive.Tell();
      DataSize = EndDataPosition - BeginDataPosition;

      // Store the amount of data we've serialized (in bytes), back before the
      // actual data
      Archive.Seek(BeginDataPosition - sizeof(DataSize));
      Archive << DataSize;

      // Go back to our current position
      Archive.Seek(EndDataPosition);
    }
  }
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActorComponent(
    FStructuredArchive::FMap &ComponentsMap,
    TSoftObjectPtr<UActorComponent> &ActorComponent) {
  QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeActorComponent);

  FString ActorComponentName;

  if (!bIsLoading) {
    ActorComponentName = ActorComponent->GetName();
  }

  FStructuredArchive::FSlot ActorComponentSlot =
      ComponentsMap.EnterElement(ActorComponentName);

  ActorComponent->SerializeScriptProperties(
      ActorComponentSlot.EnterAttribute(TEXT("Properties")));

  FStructuredArchive::FSlot CustomDataSlot =
      ActorComponentSlot.EnterAttribute(TEXT("Data"));
  FStructuredArchive::FRecord CustomDataRecord = CustomDataSlot.EnterRecord();

  // Encapsulate the record in something a Blueprint can access
  FSaveGameArchive SaveGameArchive(CustomDataRecord, ActorComponent.Get());

  ISaveGameObject::Execute_OnSerialize(ActorComponent.Get(), SaveGameArchive,
                                       bIsLoading);
}

// Instantiate the permutations of TSaveGameSerializer
#if WITH_TEXT_ARCHIVE_SUPPORT
template TSaveGameSerializer<false, true>;
template TSaveGameSerializer<true, true>;
#endif

template TSaveGameSerializer<false>;
template TSaveGameSerializer<true>;