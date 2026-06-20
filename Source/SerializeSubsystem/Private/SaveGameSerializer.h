#pragma once

#include "Misc/Build.h"

#if WITH_TEXT_ARCHIVE_SUPPORT
#include "Serialization/Formatters/JsonArchiveInputFormatter.h"
#include "Serialization/Formatters/JsonArchiveOutputFormatter.h"
#endif

#include "Components/ActorComponent.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "SaveGameMigrationStep.h"
#include "SaveGameProxyArchive.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "SerializeSubsystem.h"
#include "Templates/ChooseClass.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

class FSaveGameSerializer : public TSharedFromThis<FSaveGameSerializer> {
public:
  virtual ~FSaveGameSerializer() = default;
};

/**
 * Serializes/deserializes the game world across three separately-compressed blobs.
 *
 * Header blob (FSerializedData::Header):
 *   EngineVersion, PackageVersion [binary], VersionsOffset [binary],
 *   CustomVersions [FCustomVersionContainer, stored at VersionsOffset]
 *
 * Level blob (FSerializedData::Levels[L].Data) and
 * Streaming level blob (FSerializedData::Levels[L].StreamingLevels[SL].Data):
 *   Actors (map):
 *     ActorName → Class [if dynamically spawned], GUID [if ISaveGameSpawnActor],
 *                 DataSize [binary only], Properties, Components (map), Data
 *   DestroyedActors (array): ActorName, ...
 */
template <bool bIsLoading, bool bIsTextFormat = false>
class TSaveGameSerializer final : public FSaveGameSerializer {
  using FSaveGameMemoryArchive =
      typename TChooseClass<bIsLoading, FMemoryReader, FMemoryWriter>::Result;

  static_assert(WITH_TEXT_ARCHIVE_SUPPORT || !bIsTextFormat,
                "Engine isn't compiled with text archive support, cannot use "
                "text based TSaveGameSerializer");

  using FSaveGameFormatter = typename TChooseClass<
      bIsTextFormat && WITH_TEXT_ARCHIVE_SUPPORT,
      typename TChooseClass<bIsLoading, FJsonArchiveInputFormatter,
                            FJsonArchiveOutputFormatter>::Result,
      FBinaryArchiveFormatter>::Result;

public:
  /**
   * For binary (saving/loading) and JSON saving: InInitialData is left empty.
   * For JSON loading: pass the raw JSON bytes here — FJsonArchiveInputFormatter
   * parses the JSON eagerly in its constructor and requires the data upfront.
   */
  explicit TSaveGameSerializer(USerializeSubsystem *InSerializeSubsystem,
                               TArray<uint8> InInitialData = {});

  TArray<uint8> SerializeHeaderData();
  /** Binary loading: decompresses HeaderData then reads. */
  void DeserializeHeaderData(TArray<uint8> &HeaderData);
  /** JSON loading: data was provided at construction, just reads the header. */
  void DeserializeHeaderData();

  TArray<uint8> SerializeLevelData(TSoftObjectPtr<ULevel> Level);
  /** Binary loading: decompresses LevelData, then triggers seamless travel. */
  void DeserializeLevelData(TSoftObjectPtr<ULevel> Level,
                            TArray<uint8> &LevelData);
  /** JSON loading: data was provided at construction, triggers seamless travel. */
  void DeserializeLevelData(TSoftObjectPtr<ULevel> Level);

  TArray<uint8> SerializeStreamingLevelData(
      const TSoftObjectPtr<ULevelStreaming> &StreamingLevel);
  /** Binary loading: decompresses StreamingLevelData then deserializes. */
  void DeserializeStreamingLevelData(
      const TSoftObjectPtr<ULevelStreaming> &StreamingLevel,
      TArray<uint8> &StreamingLevelData);
  /** JSON loading: data was provided at construction, deserializes directly. */
  void DeserializeStreamingLevelData(
      const TSoftObjectPtr<ULevelStreaming> &StreamingLevel);

private:
  void OnMapLoad(UWorld *World);

  /** Shared logic for Deserialize*LevelData: validates map name and triggers
   * seamless travel (which will call OnMapLoad when the level is ready). */
  void InitiateLevelLoad(const TSoftObjectPtr<ULevel> &Level);

  /** Serializes information about the archive, like Engine Version or position
   * of versioning information */
  void SerializeHeader();

  /** Serializes the level's data into the structured archive */
  void SerializeLevel(const TSoftObjectPtr<ULevel> &Level);

  /** Serializes the streaming level's data into the structured archive */
  void SerializeStreamingLevel(
      const TSoftObjectPtr<ULevelStreaming> &StreamingLevel);

private:
  /**
   * Serializes all the actors that the SerializeSubsystem is keeping track
   * of. On load, it will also pre-spawn any actors and map any actors with
   * Spawn IDs before running the actual serialization step.
   */
  void SerializeActors(ULevel *Level,
                       TSet<TWeakObjectPtr<AActor>> &SaveGameActors,
                       FStructuredArchive::FSlot &ActorsSlot);

  /**
   * Serializes all the actor components that implements interface
   * SaveGameObject.
   */
  void SerializeActorComponents(AActor *&Actor,
                                FStructuredArchive::FSlot &ActorSlot);

  /** Serializes any destroyed level actors. On load, level actors will exist
   * again, so this will re-destroy them */
  void SerializeDestroyedActors(ULevel *Level, FActorsStruct &ActorsRecord,
                                FStructuredArchive::FSlot &DestroyedActorsSlot);

  /**
   * Serialized at the end of the archive, the versions are useful for
   * marshaling old data. These also contain the versions added by
   * USaveGameFunctionLibrary::UseCustomVersion.
   */
  void SerializeVersions();

  /**
   * Serializes the actor's data into the structured archive.
   * This data always comprises the actor's object name, and optionally its:
   * - Class: If the actor was spawned (so that it can be spawned again)
   * - SpawnID: If the actor implements ISaveGameSpawnActor. A unique identifier
   *to map the data back to an already spawned actor (like the player's
   *character)
   *
   * It also takes a lambda function that can optionally do some work or
   *serialization. Ultimately, once this lambda function is complete,
   *SerializeActor will automatically seek the archive to the end of the actor's
   *data.
   *
   * @param ActorMap The structured map that the actor data will be written to
   * @param Actor The live actor that will be serialized
   * @param BodyFunction A lambda function that will optionally do some work,
   *whether that be serializing or spawning
   */
  void SerializeActor(
      FStructuredArchive::FMap &ActorMap, AActor *&Actor,
      TFunction<void(const FString &, const FSoftClassPath &, const FGuid &,
                     FStructuredArchive::FSlot &)> &&BodyFunction);

  /**
   * Serializes the actor component data into the structured archive.
   * This data always comprises the actor's component object name and its data.
   *
   * @param ComponentsMap The structured map that the actor component data will
   *be written to
   * @param ActorComponent The live actor components that will be serialized
   */
  void SerializeActorComponent(FStructuredArchive::FMap &ComponentsMap,
                               TSoftObjectPtr<UActorComponent> &ActorComponent);

  /** Serializes an actor's script properties, components, and custom data. */
  void SerializeActorData(AActor *Actor, FStructuredArchive::FSlot &ActorSlot);

  /** Resolves SerializeSubsystem and calls FinalizeLoad with the given result. */
  void BroadcastLoadFailed(ESaveGameLoadResult Result);

  // Internal Variables
private:
  // The game instance subsystem that manages the Serialization
  const TWeakObjectPtr<USerializeSubsystem> SerializeSubsystem;

  // The data that will be serialized
  TArray<uint8> Data = {};

  // The archive that will be used to serialize the data
  FSaveGameMemoryArchive Archive;
  // The proxy archive that will be an abstraction layer for the archive to
  // resolve pointers
  TSaveGameProxyArchive<bIsLoading> ProxyArchive;
  // The formatter that will be used to serialize the data (binary or JSON)
  FSaveGameFormatter Formatter;
  // The structured archive that will be an abstraction layer for the proxy
  // archive to allow for structured serialization
  FStructuredArchive StructuredArchive;

  // The root slot of the structured archive
  FStructuredArchive::FSlot RootSlot;
  // The root record of the structured archive
  FStructuredArchive::FRecord RootRecord;

  // Offsets
  uint64 VersionOffset;
  uint64 HeaderOffset;

  // SetDefaultValue migrations queued during SerializeVersions; applied in
  // SerializeActorData after each actor's properties are deserialized.
  // PendingDefaultMigrationClasses[i] is the UClass resolved once at queue
  // time to avoid per-actor TryLoadClass calls.
  TArray<FMigration_SetDefaultValue> PendingDefaultMigrations;
  TArray<TObjectPtr<UClass>> PendingDefaultMigrationClasses;
};
