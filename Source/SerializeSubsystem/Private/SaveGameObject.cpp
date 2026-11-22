// All Rights Reserved by TacoEaters

#include "SaveGameObject.h"

#include "Misc/ScopeExit.h"

FSaveGameArchive::FSaveGameArchive(FStructuredArchive::FRecord& InRecord, UObject* InObject)
    : Record(&InRecord),
      Object(InObject),
      StartPosition(0),
      EndPosition(0)
{
	FArchive& Archive = Record->GetUnderlyingArchive();

	// Proxy archives scope nested sub-objects by these marks.
	Archive.MarkScriptSerializationStart(Object.Get());

	if (Archive.IsTextFormat())
	{
		return;
	}

	// Saving reserves the offset of the field table, written on destruction; loading reads the table at once.
	StartPosition        = Archive.Tell();
	uint64 FieldsOffset = 0;
	Archive << FieldsOffset;
	if (!Archive.IsLoading())
	{
		return;
	}

	Archive.Seek(StartPosition + FieldsOffset);
	TMap<FName, uint64> StoredFields;
	Archive << StoredFields;
	EndPosition = Archive.Tell();

	// Fields renamed through CoreRedirects since the save answer to their new name. Rebuilt rather than edited
	// in place: a key changed inside the map keeps the old name's hash and is never found again.
	for (const TPair<FName, uint64>& Field : StoredFields)
	{
		FName Name = Field.Key;
		for (const UStruct* CheckStruct = Object->GetClass(); CheckStruct != nullptr;
		     CheckStruct                = CheckStruct->GetSuperStruct())
		{
			if (const FName Redirected = FProperty::FindRedirectedPropertyName(CheckStruct, Field.Key);
			    !Redirected.IsNone())
			{
				Name = Redirected;
				break;
			}
		}
		Fields.Add(Name, Field.Value);
	}
}

FSaveGameArchive::~FSaveGameArchive()
{
	if (!IsValid())
	{
		return;
	}

	FArchive& Archive = Record->GetUnderlyingArchive();
	ON_SCOPE_EXIT
	{
		Archive.MarkScriptSerializationEnd(Object.Get());
	};

	if (Archive.IsTextFormat())
	{
		return;
	}

	if (Archive.IsSaving())
	{
		uint64 FieldsOffset = Archive.Tell() - StartPosition;
		Archive << Fields;
		EndPosition = Archive.Tell();
		Archive.Seek(StartPosition);
		Archive << FieldsOffset;
	}

	// Fields read out of order or skipped: continue after the table either way.
	Archive.Seek(EndPosition);
}
