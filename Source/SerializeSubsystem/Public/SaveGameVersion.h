#pragma once

#include "Misc/Guid.h"

class SERIALIZESUBSYSTEM_API FSaveGameVersion
{
public:
	enum Type
	{
		MinCompatibleVersion = 0,
		WithManifestAndCRC, // v1: blobs prefixed with FSaveGameManifest (magic + version + CRC32)
		// ── add new versions above this line ──────────────────────────────────────────────
		VersionPlusOne,
		// VersionPlusOne is a sentinel: LatestVersion stays correct when new entries are added above it.
		// This is the standard UE enum-version idiom.
		LatestVersion = VersionPlusOne - 1
	};

	// GUID is fixed for the lifetime of the plugin. Changing it makes all existing save files
	// incompatible because the FCustomVersionContainer keyed on this GUID would no longer match.
	// Generated once with FGuid::NewGuid() and hardcoded in SaveGameVersion.cpp.
	static const FGuid GUID;
};
