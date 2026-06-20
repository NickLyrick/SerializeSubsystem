#pragma once

#include "CoreTypes.h"
#include "Misc/Guid.h"

class SERIALIZESUBSYSTEM_API FSaveGameVersion {
public:
  enum Type {
    MinCompatibleVersion = 0,
    WithManifestAndCRC,   // v1: blobs prefixed with FSaveGameManifest (magic + version + CRC32)
    // -----<new versions can be added above this line>----------------
    VersionPlusOne,
    LatestVersion = VersionPlusOne - 1
  };

  const static FGuid GUID;
};
