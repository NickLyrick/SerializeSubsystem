#pragma once

#include "CoreMinimal.h"

#include "GameFramework/Actor.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SaveGameObject.h"

#include "SaveGameFunctionLibrary.generated.h"

UCLASS()
class SERIALIZESUBSYSTEM_API USaveGameFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Check if an object was loaded from an asset (i.e. a Static Mesh, Actor from
	 * a level, etc.)
	 *
	 * @param Object The object to check if loaded
	 * @return true if object was loaded from an asset
	 */
	UFUNCTION(BlueprintCallable, Category = "Serialize Subsystem | Utilities")
	static bool WasObjectLoaded(const UObject* Object);

	/**
	 * Check to see if the current save game is loading the archive.
	 *
	 * @param Archive The archive that the save game is serializing
	 * @return true if save game is loading, false if save game is saving
	 */
	UFUNCTION(BlueprintPure, Category = "Serialize Subsystem | Utilities")
	static bool IsLoading(const FSaveGameArchive& Archive);

	/**
	 * Helper method to serialize an actor's transform if the actor is movable.
	 * If loading, will set the actor's transform.
	 *
	 * @param Archive The archive that the save game is serializing
	 * @param Actor The actor whose transform will be serialized
	 * @return true if the transform was serialized
	 */
	UFUNCTION(BlueprintCallable, Category = "Serialize Subsystem | Serialize", meta = (DefaultToSelf = "Actor"))
	static bool SerializeActorTransform(UPARAM(ref) FSaveGameArchive& Archive, AActor* Actor);

	/**
	 * Helper method to serialize an actor's "Hidden In Game" value. If loading,
	 * will set the actor's Hidden.
	 *
	 * @param Archive The archive that the save game is serializing
	 * @param Actor The actor whose Hidden will be serialized
	 * @return true if the Hidden was serialized
	 */
	UFUNCTION(BlueprintCallable, Category = "Serialize Subsystem | Serialize", meta = (DefaultToSelf = "Actor"))
	static bool SerializeActorHiddenInGame(UPARAM(ref) FSaveGameArchive& Archive, AActor* Actor);

	/**
	 * Serialize a property to/from the specified archive.
	 *
	 * CustomThunk + CustomStructureParam: Blueprint's generic thunk cannot dispatch
	 * wildcard UPARAM(ref) types — execSerializeItem (in the .cpp) reads the actual
	 * FProperty from the Blueprint stack and handles any struct or primitive type.
	 *
	 * OnSave: stores Value to the archive (when bSave is true).
	 * OnLoad: reads from the archive into Value if the field exists.
	 * Returns false if the field is absent (e.g., new field on an old save).
	 */
	UFUNCTION(BlueprintCallable,
	          CustomThunk,
	          Category = "Serialize Subsystem | Serialize",
	          meta = (CustomStructureParam = "Value", AdvancedDisplay = "bSave"))
	static bool SerializeItem(UPARAM(ref) FSaveGameArchive& Archive, UPARAM(ref) int32& Value, bool bSave = true);
	DECLARE_FUNCTION(execSerializeItem);

	/**
	 * Registers and serializes a custom version tied to VersionEnum.
	 *
	 * OnSave: writes the current (latest) version number into the archive.
	 * OnLoad: reads the stored version number.
	 *
	 * Returns INDEX_NONE (-1) when the save pre-dates this versioning scheme
	 * (the GUID was not in the archive at all). Callers should treat -1 as version 0
	 * and apply all migrations unconditionally.
	 */
	UFUNCTION(BlueprintCallable, Category = "Serialize Subsystem | Serialize")
	static int32 UseCustomVersion(UPARAM(ref) FSaveGameArchive& Archive, const UEnum* VersionEnum);
};
