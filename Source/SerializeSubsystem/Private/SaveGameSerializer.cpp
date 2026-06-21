#include "SaveGameSerializer.h"

#include "Engine/Level.h"
#include "Misc/Crc.h"
#include "Misc/EngineVersion.h"
#include "SaveGameFunctionLibrary.h"
#include "SaveGameObject.h"
#include "SaveGameSettings.h"
#include "SaveGameVersion.h"
#include "Serialization/CustomVersion.h"
#include "UObject/CoreRedirects.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveGame, Log, All);

#define LEVEL_SUBPATH_PREFIX TEXT("PersistentLevel.")

template <bool bIsLoading>
FORCEINLINE_DEBUGGABLE bool SerializeCompressedData(FArchive& Archive, TArray<uint8>& UncompressedData);

// Fixed-size manifest written at the front of every binary blob (before compressed data).
// Readable without decompression — allows fast rejection of incompatible or corrupt files.
struct FSaveGameManifest
{
	static constexpr uint32 MAGIC = 0x53475356; // 'S','G','S','V'
	uint32 Magic = MAGIC;
	int32 PluginVersion = 0;
	uint32 DataCRC = 0; // CRC32 of the compressed bytes that follow
};
static_assert(sizeof(FSaveGameManifest) == 12, "FSaveGameManifest layout changed");

// Save path: compresses UncompressedData, prepends manifest, returns the full blob.
static TArray<uint8> CompressAndWrapBlob(const TArray<uint8>& UncompressedData)
{
	TArray<uint8> Compressed;
	FMemoryWriter CompressWriter(Compressed);
	SerializeCompressedData<false>(CompressWriter, const_cast<TArray<uint8>&>(UncompressedData));

	FSaveGameManifest Manifest;
	Manifest.PluginVersion = static_cast<int32>(FSaveGameVersion::LatestVersion);
	// CRC32: fast non-cryptographic integrity check, false-positive rate ~1/2^32 — sufficient for save files.
	Manifest.DataCRC = FCrc::MemCrc32(Compressed.GetData(), Compressed.Num());

	TArray<uint8> Blob;
	Blob.Reserve(sizeof(FSaveGameManifest) + Compressed.Num());
	Blob.Append(reinterpret_cast<const uint8*>(&Manifest), sizeof(Manifest));
	Blob.Append(MoveTemp(Compressed));
	return Blob;
}

// Load path: validates manifest magic and CRC.
// On Success, CompressorArchive is positioned past the manifest, ready for SerializeCompressedData.
static ESaveGameLoadResult ValidateManifest(const TArray<uint8>& Blob, FArchive& CompressorArchive)
{
	if (Blob.Num() < static_cast<int32>(sizeof(FSaveGameManifest)))
	{
		UE_LOG(LogSaveGame, Error, TEXT("Blob too small to contain manifest header."));
		return ESaveGameLoadResult::CorruptedData;
	}

	FSaveGameManifest Manifest;
	CompressorArchive.Serialize(&Manifest, sizeof(Manifest));

	if (Manifest.Magic != FSaveGameManifest::MAGIC)
	{
		UE_LOG(LogSaveGame,
		       Error,
		       TEXT("Manifest magic mismatch (got 0x%08X). File may be from a "
		            "legacy plugin version or is corrupted."),
		       Manifest.Magic);
		return ESaveGameLoadResult::CorruptedData;
	}

	const uint32 ActualCRC = FCrc::MemCrc32(Blob.GetData() + sizeof(Manifest), Blob.Num() - sizeof(Manifest));
	if (ActualCRC != Manifest.DataCRC)
	{
		UE_LOG(LogSaveGame,
		       Error,
		       TEXT("CRC mismatch (expected 0x%08X, got 0x%08X). File is corrupted."),
		       Manifest.DataCRC,
		       ActualCRC);
		return ESaveGameLoadResult::CorruptedData;
	}

	return ESaveGameLoadResult::Success;
}

template <bool bIsLoading>
FORCEINLINE_DEBUGGABLE bool SerializeCompressedData(FArchive& Archive, TArray<uint8>& UncompressedData)
{
	check(Archive.IsLoading() == bIsLoading);

	int64 UncompressedSize = 0;
	if constexpr (!bIsLoading)
	{
		UncompressedSize = UncompressedData.Num();
	}

	Archive << UncompressedSize;

	if constexpr (bIsLoading)
	{
		if (Archive.IsError() || UncompressedSize <= 0 || UncompressedSize > static_cast<int64>(MAX_int32))
		{
			UE_LOG(LogSaveGame,
			       Error,
			       TEXT("Decompression failed — invalid or unreadable uncompressed "
			            "size (%lld). Save file may be corrupted or truncated."),
			       UncompressedSize);
			Archive.SetError();
			return false;
		}
		UncompressedData.SetNumUninitialized(static_cast<int32>(UncompressedSize));
	}

	Archive.SerializeCompressed(UncompressedData.GetData(), UncompressedSize, NAME_Zlib);

	if constexpr (bIsLoading)
	{
		if (Archive.IsError())
		{
			UE_LOG(LogSaveGame,
			       Error,
			       TEXT("Decompression failed — archive entered error state. Save "
			            "file may be corrupted or truncated."));
			return false;
		}
	}

	return true;
}

template <bool bIsLoading, bool bIsTextFormat>
TSaveGameSerializer<bIsLoading, bIsTextFormat>::TSaveGameSerializer(USerializeSubsystem* InSerializeSubsystem,
                                                                    TArray<uint8> InInitialData)
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
	static_cast<FArchive&>(ProxyArchive).SetIsTextFormat(bIsTextFormat);

	Archive.UsingCustomVersion(FSaveGameVersion::GUID);
}

template <bool bIsLoading, bool bIsTextFormat>
TArray<uint8> TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeHeaderData()
{
	check(!bIsLoading);

	TRACE_BOOKMARK(TEXT("Begin: SerializeHeaderData[%s]"), bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

	ON_SCOPE_EXIT
	{
		TRACE_BOOKMARK(TEXT("End: SerializeHeaderData[%s]"), bIsTextFormat ? TEXT("Text") : TEXT("Binary"));
	};

	check(SerializeSubsystem.IsValid());

	SerializeHeader();
	SerializeVersions();

	// Be sure to close this, as you'll be missing closed braces for JSON
	// archives
	StructuredArchive.Close();

	if constexpr (!bIsTextFormat && !bIsLoading)
	{
		return CompressAndWrapBlob(Data);
	}

	return Data;
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeHeaderData(TArray<uint8>& HeaderData)
{
	check(bIsLoading);

	TRACE_BOOKMARK(TEXT("Begin: DeserializeHeaderData[%s]"), bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

	if constexpr (!bIsTextFormat)
	{
		FSaveGameMemoryArchive CompressorArchive(HeaderData);
		const ESaveGameLoadResult ManifestResult = ValidateManifest(HeaderData, CompressorArchive);
		if (ManifestResult != ESaveGameLoadResult::Success)
		{
			BroadcastLoadFailed(ManifestResult);
			return;
		}
		if (!SerializeCompressedData<true>(CompressorArchive, Data))
		{
			BroadcastLoadFailed(ESaveGameLoadResult::CorruptedData);
			return;
		}
	}
	// JSON: data was provided at construction, nothing to decompress.

	SerializeHeader();

	const USaveGameSettings* Settings = GetDefault<USaveGameSettings>();
	if (!Settings->IsLoadingFromIncompatibleEngineVersionAllowed() &&
	    !SavedEngineVersion.IsCompatibleWith(FEngineVersion::Current()))
	{
		UE_LOG(LogSaveGame,
		       Warning,
		       TEXT("Save was created with engine version %s; current is %s. "
		            "Blocking load. Enable bAllowLoadingFromIncompatibleEngineVersion "
		            "in SaveGame project settings to override."),
		       *SavedEngineVersion.ToString(),
		       *FEngineVersion::Current().ToString());
		BroadcastLoadFailed(ESaveGameLoadResult::EngineVersionMismatch);
		return;
	}

	if constexpr (!bIsTextFormat)
	{
		if (VersionOffset != 0)
		{
			Archive.Seek(VersionOffset);
		}
	}
	SerializeVersions();
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeHeaderData()
{
	if constexpr (bIsLoading && bIsTextFormat)
	{
		SerializeHeader();
		SerializeVersions();
	}
	else
	{
		checkNoEntry();
	}
}

template <bool bIsLoading, bool bIsTextFormat>
TArray<uint8> TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeLevelData(TSoftObjectPtr<ULevel> Level)
{
	check(!bIsLoading);

	TRACE_BOOKMARK(TEXT("Begin: SerializeLevelData[%s]"), bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

	ON_SCOPE_EXIT
	{
		TRACE_BOOKMARK(TEXT("End: SerializeLevelData[%s]"), bIsTextFormat ? TEXT("Text") : TEXT("Binary"));
	};

	check(SerializeSubsystem.IsValid());

	SerializeLevel(Level);

	// Be sure to close this, as you'll be missing closed braces for JSON
	// archives
	StructuredArchive.Close();

	if constexpr (!bIsTextFormat && !bIsLoading)
	{
		return CompressAndWrapBlob(Data);
	}

	return Data;
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::InitiateLevelLoad(const FString& LevelName)
{
	if (LevelName.IsEmpty())
	{
		UE_LOG(LogSaveGame, Error, TEXT("Cannot load level — map name is empty."));
		BroadcastLoadFailed(ESaveGameLoadResult::MapMissing);
		return;
	}

	check(SerializeSubsystem.IsValid());
	UWorld* World = SerializeSubsystem->GetWorld();

	if (World->IsInSeamlessTravel())
	{
		return;
	}

	// Register before SeamlessTravel: the engine initiates the journey asynchronously,
	// but OnMapLoad can fire as early as the next tick — subscribing after the call risks missing it.
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddThreadSafeSP(this, &TSaveGameSerializer::OnMapLoad);

	World->SeamlessTravel(LevelName, true);
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeLevelData(const FString& LevelName,
                                                                          TArray<uint8>& LevelData)
{
	if constexpr (!bIsTextFormat)
	{
		FSaveGameMemoryArchive CompressorArchive(LevelData);
		const ESaveGameLoadResult ManifestResult = ValidateManifest(LevelData, CompressorArchive);
		if (ManifestResult != ESaveGameLoadResult::Success)
		{
			BroadcastLoadFailed(ManifestResult);
			return;
		}
		if (!SerializeCompressedData<true>(CompressorArchive, Data))
		{
			BroadcastLoadFailed(ESaveGameLoadResult::CorruptedData);
			return;
		}
	}
	// JSON: data was provided at construction.

	InitiateLevelLoad(LevelName);
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeLevelData(const FString& LevelName)
{
	if constexpr (bIsLoading && bIsTextFormat)
	{
		InitiateLevelLoad(LevelName);
	}
	else
	{
		checkNoEntry();
	}
}

template <bool bIsLoading, bool bIsTextFormat>
TArray<uint8> TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeStreamingLevelData(
    const TSoftObjectPtr<ULevelStreaming>& StreamingLevel)
{
	check(!bIsLoading);

	TRACE_BOOKMARK(TEXT("Begin: SerializeStreamingLevel[%s]"), bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

	ON_SCOPE_EXIT
	{
		TRACE_BOOKMARK(TEXT("End: SerializeStreamingLevel[%s]"), bIsTextFormat ? TEXT("Text") : TEXT("Binary"));
	};

	check(SerializeSubsystem.IsValid());

	// Double check that the level is loaded
	// This is to ensure that the level is loaded before we serialize the actors
	if (StreamingLevel->IsLevelLoaded())
		SerializeStreamingLevel(StreamingLevel);

	// Be sure to close this, as you'll be missing closed braces for JSON
	// archives
	StructuredArchive.Close();

	if constexpr (!bIsTextFormat && !bIsLoading)
	{
		return CompressAndWrapBlob(Data);
	}

	return Data;
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeStreamingLevelData(
    const TSoftObjectPtr<ULevelStreaming>& StreamingLevel, TArray<uint8>& StreamingLevelData)
{
	check(bIsLoading);

	TRACE_BOOKMARK(TEXT("Begin: DeserializeStreamingLevelData[%s]"), bIsTextFormat ? TEXT("Text") : TEXT("Binary"));

	if constexpr (!bIsTextFormat)
	{
		FSaveGameMemoryArchive CompressorArchive(StreamingLevelData);
		const ESaveGameLoadResult ManifestResult = ValidateManifest(StreamingLevelData, CompressorArchive);
		if (ManifestResult != ESaveGameLoadResult::Success)
		{
			BroadcastLoadFailed(ManifestResult);
			return;
		}
		if (!SerializeCompressedData<true>(CompressorArchive, Data))
		{
			BroadcastLoadFailed(ESaveGameLoadResult::CorruptedData);
			return;
		}
	}
	// JSON: data was provided at construction.

	if (StreamingLevel->IsLevelLoaded())
	{
		SerializeStreamingLevel(StreamingLevel);
	}
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::DeserializeStreamingLevelData(
    const TSoftObjectPtr<ULevelStreaming>& StreamingLevel)
{
	if constexpr (bIsLoading && bIsTextFormat)
	{
		if (StreamingLevel->IsLevelLoaded())
		{
			SerializeStreamingLevel(StreamingLevel);
		}
	}
	else
	{
		checkNoEntry();
	}
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::OnMapLoad(UWorld* World)
{
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
	check(SerializeSubsystem->GetWorld() == World);

	SerializeLevel(World->GetCurrentLevel());

	SerializeSubsystem->FinalizeLoad(ESaveGameLoadResult::Success);

	TRACE_BOOKMARK(TEXT("End: LoadSaveGame[%s]"), bIsTextFormat ? TEXT("Text") : TEXT("Binary"));
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeHeader()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeHeader);

	if (!bIsTextFormat)
	{
		// Store the version position so that we can serialize it in the header
		HeaderOffset = Archive.Tell();
	}

	FEngineVersion EngineVersion;
	FPackageFileVersion PackageVersion;

	if (!bIsLoading)
	{
		EngineVersion = FEngineVersion::Current();
		PackageVersion = GPackageFileUEVersion;
	}

	RootRecord << SA_VALUE(TEXT("EngineVersion"), EngineVersion);

	if (!bIsTextFormat)
	{
		// This doesn't have a structured archive serialize method
		Archive << PackageVersion;

		// We're a binary archive, so let's serialize where the version is
		// so that we can read it before loading anything
		RootRecord << SA_VALUE(TEXT("VersionsOffset"), VersionOffset);
	}

	if (bIsLoading)
	{
		Archive.SetEngineVer(EngineVersion);
		Archive.SetUEVer(PackageVersion);
		SavedEngineVersion = EngineVersion;
	}
}

// SERIALIZE PERSISTENT LEVEL
template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeLevel(const TSoftObjectPtr<ULevel>& Level)
{

	if (Level.IsValid())
	{
		FStructuredArchive::FSlot ActorsSlot = RootRecord.EnterField(TEXT("Actors"));

		SerializeActors(Level.Get(), SerializeSubsystem->PersistentLevelRecord->Actors->SaveGame, ActorsSlot);

		FStructuredArchive::FSlot DestroyedActorsSlot = RootRecord.EnterField(TEXT("DestroyedActors"));
		SerializeDestroyedActors(Level.Get(), *SerializeSubsystem->PersistentLevelRecord->Actors, DestroyedActorsSlot);
	}
}

// SERIALIZE STREAMING LEVEL
template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeStreamingLevel(
    const TSoftObjectPtr<ULevelStreaming>& StreamingLevel)
{

	if (StreamingLevel.IsValid())
	{
		FStructuredArchive::FSlot ActorsSlot = RootRecord.EnterField(TEXT("Actors"));

		FActorsStruct& StreamingActors =
		    *SerializeSubsystem->PersistentLevelRecord->StreamingLevels[StreamingLevel]->Actors;

		SerializeActors(StreamingLevel->GetLoadedLevel(), StreamingActors.SaveGame, ActorsSlot);

		FStructuredArchive::FSlot DestroyedActorsSlot = RootRecord.EnterField(TEXT("DestroyedActors"));
		SerializeDestroyedActors(StreamingLevel->GetLoadedLevel(), StreamingActors, DestroyedActorsSlot);
	}
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActors(ULevel* Level,
                                                                     TSet<TWeakObjectPtr<AActor>>& SaveGameActors,
                                                                     FStructuredArchive::FSlot& ActorsSlot)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeActors);
	check(SerializeSubsystem.IsValid());

	UWorld* World = SerializeSubsystem->GetWorld();
	if (!IsValid(World))
		return;

	if (!IsValid(Level))
		return;

	// Snapshot SaveGameActors before any iteration. During loading, SpawnActor
	// triggers OnActorPreSpawn → SaveGameActors.Add(), which mutates the set
	// while NumActors is live. During saving, OnSerialize side-effects could
	// similarly add actors. A snapshot prevents iterator invalidation in both cases.
	const TArray<TWeakObjectPtr<AActor>> ActorsSnapshot = SaveGameActors.Array();
	TArray<AActor*> Actors;
	int32 NumActors = ActorsSnapshot.Num();

	FStructuredArchive::FMap ActorsMap = ActorsSlot.EnterMap(NumActors);

	const uint64 ActorsPosition = Archive.Tell();

	if (bIsLoading)
	{
		TMap<FGuid, AActor*> SpawnIDs;
		QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_InitializeActors);

		for (const TWeakObjectPtr<AActor>& ActorPtr : ActorsSnapshot)
		{
			AActor* Actor = ActorPtr.Get();
			if (IsValid(Actor) && Actor->Implements<USaveGameSpawnActor>())
			{
				const FGuid SpawnID = ISaveGameSpawnActor::Execute_GetSpawnID(Actor);
				if (SpawnID.IsValid())
				{
					if (SpawnIDs.Contains(SpawnID))
					{
						UE_LOG(LogSaveGame,
						       Error,
						       TEXT("SerializeSubsystem: SpawnID collision — %s and %s share "
						            "SpawnID %s. Second actor will not receive save data."),
						       *SpawnIDs[SpawnID]->GetName(),
						       *Actor->GetName(),
						       *SpawnID.ToString());
						ensureAlwaysMsgf(false, TEXT("SpawnID collision detected — see log above."));
					}
					else
					{
						SpawnIDs.Add(SpawnID, Actor);
					}
				}
			}
		}

		Actors.SetNumZeroed(NumActors);

		for (int32 ActorIdx = 0; ActorIdx < NumActors; ++ActorIdx)
		{
			AActor*& Actor = Actors[ActorIdx];

			SerializeActor(
			    ActorsMap,
			    Actor,
			    [&](const FString& ActorName,
			        const FSoftClassPath& Class,
			        const FGuid& SpawnID,
			        FStructuredArchive::FSlot& ActorSlot)
			    {
				    ensureAlways(!ActorName.IsEmpty());

				    if (Class.IsNull())
				    {
					    Actor = FindObjectFast<AActor>(Level, *ActorName);
				    }
				    else if (SpawnID.IsValid() && SpawnIDs.Contains(SpawnID))
				    {
					    Actor = SpawnIDs[SpawnID];
				    }
				    else
				    {
					    UClass* ActorClass = Class.TryLoadClass<AActor>();

					    if (!ActorClass)
					    {
						    UE_LOG(LogSaveGame,
						           Error,
						           TEXT("Failed to load class '%s' for actor '%s' — "
						                "actor will be skipped."),
						           *Class.ToString(),
						           *ActorName);
						    return;
					    }

					    // Some actors (e.g. GameMode-spawned pawns) are dynamically created
					    // before the serializer runs OnMapLoad, so they already exist in the
					    // level under the same name. Try to reclaim the existing instance
					    // before attempting a new spawn to avoid the "Cannot generate unique
					    // name" fatal when bNoFail is true.
					    Actor = FindObjectFast<AActor>(Level, *ActorName);

					    if (!IsValid(Actor))
					    {
						    FActorSpawnParameters SpawnParameters;
						    SpawnParameters.OverrideLevel = Level;
						    SpawnParameters.Name = *ActorName;
						    SpawnParameters.bNoFail = false;

						    Actor = Level->GetWorld()->SpawnActor(ActorClass, nullptr, nullptr, SpawnParameters);

						    if (!IsValid(Actor))
						    {
							    UE_LOG(LogSaveGame,
							           Error,
							           TEXT("Failed to spawn actor '%s' of class '%s' — "
							                "actor will be skipped."),
							           *ActorName,
							           *Class.ToString());
							    return;
						    }
					    }

					    if (IsValid(Actor) && SpawnID.IsValid() && Actor->Implements<USaveGameSpawnActor>())
					    {
						    ISaveGameSpawnActor::Execute_SetSpawnID(Actor, SpawnID);
					    }
				    }

				    if (IsValid(Actor) && SpawnID.IsValid())
				    {
					    const FTopLevelAssetPath LevelAssetPath(Level->GetPackage()->GetFName(),
					                                            Level->GetOuter()->GetFName());
					    const FString ActorSubPath = LEVEL_SUBPATH_PREFIX + ActorName;
					    ProxyArchive.AddRedirect(FSoftObjectPath(LevelAssetPath, ActorSubPath), FSoftObjectPath(Actor));
				    }

				    // JSON loading uses a single-pass approach: FStructuredArchive
				    // with JSON is forward-only and cannot seek back for a second pass,
				    // so we serialize properties immediately after spawning/finding
				    // the actor, while ActorSlot is still open.
				    if constexpr (bIsTextFormat)
				    {
					    if (!IsValid(Actor))
					    {
						    UE_LOG(LogSaveGame,
						           Warning,
						           TEXT("Actor '%s' is invalid after spawn/find — "
						                "skipping property deserialization."),
						           *ActorName);
						    return;
					    }

					    SerializeActorData(Actor, ActorSlot);
				    }
			    });
		}

		// JSON loading is fully handled above in the single-pass lambda.
		// Skip the binary second pass (which seeks back and re-reads the map).
		if constexpr (bIsTextFormat)
		{
			return;
		}
	}

	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeActorData);

		// Binary loading: seek back to re-read actor data in a second pass.
		// (First pass only spawned/found actors; this pass serializes properties.)
		if (bIsLoading && !bIsTextFormat)
		{
			Archive.Seek(ActorsPosition);
		}

		for (int32 ActorIdx = 0; ActorIdx < NumActors; ++ActorIdx)
		{
			AActor* Actor;

			if constexpr (bIsLoading)
			{
				Actor = Actors[ActorIdx];
			}
			else
			{
				Actor = ActorsSnapshot[ActorIdx].Get();
			}

			if (!IsValid(Actor))
				continue;

			SerializeActor(
			    ActorsMap,
			    Actor,
			    [&](const FString&, const FSoftClassPath&, const FGuid&, FStructuredArchive::FSlot& ActorSlot)
			    {
				    SerializeActorData(Actor, ActorSlot);
			    });
		}
	}
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::BroadcastLoadFailed(ESaveGameLoadResult Result)
{
	if (USerializeSubsystem* Subsystem = SerializeSubsystem.Get())
	{
		Subsystem->FinalizeLoad(Result);
	}
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActorData(AActor* Actor,
                                                                        FStructuredArchive::FSlot& ActorSlot)
{
	Actor->SerializeScriptProperties(ActorSlot.EnterAttribute(TEXT("Properties")));

	if constexpr (bIsLoading)
	{
		// SetDefaultValue runs post-deserialization: ImportText_Direct requires a fully constructed
		// UObject with a valid CDO, which is not available while the FStructuredArchive is open.
		if (USerializeSubsystem* Subsystem = SerializeSubsystem.Get())
		{
			for (int32 StepIndex = 0; StepIndex < Subsystem->PendingDefaultMigrations.Num(); ++StepIndex)
			{
				TObjectPtr<UClass>& TargetClass = Subsystem->PendingDefaultMigrationClasses[StepIndex];
				if (!TargetClass)
				{
					TargetClass = Subsystem->PendingDefaultMigrations[StepIndex].OwnerClass.TryLoadClass<UObject>();
				}
				if (!TargetClass || !Actor->IsA(TargetClass))
					continue;
				const FMigration_SetDefaultValue& Migration = Subsystem->PendingDefaultMigrations[StepIndex];
				FProperty* Property = FindFProperty<FProperty>(Actor->GetClass(), Migration.PropertyName);
				if (!Property)
				{
					UE_LOG(LogSaveGame,
					       Warning,
					       TEXT("Migration: property '%s' not found on %s — skipping."),
					       *Migration.PropertyName.ToString(),
					       *Actor->GetClass()->GetName());
					continue;
				}
				void* PropertyData = Property->ContainerPtrToValuePtr<void>(Actor);
				const TCHAR* ImportResult =
				    Property->ImportText_Direct(*Migration.ExportedDefaultValue, PropertyData, Actor, PPF_None);
				if (!ImportResult)
				{
					UE_LOG(LogSaveGame,
					       Error,
					       TEXT("Migration: failed to parse value '%s' for %s::%s — skipping."),
					       *Migration.ExportedDefaultValue,
					       *Actor->GetClass()->GetName(),
					       *Migration.PropertyName.ToString());
				}
			}
		}
	}

	SerializeActorComponents(Actor, ActorSlot);

	FStructuredArchive::FSlot CustomDataSlot = ActorSlot.EnterAttribute(TEXT("Data"));
	FStructuredArchive::FRecord CustomDataRecord = CustomDataSlot.EnterRecord();
	FSaveGameArchive SaveGameArchive(CustomDataRecord, Actor);
	ISaveGameObject::Execute_OnSerialize(Actor, SaveGameArchive, bIsLoading);
}

// Serialize the actor's components
template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActorComponents(AActor*& Actor,
                                                                              FStructuredArchive::FSlot& ActorSlot)
{
	if (!Actor)
		return;

	TArray<UActorComponent*> Components = Actor->GetComponentsByInterface(USaveGameObject::StaticClass());

	if (Components.IsEmpty())
		return;

	int32 NumComponents = Components.Num();

	FStructuredArchive::FSlot ActorComponentsSlot = ActorSlot.EnterAttribute(TEXT("Components"));
	FStructuredArchive::FMap ComponentsMap = ActorComponentsSlot.EnterMap(NumComponents);

	for (TSoftObjectPtr<UActorComponent> ActorComponent : Components)
	{
		if (ActorComponent.IsValid())
		{
			SerializeActorComponent(ComponentsMap, ActorComponent);
		}
	}
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeDestroyedActors(
    ULevel* Level, FActorsStruct& ActorsRecord, FStructuredArchive::FSlot& DestroyedActorsSlot)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeDestroyedActors);
	check(IsValid(Level));

	int32 NumDestroyedActors;

	if constexpr (!bIsLoading)
	{
		NumDestroyedActors = ActorsRecord.Destroyed.Num();
	}

	FStructuredArchive::FArray DestroyedActorsArray = DestroyedActorsSlot.EnterArray(NumDestroyedActors);

	if constexpr (!bIsLoading)
	{
		if (ActorsRecord.Destroyed.IsEmpty())
			return;
	}

	if constexpr (bIsLoading)
	{
		ActorsRecord.Destroyed.Reset();
		ActorsRecord.Destroyed.Reserve(NumDestroyedActors);
	}

	TSet<FSoftObjectPath>::TConstIterator DestroyedActorsIt = ActorsRecord.Destroyed.CreateConstIterator();

	for (int32 ActorIdx = 0; ActorIdx < NumDestroyedActors; ++ActorIdx)
	{
		FName ActorName;

		if constexpr (!bIsLoading)
		{
			FString ActorSubPath = DestroyedActorsIt->GetSubPathString();
			ActorSubPath.RemoveFromStart(LEVEL_SUBPATH_PREFIX);
			ActorName = *ActorSubPath;
			++DestroyedActorsIt;
		}

		DestroyedActorsArray.EnterElement() << ActorName;

		if constexpr (bIsLoading)
		{
			if (AActor* DestroyedActor = FindObjectFast<AActor>(Level, ActorName))
			{
				ActorsRecord.Destroyed.Add(DestroyedActor);
				DestroyedActor->Destroy();
			}
		}
	}
}

// Tracks redirect source strings already registered with FCoreRedirects.
// FCoreRedirects has no unregister API, so we dedup across loads to avoid
// accumulating duplicate entries in the global redirect table.
static TSet<FString> GRegisteredSaveGameRedirects;

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeVersions()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeVersions);

	if constexpr (!bIsTextFormat)
	{
		VersionOffset = Archive.Tell();
	}

	FCustomVersionContainer VersionContainer;

	if constexpr (!bIsLoading)
	{
		VersionContainer = Archive.GetCustomVersions();
	}

	VersionContainer.Serialize(RootRecord.EnterField(TEXT("Versions")));

	if constexpr (bIsLoading)
	{
		Archive.SetCustomVersions(VersionContainer);

		const FCustomVersion* PluginVersion = VersionContainer.GetVersion(FSaveGameVersion::GUID);
		if (PluginVersion && PluginVersion->Version < static_cast<int32>(FSaveGameVersion::MinCompatibleVersion))
		{
			UE_LOG(LogSaveGame,
			       Error,
			       TEXT("Save file plugin version %d is below minimum compatible "
			            "version %d — cannot load."),
			       PluginVersion->Version,
			       static_cast<int32>(FSaveGameVersion::MinCompatibleVersion));
			BroadcastLoadFailed(ESaveGameLoadResult::IncompatibleVersion);
			return;
		}

		TArray<FCoreRedirect> PendingRedirects;
		const USaveGameSettings* MigSettings = GetDefault<USaveGameSettings>();
		for (const FInstancedStruct& StepInstance : MigSettings->Migrations)
		{
			if (const FMigration_RenameField* Rename = StepInstance.GetPtr<FMigration_RenameField>())
			{
				const FCustomVersion* CustomVersion = VersionContainer.GetVersion(Rename->VersionGuid);
				if (CustomVersion && CustomVersion->Version >= Rename->TargetVersion)
					continue;
				if (Rename->OldPropertyName.IsNone() || Rename->NewPropertyName.IsNone() ||
				    Rename->OwnerClassName.IsNone())
					continue;
				const FString OwnerStr = Rename->OwnerClassName.ToString();
				const FString Old = OwnerStr + TEXT(".") + Rename->OldPropertyName.ToString();
				if (GRegisteredSaveGameRedirects.Contains(Old))
					continue;
				GRegisteredSaveGameRedirects.Add(Old);
				const FString New = OwnerStr + TEXT(".") + Rename->NewPropertyName.ToString();
				PendingRedirects.Add(FCoreRedirect(ECoreRedirectFlags::Type_Property, Old, New));
				UE_LOG(LogSaveGame, Log, TEXT("Migration: registered property rename %s -> %s"), *Old, *New);
			}
			else if (const FMigration_SetDefaultValue* Default = StepInstance.GetPtr<FMigration_SetDefaultValue>())
			{
				const FCustomVersion* CustomVersion = VersionContainer.GetVersion(Default->VersionGuid);
				if (CustomVersion && CustomVersion->Version >= Default->TargetVersion)
					continue;
				// Deferred to SerializeActorData: ImportText_Direct needs a fully constructed UObject,
				// which is not yet available while the archive header is being parsed here.
				if (USerializeSubsystem* Subsystem = SerializeSubsystem.Get())
				{
					Subsystem->PendingDefaultMigrations.Add(*Default);
					Subsystem->PendingDefaultMigrationClasses.Add(Default->OwnerClass.TryLoadClass<UObject>());
					UE_LOG(LogSaveGame,
					       Log,
					       TEXT("Migration: queued default value for %s::%s"),
					       *Default->OwnerClass.ToString(),
					       *Default->PropertyName.ToString());
				}
			}
		}
		if (!PendingRedirects.IsEmpty())
		{
			FCoreRedirects::AddRedirectList(PendingRedirects, TEXT("SaveGameMigration"));
		}
	}

	if constexpr (!bIsTextFormat)
	{
		const uint64 CurrentOffset = Archive.Tell();
		// Patch the header so VersionsOffset points at the data we just wrote.
		Archive.Seek(HeaderOffset);
		SerializeHeader();
		Archive.Seek(CurrentOffset);
	}
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActor(
    FStructuredArchive::FMap& ActorMap,
    AActor*& Actor,
    TFunction<void(const FString&, const FSoftClassPath&, const FGuid&, FStructuredArchive::FSlot&)>&& BodyFunction)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeActor);

	FString ActorName;
	FSoftClassPath Class;
	FGuid SpawnID;

	if (!bIsLoading)
	{
		ActorName = Actor->GetName();

		if (!USaveGameFunctionLibrary::WasObjectLoaded(Actor))
		{
			// We're a spawned actor, stash the class
			Class = Actor->GetClass();
		}

		if (Actor->Implements<USaveGameSpawnActor>())
		{
			SpawnID = ISaveGameSpawnActor::Execute_GetSpawnID(Actor);
		}
	}

	FStructuredArchive::FSlot ActorSlot = ActorMap.EnterElement(ActorName);

	// If we have a class, we're a spawned actor
	if (TOptional<FStructuredArchive::FSlot> ClassSlot = ActorSlot.TryEnterAttribute(TEXT("Class"), !Class.IsNull()))
	{
		ClassSlot.GetValue() << Class;
	}

	// If we have a GUID, we're a spawn actor that needs to be mapped by GUID.
	// GuidSlot must be scoped so it destructs (and calls LeaveAttribute) before
	// BodyFunction enters the "Properties" attribute on the same ActorSlot.
	// Leaving the GUID scope open while entering Properties confuses the JSON
	// formatter's context stack and triggers a SharedPointer IsValid() assertion.
	if (TOptional<FStructuredArchive::FSlot> GuidSlot = ActorSlot.TryEnterAttribute(TEXT("GUID"), SpawnID.IsValid()))
	{
		GuidSlot.GetValue() << SpawnID;
	}

	// DataSize enables corruption resilience: if an actor's data is malformed during loading,
	// Seek(BeginDataPosition + DataSize) skips it cleanly and continues with the next actor.
	// Not stored in JSON mode — FJsonArchiveOutputFormatter does not support seek.
	uint64 DataSize;

	if (!bIsTextFormat)
	{
		Archive << DataSize;
	}

	const uint64 BeginDataPosition = Archive.Tell();

	BodyFunction(ActorName, Class, SpawnID, ActorSlot);

	if (!bIsTextFormat)
	{
		if (bIsLoading)
		{
			Archive.Seek(BeginDataPosition + DataSize);
		}
		else
		{
			const uint64 EndDataPosition = Archive.Tell();
			DataSize = EndDataPosition - BeginDataPosition;

			// Patch the DataSize placeholder we wrote before the actor's data.
			Archive.Seek(BeginDataPosition - sizeof(DataSize));
			Archive << DataSize;

			Archive.Seek(EndDataPosition);
		}
	}
}

template <bool bIsLoading, bool bIsTextFormat>
void TSaveGameSerializer<bIsLoading, bIsTextFormat>::SerializeActorComponent(
    FStructuredArchive::FMap& ComponentsMap, TSoftObjectPtr<UActorComponent>& ActorComponent)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_SaveGame_SerializeActorComponent);

	FString ActorComponentName;

	if (!bIsLoading)
	{
		ActorComponentName = ActorComponent->GetName();
	}

	FStructuredArchive::FSlot ActorComponentSlot = ComponentsMap.EnterElement(ActorComponentName);

	ActorComponent->SerializeScriptProperties(ActorComponentSlot.EnterAttribute(TEXT("Properties")));

	FStructuredArchive::FSlot CustomDataSlot = ActorComponentSlot.EnterAttribute(TEXT("Data"));
	FStructuredArchive::FRecord CustomDataRecord = CustomDataSlot.EnterRecord();

	// Encapsulate the record in something a Blueprint can access
	FSaveGameArchive SaveGameArchive(CustomDataRecord, ActorComponent.Get());

	ISaveGameObject::Execute_OnSerialize(ActorComponent.Get(), SaveGameArchive, bIsLoading);
}

// Instantiate the permutations of TSaveGameSerializer
#if WITH_TEXT_ARCHIVE_SUPPORT
template TSaveGameSerializer<false, true>;
template TSaveGameSerializer<true, true>;
#endif

template TSaveGameSerializer<false>;
template TSaveGameSerializer<true>;