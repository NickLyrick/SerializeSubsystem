// All Rights Reserved by TacoEaters

#include "SaveGameStore.h"

#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

bool FSaveGameStore::EnterMap(const FString& MapPath)
{
	if (MapTablePath == MapPath)
	{
		return true;
	}

	if (!MapTablePath.IsEmpty() && !MapTable.Records.IsEmpty())
	{
		MapBlobs.Add(MapTablePath, MapTable.Compress());
	}
	MapTable.Records.Reset();
	MapTablePath = MapPath;

	TArray<uint8> Blob;
	if (!MapBlobs.RemoveAndCopyValue(MapPath, Blob))
	{
		return true;
	}
	return FSaveGameTable::Decompress(Blob, MapTable);
}

TArray<uint8> FSaveGameStore::WriteBody() const
{
	TArray<uint8> Body;
	FMemoryWriter Writer(Body);

	TArray<uint8> GlobalBlob = GlobalTable.Compress();
	Writer << GlobalBlob;

	// The other maps' blobs go in as they are stored; the current map's table is compressed fresh. A saving
	// archive only reads, hence the casts.
	const bool bWithCurrentMap = !MapTablePath.IsEmpty() && !MapTable.Records.IsEmpty();
	int32 NumMaps              = MapBlobs.Num();
	if (bWithCurrentMap)
	{
		++NumMaps;
	}
	Writer << NumMaps;
	for (const TPair<FString, TArray<uint8>>& Pair : MapBlobs)
	{
		Writer << const_cast<FString&>(Pair.Key);
		Writer << const_cast<TArray<uint8>&>(Pair.Value);
	}
	if (bWithCurrentMap)
	{
		FString MapPath       = MapTablePath;
		TArray<uint8> MapBlob = MapTable.Compress();
		Writer << MapPath;
		Writer << MapBlob;
	}
	return Body;
}

bool FSaveGameStore::ReadBody(const TArray<uint8>& Body, FSaveGameStore& OutStore)
{
	FMemoryReader Reader(Body);
	TArray<uint8> GlobalBlob;
	int32 NumMaps = 0;
	Reader << GlobalBlob;
	Reader << NumMaps;
	if (Reader.IsError() || NumMaps < 0 || NumMaps > Body.Num() || !FSaveGameTable::Decompress(GlobalBlob, OutStore.GlobalTable))
	{
		return false;
	}

	// Other maps stay compressed until the game enters them.
	for (int32 Index = 0; Index < NumMaps; ++Index)
	{
		FString MapPath;
		TArray<uint8> Blob;
		Reader << MapPath;
		Reader << Blob;
		if (Reader.IsError())
		{
			return false;
		}
		OutStore.MapBlobs.Add(MapPath, MoveTemp(Blob));
	}
	return Reader.Tell() == Body.Num();
}
