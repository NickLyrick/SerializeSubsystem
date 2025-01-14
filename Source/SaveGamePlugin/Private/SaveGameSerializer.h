#pragma once

#if WITH_TEXT_ARCHIVE_SUPPORT
#include "Serialization/Formatters/JsonArchiveOutputFormatter.h"
#endif

#include "SaveGameProxyArchive.h"
#include "Templates/ChooseClass.h"

class USaveGameSubsystem;

class FSaveGameSerializer : public TSharedFromThis<FSaveGameSerializer> {
public:
  virtual ~FSaveGameSerializer() = default;
};

// TODO: This comment is incorrect
/**
 * The class that manages serializing the world.
 *
 * Archive data structured like so:
 * - Header
 *		- Engine Versions
 * - Persistent Level #1:
 *   - Actors
 *		  - Actor Name #1:
 *			  - Class: If spawned
 *			  - SpawnID: If implements ISaveGameSpawnActor
 *			  - SaveGame Properties
 *			  - Data written by ISaveGameObject::OnSerialize
 *		  - ...
 *   - Destroyed Level Actors
 *		  - Actor Name #1
 *		  - ...
 *  - Streaming Levels
 * - Versions
 *		- Version:
 *			- ID
 *			- Version Number
 *		- ...
 */
template <bool bIsLoading, bool bIsTextFormat = false>
class TSaveGameSerializer final : public FSaveGameSerializer {
  using FSaveGameMemoryArchive =
      typename TChooseClass<bIsLoading, FMemoryReader, FMemoryWriter>::Result;

  static_assert(!bIsLoading || !bIsTextFormat,
                "This serializer hasn't been implemented for text based "
                "loading, only saving!");
  static_assert(WITH_TEXT_ARCHIVE_SUPPORT || !bIsTextFormat,
                "Engine isn't compiled with text archive support, cannot use "
                "text based TSaveGameSerializer");

  using FSaveGameFormatter = typename TChooseClass<
      bIsTextFormat && WITH_TEXT_ARCHIVE_SUPPORT,
      typename TChooseClass<bIsLoading, FBinaryArchiveFormatter,
                            FJsonArchiveOutputFormatter>::Result,
      FBinaryArchiveFormatter>::Result;

public:
  explicit TSaveGameSerializer(USaveGameSubsystem *InSaveGameSubsystem);

  // FSerializedData SerializeData();
  // bool DeserializeData(FSerializedData &RawData);

  TArray<uint8> SerializeHeaderData();
  void DeserializeHeaderData(TArray<uint8> &HeaderData);

  TArray<uint8> SerializeLevelData(TSoftObjectPtr<ULevel> Level);
  void DeserializeLevelData(TSoftObjectPtr<ULevel> Level,
                            TArray<uint8> &LevelData);

  TArray<uint8> SerializeStreamingLevelData(
      const TSoftObjectPtr<ULevelStreaming> &StreamingLevel);
  void DeserializeStreamingLevelData(
      const TSoftObjectPtr<ULevelStreaming> &StreamingLevel,
      TArray<uint8> &StreamingLevelData);

private:
  // static FString GetSaveName();

  void OnMapLoad(UWorld *World);
  // void OnStreamingLevelLoad(ULevel *Level, UWorld *World);

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
   * Serializes all the actors that the SaveGameSubsystem is keeping track
   * of. On load, it will also pre-spawn any actors and map any actors with
   * Spawn IDs before running the actual serialization step.
   */
  void SerializeActors(ULevel *Level,
                       TSet<TWeakObjectPtr<AActor>> &SaveGameActors,
                       FStructuredArchive::FSlot &ActorsSlot);

  /** Serializes any destroyed level actors. On load, level actors will exist
   * again, so this will re-destroy them */
  void SerializeDestroyedActors(ULevel *Level,
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

  // Internal Variables
private:
  // The game instance subsystem that manages the Serialization
  const TWeakObjectPtr<USaveGameSubsystem> SaveGameSubsystem;

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
};
