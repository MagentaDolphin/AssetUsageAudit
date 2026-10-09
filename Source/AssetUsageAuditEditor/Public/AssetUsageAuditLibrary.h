// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AssetUsageAuditLibrary.generated.h"

/**
 * Blueprint and Python entry points for the audit.
 *
 * Phase 1 uses these as the only way to run the analysis, before the Slate panel exists. They
 * remain useful afterwards: a TA can script a one-off audit from an Editor Utility Widget without
 * touching the main tool, and the same calls drive the Python bridge.
 *
 * These are thin wrappers. All logic lives in AssetUsageAuditCore so that the panel, these
 * functions and a future commandlet cannot drift apart.
 */
UCLASS()
class ASSETUSAGEAUDITEDITOR_API UAssetUsageAuditLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Run a full audit and write JSON + CSV reports.
     *
     * @param LevelPackageNames  Levels to analyse, e.g. "/Game/Maps/WP_Example". Empty means all.
     * @param OutputDirectory    Destination. Empty uses the configured default.
     * @param OutReportPath      Absolute path of the JSON report on success.
     * @return true when both reports were written.
     */
    UFUNCTION(BlueprintCallable, Category = "Asset Usage Audit", meta = (AutoCreateRefTerm = "LevelPackageNames"))
    static bool RunAudit(const TArray<FString>& LevelPackageNames, const FString& OutputDirectory, FString& OutReportPath);

    /**
     * Assets reachable from one level, resolved transitively through Blueprints and other assets.
     *
     * This is the question the tool exists to answer, exposed on its own for scripting.
     */
    UFUNCTION(BlueprintCallable, Category = "Asset Usage Audit")
    static TArray<FString> GetAssetsUsedOnLevel(const FString& LevelPackageName);

    /**
     * Levels that use the given asset.
     *
     * Includes both a sublevel and its parent map when the asset sits in a sublevel, because the
     * parent's traversal reaches through ULevelStreaming::WorldAsset.
     */
    UFUNCTION(BlueprintCallable, Category = "Asset Usage Audit")
    static TArray<FString> GetLevelsUsingAsset(const FString& AssetPackageName);

    /**
     * Time a full sweep and log the breakdown.
     *
     * Exists to answer the open question in the plan: whether an in-memory graph cache is needed
     * at all. Measure before adding one.
     */
    UFUNCTION(BlueprintCallable, Category = "Asset Usage Audit")
    static float MeasureFullSweepSeconds();
};
