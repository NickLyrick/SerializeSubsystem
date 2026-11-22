// All Rights Reserved by TacoEaters

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
struct SERIALIZESUBSYSTEM_API FSaveGameArchive final
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

		if (Archive.IsTextFormat())
		{
			if (Archive.IsLoading())
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
			// JSON saving: track field names so the IsSaving dedup check above fires
			// on duplicate calls, mirroring binary mode behaviour.
			Fields.Add(FieldName, 0);
		}
		else
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
	 * Saving an actor: return false to leave it out of the save (no record; a spawned actor is then not
	 * spawned again on load). The value is ignored for components and when loading.
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
 * Save identity of an actor the game spawns (ADR-0060 §1): its record is keyed by SpawnID. A respawnable actor
 * (a dropped item) is spawned again by the plugin on load; one the game spawns itself (a player's PlayerState)
 * returns false from ShouldRespawnOnLoad and only receives its record once it exists.
 *
 * SpawnID must be stable across sessions: assigned once (a new GUID at spawn, or one derived from a stable
 * identity) and never changed.
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

	/** Whether the plugin spawns this actor again when its save loads; false when the game spawns it itself. */
	UFUNCTION(BlueprintNativeEvent, Category = "SaveGame|Spawn")
	bool ShouldRespawnOnLoad() const;
	virtual bool ShouldRespawnOnLoad_Implementation() const
	{
		return true;
	}
};
