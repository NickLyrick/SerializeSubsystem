// All Rights Reserved by TacoEaters

#pragma once

#include "CoreMinimal.h"

#include "SaveGameTypes.h"

/**
 * The frame of a save file (ADR-0060 §5): Magic | FormatVersion | GameId | HeaderSize | HeaderCrc | Header |
 * BodySize | BodyCrc | Body. The header is small and read on its own by slot lists; the body holds the
 * compressed record tables and is opaque here.
 */
class SERIALIZESUBSYSTEM_API FSaveGameFile final
{
public:
	// True when File has this format's frame and both CRCs match; says nothing about which game wrote it.
	static bool IsIntact(const TArray<uint8>& File);

	// Reads the header of File. ForeignGame when another game wrote it, CorruptedData when the frame is broken.
	static ESaveGameLoadResult ReadHeader(const TArray<uint8>& File, FSaveGameFileHeader& OutHeader);

	// Reads the header and the body of File, with the same verdicts as ReadHeader.
	static ESaveGameLoadResult Read(const TArray<uint8>& File, FSaveGameFileHeader& OutHeader, TArray<uint8>& OutBody);

	// Frames Header (its GameId included) and Body into a file.
	static TArray<uint8> Write(const FSaveGameFileHeader& Header, const TArray<uint8>& Body);

	// GameId of this project (USaveGameSettings::GameId).
	static FGuid GetProjectGameId();

	// Compresses Data (Oodle, FOodleCompressedArray framing); empty when compression failed.
	static TArray<uint8> Compress(const TArray<uint8>& Data);

	// Inverse of Compress; false when Blob is not a valid compressed block.
	static bool Decompress(const TArray<uint8>& Blob, TArray<uint8>& OutData);
};
