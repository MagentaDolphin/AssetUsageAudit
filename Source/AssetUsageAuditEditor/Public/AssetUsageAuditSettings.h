// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetUsageAuditTypes.h"
#include "Engine/DeveloperSettings.h"
#include "AssetUsageAuditSettings.generated.h"

/**
 * Team-shared configuration, written to Config/DefaultEditor.ini.
 *
 * That file is normally under version control, so the exclusion list and the
 * type presets are reviewable and shared. Per-user state - last filter, column layout - belongs
 * in UAssetUsageAuditUserSettings instead, which stays in Saved/Config.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Asset Usage Audit"))
class ASSETUSAGEAUDITEDITOR_API UAssetUsageAuditSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UAssetUsageAuditSettings();

    /** Default folder for exported files and reports. Empty means <Project>/Saved/AssetUsageAudit. */
    UPROPERTY(config, EditAnywhere, Category = "Export", meta = (ToolTip = "Default destination for exported assets and reports. Leave empty to use Saved/AssetUsageAudit."))
    FString DefaultExportDirectory;

    /** Overwrite existing files on export, or add/increment a numeric suffix. */
    UPROPERTY(config, EditAnywhere, Category = "Export", meta = (ToolTip = "When a file of the same name already exists: replace it, or write Foo_1, Foo_2 and so on."))
    bool bOverwriteExistingFiles = false;

    /**
     * Asset class short name -> interchange file extension, used by the "Convert" export mode.
     *
     * Empty falls back to the built-in table. Lookup follows the class hierarchy, so one entry for
     * MaterialInterface would cover every material instance. Only add a class the engine ships a
     * UExporter for: an entry with no exporter behind it produces no file and reads as a bug.
     */
    UPROPERTY(config, EditAnywhere, Category = "Export", meta = (ToolTip = "Which file format each asset class converts to, e.g. StaticMesh -> fbx. Leave empty for the built-in defaults."))
    TMap<FString, FString> ExchangeFormatByClass;

    /**
     * How exported files are arranged under the destination.
     *
     * Replaced an earlier bMirrorFolderStructure boolean. Adding "one folder per asset" as a second
     * boolean would have made "mirror the tree AND a folder per asset" expressible, and it means
     * nothing - the enum makes the three arrangements mutually exclusive, which they are.
     */
    UPROPERTY(config, EditAnywhere, Category = "Export", meta = (ToolTip = "Flat: every file side by side. Mirror: recreate the /Game tree. Folder per asset: each ticked asset gets its own folder with its dependencies beside it."))
    EExportLayout ExportLayout = EExportLayout::Flat;

    /**
     * Within each asset's folder, sort its dependencies into per-type subfolders.
     *
     * Only has an effect with the folder-per-asset layout - the other two have no per-asset folder
     * to sort inside. Folder names come from the same type presets the Types filter shows, so the
     * folders match the words used to filter.
     */
    UPROPERTY(config, EditAnywhere, Category = "Export", meta = (EditCondition = "ExportLayout == EExportLayout::FolderPerAsset", ToolTip = "Put textures, materials and meshes into their own subfolders inside each asset's folder."))
    bool bGroupDependenciesByType = false;

    /**
     * Package or content-relative prefixes excluded from the sweep.
     *
     * Defaults cover this project's bought content packs. Roughly 65% of the project's assets are
     * third-party, so without this the report is dominated by content nobody audits.
     */
    UPROPERTY(config, EditAnywhere, Category = "Filtering", meta = (ToolTip = "Folders to skip entirely. Accepts /Game/... or Content/... form."))
    TArray<FString> ExcludedPackagePaths;

    /** Roots to sweep. Empty means /Game. */
    UPROPERTY(config, EditAnywhere, Category = "Filtering", meta = (ToolTip = "Folders to analyse. Leave empty to sweep all of /Game."))
    TArray<FString> IncludedPackagePaths;

    /** Read Config/ and Source/ for path literals the Asset Registry cannot see. */
    UPROPERTY(config, EditAnywhere, Category = "Filtering", meta = (ToolTip = "Scan Config/ and Source/ for /Game paths. Without this the project's GameMode and GameInstance report as unused, because only DefaultEngine.ini names them."))
    bool bScanIndirectReferences = true;

    /** Hide __ExternalActors__ / __ExternalObjects__ rows; they are plumbing, not artist-facing. */
    UPROPERTY(config, EditAnywhere, Category = "Filtering", meta = (ToolTip = "Hide One File Per Actor packages from the results. They are still traversed."))
    bool bHideExternalPackages = true;

    static const UAssetUsageAuditSettings* Get();

    /** Resolved absolute export directory, applying the Saved/AssetUsageAudit fallback. */
    FString GetResolvedExportDirectory() const;

    virtual FName GetCategoryName() const override;
};
