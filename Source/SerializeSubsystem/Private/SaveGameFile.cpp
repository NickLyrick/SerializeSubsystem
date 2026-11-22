// All Rights Reserved by TacoEaters

#include "SaveGameFile.h"

#include "Compression/OodleDataCompressionUtil.h"
#include "Misc/Crc.h"
#include "SaveGameSettings.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveGameFile, Log, All);

namespace
{
	constexpr uint32 FileMagic     = 0x31464753; // "SGF1"
	constexpr uint32 FormatVersion = 1;

	// Magic + FormatVersion + GameId + HeaderSize + HeaderCrc.
	constexpr int64 FixedPrefixSize = sizeof(uint32) * 2 + sizeof(FGuid) + sizeof(uint32) * 2;

	// Uncompressed tables above this are refused as corrupted rather than allocated.
	constexpr int64 MaxUncompressedSize = 256ll * 1024 * 1024;

	/** Offsets of the parts of a file whose frame checked out. */
	struct FFileLayout final
	{
		FGuid GameId;
		int64 HeaderOffset = 0;
		int64 BodyOffset   = 0;
		uint32 HeaderSize = 0;
		uint32 BodySize   = 0;
	};

	// Reads the frame of File; false when anything about it is off (sizes, magic, format, CRCs).
	bool ReadLayout(const TArray<uint8>& File, FFileLayout& OutLayout)
	{
		if (File.Num() < FixedPrefixSize + static_cast<int64>(sizeof(uint32) * 2))
		{
			return false;
		}

		FMemoryReader Reader(File);
		uint32 Magic   = 0;
		uint32 Version = 0;
		uint32 HeaderCrc = 0;
		Reader << Magic;
		Reader << Version;
		Reader << OutLayout.GameId;
		Reader << OutLayout.HeaderSize;
		Reader << HeaderCrc;
		if (Magic != FileMagic || Version != FormatVersion)
		{
			return false;
		}

		OutLayout.HeaderOffset = Reader.Tell();
		const int64 BodyFrameOffset = OutLayout.HeaderOffset + OutLayout.HeaderSize;
		if (BodyFrameOffset + static_cast<int64>(sizeof(uint32) * 2) > File.Num())
		{
			return false;
		}
		if (FCrc::MemCrc32(File.GetData() + OutLayout.HeaderOffset, OutLayout.HeaderSize) != HeaderCrc)
		{
			return false;
		}

		Reader.Seek(BodyFrameOffset);
		uint32 BodyCrc = 0;
		Reader << OutLayout.BodySize;
		Reader << BodyCrc;
		OutLayout.BodyOffset = Reader.Tell();
		if (OutLayout.BodyOffset + OutLayout.BodySize != File.Num())
		{
			return false;
		}
		return FCrc::MemCrc32(File.GetData() + OutLayout.BodyOffset, OutLayout.BodySize) == BodyCrc;
	}

	// The header fields in file order, both ways. GameId lives in the frame, outside the header bytes.
	void SerializeHeaderFields(FArchive& Archive, FSaveGameFileHeader& Header)
	{
		Archive << Header.MapPath;
		Archive << Header.SavedAtUtc;
		Archive << Header.EngineVersion;
		Archive << Header.UserMeta;
	}

	ESaveGameLoadResult ReadHeaderAt(const TArray<uint8>& File,
	                                 const FFileLayout& Layout,
	                                 FSaveGameFileHeader& OutHeader)
	{
		if (Layout.GameId != FSaveGameFile::GetProjectGameId())
		{
			return ESaveGameLoadResult::ForeignGame;
		}

		const TArray<uint8> HeaderBytes(File.GetData() + Layout.HeaderOffset, Layout.HeaderSize);
		FMemoryReader Reader(HeaderBytes);
		SerializeHeaderFields(Reader, OutHeader);
		OutHeader.GameId = Layout.GameId;
		if (Reader.IsError() || Reader.Tell() != HeaderBytes.Num())
		{
			return ESaveGameLoadResult::CorruptedData;
		}
		return ESaveGameLoadResult::Success;
	}
} // namespace

bool FSaveGameFile::IsIntact(const TArray<uint8>& File)
{
	FFileLayout Layout;
	return ReadLayout(File, Layout);
}

ESaveGameLoadResult FSaveGameFile::ReadHeader(const TArray<uint8>& File, FSaveGameFileHeader& OutHeader)
{
	FFileLayout Layout;
	if (!ReadLayout(File, Layout))
	{
		return ESaveGameLoadResult::CorruptedData;
	}
	return ReadHeaderAt(File, Layout, OutHeader);
}

ESaveGameLoadResult FSaveGameFile::Read(const TArray<uint8>& File,
                                       FSaveGameFileHeader& OutHeader,
                                       TArray<uint8>& OutBody)
{
	FFileLayout Layout;
	if (!ReadLayout(File, Layout))
	{
		return ESaveGameLoadResult::CorruptedData;
	}

	const ESaveGameLoadResult Result = ReadHeaderAt(File, Layout, OutHeader);
	if (Result == ESaveGameLoadResult::Success)
	{
		OutBody = TArray<uint8>(File.GetData() + Layout.BodyOffset, Layout.BodySize);
	}
	return Result;
}

TArray<uint8> FSaveGameFile::Write(const FSaveGameFileHeader& Header, const TArray<uint8>& Body)
{
	TArray<uint8> HeaderBytes;
	{
		FSaveGameFileHeader Copy = Header;
		FMemoryWriter HeaderWriter(HeaderBytes);
		SerializeHeaderFields(HeaderWriter, Copy);
	}

	TArray<uint8> File;
	FMemoryWriter Writer(File);
	uint32 Magic      = FileMagic;
	uint32 Version    = FormatVersion;
	FGuid GameId      = Header.GameId;
	uint32 HeaderSize = HeaderBytes.Num();
	uint32 HeaderCrc  = FCrc::MemCrc32(HeaderBytes.GetData(), HeaderBytes.Num());
	uint32 BodySize   = Body.Num();
	uint32 BodyCrc    = FCrc::MemCrc32(Body.GetData(), Body.Num());
	Writer << Magic;
	Writer << Version;
	Writer << GameId;
	Writer << HeaderSize;
	Writer << HeaderCrc;
	Writer.Serialize(HeaderBytes.GetData(), HeaderBytes.Num());
	Writer << BodySize;
	Writer << BodyCrc;
	Writer.Serialize(const_cast<uint8*>(Body.GetData()), Body.Num());
	return File;
}

FGuid FSaveGameFile::GetProjectGameId()
{
	return GetDefault<USaveGameSettings>()->GameId;
}

TArray<uint8> FSaveGameFile::Compress(const TArray<uint8>& Data)
{
	// The engine's framing: both sizes, then the Oodle block. It fails only on bad parameters or out of memory.
	TArray<uint8> Blob;
	if (!FOodleCompressedArray::CompressTArray(
	        Blob, Data, FOodleDataCompression::ECompressor::Kraken, FOodleDataCompression::ECompressionLevel::Normal))
	{
		UE_LOG(LogSaveGameFile, Error, TEXT("Compressing %d bytes failed."), Data.Num());
		Blob.Reset();
	}
	return Blob;
}

bool FSaveGameFile::Decompress(const TArray<uint8>& Blob, TArray<uint8>& OutData)
{
	// The sizes are checked before anything is allocated: a damaged block must not ask for gigabytes.
	int32 CompressedSize   = 0;
	int32 DecompressedSize = 0;
	const int32 DataOffset = FOodleCompressedArray::PeekSizes(Blob, CompressedSize, DecompressedSize);
	if (DataOffset == 0 || DecompressedSize < 0 || DecompressedSize > MaxUncompressedSize ||
	    static_cast<int64>(DataOffset) + CompressedSize != Blob.Num())
	{
		return false;
	}
	return FOodleCompressedArray::DecompressToTArray(OutData, Blob);
}
