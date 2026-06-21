#pragma once

#include "Serialization/NameAsStringProxyArchive.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

/**
 * Proxy archive that stores all object references as FSoftObjectPath strings rather than
 * raw pointers, making them stable across sessions. Also supports per-load path redirects
 * (used to remap references from old actor names to freshly spawned actor instances).
 *
 * Redirect resolution here is separate from UE's built-in FixupCoreRedirects:
 * FixupCoreRedirects handles class/asset renames registered in DefaultEngine.ini, while
 * AddRedirect handles runtime remapping (e.g., a spawned actor's old path → new path)
 * without requiring an engine restart or config change.
 */
template <bool bIsLoading>
struct TSaveGameProxyArchive final : FNameAsStringProxyArchive
{
	explicit TSaveGameProxyArchive(FArchive& InInnerArchive)
	    : FNameAsStringProxyArchive(InInnerArchive)
	{
		// Tells Serialize() methods to only process UPROPERTY(SaveGame) fields.
		ArIsSaveGame = true;
	}

	void AddRedirect(const FSoftObjectPath& From, const FSoftObjectPath& To)
	{
		if (From != To)
		{
			Redirects.Add(From, To);
		}
	}

	virtual FArchive& operator<<(FSoftObjectPath& Value) override
	{
		Value.SerializePath(*this);

		if (bIsLoading && !Value.IsNull())
		{
			// Apply engine-level redirects (class renames, package moves from DefaultEngine.ini).
			Value.FixupCoreRedirects();
		}

		if (bIsLoading && Redirects.Contains(Value))
		{
			Value = Redirects[Value];
		}

		return *this;
	}

	virtual FArchive& operator<<(FSoftObjectPtr& Value) override
	{
		FSoftObjectPath Path;

		if (!bIsLoading)
		{
			Path = Value.ToSoftObjectPath();
		}

		*this << Path;

		if (bIsLoading)
		{
			Value = FSoftObjectPtr(Path);
		}

		return *this;
	}

	virtual FArchive& operator<<(UObject*& Value) override
	{
		return SerializeObject(Value);
	}
	virtual FArchive& operator<<(FWeakObjectPtr& Value) override
	{
		return SerializeObject(Value);
	}
	virtual FArchive& operator<<(FObjectPtr& Value) override
	{
		return SerializeObject(Value);
	}

private:
	TMap<FSoftObjectPath, FSoftObjectPath> Redirects;

	template <typename ObjectType>
	static FSoftObjectPath ToSoftObjectPath(const ObjectType& Value)
	{
		return FSoftObjectPath(Value);
	}

	static FSoftObjectPath ToSoftObjectPath(const FWeakObjectPtr& Value)
	{
		return FSoftObjectPath(Value.Get());
	}

	template <typename ObjectType>
	FArchive& SerializeObject(ObjectType& Value)
	{
		FSoftObjectPath Path;

		if (!bIsLoading)
		{
			Path = ToSoftObjectPath(Value);
		}

		*this << Path;

		if (bIsLoading)
		{
			// ResolveObject first (no disk I/O): supports World Partition where tiles may not be loaded.
			// TryLoad as fallback only if the object is not already in memory.
			UObject* Object = Path.ResolveObject();
			Value = Object;

			if (!IsValid(Object) && !Path.IsNull())
			{
				Value = Path.TryLoad();
			}
		}

		return *this;
	}
};
