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

	TMap<FString, TArray<uint8>> Maps = MapBlobs;
	if (!MapTablePath.IsEmpty() && !MapTable.Records.IsEmpty())
	{
		Maps.Add(MapTablePath, MapTable.Compress());
	}
	int32 NumMaps = Maps.Num();
	Writer << NumMaps;
	for (TPair<FString, TArray<uint8>>& Pair : Maps)
	{
		Writer << Pair.Key;
		Writer << Pair.Value;
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
