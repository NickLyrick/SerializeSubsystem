// All Rights Reserved by TacoEaters

#pragma once

#include "Serialization/NameAsStringProxyArchive.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

/**
 * Proxy archive of one save record (ADR-0060 §2): only SaveGame properties, every one written in full, names as
 * strings and object references as soft paths.
 *
 * Full, not a delta against the archetype: a placed actor whose level overrides a default (a door authored open)
 * and that the player changed back to the class default would otherwise write nothing for that field, and the
 * load would leave the level's value in place.
 */
template <bool bIsLoading>
struct TSaveGameProxyArchive final : FNameAsStringProxyArchive
{
	explicit TSaveGameProxyArchive(FArchive& InInnerArchive)
	    : FNameAsStringProxyArchive(InInnerArchive)
	{
		ArIsSaveGame = true;
		ArNoDelta    = true;
	}

	virtual FArchive& operator<<(FSoftObjectPath& Value) override
	{
		Value.SerializePath(*this);
		if (bIsLoading && !Value.IsNull())
		{
			// Class and asset renames registered in config.
			Value.FixupCoreRedirects();
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
			UObject* Object = Path.ResolveObject();
			// Only assets load from disk. A path into a package (an actor in a level or a cell) resolves to what is
			// in memory or to nothing: loading it would pull a level package in outside the game world.
			if (Object == nullptr && !Path.IsNull() && Path.GetSubPathString().IsEmpty())
			{
				Object = Path.TryLoad();
			}
			Value = Object;
		}
		return *this;
	}
};
