# SerializeSubsystem — Code Review
**Дата:** 2026-06-20  
**Ревьюер:** Claude Sonnet 4.6  
**Ветка:** save-system  
**Цель:** довести систему от "рабочего прототипа" до шипабельного состояния

---

## Общая оценка

Архитектура **правильная**. Система использует все верные примитивы UE5:
`FStructuredArchive` + `FBinaryArchiveFormatter` / `FJsonArchiveOutputFormatter`,
`FCustomVersionContainer`, `CoreRedirects`, `SeamlessTravel`, `TWeakObjectPtr`,
`TSoftObjectPtr` для ссылок. Это не случайный набор — за этим стоит понимание
движка на хорошем уровне.

По дорожной карте (`SaveSystem_Roadmap.md`) завершены **Phase 0 и Phase 1**
полностью, **Phase 3 частично**. Phase 2 (World Partition) — не сделана.

**Итоговая оценка: 7/10**  
Система готова к использованию в игре без WP. До "идеала" не хватает: WP-поддержки,
атомарной записи, async-сохранения, автотестов и нескольких конкретных багов.

---

## Что реализовано (выполнено хорошо)

### Phase 0 — Crash Protection ✅
- `ensureMsgf(!IsLoadingSaveGame())` в `Save()` и `Load()` — конкурентный вызов защищён.
- `ActorsSnapshot = SaveGameActors.Array()` перед итерацией — мутация сета во время обхода исключена.
- `Class.TryLoadClass<AActor>()` с null-чеком и `UE_LOG(Error)` + `return` — краш на отсутствующем классе исключён.
- Проверка `Ar.IsError()` после `SerializeCompressed` в обоих направлениях.
- `BroadcastLoadFailed()` через `TWeakObjectPtr` — безопасен при уничтожении субсистемы во время загрузки.

### Phase 1 — Versioning ✅
- `FSaveGameManifest` (magic `0x53475356` + plugin version + CRC32) — читается без распаковки, быстрый reject.
- `FSaveGameVersion` с `MinCompatibleVersion` и `FDevVersionRegistration`.
- `ESaveGameLoadResult` (Success / CorruptedData / IncompatibleVersion / EngineVersionMismatch / MapMissing) — внятный API для UI.
- `OnLoadCompleted` (BlueprintAssignable) — пользователь получает результат.
- `bAllowLoadingFromIncompatibleEngineVersion` в `USaveGameSettings` — EngineVersion mismatch не крашит, а конфигурируется.
- Миграционный фреймворк: `FMigration_RenameField` (CoreRedirects) + `FMigration_SetDefaultValue` (ImportText_Direct), `TArray<FInstancedStruct>` в `USaveGameSettings`.
- `UseCustomVersion()` / `GetVersionId()` через BlueprintFunctionLibrary — удобно подключить из Blueprint.

### Serializer design ✅
- `TSaveGameProxyArchive<bIsLoading>` — сохраняет объектные ссылки через `FSoftObjectPath`, применяет CoreRedirects и спавн-редиректы.
- `FSaveGameArchive` — field map с offset-сикингом, CoreRedirects на field names при загрузке, `MarkScriptSerializationStart/End`.
- `SerializeActor` — DataSize patch-back для skip неизвестных акторов при загрузке.
- Два прохода при binary-загрузке (первый: спавн + SpawnID map, второй: сериализация данных) — правильно, без этого redirect на спавненных акторов сломан.
- JSON — однопроходная загрузка в лямбде, без обратного сика — правильно, `FJsonArchiveInputFormatter` forward-only.

---

## Баги и проблемы

### КРИТИЧНЫЕ (крашат или ломают данные)

#### BUG-1: Дублирующийся null-чек в `OnLevelAddedToWorld`
**Файл:** `SerializeSubsystem.cpp`, строки 264–270

```cpp
if (!StreamingLevel.IsValid())
    return;

if (!StreamingLevel.IsValid())  // ← ТОЧНАЯ КОПИЯ первой строки
    return;
```

Второй чек никогда не сработает (условие уже проверено), но намеревался проверять
что-то другое — скорее всего `FindStreamingLevel` вернул nullptr или StreamingLevel
не найден в `PersistentLevelRecord->StreamingLevels`. Это маскирует ситуацию, когда
уровень добавлен в мир, но не зарегистрирован в нашей карте. Результат: обращение
к `PersistentLevelRecord->StreamingLevels[StreamingLevel]` через несколько строк
с невалидным ключом → потенциальный краш.

**Исправление:** второй чек должен быть:
```cpp
if (!PersistentLevelRecord->StreamingLevels.Contains(StreamingLevel))
    return;
```

---

#### BUG-2: `OnLevelRemovedFromWorld` не проверяет активную загрузку
**Файл:** `SerializeSubsystem.cpp`, `OnLevelRemovedFromWorld`

Seamless Travel, который мы сами запускаем в `InitiateLevelLoad`, вызывает
`PreLevelRemovedFromWorld` до того, как загрузка завершится. В этот момент
`IsLoadingSaveGame()` == true, но метод всё равно создаёт новый `TSaveGameSerializer`
и перезаписывает `SerializedData` частичными данными уходящего уровня.

Следствие: данные стримингового уровня, которые мы собирались загрузить,
перезатираются нулями перед тем, как они были применены.

**Исправление:**
```cpp
void USerializeSubsystem::OnLevelRemovedFromWorld(ULevel* Level, UWorld* World)
{
    if (IsLoadingSaveGame())
        return;  // ← добавить в самом начале
    ...
}
```

---

#### BUG-3: `FMigration_SetDefaultValue` не применяется, если версия отсутствует в архиве
**Файл:** `SaveGameSerializer.cpp`, `SerializeVersions()`

```cpp
// FMigration_RenameField:
if (Cv && Cv->Version >= Rename->TargetVersion)
    continue;
// ↑ Если Cv == nullptr (нет версии в архиве) — redirect ПРИМЕНЯЕТСЯ ✓

// FMigration_SetDefaultValue:
if (!Cv || Cv->Version >= Default->TargetVersion)
    continue;
// ↑ Если Cv == nullptr — default value НЕ ПРИМЕНЯЕТСЯ ✗
```

Логика обратная. Очень старый сейв (без кастомной версии) получит
RenameField-миграции, но не SetDefaultValue-миграции. Несогласованность
приведёт к частично применённым миграциям и тихому повреждению состояния.

**Исправление:**
```cpp
if (Cv && Cv->Version >= Default->TargetVersion)
    continue;
```

---

#### BUG-4: `ImportText_Direct` результат не проверяется
**Файл:** `SaveGameSerializer.cpp`, строка 744

```cpp
Prop->ImportText_Direct(*Migration.ExportedDefaultValue, PropData, Actor, PPF_None);
```

`ImportText_Direct` возвращает `const TCHAR*` — `nullptr` при ошибке парсинга.
Если `ExportedDefaultValue` содержит опечатку (например, `"(X=0 Y=0)"` вместо
`"(X=0,Y=0)"`), функция вернёт nullptr, частично инициализирует проперти и
молча продолжит. Игра загрузится, но с некорректными данными.

**Исправление:**
```cpp
const TCHAR* Result = Prop->ImportText_Direct(...);
if (!Result)
{
    UE_LOG(LogSaveGame, Error, TEXT("Migration: failed to import value '%s' for %s::%s"),
           *Migration.ExportedDefaultValue, *Actor->GetClass()->GetName(),
           *Migration.PropertyName.ToString());
}
```

---

### ВЫСОКИЙ ПРИОРИТЕТ (надёжность)

#### ISSUE-5: `GRegisteredSaveGameRedirects` растёт без ограничений
**Файл:** `SaveGameSerializer.cpp`, строка 842

```cpp
static TSet<FString> GRegisteredSaveGameRedirects;
```

Статический сет аккумулирует строки каждой загрузки на протяжении сессии.
`FCoreRedirects` не имеет API для удаления, поэтому дедупликация через этот сет
— правильная идея. Но проблема в другом: после hot-reload модуля статик
сбрасывается, а redirects в движке остаются. Следующая загрузка попытается
добавить те же redirects снова, и FCoreRedirects получит дубли.

**Рекомендация:** вместо статика использовать member-поле в субсистеме, либо
хранить сет в UObject (пережил hot-reload) — например, в `USaveGameSettings`.

---

#### ISSUE-6: Нет атомарной записи
Нет нигде. Если процесс упадёт в середине сохранения (например, OOM при
сжатии), файл будет частично перезаписан — и следующая загрузка вернёт
`CorruptedData`. Пользователь теряет прогресс.

**Рекомендация:** write-to-temp + rename:
1. Записать в `SlotName_tmp`.
2. Если успешно — `IFileManager::Get().Move(Final, Tmp)`.
3. Это атомарная операция на всех поддерживаемых ОС.

Или: backup-slot (перед записью скопировать старый файл в `SlotName_bak`,
при следующей неудачной загрузке попробовать бекап).

---

#### ISSUE-7: Сохранение блокирует game thread
`USerializeSubsystem::Save()` синхронное. Сжатие больших уровней (Zlib в
`FMemoryWriter`) может занять 50–200 мс на mid-range ПК — заметный фриз.

**Рекомендация:** обернуть в `Async(EAsyncExecution::TaskGraph, ...)`,
результат вернуть через делегат `OnSaveCompleted`. Флаг `bIsSaveInProgress`
по аналогии с `IsLoadingSaveGame()`.

---

#### ISSUE-8: World Partition — не поддерживается (Phase 2 не реализована)

**Два ключевых места:**

1. `FLevelStruct::FLevelStruct(UWorld*)` (`SaveGameSturct.h`):
```cpp
for (TSoftObjectPtr StreamingLevel : World->GetStreamingLevels())
    StreamingLevels.Add(StreamingLevel, MakeShared<FStreamingLevelStruct>());
```
При WP `GetStreamingLevels()` возвращает тысячи `UWorldPartitionRuntimeLevelStreamingCell`
объектов, чьи имена недетерминированы между сессиями. Ключ
`TSoftObjectPtr<ULevelStreaming>` в `FLevelData::StreamingLevels` становится
нестабильным — данные не найдутся при следующей загрузке.

2. `SerializationStructs.h`:
```cpp
TMap<TSoftObjectPtr<ULevelStreaming>, FStreamingLevelData> StreamingLevels;
```
Этот же ключ попадает в `FSerializedData`, который сохраняется пользователем.
При несовпадении ключей — молчаливая потеря данных стриминговых уровней.

**Вывод:** если проект использует World Partition — Phase 2 из roadmap блокирует.
Если только классический Level Streaming — некритично.

---

#### ISSUE-9: `FindStreamingLevel` использует FName vs FString сравнение
**Файл:** `SaveGameSturct.h`

```cpp
if (StreamingLevel.Key->GetWorldAssetPackageName() == Level->GetOutermost()->GetFName())
```

`GetWorldAssetPackageName()` → `FString`, `GetFName()` → `FName`.
Implicit conversion `FName → FString` работает, но при case-sensitive именах пакетов
(Linux, консоли) может возвращать разные результаты. Лучше:

```cpp
FName PackageFName(*StreamingLevel.Key->GetWorldAssetPackageName());
if (PackageFName == Level->GetOutermost()->GetFName())
```

---

### СРЕДНИЙ ПРИОРИТЕТ (качество кода)

#### ISSUE-10: Нет автотестов (Phase 3 пункт 4)

Ключевые сценарии без покрытия:
- Save → Load roundtrip (transform, SaveGame-проперти, destroyed actors)
- Намеренно испорченный файл → `CorruptedData`, без краша
- Миграция версий (RenameField, SetDefaultValue)
- Save во время загрузки → ensure + no crash

Без тестов каждый рефакторинг — риск регрессии, обнаруживаемой только руками.

---

#### ISSUE-11: Опечатка в имени файла
`SaveGameSturct.h` → должно быть `SaveGameStruct.h`.

Пропущена буква `r`. Не крашит, но неудобно и непрофессионально.
Переименование потребует обновления инклюда в `SerializeSubsystem.h`.

---

#### ISSUE-12: `StructuredArchive.Close()` не вызывается в `Deserialize*` путях
**Файл:** `SaveGameSerializer.cpp`

В serialize-путях (`SerializeHeaderData`, `SerializeLevelData`, `SerializeStreamingLevelData`)
явно вызывается `StructuredArchive.Close()`. В deserialize-путях — нет.
Для `FBinaryArchiveFormatter` это не критично (деструктор сделает своё),
но для `FJsonArchiveInputFormatter` отсутствие Close может оставить незакрытые
JSON-скобки в внутреннем стеке. При загрузке из JSON в дебаге возможны
ассерты в деструкторе форматтера.

---

#### ISSUE-13: SpawnID коллизия детектируется, но не восстанавливается
**Файл:** `SaveGameSerializer.cpp`, строка 567-573

```cpp
ensureMsgf(!SpawnIDs.Contains(SpawnID), TEXT("SpawnID collision ..."));
SpawnIDs.Add(SpawnID, Actor);  // второй перезаписывает первый
```

Коллизия залоггирована, но второй актор перезатирает первый в `SpawnIDs`.
Первый актор не получит своих данных. В дев-сборке `ensure` поможет найти
проблему, но в шиппинг — тихая потеря данных актора.

Простейшее исправление: при коллизии пропускать второго, не добавлять.

---

## Чего нет, но должно быть (приоритет по убыванию)

| # | Что | Phase в roadmap | Блокирует шип? |
|---|-----|-----------------|----------------|
| 1 | World Partition support | Phase 2 | Да, если WP используется |
| 2 | Атомарная запись / backup slot | Phase 3 | Да — пользователи теряют прогресс |
| 3 | Async Save | Phase 5 | Нет, но QoL |
| 4 | Автотесты (4 сценария) | Phase 3 | Нет, но confidence |
| 5 | `USaveGameComponent` (optional) | Phase 4 | Нет |
| 6 | Документация формата файла | Phase 5 | Нет |

---

## Сводка найденных проблем

| ID | Описание | Серьёзность | Файл | Строки |
|----|----------|------------|------|--------|
| BUG-1 | Дублирующийся null-чек, маскирует unregistered level | Critical | SerializeSubsystem.cpp | 264–270 |
| BUG-2 | OnLevelRemovedFromWorld не гардится от IsLoadingSaveGame | Critical | SerializeSubsystem.cpp | 305 |
| BUG-3 | SetDefaultValue migration не применяется при отсутствии версии | Critical | SaveGameSerializer.cpp | 901–905 |
| BUG-4 | ImportText_Direct результат не проверяется | High | SaveGameSerializer.cpp | 744 |
| ISSUE-5 | GRegisteredSaveGameRedirects не пережил hot-reload | High | SaveGameSerializer.cpp | 842 |
| ISSUE-6 | Нет атомарной записи / backup slot | High | — | — |
| ISSUE-7 | Save блокирует game thread | Medium | SerializeSubsystem.cpp | Save() |
| ISSUE-8 | World Partition не поддерживается | High (WP) | SaveGameSturct.h | FLevelStruct |
| ISSUE-9 | FName vs FString сравнение в FindStreamingLevel | Medium | SaveGameSturct.h | FindStreamingLevel |
| ISSUE-10 | Нет автотестов | Medium | — | — |
| ISSUE-11 | Опечатка в имени файла SaveGameSturct.h | Low | SaveGameSturct.h | — |
| ISSUE-12 | StructuredArchive не закрывается в deserialize-путях | Low | SaveGameSerializer.cpp | — |
| ISSUE-13 | SpawnID коллизия — второй актор перезаписывает первого | Medium | SaveGameSerializer.cpp | 567–573 |

---

## Рекомендуемый порядок работы

1. **BUG-1, BUG-2, BUG-3** — исправить немедленно, они меняют поведение при загрузке.
2. **BUG-4, ISSUE-13** — исправить до следующего теста с сохранениями.
3. **ISSUE-6** (атомарная запись) — до первого публичного теста/альфы.
4. **ISSUE-8** (World Partition) — определить, нужен ли WP в проекте, и реализовать Phase 2 если да.
5. **ISSUE-10** (тесты) — параллельно с ISSUE-8.
6. **ISSUE-5, ISSUE-7, ISSUE-9, ISSUE-11, ISSUE-12** — по мере возможности.
