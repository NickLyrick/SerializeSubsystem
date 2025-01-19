#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "SaveGameSettings.generated.h"

USTRUCT(BlueprintType, BlueprintInternalUseOnly)
struct FSaveGameVersionInfo {
  GENERATED_BODY()

public:
  FSaveGameVersionInfo() : ID(FGuid::NewGuid()), Enum(nullptr) {}

  /** A unique ID for this version, used by the Custom Version Container in a
   * save game archive. Do not change! */
  UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "Save Game")
  FGuid ID;

  /** The enum to use for versioning. System will use last value as the "latest
   * version" number. Do not change! */
  UPROPERTY(EditAnywhere, Category = "Save Game")
  TObjectPtr<UEnum> Enum;
};

UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Save Game"))
class SERIALIZESUBSYSTEM_API USaveGameSettings : public UDeveloperSettings {
  GENERATED_BODY()

public:
  FGuid GetVersionId(const UEnum *VersionEnum) const;

#if WITH_EDITOR
  virtual void
  PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent) override;
#endif

protected:
  /**
   * The list of possible versions and their corresponding enums. Must add
   * versions here before calling USaveGameFunctionLibrary::UseCustomVersion
   */
  UPROPERTY(EditAnywhere, Config, Category = Version)
  TArray<FSaveGameVersionInfo> Versions;

private:
  mutable TMap<TObjectPtr<UEnum>, FGuid> CachedVersions;
};
