// All Rights Reserved by TacoEaters

#include "SaveGameRecords.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/Crc.h"
#include "SaveGameFile.h"
#include "SaveGameMigrationStep.h"
#include "SaveGameObject.h"
#include "SaveGameProxyArchive.h"
#include "SaveGameSettings.h"
#include "Serialization/Formatters/BinaryArchiveFormatter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/StructuredArchive.h"
#include "UObject/CoreRedirects.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveGameRecords, Log, All);

namespace
{
	// Property renames registered with FCoreRedirects; it has no unregister, so each is added once per process.
	TSet<FString> RegisteredRenames;

	// Writes Body through a save-game proxy into a fresh buffer; Versions grows by the custom versions it used.
	TArray<uint8> WritePart(FSaveGameVersions& Versions, const TFunctionRef<void(FArchive&)>& Body)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		Writer.SetCustomVersions(Versions.CustomVersions);
		{
			TSaveGameProxyArchive<false> Proxy(Writer);
			Body(Proxy);
		}
		Versions.CustomVersions = Writer.GetCustomVersions();
		return Bytes;
	}

	// Reads Bytes written by WritePart with Versions; false when the archive ran off its data.
	bool ReadPart(const TArray<uint8>& Bytes,
	              const FSaveGameVersions& Versions,
	              const TFunctionRef<void(FArchive&)>& Body)
	{
		FMemoryReader Reader(Bytes);
		Reader.SetCustomVersions(Versions.CustomVersions);
		Reader.SetUEVer(Versions.PackageVersion);
		Reader.SetEngineVer(Versions.EngineVersion);
		TSaveGameProxyArchive<true> Proxy(Reader);
		Body(Proxy);
		return !Reader.IsError() && !Proxy.IsError();
	}

	// SaveGame-flagged UPROPERTYs of Object, tagged: fields added or removed since the save are tolerated.
	void SerializeProperties(const UObject* Object, FArchive& Archive)
	{
		Object->SerializeScriptProperties(Archive);
	}

	// ISaveGameObject::OnSerialize of Object; returns what it returned.
	bool SerializeCustomData(UObject* Object, FArchive& Archive, const bool bIsLoading)
	{
		FBinaryArchiveFormatter Formatter(Archive);
		FStructuredArchive Structured(Formatter);
		// The record and the save archive close (writing their field table) before the structured archive does.
		const bool bKeep = [&Structured, Object, bIsLoading]
		{
			FStructuredArchive::FRecord Record = Structured.Open().EnterRecord();
			FSaveGameArchive SaveArchive(Record, Object);
			return ISaveGameObject::Execute_OnSerialize(Object, SaveArchive, bIsLoading);
		}();
		Structured.Close();
		return bKeep;
	}

	bool IsOlderThan(const FCustomVersionContainer& Versions, const FSaveGameMigrationStep& Step)
	{
		const FCustomVersion* Version = Versions.GetVersion(Step.VersionGuid);
		return Version == nullptr || Version->Version < Step.TargetVersion;
	}

	// Registers the property renames of every migration Versions is older than, before any property is read.
	void RegisterRenames(const FCustomVersionContainer& Versions)
	{
		TArray<FCoreRedirect> Redirects;
		for (const FInstancedStruct& StepInstance : GetDefault<USaveGameSettings>()->Migrations)
		{
			const FMigration_RenameField* Rename = StepInstance.GetPtr<FMigration_RenameField>();
			if (Rename == nullptr || !IsOlderThan(Versions, *Rename) || Rename->OwnerClassName.IsNone() ||
			    Rename->OldPropertyName.IsNone() || Rename->NewPropertyName.IsNone())
			{
				continue;
			}
			const FString Owner = Rename->OwnerClassName.ToString();
			const FString Old   = Owner + TEXT(".") + Rename->OldPropertyName.ToString();
			if (RegisteredRenames.Contains(Old))
			{
				continue;
			}
			RegisteredRenames.Add(Old);
			Redirects.Emplace(ECoreRedirectFlags::Type_Property, Old, Owner + TEXT(".") + Rename->NewPropertyName.ToString());
		}
		if (!Redirects.IsEmpty())
		{
			FCoreRedirects::AddRedirectList(Redirects, TEXT("SaveGameMigration"));
		}
	}

	// Overrides the properties named by every default-value migration Versions is older than.
	void ApplyDefaultValues(UObject* Object, const FCustomVersionContainer& Versions)
	{
		for (const FInstancedStruct& StepInstance : GetDefault<USaveGameSettings>()->Migrations)
		{
			const FMigration_SetDefaultValue* Step = StepInstance.GetPtr<FMigration_SetDefaultValue>();
			if (Step == nullptr || !IsOlderThan(Versions, *Step))
			{
				continue;
			}
			if (const UClass* OwnerClass = Step->OwnerClass.TryLoadClass<UObject>();
			    OwnerClass == nullptr || !Object->IsA(OwnerClass))
			{
				continue;
			}
			const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Step->PropertyName);
			if (Property == nullptr)
			{
				UE_LOG(LogSaveGameRecords,
				       Warning,
				       TEXT("Migration: no property '%s' on %s."),
				       *Step->PropertyName.ToString(),
				       *Object->GetClass()->GetName());
				continue;
			}
			if (Property->ImportText_Direct(
			        *Step->ExportedDefaultValue, Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None) ==
			    nullptr)
			{
				UE_LOG(LogSaveGameRecords,
				       Error,
				       TEXT("Migration: cannot parse '%s' for %s::%s."),
				       *Step->ExportedDefaultValue,
				       *Object->GetClass()->GetName(),
				       *Step->PropertyName.ToString());
			}
		}
	}

	TArray<UActorComponent*> GetSaveGameComponents(const AActor* Actor)
	{
		return Actor->GetComponentsByInterface(USaveGameObject::StaticClass());
	}
} // namespace

TSharedRef<FSaveGameVersions> FSaveGameVersions::MakeCurrent()
{
	TSharedRef<FSaveGameVersions> Versions = MakeShared<FSaveGameVersions>();
	Versions->EngineVersion  = FEngineVersion::Current();
	Versions->PackageVersion = GPackageFileUEVersion;
	for (const FSaveGameVersionInfo& Info : GetDefault<USaveGameSettings>()->Versions)
	{
		if (Info.ID.IsValid() && Info.VersionEnum != nullptr)
		{
			Versions->CustomVersions.SetVersion(Info.ID, Info.VersionEnum->GetMaxEnumValue() - 1, Info.VersionEnum->GetFName());
		}
	}
	return Versions;
}

TArray<uint8> FSaveGameTable::Compress() const
{
	// Version sets are shared between records; written once each, records point at them by index.
	TArray<const FSaveGameVersions*> Sets;
	for (const TPair<FName, FSaveGameRecord>& Pair : Records)
	{
		Sets.AddUnique(Pair.Value.Versions.Get());
	}

	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);

	int32 NumSets = Sets.Num();
	Writer << NumSets;
	for (const FSaveGameVersions* Set : Sets)
	{
		FCustomVersionContainer CustomVersions = Set->CustomVersions;
		FEngineVersion EngineVersion          = Set->EngineVersion;
		FPackageFileVersion PackageVersion    = Set->PackageVersion;
		CustomVersions.Serialize(Writer);
		Writer << EngineVersion;
		Writer << PackageVersion;
	}

	int32 NumRecords = Records.Num();
	Writer << NumRecords;
	for (const TPair<FName, FSaveGameRecord>& Pair : Records)
	{
		const FSaveGameRecord& Record = Pair.Value;
		FString Key                   = Pair.Key.ToString();
		uint8 Flags                   = Record.Flags;
		int32 SetIndex                = Sets.IndexOfByKey(Record.Versions.Get());
		Writer << Key;
		Writer << Flags;
		Writer << SetIndex;
		if (Record.HasFlag(SaveGameRecordFlags::Spawned))
		{
			FString ClassPath   = Record.Class.ToString();
			FTransform Transform = Record.Transform;
			FString LevelKey    = Record.LevelKey.ToString();
			Writer << ClassPath;
			Writer << Transform;
			Writer << LevelKey;
		}
		TArray<uint8> Data = Record.Data;
		uint32 Crc         = FCrc::MemCrc32(Data.GetData(), Data.Num());
		Writer << Data;
		Writer << Crc;
	}

	return FSaveGameFile::Compress(Bytes);
}

bool FSaveGameTable::Decompress(const TArray<uint8>& Blob, FSaveGameTable& OutTable)
{
	TArray<uint8> Bytes;
	if (!FSaveGameFile::Decompress(Blob, Bytes))
	{
		return false;
	}

	FMemoryReader Reader(Bytes);
	int32 NumSets = 0;
	Reader << NumSets;
	if (NumSets < 0 || NumSets > Bytes.Num())
	{
		return false;
	}
	TArray<TSharedPtr<const FSaveGameVersions>> Sets;
	for (int32 Index = 0; Index < NumSets && !Reader.IsError(); ++Index)
	{
		TSharedRef<FSaveGameVersions> Set = MakeShared<FSaveGameVersions>();
		Set->CustomVersions.Serialize(Reader);
		Reader << Set->EngineVersion;
		Reader << Set->PackageVersion;
		Sets.Add(Set);
	}

	int32 NumRecords = 0;
	Reader << NumRecords;
	if (Reader.IsError() || NumRecords < 0 || NumRecords > Bytes.Num())
	{
		return false;
	}

	OutTable.Records.Reset();
	OutTable.Records.Reserve(NumRecords);
	for (int32 Index = 0; Index < NumRecords && !Reader.IsError(); ++Index)
	{
		FString Key;
		FSaveGameRecord Record;
		int32 SetIndex = INDEX_NONE;
		Reader << Key;
		Reader << Record.Flags;
		Reader << SetIndex;
		if (Record.HasFlag(SaveGameRecordFlags::Spawned))
		{
			FString ClassPath;
			FString LevelKey;
			Reader << ClassPath;
			Reader << Record.Transform;
			Reader << LevelKey;
			Record.Class    = FSoftClassPath(ClassPath);
			Record.LevelKey = FName(*LevelKey);
		}
		uint32 Crc = 0;
		Reader << Record.Data;
		Reader << Crc;
		if (Reader.IsError())
		{
			break;
		}
		if (!Sets.IsValidIndex(SetIndex) || FCrc::MemCrc32(Record.Data.GetData(), Record.Data.Num()) != Crc)
		{
			UE_LOG(LogSaveGameRecords, Error, TEXT("Record '%s' is damaged and is skipped."), *Key);
			continue;
		}
		Record.Versions = Sets[SetIndex];
		OutTable.Records.Add(FName(*Key), MoveTemp(Record));
	}

	if (Reader.IsError())
	{
		UE_LOG(LogSaveGameRecords, Error, TEXT("A save table ends early; %d records were read."), OutTable.Records.Num());
	}
	return true;
}

bool FSaveGameActorSerializer::Write(AActor* Actor, FSaveGameVersions& Versions, TArray<uint8>& OutData)
{
	TArray<uint8> ActorProperties = WritePart(Versions,
	                                          [Actor](FArchive& Archive)
	                                          {
		                                          SerializeProperties(Actor, Archive);
	                                          });

	TArray<FString> ComponentNames;
	TArray<TArray<uint8>> ComponentProperties;
	TArray<TArray<uint8>> ComponentData;
	for (UActorComponent* Component : GetSaveGameComponents(Actor))
	{
		ComponentNames.Add(Component->GetName());
		ComponentProperties.Add(WritePart(Versions,
		                                  [Component](FArchive& Archive)
		                                  {
			                                  SerializeProperties(Component, Archive);
		                                  }));
		ComponentData.Add(WritePart(Versions,
		                            [Component](FArchive& Archive)
		                            {
			                            SerializeCustomData(Component, Archive, false);
		                            }));
	}

	bool bKeep = true;
	TArray<uint8> ActorData = WritePart(Versions,
	                                    [Actor, &bKeep](FArchive& Archive)
	                                    {
		                                    bKeep = SerializeCustomData(Actor, Archive, false);
	                                    });
	if (!bKeep)
	{
		return false;
	}

	OutData.Reset();
	FMemoryWriter Writer(OutData);
	Writer << ActorProperties;
	int32 NumComponents = ComponentNames.Num();
	Writer << NumComponents;
	for (int32 Index = 0; Index < NumComponents; ++Index)
	{
		Writer << ComponentNames[Index];
		Writer << ComponentProperties[Index];
		Writer << ComponentData[Index];
	}
	Writer << ActorData;
	return true;
}

bool FSaveGameActorSerializer::Read(AActor* Actor, const TArray<uint8>& Data, const FSaveGameVersions& Versions)
{
	FMemoryReader Reader(Data);
	TArray<uint8> ActorProperties;
	int32 NumComponents = 0;
	Reader << ActorProperties;
	Reader << NumComponents;
	if (Reader.IsError() || NumComponents < 0 || NumComponents > Data.Num())
	{
		return false;
	}

	RegisterRenames(Versions.CustomVersions);

	bool bIntact = ReadPart(ActorProperties,
	                        Versions,
	                        [Actor](FArchive& Archive)
	                        {
		                        SerializeProperties(Actor, Archive);
	                        });
	ApplyDefaultValues(Actor, Versions.CustomVersions);

	// Components by name: a component added or removed since the save leaves the others in place.
	const TArray<UActorComponent*> Components = GetSaveGameComponents(Actor);
	for (int32 Index = 0; Index < NumComponents && !Reader.IsError(); ++Index)
	{
		FString Name;
		TArray<uint8> Properties;
		TArray<uint8> CustomData;
		Reader << Name;
		Reader << Properties;
		Reader << CustomData;

		UActorComponent* const* Found = Components.FindByPredicate(
		    [&Name](const UActorComponent* Component)
		    {
			    return Component->GetName() == Name;
		    });
		if (Found == nullptr || Reader.IsError())
		{
			continue;
		}
		UActorComponent* Component = *Found;
		bIntact &= ReadPart(Properties,
		                    Versions,
		                    [Component](FArchive& Archive)
		                    {
			                    SerializeProperties(Component, Archive);
		                    });
		ApplyDefaultValues(Component, Versions.CustomVersions);
		bIntact &= ReadPart(CustomData,
		                    Versions,
		                    [Component](FArchive& Archive)
		                    {
			                    SerializeCustomData(Component, Archive, true);
		                    });
	}

	TArray<uint8> ActorData;
	Reader << ActorData;
	if (Reader.IsError())
	{
		return false;
	}
	bIntact &= ReadPart(ActorData,
	                    Versions,
	                    [Actor](FArchive& Archive)
	                    {
		                    SerializeCustomData(Actor, Archive, true);
	                    });
	return bIntact;
}

#if !UE_BUILD_SHIPPING
namespace
{
	// The parts of an actor state in the order Write lays them out, by name.
	TArray<TPair<FString, TArray<uint8>>> SplitParts(const TArray<uint8>& Data)
	{
		TArray<TPair<FString, TArray<uint8>>> Parts;
		FMemoryReader Reader(Data);
		TArray<uint8> Bytes;
		int32 NumComponents = 0;
		Reader << Bytes;
		Parts.Emplace(TEXT("Actor.Properties"), Bytes);
		Reader << NumComponents;
		for (int32 Index = 0; Index < NumComponents && !Reader.IsError(); ++Index)
		{
			FString Name;
			TArray<uint8> Properties;
			TArray<uint8> CustomData;
			Reader << Name;
			Reader << Properties;
			Reader << CustomData;
			Parts.Emplace(Name + TEXT(".Properties"), Properties);
			Parts.Emplace(Name + TEXT(".Data"), CustomData);
		}
		Reader << Bytes;
		Parts.Emplace(TEXT("Actor.Data"), Bytes);
		return Parts;
	}

	// Printable bytes of Bytes around Offset, others as dots.
	FString Around(const TArray<uint8>& Bytes, const int32 Offset)
	{
		FString Text;
		for (int32 Index = FMath::Max(0, Offset - 48); Index < FMath::Min(Bytes.Num(), Offset + 48); ++Index)
		{
			TCHAR Character = TEXT('.');
			if (Bytes[Index] >= 32 && Bytes[Index] < 127)
			{
				Character = static_cast<TCHAR>(Bytes[Index]);
			}
			Text.AppendChar(Character);
		}
		return Text;
	}
} // namespace

FString FSaveGameActorSerializer::DescribeDifference(const TArray<uint8>& Before, const TArray<uint8>& After)
{
	const TArray<TPair<FString, TArray<uint8>>> BeforeParts = SplitParts(Before);
	const TArray<TPair<FString, TArray<uint8>>> AfterParts  = SplitParts(After);
	FString Text;
	for (const TPair<FString, TArray<uint8>>& Part : AfterParts)
	{
		const TPair<FString, TArray<uint8>>* Old = BeforeParts.FindByPredicate(
		    [&Part](const TPair<FString, TArray<uint8>>& Candidate)
		    {
			    return Candidate.Key == Part.Key;
		    });
		if (Old == nullptr)
		{
			Text += FString::Printf(TEXT("\n    %s: new"), *Part.Key);
			continue;
		}
		if (Old->Value == Part.Value)
		{
			continue;
		}
		int32 Offset = 0;
		while (Offset < Old->Value.Num() && Offset < Part.Value.Num() && Old->Value[Offset] == Part.Value[Offset])
		{
			++Offset;
		}
		Text += FString::Printf(TEXT("\n    %s: %d -> %d B, first difference at %d\n      before: %s\n      after:  %s"),
		                        *Part.Key,
		                        Old->Value.Num(),
		                        Part.Value.Num(),
		                        Offset,
		                        *Around(Old->Value, Offset),
		                        *Around(Part.Value, Offset));
	}
	return Text;
}
#endif
