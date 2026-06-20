#pragma once

#include "Misc/Build.h"
#include "Misc/EngineVersion.h"

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
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
#include <type_traits>

class FSaveGameSerializer : public TSharedFromThis<FSaveGameSerializer>
{
public:
	virtual ~FSaveGameSerializer() = default;
};

/**
 * Four template instantiations exist (see bottom of .cpp):
 *   <false, false> = Binary Save   <true, false> = Binary Load
 *   <false, true>  = JSON Save     <true, true>  = JSON Load
 * JSON instantiations are compiled only under #if !UE_BUILD_SHIPPING && WITH_TEXT_ARCHIVE_SUPPORT.
 *
 * Blob layout (three separately-compressed blobs per save):
 *
 *   Header blob (FSerializedData::Header):
 *     EngineVersion, PackageVersion [binary], VersionsOffset [binary],
 *     CustomVersions [FCustomVersionContainer] — stored at VersionsOffset so the
 *     deserializer can seek directly to version data before touching actor blobs.
 *
 *   Level blob  (FSerializedData::Levels[L].Data) and
 *   Streaming level blob (FSerializedData::Levels[L].StreamingLevels[SL].Data):
 *     Actors (map):
 *       ActorName → Class [if dynamically spawned], GUID [if ISaveGameSpawnActor],
 *                   DataSize [binary only — enables skip-on-corruption], Properties,
 *                   Components (map), Data
 *     DestroyedActors (array): ActorName, ...
 */
template <bool bIsLoading, bool bIsTextFormat = false>
class TSaveGameSerializer final : public FSaveGameSerializer
{
	using FSaveGameMemoryArchive = std::conditional_t<bIsLoading, FMemoryReader, FMemoryWriter>;

	static_assert(WITH_TEXT_ARCHIVE_SUPPORT || !bIsTextFormat,
	              "Engine isn't compiled with text archive support, cannot use "
	              "text based TSaveGameSerializer");

	using FSaveGameFormatter =
	    std::conditional_t<bIsTextFormat && WITH_TEXT_ARCHIVE_SUPPORT,
	                       std::conditional_t<bIsLoading, FJsonArchiveInputFormatter, FJsonArchiveOutputFormatter>,
	                       FBinaryArchiveFormatter>;

public:
	/**
	 * For binary (saving/loading) and JSON saving: InInitialData is left empty.
	 * For JSON loading: pass the raw JSON bytes here — FJsonArchiveInputFormatter
	 * parses the JSON eagerly in its constructor and requires the data upfront.
	 */
	explicit TSaveGameSerializer(USerializeSubsystem* InSerializeSubsystem, TArray<uint8> InInitialData = {});

	TArray<uint8> SerializeHeaderData();
	/** Binary loading: decompresses HeaderData then reads. */
	void DeserializeHeaderData(TArray<uint8>& HeaderData);
	/** JSON loading: data was provided at construction, just reads the header. */
	void DeserializeHeaderData();

	TArray<uint8> SerializeLevelData(TSoftObjectPtr<ULevel> Level);
	/** Binary loading: decompresses LevelData, then triggers seamless travel. */
	void DeserializeLevelData(const FString& LevelName, TArray<uint8>& LevelData);
	/** JSON loading: data was provided at construction, triggers seamless travel. */
	void DeserializeLevelData(const FString& LevelName);

	TArray<uint8> SerializeStreamingLevelData(const TSoftObjectPtr<ULevelStreaming>& StreamingLevel);
	/** Binary loading: decompresses StreamingLevelData then deserializes. */
	void DeserializeStreamingLevelData(const TSoftObjectPtr<ULevelStreaming>& StreamingLevel,
	                                   TArray<uint8>& StreamingLevelData);
	/** JSON loading: data was provided at construction, deserializes directly. */
	void DeserializeStreamingLevelData(const TSoftObjectPtr<ULevelStreaming>& StreamingLevel);

private:
	void OnMapLoad(UWorld* World);

	void InitiateLevelLoad(const FString& LevelName);
	void SerializeHeader();
	void SerializeLevel(const TSoftObjectPtr<ULevel>& Level);
	void SerializeStreamingLevel(const TSoftObjectPtr<ULevelStreaming>& StreamingLevel);

private:
	void SerializeActors(ULevel* Level, TSet<TWeakObjectPtr<AActor>>& SaveGameActors, FStructuredArchive::FSlot& ActorsSlot);
	void SerializeActorComponents(AActor*& Actor, FStructuredArchive::FSlot& ActorSlot);
	void SerializeDestroyedActors(ULevel* Level, FActorsStruct& ActorsRecord, FStructuredArchive::FSlot& DestroyedActorsSlot);
	void SerializeVersions();

	// BodyFunction is called after header fields (ActorName, Class, SpawnID) are read/written.
	// Binary mode: also writes/reads DataSize around BodyFunction for skip-on-corruption.
	void SerializeActor(FStructuredArchive::FMap& ActorMap,
	                    AActor*& Actor,
	                    TFunction<void(const FString&, const FSoftClassPath&, const FGuid&, FStructuredArchive::FSlot&)>&& BodyFunction);

	void SerializeActorComponent(FStructuredArchive::FMap& ComponentsMap, TSoftObjectPtr<UActorComponent>& ActorComponent);
	void SerializeActorData(AActor* Actor, FStructuredArchive::FSlot& ActorSlot);
	void BroadcastLoadFailed(ESaveGameLoadResult Result);

private:
	const TWeakObjectPtr<USerializeSubsystem> SerializeSubsystem;

	// Declaration order matters: Data must be initialized before Archive (Archive wraps Data),
	// and Archive before ProxyArchive, ProxyArchive before Formatter, Formatter before StructuredArchive.
	// For JSON loading, Data must also be populated before Formatter construction
	// because FJsonArchiveInputFormatter parses the JSON eagerly in its constructor.
	TArray<uint8>                     Data = {};
	FSaveGameMemoryArchive            Archive;
	TSaveGameProxyArchive<bIsLoading> ProxyArchive;
	FSaveGameFormatter                Formatter;
	FStructuredArchive                StructuredArchive;
	FStructuredArchive::FSlot         RootSlot;
	FStructuredArchive::FRecord       RootRecord;

	// VersionsOffset: position of the version table inside the binary blob.
	// Written into the header so DeserializeHeaderData can seek directly to version
	// data and check compatibility before decompressing the (potentially large) actor blobs.
	uint64 VersionOffset;
	uint64 HeaderOffset;

	FEngineVersion SavedEngineVersion;
};
