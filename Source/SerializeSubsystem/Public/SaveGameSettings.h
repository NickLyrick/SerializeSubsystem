#pragma once

#include "CoreMinimal.h"

#include "Engine/DeveloperSettings.h"
#include "SaveGameMigrationStep.h"
#include "StructUtils/InstancedStruct.h"

#include "SaveGameSettings.generated.h"

USTRUCT(BlueprintType, BlueprintInternalUseOnly)
struct FSaveGameVersionInfo
{
	GENERATED_BODY()

public:
	FSaveGameVersionInfo()
	    : ID(FGuid::NewGuid()),
	      VersionEnum(nullptr)
	{
	}

	/** Stable unique ID for this version slot — used as the FCustomVersion GUID in save archives.
	 *  Generated once; changing it makes all existing saves with this version unreadable. */
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, meta = (IgnoreForMemberInitializationTest), Category = "Save Game")
	FGuid ID;

	/** Enum whose last value is treated as the current version number.
	 *  Changing the enum's last value bumps the version; never remove existing entries. */
	UPROPERTY(EditAnywhere, Category = "Save Game")
	TObjectPtr<UEnum> VersionEnum;
};

UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Save Game"))
class SERIALIZESUBSYSTEM_API USaveGameSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	FGuid GetVersionId(const UEnum* VersionEnum) const;
	bool IsLoadingFromIncompatibleEngineVersionAllowed() const
	{
		return bAllowLoadingFromIncompatibleEngineVersion;
	}

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

public:
	/**
	 * When false (default), loading a save that was created on a different engine
	 * version is rejected with ESaveGameLoadResult::EngineVersionMismatch.
	 * Set true only to allow cross-engine loading during development.
	 */
	UPROPERTY(EditAnywhere, Config, Category = Version)
	bool bAllowLoadingFromIncompatibleEngineVersion = false;

	/**
	 * The list of possible versions and their corresponding enums. Must add
	 * versions here before calling USaveGameFunctionLibrary::UseCustomVersion
	 */
	UPROPERTY(EditAnywhere, Config, Category = Version)
	TArray<FSaveGameVersionInfo> Versions;

	/**
	 * Migration steps applied on load when upgrading from an older save version.
	 * Each step specifies a VersionGuid + TargetVersion pair; it is applied when
	 * the loaded save's version for that GUID is strictly below TargetVersion.
	 *
	 * Built-in step types:
	 *   FMigration_RenameField     — registers a CoreRedirect before deserialization
	 *   FMigration_SetDefaultValue — overrides a property value after deserialization
	 */
	UPROPERTY(EditAnywhere,
	          Config,
	          Category = Migration,
	          meta = (BaseStruct = "/Script/SerializeSubsystem.SaveGameMigrationStep", ExcludeBaseStruct))
	TArray<FInstancedStruct> Migrations;

private:
	// mutable: GetVersionId() is const (called from CDO context) but populates this cache lazily on first call.
	mutable TMap<TObjectPtr<UEnum>, FGuid> CachedVersions;
};
