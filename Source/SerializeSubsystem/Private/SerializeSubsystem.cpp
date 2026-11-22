// All Rights Reserved by TacoEaters

#include "SerializeSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/NetDriver.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Hash/CityHash.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "SaveGameFile.h"
#include "SaveGameObject.h"
#include "SaveGameSettings.h"
#include "SaveGameStore.h"
#include "Streaming/LevelStreamingDelegates.h"
#include "TimerManager.h"
#include "UObject/Package.h"

#if UE_WITH_IRIS
#include "Iris/ReplicationSystem/ReplicationSystem.h"
#include "Net/Iris/ReplicationSystem/EngineReplicationBridge.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSerializeSubsystem, Log, All);

namespace
{
	const TCHAR* const SpawnKeyPrefix = TEXT("G:");
	const TCHAR* const ClassKeyPrefix = TEXT("C:");

	bool IsSaveable(const AActor* Actor)
	{
		return IsValid(Actor) && Actor->Implements<USaveGameObject>();
	}

	bool IsServerGameWorld(const UWorld* World)
	{
		return World != nullptr && World->IsGameWorld() && World->GetNetMode() != NM_Client;
	}

	FString GetMapPath(const UWorld* World)
	{
		return UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	}

	// Levels whose actor names are unique across the whole map: the persistent level and World Partition cells.
	bool IsMapLevel(const ULevel* Level)
	{
		return Level->IsPersistentLevel() || Level->IsWorldPartitionRuntimeCell();
	}

	// A plain streaming sublevel by its package; None for the persistent level and World Partition cells.
	FName GetLevelKey(const ULevel* Level)
	{
		if (Level == nullptr || IsMapLevel(Level))
		{
			return NAME_None;
		}
		return FName(*UWorld::RemovePIEPrefix(Level->GetOutermost()->GetName()));
	}

	bool IsClassKey(const FName Key)
	{
		return Key.ToString().StartsWith(ClassKeyPrefix);
	}

	FSaveGamePristine MakePristine(const TArray<uint8>& Data)
	{
		FSaveGamePristine Pristine;
		Pristine.Hash = CityHash64(reinterpret_cast<const char*>(Data.GetData()), Data.Num());
		Pristine.Size = Data.Num();
		return Pristine;
	}

	FGuid GetSpawnId(const AActor* Actor)
	{
		if (Actor->Implements<USaveGameSpawnActor>())
		{
			return ISaveGameSpawnActor::Execute_GetSpawnID(Actor);
		}
		return FGuid();
	}

	// Destroys a placed actor the save says the game destroyed, before its level finishes appearing.
	void DestroyTombstoned(AActor* Actor, TArray<FSaveGamePendingNetDestruction>& PendingNetDestructions)
	{
		// The net driver treats a replicated placed actor as a startup actor only from InitializeNetworkActors,
		// which runs after this. Marked here, initially dormant, so its destruction is sent by name to clients
		// that stream the level in themselves (UNetDriver::NotifyActorDestroyed, Iris and legacy alike).
		if (Actor->GetIsReplicated())
		{
			Actor->bNetStartup = true;
			Actor->NetDormancy = DORM_Initial;
			// Under a live net driver (seamless travel, a cell streamed in) the destruction is stored again once
			// the level replicates: see FSaveGamePendingNetDestruction.
			if (const UNetDriver* NetDriver = Actor->GetWorld()->GetNetDriver();
			    NetDriver != nullptr && NetDriver->IsServer())
			{
				FSaveGamePendingNetDestruction Pending;
				Pending.Level    = Actor->GetLevel();
				Pending.Name     = Actor->GetFName();
				Pending.Location = Actor->GetActorLocation();
				PendingNetDestructions.Add(Pending);
			}
		}
		Actor->Destroy();
	}

#if UE_WITH_IRIS
	// Iris creates a level's replication group only around a replicating actor of that level, so a level whose only
	// replicated actors were tombstoned never gets one. The group is created around the game state for this one call
	// and the game state is taken out again at once; what remains is the empty group, filtered by level visibility.
	void EnsureLevelGroup(const UWorld& World,
	                      UReplicationSystem& ReplicationSystem,
	                      UEngineReplicationBridge& Bridge,
	                      const ULevel& Level)
	{
		if (Bridge.GetContainerGroup(&Level).IsValid())
		{
			return;
		}
		const AGameStateBase* Anchor = World.GetGameState();
		if (Anchor == nullptr)
		{
			return;
		}
		const UE::Net::FNetRefHandle AnchorHandle = Bridge.GetReplicatedRefHandle(Anchor);
		if (!AnchorHandle.IsValid())
		{
			return;
		}
		Bridge.AddRootObjectToContainerGroup(Anchor, &Level);
		if (const UE::Net::FNetObjectGroupHandle Group = Bridge.GetContainerGroup(&Level); Group.IsValid())
		{
			ReplicationSystem.RemoveFromGroup(Group, AnchorHandle);
		}
	}
#endif

	// Stores the pending destructions of Level (of every level when null) for clients, scoped to the level's
	// replication group so a client gets them once it has the level.
	void StorePendingNetDestructions(const UWorld& World,
	                                 const ULevel* Level,
	                                 TArray<FSaveGamePendingNetDestruction>& PendingNetDestructions)
	{
#if UE_WITH_IRIS
		UReplicationSystem*       ReplicationSystem = nullptr;
		UEngineReplicationBridge* Bridge            = nullptr;
		if (const UNetDriver* NetDriver = World.GetNetDriver(); NetDriver != nullptr)
		{
			ReplicationSystem = NetDriver->GetReplicationSystem();
		}
		if (ReplicationSystem != nullptr)
		{
			Bridge = ReplicationSystem->GetReplicationBridgeAs<UEngineReplicationBridge>();
		}
#endif
		for (int32 Index = PendingNetDestructions.Num() - 1; Index >= 0; --Index)
		{
			const FSaveGamePendingNetDestruction& Pending      = PendingNetDestructions[Index];
			ULevel*                               PendingLevel = Pending.Level.Get();
			if (PendingLevel != nullptr && Level != nullptr && PendingLevel != Level)
			{
				continue;
			}
#if UE_WITH_IRIS
			// The legacy driver keys destructions by level itself; only Iris needs the second store.
			if (PendingLevel != nullptr && Bridge != nullptr)
			{
				EnsureLevelGroup(World, *ReplicationSystem, *Bridge, *PendingLevel);
				const UE::Net::FDestructionParameters Parameters{
				    .Location                        = Pending.Location,
				    .Container                       = PendingLevel,
				    .bUseDistanceBasedPrioritization = true,
				    .NetFactoryId                    = Bridge->GetActorFactoryId(),
				};
				Bridge->AddStaticDestructionInfo(Pending.Name.ToString(), PendingLevel, Parameters);
			}
#endif
			PendingNetDestructions.RemoveAtSwap(Index);
		}
	}

	FAutoConsoleCommandWithWorld DumpCommand(TEXT("SaveGame.Dump"),
	                                         TEXT("Logs every recorded save state: scope, key, flags and size."),
	                                         FConsoleCommandWithWorldDelegate::CreateLambda(
	                                             [](const UWorld* World)
	                                             {
		                                             const UGameInstance* GameInstance = nullptr;
		                                             if (World != nullptr)
		                                             {
			                                             GameInstance = World->GetGameInstance();
		                                             }
		                                             if (GameInstance == nullptr)
		                                             {
			                                             return;
		                                             }
		                                             if (const USerializeSubsystem* Subsystem =
		                                                     GameInstance->GetSubsystem<USerializeSubsystem>())
		                                             {
			                                             UE_LOG(LogSerializeSubsystem,
			                                                    Display,
			                                                    TEXT("%s"),
			                                                    *Subsystem->DescribeRecords());
		                                             }
	                                             }));
} // namespace

void USerializeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Store = MakeShared<FSaveGameStore>();
	if (!GetDefault<USaveGameSettings>()->GameId.IsValid())
	{
		UE_LOG(LogSerializeSubsystem, Error, TEXT("Project Settings -> Save Game has no GameId: saves cannot tell games apart."));
	}

	FWorldDelegates::OnPostWorldInitialization.AddUObject(this, &ThisClass::HandlePostWorldInitialization);
	FWorldDelegates::OnWorldInitializedActors.AddUObject(this, &ThisClass::HandleWorldInitializedActors);
	FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &ThisClass::HandleWorldBeginTearDown);
	FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::HandleWorldCleanup);
	FWorldDelegates::PreLevelRemovedFromWorld.AddUObject(this, &ThisClass::HandlePreLevelRemoved);
	FWorldDelegates::LevelAddedToWorld.AddUObject(this, &ThisClass::HandleLevelAddedToWorld);
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::HandlePostLoadMap);
	FLevelStreamingDelegates::OnLevelBeginMakingVisible.AddUObject(this, &ThisClass::HandleLevelBeginMakingVisible);
	FGameModeEvents::OnGameModePostLoginEvent().AddUObject(this, &ThisClass::HandlePostLogin);

	// The game instance may already run a world (PIE creates it before the subsystems).
	EnsureWorld(GetWorld());
}

void USerializeSubsystem::Deinitialize()
{
	FWorldDelegates::OnPostWorldInitialization.RemoveAll(this);
	FWorldDelegates::OnWorldInitializedActors.RemoveAll(this);
	FWorldDelegates::OnWorldBeginTearDown.RemoveAll(this);
	FWorldDelegates::OnWorldCleanup.RemoveAll(this);
	FWorldDelegates::PreLevelRemovedFromWorld.RemoveAll(this);
	FWorldDelegates::LevelAddedToWorld.RemoveAll(this);
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
	FLevelStreamingDelegates::OnLevelBeginMakingVisible.RemoveAll(this);
	FGameModeEvents::OnGameModePostLoginEvent().RemoveAll(this);

	ReleaseWorld();
	Super::Deinitialize();
}

TArray<uint8> USerializeSubsystem::CaptureSaveFile(const TArray<uint8>& UserMeta)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USerializeSubsystem::CaptureSaveFile);

	if (UWorld* World = GetWorld(); IsLoadingSaveGame() || !EnsureWorld(World) || !IsTrackedWorld(World))
	{
		return TArray<uint8>();
	}

	CaptureAll();

	FSaveGameFileHeader Header;
	Header.GameId        = FSaveGameFile::GetProjectGameId();
	Header.MapPath       = WorldState->MapPath;
	Header.SavedAtUtc    = FDateTime::UtcNow();
	Header.EngineVersion = FEngineVersion::Current();
	Header.UserMeta      = UserMeta;
	return FSaveGameFile::Write(Header, Store->WriteBody());
}

ESaveGameLoadResult USerializeSubsystem::LoadSaveFile(const TArray<uint8>& File)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USerializeSubsystem::LoadSaveFile);

	UWorld* World                    = GetWorld();
	ESaveGameLoadResult Result       = ESaveGameLoadResult::Success;
	const TSharedRef<FSaveGameStore> NewStore = MakeShared<FSaveGameStore>();
	FSaveGameFileHeader Header;
	TArray<uint8> Body;

	// A travel already pending makes ServerTravel accept and do nothing: the load would never land.
	if (IsLoadingSaveGame() || !IsServerGameWorld(World) || !World->NextURL.IsEmpty() || World->IsInSeamlessTravel())
	{
		Result = ESaveGameLoadResult::Busy;
	}
	if (Result == ESaveGameLoadResult::Success)
	{
		Result = FSaveGameFile::Read(File, Header, Body);
	}
	if (Result == ESaveGameLoadResult::Success &&
	    !GetDefault<USaveGameSettings>()->IsLoadingFromIncompatibleEngineVersionAllowed() &&
	    !Header.EngineVersion.IsCompatibleWith(FEngineVersion::Current()))
	{
		Result = ESaveGameLoadResult::EngineVersionMismatch;
	}
	// The whole file is read before anything changes: a bad save never takes the player out of the current world.
	if (Result == ESaveGameLoadResult::Success &&
	    (!FSaveGameStore::ReadBody(Body, *NewStore) || !NewStore->EnterMap(Header.MapPath)))
	{
		Result = ESaveGameLoadResult::CorruptedData;
	}
	if (Result == ESaveGameLoadResult::Success && !FPackageName::DoesPackageExist(Header.MapPath))
	{
		Result = ESaveGameLoadResult::MapMissing;
	}
	if (Result == ESaveGameLoadResult::Success)
	{
		// ServerTravel, not SeamlessTravel on the world: clients travel with the host. Relative, so the options of
		// the running session (?listen) carry over; an absolute URL lost them in PIE and dropped every client.
		if (!World->ServerTravel(Header.MapPath, false))
		{
			Result = ESaveGameLoadResult::MapMissing;
		}
	}

	if (Result != ESaveGameLoadResult::Success)
	{
		UE_LOG(LogSerializeSubsystem, Warning, TEXT("Load refused: %s."), *UEnum::GetValueAsString(Result));
		// OnLoadCompleted belongs to the load in flight: its listeners must not hear this request's refusal.
		if (!IsLoadingSaveGame())
		{
			OnLoadCompleted.Broadcast(Result);
		}
		return Result;
	}

	Store                = NewStore;
	LoadTargetMap        = Header.MapPath;
	bDiscardCurrentWorld = true;
	PendingRespawns.Reset();
	return Result;
}

void USerializeSubsystem::StartNewGame()
{
	// The store a load is travelling with belongs to that load.
	if (IsLoadingSaveGame())
	{
		UE_LOG(LogSerializeSubsystem, Warning, TEXT("StartNewGame ignored: a load is in flight."));
		return;
	}
	Store = MakeShared<FSaveGameStore>();
	if (WorldState.IsValid())
	{
		Store->EnterMap(WorldState->MapPath);
	}
}

void USerializeSubsystem::RestoreActor(AActor* Actor)
{
	if (!IsSaveable(Actor) || !EnsureWorld(Actor->GetWorld()) || !IsTrackedWorld(Actor->GetWorld()))
	{
		return;
	}

	// First seen here: the game spawned it before this world was tracked. A copy, not a pointer into the map:
	// MakeKey runs game code that may spawn or destroy actors and so edit it.
	FSaveGameTrackedActor Tracked;
	Tracked.bSpawned = true;
	if (const FSaveGameTrackedActor* Found = WorldState->Tracked.Find(Actor))
	{
		Tracked = *Found;
	}
	if (Tracked.bRestored)
	{
		return;
	}

	const FActorKey Key = MakeKey(Actor, Tracked.bSpawned);
	// A respawnable actor receives its record only when the plugin spawns it.
	if (Tracked.bSpawned && Key.bRespawn)
	{
		return;
	}
	// A class key names a singleton: with two of them alive neither can tell the record is its own.
	if (IsClassKey(Key.Key) && CountTrackedOfClass(Actor->GetClass()) > 1)
	{
		UE_LOG(LogSerializeSubsystem,
		       Warning,
		       TEXT("Several spawned '%s' without a spawn id: none is restored."),
		       *Actor->GetClass()->GetName());
		return;
	}
	Tracked.bRestored = true;
	WorldState->Tracked.Add(Actor, Tracked);

	const FSaveGameTable* Table = &Store->MapTable;
	if (Key.bGlobal)
	{
		Table = &Store->GlobalTable;
	}
	if (const FSaveGameRecord* Record = Table->Records.Find(Key.Key))
	{
		const FSaveGameRecord Copy = *Record;
		ApplyRecord(Actor, Copy);
	}
}

bool USerializeSubsystem::IsLoadingSaveGame() const
{
	return !LoadTargetMap.IsEmpty();
}

FString USerializeSubsystem::GetActorSaveKey(const AActor* Actor) const
{
	if (!IsSaveable(Actor))
	{
		return FString();
	}
	bool bSpawned = false;
	if (WorldState.IsValid())
	{
		if (const FSaveGameTrackedActor* Tracked = WorldState->Tracked.Find(Actor))
		{
			bSpawned = Tracked->bSpawned;
		}
	}
	return MakeKey(Actor, bSpawned).Key.ToString();
}

FString USerializeSubsystem::DescribeRecords() const
{
	TStringBuilder<4096> Text;
	const auto Describe = [&Text](const TCHAR* Scope, const FSaveGameTable& Table)
	{
		for (const TPair<FName, FSaveGameRecord>& Pair : Table.Records)
		{
			const FSaveGameRecord& Record = Pair.Value;
			Text.Appendf(TEXT("\n  [%s] %s"), Scope, *Pair.Key.ToString());
			if (Record.HasFlag(SaveGameRecordFlags::Destroyed))
			{
				Text.Append(TEXT(" destroyed"));
			}
			if (Record.HasFlag(SaveGameRecordFlags::Spawned))
			{
				Text.Append(TEXT(" spawned"));
			}
			if (Record.HasFlag(SaveGameRecordFlags::Respawn))
			{
				Text.Append(TEXT(" respawn"));
			}
			Text.Appendf(TEXT(" %d B"), Record.Data.Num());
		}
	};

	Text.Appendf(TEXT("Save records: %d global, %d in %s, %d other maps"),
	             Store->GlobalTable.Records.Num(),
	             Store->MapTable.Records.Num(),
	             *Store->MapTablePath,
	             Store->MapBlobs.Num());
	if (WorldState.IsValid())
	{
		Text.Appendf(TEXT("\n  tracked %d, pristine %d"), WorldState->Tracked.Num(), WorldState->Pristine.Num());
	}
	Describe(TEXT("global"), Store->GlobalTable);
	Describe(TEXT("map"), Store->MapTable);
	return Text.ToString();
}

bool USerializeSubsystem::EnsureWorld(UWorld* World)
{
	if (!IsServerGameWorld(World) || World->GetGameInstance() != GetGameInstance())
	{
		return false;
	}
	if (WorldState.IsValid() && WorldState->World.Get() == World)
	{
		return true;
	}

	ReleaseWorld();
	WorldState                  = MakeShared<FSaveGameWorldState>();
	WorldState->World           = World;
	WorldState->MapPath         = GetMapPath(World);
	WorldState->PreSpawnHandle  = World->AddOnActorPreSpawnInitialization(
        FOnActorSpawned::FDelegate::CreateUObject(this, &ThisClass::HandleActorPreSpawn));
	WorldState->DestroyedHandle = World->AddOnActorDestroyedHandler(
	    FOnActorDestroyed::FDelegate::CreateUObject(this, &ThisClass::HandleActorDestroyed));
	WorldState->BeginPlayHandle = World->OnWorldBeginPlay.AddUObject(this, &ThisClass::HandleWorldBeginPlay);

	if (!Store->EnterMap(WorldState->MapPath))
	{
		UE_LOG(LogSerializeSubsystem, Error, TEXT("Records of '%s' are damaged; the map starts as authored."), *WorldState->MapPath);
	}
	bDiscardCurrentWorld = false;
	return true;
}

void USerializeSubsystem::ReleaseWorld()
{
	if (WorldState.IsValid())
	{
		if (UWorld* World = WorldState->World.Get())
		{
			World->RemoveOnActorPreSpawnInitialization(WorldState->PreSpawnHandle);
			World->RemoveOnActorDestroyedHandler(WorldState->DestroyedHandle);
			World->OnWorldBeginPlay.Remove(WorldState->BeginPlayHandle);
		}
	}
	WorldState.Reset();
	PendingRespawns.Reset();
}

bool USerializeSubsystem::IsTrackedWorld(const UWorld* World) const
{
	return World != nullptr && WorldState.IsValid() && WorldState->World.Get() == World && !bDiscardCurrentWorld;
}

USerializeSubsystem::FActorKey USerializeSubsystem::MakeKey(const AActor* Actor, const bool bSpawned)
{
	FActorKey Result;
	if (!bSpawned)
	{
		FString Key = Actor->GetName();
		if (const FName LevelKey = GetLevelKey(Actor->GetLevel()); !LevelKey.IsNone())
		{
			Key = LevelKey.ToString() + TEXT(":") + Key;
		}
		Result.Key = FName(*Key);
		return Result;
	}

	if (const FGuid SpawnId = GetSpawnId(Actor); SpawnId.IsValid())
	{
		Result.Key      = FName(FString(SpawnKeyPrefix) + SpawnId.ToString(EGuidFormats::Digits));
		Result.bRespawn = ISaveGameSpawnActor::Execute_ShouldRespawnOnLoad(Actor);
		Result.bGlobal  = !Result.bRespawn;
		return Result;
	}

	// Spawned without an identity: a singleton of its class (GameState).
	Result.Key     = FName(FString(ClassKeyPrefix) + Actor->GetClass()->GetPathName());
	Result.bGlobal = true;
	return Result;
}

int32 USerializeSubsystem::CountTrackedOfClass(const UClass* Class) const
{
	int32 Count = 0;
	for (const TPair<TWeakObjectPtr<AActor>, FSaveGameTrackedActor>& Pair : WorldState->Tracked)
	{
		if (const AActor* Actor = Pair.Key.Get(); IsSaveable(Actor) && Actor->GetClass() == Class)
		{
			++Count;
		}
	}
	return Count;
}

TArray<TPair<AActor*, FSaveGameTrackedActor>> USerializeSubsystem::SnapshotTracked() const
{
	TArray<TPair<AActor*, FSaveGameTrackedActor>> Snapshot;
	Snapshot.Reserve(WorldState->Tracked.Num());
	for (TMap<TWeakObjectPtr<AActor>, FSaveGameTrackedActor>::TIterator It(WorldState->Tracked); It; ++It)
	{
		AActor* Actor = It->Key.Get();
		if (!IsSaveable(Actor))
		{
			It.RemoveCurrent();
			continue;
		}
		Snapshot.Emplace(Actor, It->Value);
	}
	return Snapshot;
}

void USerializeSubsystem::ApplyLevel(const ULevel* Level) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USerializeSubsystem::ApplyLevel);
	UE_LOG(LogSerializeSubsystem, Verbose, TEXT("Applying level %s (%d actors)."), *Level->GetOutermost()->GetName(), Level->Actors.Num());

	// Shown again with the same level object: hidden without unloading, or unloaded and asked back before garbage
	// collection (World Partition reuses it). Its actors are as the game left them and their pristine hashes still
	// hold; hashing them again would take the changes for authored state and drop their records. A spawned actor
	// whose key came back alive elsewhere meanwhile (respawned when its cell unloaded) is the stale copy.
	TArray<TPair<TWeakObjectPtr<AActor>, FSaveGameTrackedActor>> Hidden;
	if (WorldState->HiddenLevels.RemoveAndCopyValue(Level, Hidden))
	{
		TSet<FName> Alive;
		for (const TPair<AActor*, FSaveGameTrackedActor>& Entry : SnapshotTracked())
		{
			if (Entry.Value.bSpawned)
			{
				Alive.Add(MakeKey(Entry.Key, true).Key);
			}
		}
		for (const TPair<TWeakObjectPtr<AActor>, FSaveGameTrackedActor>& Entry : Hidden)
		{
			AActor* Actor = Entry.Key.Get();
			if (!IsSaveable(Actor))
			{
				continue;
			}
			if (Entry.Value.bSpawned && Alive.Contains(MakeKey(Actor, true).Key))
			{
				Actor->Destroy();
				continue;
			}
			WorldState->Tracked.Add(Actor, Entry.Value);
		}
		return;
	}

	for (const TArray<TObjectPtr<AActor>> Actors = Level->Actors; AActor* Actor : Actors)
	{
		if (!IsSaveable(Actor) || WorldState->Tracked.Contains(Actor))
		{
			continue;
		}

		// An actor with a spawn identity is never a placed one, even when it reached the level without a spawn the
		// plugin saw: seamless travel carries player states over. Keyed by name it would leave a tombstone behind.
		if (GetSpawnId(Actor).IsValid())
		{
			FSaveGameTrackedActor Carried;
			Carried.bSpawned = true;
			WorldState->Tracked.Add(Actor, Carried);
			continue;
		}

		// A copy: the game code run below may destroy actors and so edit the table.
		const FActorKey Key = MakeKey(Actor, false);
		TOptional<FSaveGameRecord> Record;
		if (const FSaveGameRecord* Found = Store->MapTable.Records.Find(Key.Key))
		{
			Record = *Found;
		}
		if (Record.IsSet() && Record->HasFlag(SaveGameRecordFlags::Destroyed))
		{
			DestroyTombstoned(Actor, WorldState->PendingNetDestructions);
			continue;
		}

		// The state the level authored: a later capture equal to it means there is nothing to record.
		if (TArray<uint8> Pristine; FSaveGameActorSerializer::Write(Actor, *Store->CurrentVersions, Pristine))
		{
			WorldState->Pristine.Add(Key.Key, MakePristine(Pristine));
#if !UE_BUILD_SHIPPING
			WorldState->PristineData.Add(Key.Key, Pristine);
#endif
		}

		FSaveGameTrackedActor Tracked;
		Tracked.bRestored = true;
		WorldState->Tracked.Add(Actor, Tracked);
		if (!Record.IsSet())
		{
			continue;
		}
		ApplyRecord(Actor, *Record);
		// An initially dormant placed actor is not replicated at all: clients would keep the authored state of
		// their own copy. Dormant-all still sends it once to every connection.
		if (Actor->GetIsReplicated() && Actor->NetDormancy == DORM_Initial)
		{
			Actor->NetDormancy = DORM_DormantAll;
		}
	}
}

void USerializeSubsystem::ApplyGlobalRecords()
{
	TArray<AActor*> Pending;
	for (const TPair<TWeakObjectPtr<AActor>, FSaveGameTrackedActor>& Pair : WorldState->Tracked)
	{
		if (Pair.Value.bSpawned && !Pair.Value.bRestored && Pair.Key.IsValid())
		{
			Pending.Add(Pair.Key.Get());
		}
	}
	for (AActor* Actor : Pending)
	{
		RestoreActor(Actor);
	}
}

void USerializeSubsystem::RespawnRecords(ULevel* Level, const FName LevelKey, const TArray<FName>* Keys) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USerializeSubsystem::RespawnRecords);

	// Alive = tracked in the world. A copy left in a level that went away does not count: if that level comes back,
	// ApplyLevel destroys the copy.
	TSet<FName> Alive;
	for (const TPair<AActor*, FSaveGameTrackedActor>& Entry : SnapshotTracked())
	{
		if (Entry.Value.bSpawned)
		{
			Alive.Add(MakeKey(Entry.Key, true).Key);
		}
	}

	// Keys first, spawns after: a spawned actor's own load may edit the table.
	TArray<FName> ToSpawn;
	for (const TPair<FName, FSaveGameRecord>& Pair : Store->MapTable.Records)
	{
		const bool bSelected = Keys == nullptr || Keys->Contains(Pair.Key);
		if (bSelected && Pair.Value.HasFlag(SaveGameRecordFlags::Respawn) &&
		    (Keys != nullptr || Pair.Value.LevelKey == LevelKey) && !Alive.Contains(Pair.Key))
		{
			ToSpawn.Add(Pair.Key);
		}
	}

	for (const FName Key : ToSpawn)
	{
		if (const FSaveGameRecord* Record = Store->MapTable.Records.Find(Key))
		{
			const FSaveGameRecord Copy = *Record;
			Respawn(Level, Key, Copy);
		}
	}
}

AActor* USerializeSubsystem::Respawn(ULevel* Level, const FName Key, const FSaveGameRecord& Record) const
{
	UClass* Class = Record.Class.TryLoadClass<AActor>();
	FGuid SpawnId;
	FGuid::Parse(Key.ToString().RightChop(FCString::Strlen(SpawnKeyPrefix)), SpawnId);
	if (Class == nullptr || !SpawnId.IsValid())
	{
		UE_LOG(LogSerializeSubsystem, Error, TEXT("Cannot respawn '%s' of class '%s'."), *Key.ToString(), *Record.Class.ToString());
		return nullptr;
	}

	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel                  = Level;
	Parameters.bDeferConstruction             = true;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = Level->GetWorld()->SpawnActor(Class, &Record.Transform, Parameters);
	if (Actor == nullptr)
	{
		return nullptr;
	}

	ISaveGameSpawnActor::Execute_SetSpawnID(Actor, SpawnId);
	// Construction scripts add components a record may name: the state goes in once the actor is whole.
	Actor->FinishSpawning(Record.Transform);
	if (FSaveGameTrackedActor* Tracked = WorldState->Tracked.Find(Actor))
	{
		Tracked->bRestored = true;
	}
	ApplyRecord(Actor, Record);
	return Actor;
}

void USerializeSubsystem::ApplyRecord(AActor* Actor, const FSaveGameRecord& Record)
{
	if (!Record.Versions.IsValid())
	{
		return;
	}
	if (!FSaveGameActorSerializer::Read(Actor, Record.Data, *Record.Versions))
	{
		UE_LOG(LogSerializeSubsystem, Error, TEXT("The record of '%s' is malformed; it was restored in part."), *Actor->GetName());
	}
	// Restored after BeginPlay (a late player, a respawn during play): wake a dormant actor so clients get it.
	if (Actor->HasActorBegunPlay() && Actor->NetDormancy > DORM_Awake)
	{
		Actor->FlushNetDormancy();
	}
}

void USerializeSubsystem::RespawnPending()
{
	const UWorld* World = nullptr;
	if (WorldState.IsValid())
	{
		World = WorldState->World.Get();
	}
	if (!IsTrackedWorld(World) || PendingRespawns.IsEmpty())
	{
		return;
	}
	const TArray<FName> Keys = MoveTemp(PendingRespawns);
	RespawnRecords(World->PersistentLevel, NAME_None, &Keys);
}

void USerializeSubsystem::CaptureActor(AActor* Actor, const FSaveGameTrackedActor& Tracked, const FActorKey& Key) const
{
	FSaveGameTable* Table = &Store->MapTable;
	if (Key.bGlobal)
	{
		Table = &Store->GlobalTable;
	}

	TArray<uint8> Data;
	if (!FSaveGameActorSerializer::Write(Actor, *Store->CurrentVersions, Data))
	{
		Table->Records.Remove(Key.Key);
		return;
	}

	if (!Tracked.bSpawned)
	{
		if (const FSaveGamePristine* Pristine = WorldState->Pristine.Find(Key.Key))
		{
			const FSaveGamePristine Current = MakePristine(Data);
			if (Current.Hash == Pristine->Hash && Current.Size == Pristine->Size)
			{
				Table->Records.Remove(Key.Key);
				return;
			}
		}
	}

	FSaveGameRecord& Record = Table->Records.FindOrAdd(Key.Key);
	Record                  = FSaveGameRecord();
	Record.Data             = MoveTemp(Data);
	Record.Versions         = Store->CurrentVersions;
	if (Tracked.bSpawned)
	{
		Record.Flags = SaveGameRecordFlags::Spawned;
		if (Key.bRespawn)
		{
			Record.Flags |= SaveGameRecordFlags::Respawn;
		}
		Record.Class     = Actor->GetClass();
		Record.Transform = Actor->GetActorTransform();
		Record.LevelKey  = GetLevelKey(Actor->GetLevel());
	}
}

void USerializeSubsystem::CaptureAll() const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USerializeSubsystem::CaptureAll);

	TSet<FName> Singletons;
	for (const TPair<AActor*, FSaveGameTrackedActor>& Entry : SnapshotTracked())
	{
		AActor* Actor = Entry.Key;
		if (!IsSaveable(Actor))
		{
			continue;
		}

		const FActorKey Key = MakeKey(Actor, Entry.Value.bSpawned);
		if (IsClassKey(Key.Key))
		{
			bool bAlreadySeen = false;
			Singletons.Add(Key.Key, &bAlreadySeen);
			if (bAlreadySeen)
			{
				UE_LOG(LogSerializeSubsystem,
				       Warning,
				       TEXT("Two spawned '%s' without a spawn id: only the first is saved."),
				       *Actor->GetClass()->GetName());
				continue;
			}
		}
		CaptureActor(Actor, Entry.Value, Key);
	}
}

void USerializeSubsystem::FinishLoad(const ESaveGameLoadResult Result)
{
	LoadTargetMap.Empty();
	OnLoadCompleted.Broadcast(Result);
}

void USerializeSubsystem::HandlePostWorldInitialization(UWorld* World, UWorld::InitializationValues /*Values*/)
{
	EnsureWorld(World);
}

void USerializeSubsystem::HandleWorldInitializedActors(const FActorsInitializedParams& Params)
{
	if (!EnsureWorld(Params.World) || !IsTrackedWorld(Params.World))
	{
		return;
	}
	// Every level already in the world: the persistent one and the levels shown before actors were initialized.
	for (ULevel* Level : Params.World->GetLevels())
	{
		if (Level == nullptr)
		{
			continue;
		}
		ApplyLevel(Level);
		if (!IsMapLevel(Level))
		{
			RespawnRecords(Level, GetLevelKey(Level));
		}
	}
	ApplyGlobalRecords();
	RespawnRecords(Params.World->PersistentLevel, NAME_None);
}

void USerializeSubsystem::HandleWorldBeginTearDown(UWorld* World) const
{
	// Before EndPlay: the states the game is leaving stay with the session for when it comes back.
	if (IsTrackedWorld(World))
	{
		CaptureAll();
	}
}

void USerializeSubsystem::HandleWorldCleanup(UWorld* World, bool /*bSessionEnded*/, bool /*bCleanupResources*/)
{
	if (WorldState.IsValid() && WorldState->World.Get() == World)
	{
		ReleaseWorld();
	}
}

void USerializeSubsystem::HandlePostLoadMap(UWorld* World)
{
	if (!EnsureWorld(World) || !IsTrackedWorld(World))
	{
		return;
	}

	// Players already in the world (the host logs in while the map loads; seamless travel keeps controllers).
	ApplyGlobalRecords();

	if (!IsLoadingSaveGame() || World->IsInSeamlessTravel())
	{
		return;
	}
	if (WorldState->MapPath == LoadTargetMap)
	{
		FinishLoad(ESaveGameLoadResult::Success);
		return;
	}
	// The travel ended somewhere else: the engine fell back to another map.
	UE_LOG(LogSerializeSubsystem, Error, TEXT("Load travelled to '%s' instead of '%s'."), *WorldState->MapPath, *LoadTargetMap);
	FinishLoad(ESaveGameLoadResult::MapMissing);
}

void USerializeSubsystem::HandleLevelBeginMakingVisible(UWorld* World,
                                                        const ULevelStreaming* /*StreamingLevel*/,
                                                        ULevel* Level)
{
	// Before the world initializes its actors AActor::ProcessEvent runs no events, OnSerialize included: a level
	// shown that early (the starting cells) is applied in HandleWorldInitializedActors, still before BeginPlay.
	if (Level == nullptr || !EnsureWorld(World) || !IsTrackedWorld(World) || !World->AreActorsInitialized())
	{
		return;
	}
#if WITH_EDITOR
	// Editor builds rerun construction scripts while a level loaded from disk registers its components, a step after
	// this one (World.cpp AddToWorld, Level.cpp IncrementalRunConstructionScripts): Blueprint components would be
	// recreated after their record went in. That pass runs now instead, and the engine finds it done. Cooked builds
	// never rerun them.
	if (!Level->bHasRerunConstructionScripts)
	{
		Level->UpdateLevelComponents(true);
	}
#endif
	ApplyLevel(Level);
}

void USerializeSubsystem::HandleLevelAddedToWorld(ULevel* Level, UWorld* World) const
{
	// Spawned actors of a plain sublevel come back once the level transform is applied: spawned earlier, the
	// level would move them by its offset a second time.
	// A level added before actors are initialized gets its respawns in HandleWorldInitializedActors.
	if (Level != nullptr && !IsMapLevel(Level) && IsTrackedWorld(World) && World->AreActorsInitialized())
	{
		RespawnRecords(Level, GetLevelKey(Level));
	}
	// Its actors have begun play and replicate now. Levels added before the world began play wait for that.
	if (Level != nullptr && IsTrackedWorld(World) && World->HasBegunPlay())
	{
		StorePendingNetDestructions(*World, Level, WorldState->PendingNetDestructions);
	}
}

void USerializeSubsystem::HandleWorldBeginPlay() const
{
	if (const UWorld* World = WorldState->World.Get(); World != nullptr && IsTrackedWorld(World))
	{
		StorePendingNetDestructions(*World, nullptr, WorldState->PendingNetDestructions);
	}
}

void USerializeSubsystem::HandlePreLevelRemoved(ULevel* Level, UWorld* World)
{
	if (Level == nullptr || !IsTrackedWorld(World) || World->bIsTearingDown)
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(USerializeSubsystem::HandlePreLevelRemoved);

	// Unloading, not just hiding: a cell's respawnable actors go with it and come back in the persistent level.
	const ULevelStreaming* Streaming = ULevelStreaming::FindStreamingLevel(Level);
	const bool bUnloadingCell = Level->IsWorldPartitionRuntimeCell() && (Streaming == nullptr || !Streaming->ShouldBeLoaded());
	WorldState->HiddenLevels.FindOrAdd(Level).Reset();
	for (const TPair<AActor*, FSaveGameTrackedActor>& Entry : SnapshotTracked())
	{
		AActor* Actor = Entry.Key;
		if (!IsSaveable(Actor) || Actor->GetLevel() != Level)
		{
			continue;
		}

		const FActorKey Key = MakeKey(Actor, Entry.Value.bSpawned);
		CaptureActor(Actor, Entry.Value, Key);
		// A cell has no identity to come back to: its respawnable actors move to the persistent level.
		if (bUnloadingCell && Entry.Value.bSpawned && Key.bRespawn)
		{
			PendingRespawns.AddUnique(Key.Key);
		}
		WorldState->Tracked.Remove(Actor);
		WorldState->HiddenLevels.FindOrAdd(Level).Emplace(Actor, Entry.Value);
	}
	// Levels unloaded since they were hidden leave stale keys behind.
	for (TMap<TWeakObjectPtr<const ULevel>, TArray<TPair<TWeakObjectPtr<AActor>, FSaveGameTrackedActor>>>::TIterator It(
	         WorldState->HiddenLevels);
	     It;
	     ++It)
	{
		if (!It->Key.IsValid())
		{
			It.RemoveCurrent();
		}
	}

	if (!PendingRespawns.IsEmpty())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::RespawnPending));
	}
}

void USerializeSubsystem::HandlePostLogin(AGameModeBase* /*GameMode*/, APlayerController* NewPlayer)
{
	if (NewPlayer != nullptr && NewPlayer->PlayerState != nullptr)
	{
		RestoreActor(NewPlayer->PlayerState);
	}
}

void USerializeSubsystem::HandleActorPreSpawn(AActor* Actor) const
{
	if (IsSaveable(Actor) && IsTrackedWorld(Actor->GetWorld()))
	{
		FSaveGameTrackedActor Tracked;
		Tracked.bSpawned = true;
		WorldState->Tracked.Add(Actor, Tracked);
	}
}

void USerializeSubsystem::HandleActorDestroyed(AActor* Actor) const
{
	const UWorld* World = Actor->GetWorld();
	if (!IsTrackedWorld(World) || World->bIsTearingDown)
	{
		return;
	}
	if (const ULevel* Level = Actor->GetLevel(); Level != nullptr && Level->bIsBeingRemoved)
	{
		return;
	}

	FSaveGameTrackedActor Tracked;
	if (!WorldState->Tracked.RemoveAndCopyValue(Actor, Tracked))
	{
		return;
	}

	const FActorKey Key = MakeKey(Actor, Tracked.bSpawned);
	if (Tracked.bSpawned)
	{
		// A taken drop is gone for good. A player who leaves is captured now: this runs before Destroyed() and
		// EndPlay (UWorld::DestroyActor), so the state is still whole.
		if (Key.bRespawn)
		{
			Store->MapTable.Records.Remove(Key.Key);
		}
		else
		{
			CaptureActor(Actor, Tracked, Key);
		}
		return;
	}

	FSaveGameRecord& Record = Store->MapTable.Records.FindOrAdd(Key.Key);
	Record                  = FSaveGameRecord();
	Record.Flags            = SaveGameRecordFlags::Destroyed;
	Record.Versions         = Store->CurrentVersions;
}

#if !UE_BUILD_SHIPPING
FString USerializeSubsystem::DescribeDifferences() const
{
	if (!WorldState.IsValid())
	{
		return FString();
	}
	FString Text;
	for (const TPair<AActor*, FSaveGameTrackedActor>& Entry : SnapshotTracked())
	{
		if (Entry.Value.bSpawned)
		{
			continue;
		}
		const FActorKey Key                = MakeKey(Entry.Key, false);
		const TArray<uint8>* PristineBytes = WorldState->PristineData.Find(Key.Key);
		TArray<uint8> Current;
		if (PristineBytes == nullptr || !FSaveGameActorSerializer::Write(Entry.Key, *Store->CurrentVersions, Current))
		{
			continue;
		}
		if (const FString Difference = FSaveGameActorSerializer::DescribeDifference(*PristineBytes, Current);
		    !Difference.IsEmpty())
		{
			Text += TEXT("\n  ") + Key.Key.ToString() + Difference;
		}
	}
	return Text;
}
#endif
