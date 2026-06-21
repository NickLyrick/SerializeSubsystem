#include "SaveGameVersion.h"

#include "UObject/DevObjectVersion.h"

// GUID is hardcoded — generated once with FGuid::NewGuid() and must never change.
// Changing it would break all existing save files (FCustomVersionContainer lookup would fail).
const FGuid FSaveGameVersion::GUID(0xD7535CE7, 0x72F742B5, 0x9D1183D4, 0x33B49065);

FDevVersionRegistration GRegisterSaveGameVersion(FSaveGameVersion::GUID, FSaveGameVersion::LatestVersion, TEXT("SaveGame"));
