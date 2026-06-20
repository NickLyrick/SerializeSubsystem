#include "SaveGameFunctionLibrary.h"

#include "SaveGameSettings.h"
#include "Serialization/CustomVersion.h"

#if WITH_EDITOR
#include "Blueprint/BlueprintExceptionInfo.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/MessageLog.h"
#include "Misc/UObjectToken.h"

void BreakpointWithError(FFrame& Stack, const FText& Text)
{
	const FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::Breakpoint, Text);

	const int32 BreakpointOpCodeOffset = Stack.Code - Stack.Node->Script.GetData() - 1;
	const UEdGraphNode* Node =
	    FKismetDebugUtilities::FindSourceNodeForCodeLocation(Stack.Object, Stack.Node, BreakpointOpCodeOffset, true);

	struct Local
	{
		static void OnMessageLogLinkActivated(const class TSharedRef<IMessageToken>& Token)
		{
			if (Token->GetType() == EMessageToken::Object)
			{
				const TSharedRef<FUObjectToken> UObjectToken = StaticCastSharedRef<FUObjectToken>(Token);
				if (UObjectToken->GetObject().IsValid())
				{
					FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(UObjectToken->GetObject().Get());
				}
			}
		}
	};

	FMessageLog MessageLog("PIE");
	MessageLog.Error()
	    ->AddToken(
	        FUObjectToken::Create(Node, Node->GetNodeTitle(ENodeTitleType::ListView))
	            ->OnMessageTokenActivated(FOnMessageTokenActivated::CreateStatic(&Local::OnMessageLogLinkActivated)))
	    ->AddToken(FTextToken::Create(Text));
	MessageLog.Open(EMessageSeverity::Error);

	FBlueprintCoreDelegates::ThrowScriptException(Stack.Object, Stack, ExceptionInfo);
}
#endif

bool USaveGameFunctionLibrary::WasObjectLoaded(const UObject* Object)
{
	return Object && Object->HasAnyFlags(RF_WasLoaded | RF_LoadCompleted);
}

bool USaveGameFunctionLibrary::IsLoading(const FSaveGameArchive& Archive)
{
	return Archive.IsValid() && Archive.GetRecord().GetUnderlyingArchive().IsLoading();
}

bool USaveGameFunctionLibrary::SerializeActorTransform(FSaveGameArchive& Archive, AActor* Actor)
{
	if (Archive.IsValid() && IsValid(Actor))
	{
		const bool bIsMovable = Actor->IsRootComponentMovable();
		const bool bIsLoading = Archive.GetRecord().GetUnderlyingArchive().IsLoading();

		// Skip saving for non-movable actors: their transform is baked into the level.
		return (bIsLoading || bIsMovable) &&
		       Archive.SerializeField(TEXT("ActorTransform"),
		                              [&](FStructuredArchive::FSlot Slot)
		                              {
			                              FTransform ActorTransform;

			                              if (!bIsLoading)
			                              {
				                              ActorTransform = Actor->GetActorTransform();
			                              }

			                              Slot << ActorTransform;

			                              if (bIsLoading && bIsMovable)
			                              {
				                              Actor->SetActorTransform(
				                                  ActorTransform, false, nullptr, ETeleportType::TeleportPhysics);
			                              }
		                              });
	}

	return false;
}

bool USaveGameFunctionLibrary::SerializeActorHiddenInGame(FSaveGameArchive& Archive, AActor* Actor)
{
	if (Archive.IsValid() && IsValid(Actor))
	{
		const bool bIsLoading = Archive.GetRecord().GetUnderlyingArchive().IsLoading();

		return Archive.SerializeField(TEXT("HiddenInGame"),
		                              [&](FStructuredArchive::FSlot Slot)
		                              {
			                              bool bIsHiddenInGame;

			                              if (!bIsLoading)
			                              {
				                              bIsHiddenInGame = Actor->IsHidden();
			                              }

			                              Slot << bIsHiddenInGame;

			                              if (bIsLoading)
			                              {
				                              Actor->SetHidden(bIsHiddenInGame);
			                              }
		                              });
	}

	return false;
}

bool USaveGameFunctionLibrary::SerializeItem(FSaveGameArchive& Archive, int32& Value, bool bSave)
{
	// The UFUNCTION body is never called natively. Blueprint always routes through execSerializeItem
	// (CustomThunk), which reads the actual FProperty type from the Blueprint execution stack.
	checkf(false, TEXT("SerializeItem must be called from Blueprint — native call is not supported."));
	return false;
}

DEFINE_FUNCTION(USaveGameFunctionLibrary::execSerializeItem)
{
	P_GET_STRUCT_REF(FSaveGameArchive, Archive);

	// Step past the wildcard Value pin to get the actual FProperty and its address on the stack.
	// This is the reason CustomThunk is needed: the standard generated thunk cannot handle
	// UPARAM(ref) with CustomStructureParam (wildcard) types.
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const FProperty* ValueProperty = Stack.MostRecentProperty;
	uint8* ValueAddress = Stack.MostRecentPropertyAddress;

	// If we're saving, should we serialize this value?
	P_GET_UBOOL(bSave);

	P_FINISH;

	P_NATIVE_BEGIN;

	*(bool*)RESULT_PARAM = false;

#if WITH_EDITOR
	if (ValueProperty &&
	    (!ValueProperty->HasAnyPropertyFlags(CPF_Edit) || ValueProperty->HasAnyPropertyFlags(CPF_BlueprintReadOnly)))
	{
		BreakpointWithError(Stack,
		                    FText::Format(NSLOCTEXT("SaveGame",
		                                            "SerialiseItem_NotVariableException",
		                                            "'{0}' connected to the Value pin is "
		                                            "not an editable variable!"),
		                                  ValueProperty->GetDisplayNameText()));
	}
	else
#endif
	    if (ValueProperty && Archive.IsValid() && (IsLoading(Archive) || bSave))
	{
		Archive.SerializeField(ValueProperty->GetFName(),
		                       [&](FStructuredArchive::FSlot Slot)
		                       {
			                       // Note: SerializeItem will not handle type conversions, though
			                       // ConvertFromType will do this with some questionable address
			                       // arithmetic
			                       ValueProperty->SerializeItem(Slot, ValueAddress, nullptr);
			                       *(bool*)RESULT_PARAM = true;
		                       });
	}

	P_NATIVE_END;
}

int32 USaveGameFunctionLibrary::UseCustomVersion(FSaveGameArchive& Archive, const UEnum* VersionEnum)
{
	if (Archive.IsValid() && IsValid(VersionEnum))
	{
		FArchive& UnderlyingArchive = Archive.GetRecord().GetUnderlyingArchive();
		const FGuid VersionId = GetDefault<USaveGameSettings>()->GetVersionId(VersionEnum);

		if (VersionId.IsValid())
		{
			if (UnderlyingArchive.IsLoading())
			{
				// If the archive has one, return its saved version
				const FCustomVersion* CustomVersion = UnderlyingArchive.GetCustomVersions().GetVersion(VersionId);
				return CustomVersion ? CustomVersion->Version : INDEX_NONE;
			}

			// Get the latest version and save it
			const int32 Version = VersionEnum->GetMaxEnumValue() - 1;
			UnderlyingArchive.SetCustomVersion(VersionId, Version, VersionEnum->GetFName());
			return Version;
		}
	}

	return INDEX_NONE;
}
