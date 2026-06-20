#pragma once

#include "CoreMinimal.h"

#include "SaveGameMigrationStep.generated.h"

/**
 * Base struct for a single save-game migration step.
 *
 * Add concrete steps (FMigration_RenameField, FMigration_SetDefaultValue) in
 * Project Settings → Save Game → Migrations. A step is applied when loading a
 * save whose custom version for VersionGuid is STRICTLY BELOW TargetVersion —
 * equal means data is already in the expected format, so the step is skipped.
 */
USTRUCT(BlueprintType)
struct SERIALIZESUBSYSTEM_API FSaveGameMigrationStep
{
	GENERATED_BODY()

	/** GUID of the FCustomVersion this migration targets. Must match a GUID
	 *  registered in Project Settings → Save Game → Versions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Migration")
	FGuid VersionGuid;

	/** Applied when the loaded save's version for VersionGuid < TargetVersion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Migration", meta = (ClampMin = "1"))
	int32 TargetVersion = 1;
};

/**
 * Registers a CoreRedirect for the duration of the load so that the old property name in
 * the archive resolves to the new property name in current code transparently.
 *
 * Registration happens inside SerializeVersions() (during header deserialization), before
 * FStructuredArchive starts parsing actor field names — that ordering is required for the
 * redirect to take effect.
 *
 * OwnerClassName is the short UE class name:
 *   C++ actors:       "AMyActor"
 *   Blueprint actors: "BP_MyActor_C"
 */
USTRUCT(BlueprintType)
struct SERIALIZESUBSYSTEM_API FMigration_RenameField : public FSaveGameMigrationStep
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Migration")
	FName OwnerClassName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Migration")
	FName OldPropertyName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Migration")
	FName NewPropertyName;
};

/**
 * After actor properties are deserialized, overrides a named property with ExportedDefaultValue
 * on every actor that IsA(OwnerClass).
 *
 * Use when a new UPROPERTY(SaveGame) is introduced in TargetVersion and the CDO default is not
 * the right starting value for data migrated from older saves.
 *
 * Do NOT combine with FMigration_RenameField for the same property in the same version:
 * the CoreRedirect registered by RenameField renames the field during deserialization,
 * so SetDefaultValue would subsequently fail to find the old property name.
 *
 * ExportedDefaultValue uses UE's text export format — same as Details-panel
 * "Copy Value as Text" (e.g. "100.0" for float, "(X=0,Y=0,Z=1)" for FVector).
 */
USTRUCT(BlueprintType)
struct SERIALIZESUBSYSTEM_API FMigration_SetDefaultValue : public FSaveGameMigrationStep
{
	GENERATED_BODY()

	/** Class whose instances receive the override. Matches IsA so subclasses are included. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Migration")
	FSoftClassPath OwnerClass;

	/** The property to override (must be UPROPERTY on OwnerClass or a parent). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Migration")
	FName PropertyName;

	/** The value to import, in UE exported-text format. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Migration")
	FString ExportedDefaultValue;
};
