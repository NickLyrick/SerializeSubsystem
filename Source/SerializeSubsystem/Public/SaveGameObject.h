#pragma once

#include "CoreMinimal.h"

#include "UObject/Interface.h"

#include "SaveGameObject.generated.h"

/**
 * Blueprint-accessible wrapper around FStructuredArchive::FRecord for use inside OnSerialize().
 *
 * Binary mode: on construction, records the archive position (StartPosition). In the destructor,
 * seeks back and writes field-name → offset pairs so that fields can be located by name on load,
 * even when the serialized field set has changed between save versions (unknown fields are skipped
 * by seeking past them rather than failing).
 *
 * Loading: checks serialized field names against CoreRedirects and redirects if needed, then
 * seeks to each field's stored offset before calling the deserialization function.
 */
USTRUCT(BlueprintType, BlueprintInternalUseOnly)
struct SERIALIZESUBSYSTEM_API FSaveGameArchive
{
	GENERATED_BODY()

public:
	FSaveGameArchive()
	    : Record(nullptr),
	      Object(nullptr),
	      StartPosition(0),
	      EndPosition(0)
	{
	}

	FSaveGameArchive(class FStructuredArchive::FRecord& InRecord, UObject* InObject);
	~FSaveGameArchive();

	bool IsValid() const
	{
		return Record != nullptr;
	}

	class FStructuredArchive::FRecord& GetRecord() const
	{
		return *Record;
	}

	/**
	 * Serializes a named field using the provided lambda.
	 * Binary mode: stores the field's byte offset so loading can seek to it by name — supports
	 * schema changes where fields appear in a different order or are missing entirely.
	 * Returns false if the field is unknown (loading) or already serialized (saving).
	 */
	template <typename FSerializeFunc>
	bool SerializeField(FName FieldName, FSerializeFunc SerializeFunction)
	{
		if (!IsValid())
		{
			return false;
		}

		FArchive& Archive = Record->GetUnderlyingArchive();

		if (Archive.IsSaving() && Fields.Contains(FieldName))
		{
			return false;
		}

		if (!Archive.IsTextFormat())
		{
			if (Archive.IsLoading())
			{
				if (!Fields.Contains(FieldName))
				{
					return false;
				}

				Archive.Seek(StartPosition + Fields[FieldName]);
			}
			else
			{
				// Store offset relative to StartPosition so the table is valid even if
				// the archive is later moved or copied.
				Fields.Add(FieldName, Archive.Tell() - StartPosition);
			}
		}
		else if (Archive.IsLoading())
		{
			// FJsonArchiveInputFormatter::EnterField does check(Field.IsValid()) and
			// crashes when the field is absent. TryEnterField returns an empty optional
			// instead, matching the binary path's graceful missing-field handling.
			TOptional<FStructuredArchive::FSlot> Slot = Record->TryEnterField(*FieldName.ToString(), false);
			if (!Slot.IsSet())
			{
				return false;
			}
			SerializeFunction(Slot.GetValue());
			return true;
		}
		else
		{
			// JSON saving: track field names so the IsSaving dedup check above fires
			// on duplicate calls, mirroring binary mode behaviour.
			Fields.Add(FieldName, 0);
		}

		SerializeFunction(Record->EnterField(*FieldName.ToString()));

		return true;
	}

private:
	FSaveGameArchive(FSaveGameArchive&) = delete;

	class FStructuredArchive::FRecord* Record;
	TWeakObjectPtr<> Object;
	uint64 StartPosition;
	uint64 EndPosition;

	TMap<FName, uint64> Fields;
};

template <>
struct TStructOpsTypeTraits<FSaveGameArchive> : public TStructOpsTypeTraitsBase2<FSaveGameArchive>
{
	enum
	{
		WithCopy = false
	};
};

UINTERFACE(MinimalAPI)
class USaveGameObject : public UInterface
{
	GENERATED_BODY()
};

class SERIALIZESUBSYSTEM_API ISaveGameObject
{
	GENERATED_BODY()

public:
	/**
	 * Called after UPROPERTY(SaveGame) properties are serialized. Use this for fields that
	 * cannot carry SaveGame (engine properties like transform, velocity, etc.) or to run
	 * post-load fixup logic.
	 *
	 * Return value is required by the BlueprintNativeEvent contract (enables local variables
	 * and SerializeItem usage in Blueprint) but is not checked by the serializer.
	 *
	 * Return false to exclude this actor from serialization entirely (useful for transient
	 * actors that should not exist in a loaded save, e.g., tutorial markers).
	 */
	UFUNCTION(BlueprintNativeEvent, Category = SaveGame)
	bool OnSerialize(UPARAM(ref) FSaveGameArchive& Archive, bool bIsLoading);
};

UINTERFACE(MinimalAPI)
class USaveGameSpawnActor : public UInterface
{
	GENERATED_BODY()
};

/**
 * Implement on actors that are spawned by gameplay code before the save system runs
 * (e.g., the player character spawned by GameMode). The save system uses SpawnID to match
 * saved data to an already-spawned actor instead of trying to spawn a duplicate.
 *
 * SpawnID must be stable across sessions — generate once (e.g., from a GUID in a Data Asset)
 * and never change it.
 */
class SERIALIZESUBSYSTEM_API ISaveGameSpawnActor
{
	GENERATED_BODY()

public:
	/** Returns a stable, session-persistent unique ID for this actor. */
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "SaveGame|Spawn")
	const FGuid GetSpawnID() const;

	/** Assigns a SpawnID to this actor (called by the save system after spawning). */
	UFUNCTION(BlueprintNativeEvent, Category = "SaveGame|Spawn")
	bool SetSpawnID(const FGuid& NewID);
};
