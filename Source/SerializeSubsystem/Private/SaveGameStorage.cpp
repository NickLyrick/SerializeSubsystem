// All Rights Reserved by TacoEaters

#include "SaveGameStorage.h"

#include "Async/Async.h"
#include "PlatformFeatures.h"
#include "SaveGameFile.h"
#include "SaveGameSystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveGameStorage, Log, All);

namespace
{
	const TCHAR* const NewSuffix    = TEXT(".new");
	const TCHAR* const BackupSuffix = TEXT(".bak");

	// One slot operation at a time: an async save and a load must never interleave their files.
	FCriticalSection StorageLock;

	ISaveGameSystem* GetSaveSystem()
	{
		return IPlatformFeaturesModule::Get().GetSaveGameSystem();
	}

	// Reads Name and returns its bytes only when they form an intact save file.
	bool ReadIntact(ISaveGameSystem& SaveSystem, const FString& Name, const int32 UserIndex, TArray<uint8>& OutFile)
	{
		if (!SaveSystem.DoesSaveGameExist(*Name, UserIndex))
		{
			return false;
		}
		if (!SaveSystem.LoadGame(false, *Name, UserIndex, OutFile))
		{
			return false;
		}
		return FSaveGameFile::IsIntact(OutFile);
	}

	// Writes File into Name and reads it back; true only when the stored bytes equal File.
	bool WriteVerified(ISaveGameSystem& SaveSystem, const FString& Name, const int32 UserIndex, const TArray<uint8>& File)
	{
		if (!SaveSystem.SaveGame(false, *Name, UserIndex, File))
		{
			return false;
		}
		TArray<uint8> Stored;
		return SaveSystem.LoadGame(false, *Name, UserIndex, Stored) && Stored == File;
	}

	void DeleteIfExists(ISaveGameSystem& SaveSystem, const FString& Name, const int32 UserIndex)
	{
		if (SaveSystem.DoesSaveGameExist(*Name, UserIndex))
		{
			SaveSystem.DeleteGame(false, *Name, UserIndex);
		}
	}
} // namespace

bool FSaveGameStorage::WriteSlot(const FString& SlotName, const int32 UserIndex, const TArray<uint8>& File)
{
	ISaveGameSystem* SaveSystem = GetSaveSystem();
	if (SaveSystem == nullptr || SlotName.IsEmpty())
	{
		return false;
	}

	FScopeLock Lock(&StorageLock);
	const FString NewName    = SlotName + NewSuffix;
	const FString BackupName = SlotName + BackupSuffix;

	// 1. The new bytes land beside the slot first: a failure here leaves the slot exactly as it was.
	if (!WriteVerified(*SaveSystem, NewName, UserIndex, File))
	{
		UE_LOG(LogSaveGameStorage, Error, TEXT("Writing '%s' failed; the slot keeps its previous save."), *NewName);
		DeleteIfExists(*SaveSystem, NewName, UserIndex);
		return false;
	}

	// 2. The previous save, if intact, becomes the backup.
	if (TArray<uint8> Previous; ReadIntact(*SaveSystem, SlotName, UserIndex, Previous))
	{
		if (!WriteVerified(*SaveSystem, BackupName, UserIndex, Previous))
		{
			UE_LOG(LogSaveGameStorage, Warning, TEXT("Backing up '%s' failed; writing on without a backup."), *SlotName);
		}
	}

	// 3. The slot itself. A failure leaves ".new" as the readable latest save.
	if (!WriteVerified(*SaveSystem, SlotName, UserIndex, File))
	{
		UE_LOG(LogSaveGameStorage, Error, TEXT("Writing '%s' failed; '%s' holds the new save."), *SlotName, *NewName);
		return false;
	}

	DeleteIfExists(*SaveSystem, NewName, UserIndex);
	return true;
}

void FSaveGameStorage::WriteSlotAsync(const FString& SlotName,
                                      const int32 UserIndex,
                                      TArray<uint8> File,
                                      TFunction<void(bool)>&& OnWritten)
{
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
	          [SlotName, UserIndex, File = MoveTemp(File), OnWritten = MoveTemp(OnWritten)]() mutable
	          {
		          const bool bWritten = WriteSlot(SlotName, UserIndex, File);
		          AsyncTask(ENamedThreads::GameThread,
		                    [bWritten, OnWritten = MoveTemp(OnWritten)]
		                    {
			                    if (OnWritten)
			                    {
				                    OnWritten(bWritten);
			                    }
		                    });
	          });
}

bool FSaveGameStorage::ReadSlot(const FString& SlotName, const int32 UserIndex, TArray<uint8>& OutFile)
{
	ISaveGameSystem* SaveSystem = GetSaveSystem();
	if (SaveSystem == nullptr || SlotName.IsEmpty())
	{
		return false;
	}

	FScopeLock Lock(&StorageLock);
	const FString Candidates[] = {SlotName, SlotName + NewSuffix, SlotName + BackupSuffix};
	for (const FString& Candidate : Candidates)
	{
		if (ReadIntact(*SaveSystem, Candidate, UserIndex, OutFile))
		{
			if (Candidate != SlotName)
			{
				UE_LOG(LogSaveGameStorage, Warning, TEXT("'%s' is missing or damaged; read '%s' instead."), *SlotName, *Candidate);
			}
			return true;
		}
	}
	OutFile.Reset();
	return false;
}

bool FSaveGameStorage::DeleteSlot(const FString& SlotName, const int32 UserIndex)
{
	ISaveGameSystem* SaveSystem = GetSaveSystem();
	if (SaveSystem == nullptr || SlotName.IsEmpty())
	{
		return false;
	}

	FScopeLock Lock(&StorageLock);
	const FString Names[] = {SlotName, SlotName + NewSuffix, SlotName + BackupSuffix};
	bool bAllGone = true;
	for (const FString& Name : Names)
	{
		DeleteIfExists(*SaveSystem, Name, UserIndex);
		bAllGone &= !SaveSystem->DoesSaveGameExist(*Name, UserIndex);
	}
	return bAllGone;
}

TArray<FString> FSaveGameStorage::ListSlots(const int32 UserIndex)
{
	TArray<FString> Slots;
	ISaveGameSystem* SaveSystem = GetSaveSystem();
	TArray<FString> Names;
	if (SaveSystem == nullptr || !SaveSystem->GetSaveGameNames(Names, UserIndex))
	{
		return Slots;
	}

	for (FString& Name : Names)
	{
		Name.RemoveFromEnd(NewSuffix, ESearchCase::CaseSensitive);
		Name.RemoveFromEnd(BackupSuffix, ESearchCase::CaseSensitive);
		Slots.AddUnique(Name);
	}
	return Slots;
}
