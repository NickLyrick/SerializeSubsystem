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
	      Enum(nullptr)
	{
	}

	/** A unique ID for this version, used by the Custom Version Container in a
	 * save game archive. Do not change! */
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "Save Game")
	FGuid ID;

	/** The enum to use for versioning. System will use last value as the "latest
	 * version" number. Do not change! */
	UPROPERTY(EditAnywhere, Category = "Save Game")
	TObjectPtr<UEnum> Enum;
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
	mutable TMap<TObjectPtr<UEnum>, FGuid> CachedVersions;
};
