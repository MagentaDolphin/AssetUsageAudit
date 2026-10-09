// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AssetUsageAuditUserSettings.generated.h"

/**
 * Per-user panel state, written to Saved/Config/.../EditorPerProjectUserSettings.ini.
 *
 * Separate from UAssetUsageAuditSettings on purpose. That one is DefaultConfig and lands in
 * Config/DefaultEditor.ini, which is version-controlled: exclusion lists and export formats are
 * team decisions and should be reviewable. Which types someone happened to tick last Tuesday is
 * not, and committing it would make every teammate's panel jump around on sync.
 *
 * Not shown in Project Settings - GetCategoryName is inherited but the class carries no
 * EditAnywhere properties, so there is nothing to render. This is state, not configuration.
 */
UCLASS(config = EditorPerProjectUserSettings)
class ASSETUSAGEAUDITEDITOR_API UAssetUsageAuditUserSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    static UAssetUsageAuditUserSettings* Get();

    /** Audit scope: level packages to sweep. Empty means all levels. */
    UPROPERTY(config)
    TArray<FString> LastLevelPackages;

    /** Short class names ticked in the type menu. */
    UPROPERTY(config)
    TArray<FString> VisibleTypeNames;

    /** Substring filter on a Blueprint's generated class. */
    UPROPERTY(config)
    FString GeneratedClassFilter;

    /**
     * Verdicts left visible, by name rather than by index.
     *
     * Names because the enum will gain values: an index saved today would silently mean a different
     * verdict after the next one is inserted, and the user would find their filter quietly changed.
     */
    UPROPERTY(config)
    TArray<FString> VisibleVerdictNames;

    /** EAuditViewMode as an integer. Validated on load. */
    UPROPERTY(config)
    int32 ViewMode = 0;

    /** EFileExportMode as an integer. Validated on load. */
    UPROPERTY(config)
    int32 ExportMode = 0;

    /** EReferenceStrengthFilter as an integer. Validated on load. */
    UPROPERTY(config)
    int32 ReferenceStrength = 0;

    /** EAssetUsageProvenance bitmask that a row must carry. */
    UPROPERTY(config)
    int32 RequiredProvenance = 0;

    /** Whether the export pulls in referenced assets. */
    UPROPERTY(config)
    bool bIncludeDependencies = true;

    /**
     * Deliberately absent: the search box.
     *
     * Restoring it would reopen the panel showing nothing, with the reason sitting in a text field
     * the user is not looking at. A filter that hides everything must be something they just typed.
     */
};
