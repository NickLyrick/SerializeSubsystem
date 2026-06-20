# SerializeSubsystem

An Unreal Engine 5 plugin that serializes and deserializes the full game world
(actors, components, streaming levels) to and from binary save files.

---

## Features

- **Actor serialization** — saves any actor that implements `ISaveGameObject`
- **Spawned-actor support** — dynamically-spawned actors are destroyed and
  re-spawned on load via `ISaveGameSpawnActor`
- **Streaming level support** — each sublevel is saved/loaded independently
- **Versioning** — `FCustomVersionContainer` per blob; incompatible files are
  rejected before decompression
- **Integrity check** — CRC32 stored in the manifest header catches corruption
  before the decompression step
- **JSON debug output** — non-shipping builds also write human-readable `.json`
  files alongside the binary blobs; `LoadFromJson()` enables test round-trips

---

## Quickstart

### 1. Make an actor saveable

Implement `ISaveGameObject` on your actor (or actor component):

```cpp
UCLASS()
class AMyActor : public AActor, public ISaveGameObject {
    GENERATED_BODY()

    // Mark properties you want saved with UPROPERTY(SaveGame)
    UPROPERTY(SaveGame)
    float Health = 100.f;

    // Called on both save and load
    virtual void OnSerialize_Implementation(FSaveGameArchive& Archive,
                                            bool bIsLoading) override
    {
        // Custom data beyond UPROPERTY(SaveGame):
        USaveGameFunctionLibrary::SerializeItem(Archive, Health);
    }
};
```

### 2. Make a spawned actor remappable (optional)

If your actor is spawned at runtime (not placed in the level), also implement
`ISaveGameSpawnActor` to give it a stable GUID so it can be matched across
save/load:

```cpp
UCLASS()
class AMySpawnedActor : public AActor,
                        public ISaveGameObject,
                        public ISaveGameSpawnActor {
    GENERATED_BODY()

    UPROPERTY(SaveGame)
    FGuid SpawnID;

    virtual FGuid GetSpawnID_Implementation() const override { return SpawnID; }
    virtual void  SetSpawnID_Implementation(FGuid ID) override { SpawnID = ID; }
};
```

### 3. Save and load

```cpp
// Save
FSerializedData SaveData;
GetGameInstance()->GetSubsystem<USerializeSubsystem>()->Save(SaveData);
// Persist SaveData however you like (disk, cloud, etc.)

// Load
GetGameInstance()->GetSubsystem<USerializeSubsystem>()->Load(SaveData);

// Listen for the result
SerializeSubsystem->OnLoadCompleted.AddDynamic(this, &UMyObject::OnLoadResult);

void UMyObject::OnLoadResult(ESaveGameLoadResult Result) {
    if (Result == ESaveGameLoadResult::Success) { /* ... */ }
}
```

---

## File format

Three separate binary blobs are produced per save. Each blob has the layout:

```
[FSaveGameManifest 12 bytes]  ← magic, plugin version, CRC32 of the rest
[int64 UncompressedSize]      ← written by SerializeCompressed
[zlib-compressed payload]
```

### Header blob (`FSerializedData::Header`)

Payload (decompressed):
```
EngineVersion
PackageVersion
VersionsOffset            ← byte offset to CustomVersions within this blob
...
CustomVersions            ← FCustomVersionContainer at VersionsOffset
```

### Level blob (`FSerializedData::Levels[L].Data`)

Payload (decompressed):
```
Actors (map):
  ActorName →
    Class?      ← present only if actor was dynamically spawned
    GUID?       ← present only if actor implements ISaveGameSpawnActor
    DataSize    ← byte count of the actor payload (binary only)
    Properties  ← SaveGame-tagged UPROPERTYs
    Components (map):
      ComponentName →
        Properties
        Data    ← ISaveGameObject::OnSerialize output
    Data        ← ISaveGameObject::OnSerialize output
DestroyedActors (array):
  ActorName, ...
```

### Streaming level blob (`FSerializedData::Levels[L].StreamingLevels[SL].Data`)

Same structure as the Level blob.

---

## Versioning

### Plugin version (`FSaveGameVersion`)

`SaveGameVersion.h` contains the authoritative plugin version enum.
Each enum value that introduces a format change should be documented inline.

```cpp
enum Type {
    MinCompatibleVersion = 0,   // raise to block old saves
    WithManifestAndCRC,         // v1: FSaveGameManifest prefix + CRC32
    // -----<new versions can be added above this line>---------
    VersionPlusOne,
    LatestVersion = VersionPlusOne - 1
};
```

New saves always carry `LatestVersion`. On load, any blob whose manifest
`PluginVersion < MinCompatibleVersion` is rejected with
`ESaveGameLoadResult::IncompatibleVersion`.

### Custom versions (gameplay data)

Use `USaveGameFunctionLibrary::UseCustomVersion` inside `OnSerialize` to track
the version of individual actor data, enabling conditional migration logic:

```cpp
void AMyActor::OnSerialize_Implementation(FSaveGameArchive& Archive, bool bIsLoading) {
    const int32 Ver = USaveGameFunctionLibrary::UseCustomVersion(Archive, StaticEnum<EMyActorVersion>());

    USaveGameFunctionLibrary::SerializeItem(Archive, Health);

    if (!bIsLoading || Ver >= EMyActorVersion::AddedArmor) {
        USaveGameFunctionLibrary::SerializeItem(Archive, Armor);
    }
}
```

For the custom version enum to be recognized by the plugin you must register it
in **Project Settings → Save Game → Versions**.

### Engine version policy

By default the plugin rejects saves from a different engine version
(`ESaveGameLoadResult::EngineVersionMismatch`). To allow cross-version loading
during development, enable **bAllowLoadingFromIncompatibleEngineVersion** in
Project Settings → Save Game.

---

## Settings (`USaveGameSettings`)

| Setting | Default | Description |
|---|---|---|
| `bAllowLoadingFromIncompatibleEngineVersion` | `false` | Allow loading saves from a different engine version |
| `Versions` | `[]` | Custom version enums registered for `UseCustomVersion` |
