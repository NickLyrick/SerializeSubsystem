// All Rights Reserved by TacoEaters

#pragma once

#include "CoreMinimal.h"

#include "Misc/EngineVersion.h"

#include "SaveGameTypes.generated.h"

/** Verdict of a load: passed to OnLoadCompleted and returned by the calls that read a save file. */
UENUM(BlueprintType)
enum class ESaveGameLoadResult : uint8
{
	Success = 0 UMETA(DisplayName = "Success"),
	CorruptedData UMETA(DisplayName = "Corrupted Data"),
	IncompatibleVersion UMETA(DisplayName = "Incompatible Version"),
	EngineVersionMismatch UMETA(DisplayName = "Engine Version Mismatch"),
	MapMissing UMETA(DisplayName = "Map Missing"),
	ForeignGame UMETA(DisplayName = "Foreign Game"),
	Busy UMETA(DisplayName = "Busy"),
};

/** What a save file says about itself before its world is read (ADR-0060 §5). */
struct SERIALIZESUBSYSTEM_API FSaveGameFileHeader final
{
	// Engine that wrote the file.
	FEngineVersion EngineVersion;
	// Long package name of the map the save travels to.
	FString MapPath;
	// The game's own summary of the slot (name, playtime, thumbnail), opaque to the plugin.
	TArray<uint8> UserMeta;
	// Game that wrote the file (USaveGameSettings::GameId).
	FGuid GameId;
	// Moment of saving.
	FDateTime SavedAtUtc;
};
