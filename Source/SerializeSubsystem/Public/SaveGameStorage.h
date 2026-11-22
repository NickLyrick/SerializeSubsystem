// All Rights Reserved by TacoEaters

#pragma once

#include "CoreMinimal.h"

/**
 * Slot files on the platform's save system (ADR-0060 §5), never a bare file path: consoles and cloud-backed
 * locations live behind ISaveGameSystem. A write goes to "<Slot>.new", is read back and compared, the previous
 * file moves to "<Slot>.bak", and only then "<Slot>" is replaced; a read takes the first intact file of
 * "<Slot>", "<Slot>.new", "<Slot>.bak". At rest a slot is two files.
 */
class SERIALIZESUBSYSTEM_API FSaveGameStorage final
{
public:
	// Writes File into SlotName through the .new/.bak protocol. True when the slot holds File afterwards.
	static bool WriteSlot(const FString& SlotName, int32 UserIndex, const TArray<uint8>& File);

	// WriteSlot on a background thread; OnWritten runs on the game thread with its result.
	static void WriteSlotAsync(const FString& SlotName,
	                           int32 UserIndex,
	                           TArray<uint8> File,
	                           TFunction<void(bool)>&& OnWritten);

	// Reads the first intact file of the slot (main, then .new, then .bak). False when none is intact.
	static bool ReadSlot(const FString& SlotName, int32 UserIndex, TArray<uint8>& OutFile);

	// Deletes every file of the slot. True when none is left.
	static bool DeleteSlot(const FString& SlotName, int32 UserIndex);

	// Names of every slot that has at least one file, without the .new/.bak suffixes, in no particular order.
	static TArray<FString> ListSlots(int32 UserIndex);
};
