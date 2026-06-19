# SerializeSubsystem — Production-Ready Roadmap

## Context and Assessment

This is a working, architecturally sound prototype (proxy archive, structured archive,
SpawnID redirect, versioning via CustomVersionContainer — all done correctly).
However: this code comes from a previous project, not written from scratch for the
current requirements. The practical path is not to rewrite from scratch, but to
**harden and close the gaps**, prioritizing by risk rather than architectural "beauty".

Rejected as overengineering for the current stage:
- Gameplay Feature wrapper — provides no benefit for infrastructure always-on code,
  conflicts with PreLoadingScreen initialization. Do not implement.
- Full ISaveGameStreamingProvider for World Partition — implement only if the project
  actually uses WP. Otherwise use the cheap intermediate step (see Phase 2).
- FInstancedStruct in core serialization path — do not touch, overhead without benefit.
  Use only pointedly in migration steps (Phase 4) and optionally in custom actor
  payloads (not a priority).

---

## PHASE 0 — Audit and Crash Protection (backlog blocker, do first)

Goal: the system cannot be brought down by corrupted/unexpected data.
This is not an "improvement", it is a prerequisite for shipping anything.

1. Null-check after `Class.TryLoadClass<AActor>()` before `SpawnActor` in
   `SerializeActors` (SerializeActor lambda in SaveGameSerializer.cpp).
   On failure — `UE_LOG(Error)` + skip actor, no crash.
2. Remove all commented-out dead code from `SaveGameSerializer.cpp`
   (`SerializeData`, `DeserializeData`, `LoadPreviousSaveData`,
   `GetSaveName`) — it references non-existent members and misleads
   readers and contributors.
3. Remove the extra `#pragma once` at the top of the .cpp file.
4. Guard against concurrent Save()/Load(): `bIsLoadInProgress` flag in
   `USerializeSubsystem`, explicit `ensure`/`return false` on Save attempt
   during an in-progress seamless travel load.
5. Replace the silent `return` in `DeserializeLevelData` on empty `MapName`
   with log + `OnLoadFailed` delegate (see Phase 1, item 3).
6. Protect `TSet` against modification during iteration in `SerializeActors`
   (copy `SaveGameActors` into `TArray` before second pass, or
   `ensure()` on Num() change between iterations).
7. Verify result of `Archive.SerializeCompressed` / try-guard around
   decompression — a corrupted file should produce a managed error,
   not UB/crash.

**Exit criteria:** automated test "feed the system an intentionally corrupted/truncated
save file" does not crash the game and returns an error.

---

## PHASE 1 — Versioning and Backward Compatibility

Goal: introducing a new data version does not silently break old saves.

1. Add `MinCompatibleVersion` to `FSaveGameVersion`, explicit check on load
   (compare plugin `FCustomVersion` from archive tail).
2. Result type instead of bool/void for Load(): `ESaveGameLoadResult`
   (Success / IncompatibleVersion / CorruptedData / EngineVersionMismatch /
   MapMissing). Expose externally via `OnLoadCompleted(Result)` delegate
   in `USerializeSubsystem` (BlueprintAssignable).
3. Lightweight "manifest" header at the very start of the file (signature + plugin
   version), readable without full decompression — for fast rejection of
   incompatible files before heavy work.
4. Policy for EngineVersion mismatch — configurable flag in
   `USaveGameSettings` (`bAllowLoadingFromIncompatibleEngineVersion`),
   default — false with log.
5. Migration framework (minimal version):
   - Interface `FSaveGameMigrationStep` (USTRUCT, base for `FInstancedStruct`).
   - 2 built-in types: `FMigration_RenameField`, `FMigration_SetDefaultValue`.
   - `TArray<FInstancedStruct>` of migrations in `USaveGameSettings`,
     bound to `TargetVersion`.
   - Applied after `SerializeVersions()` on load, before returning control
     to gameplay.

**Exit criteria:** automated test "save of version N-1 loads in build version N
and data migrates correctly".

---

## PHASE 2 — Streaming Agnosticism (cheap, without full WP provider)

Goal: no dependency on whether it's classic Level Streaming or World
Partition, without over-complication.

1. Replace `TSoftObjectPtr<ULevelStreaming>` key in `FLevelStruct` /
   `FSerializedData::Levels[].StreamingLevels` with a key by `ULevel*`
   (via `FSoftObjectPath` package path) — both WP cells and regular
   sublevels at runtime are `ULevel*`, `LevelAddedToWorld` /
   `PreLevelRemovedFromWorld` events fire identically for both cases.
2. Remove direct calls to `World->GetStreamingLevels()` from places not
   directly related to UI/diagnostics (this API is specific to classic
   streaming and does not reflect WP cells).
3. This is a breaking format change → wrap in a new version +
   migration step to convert old key to new (Phase 1 framework must
   be ready by this point).

**Do not implement in this phase:** full `ISaveGameStreamingProvider`
interface with separate implementations for WP/Level Streaming — excessive
until there is a confirmed need to work with both methods in one project.

---

## PHASE 3 — Data Reliability and Diagnostics

1. Checksum (CRC32) in Header for damage detection before decompression.
2. Structured logging under a dedicated `LogSaveGame` log category
   (currently almost no logs) — Save/Load lifecycle, errors, warnings about
   SpawnID collisions.
3. `ensure()` on SpawnID collision (two different actors with the same ID).
4. Automated tests (UE Automation Framework):
   - Save → Load roundtrip, compare transform/SaveGame-properties/
     Destroyed-actors before and after.
   - Loading an intentionally corrupted file → expected
     `ESaveGameLoadResult::CorruptedData`, no crash.
   - Migration between versions (from Phase 1).
   - Save during incomplete Load → expected failure, no race.

**Exit criteria:** CI runs all 4 tests green on every PR.

---

## PHASE 4 — Granular Control (optional, as needed by the project)

Implement only if the new project actually requires:
- per-instance exclusion from saving,
- priority ordering of restoration during async load.

1. `USaveGameComponent` (optional) with `bExcludeFromSave`,
   `RestorePriority`. The `ISaveGameObject` interface remains the primary
   mechanism, the component is only metadata on top of it (see "component
   doesn't replace interface" rule).
2. Sort by `RestorePriority` before the second pass in `SerializeActors`
   during load.
3. **Do not implement GFP wrapper** for attaching the component — plain
   `AddComponents` via Blueprint/Subobject in specific project classes is
   sufficient at this scale.

---

## PHASE 5 — Polish and Documentation

1. Update the file format schema comment in `SaveGameSerializer.h`
   (marked as `TODO: This comment is incorrect`).
2. README.md describing the file format, versioning, how to add migrations,
   how to add custom save data to an actor.
3. Remove remaining `TODO: Look`, `TODO: Ensure that is working` — either
   close them or file explicit issues with specifics, do not leave as mute
   markers in production code.
4. Profile `SerializeActors` on a scene with many actors, optimize if
   necessary (async save via `Async(EAsyncExecution::TaskGraph, ...)` if
   profiling shows a freeze).

---

## Priority Summary

| Phase | What it closes | Release blocker? |
|---|---|---|
| 0 | Crashes on bad data | Yes, absolute |
| 1 | Version compatibility | Yes, for any project with post-release patches |
| 2 | Independence from streaming method | Yes, if requirement is explicitly stated |
| 3 | Diagnosability, tests | Yes, for production confidence |
| 4 | Granular actor control | No, on demand from game design |
| 5 | Documentation, polish | No, but required before publishing to a team |
