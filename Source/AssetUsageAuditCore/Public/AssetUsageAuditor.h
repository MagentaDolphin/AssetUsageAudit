// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetUsageAuditTypes.h"
#include "AssetTypeFilter.h"
#include "AssetUsageGraph.h"
#include "LevelUsageResolver.h"

class IAssetRegistry;

struct FAssetUsageAuditRequest
{
    /** Roots to sweep. Empty means /Game. */
    TArray<FString> IncludePackagePaths;

    /** Package or content-relative prefixes to skip, e.g. "Content/3rdParty". */
    TArray<FString> ExcludePackagePaths;

    /** Levels to consider. Empty means every level found. */
    TArray<FName> LevelPackages;

    /** Type filter applied to the reported rows, not to the traversal. */
    FAssetTypeFilter TypeFilter;

    /** Scan Config/ and Source/ for path literals the registry cannot see. */
    bool bScanIndirectReferences = true;

    /** Drop rows for __ExternalActors__ / __ExternalObjects__ packages. */
    bool bHideExternalPackages = true;

    /** Drop redirector rows; they are plumbing, not content an artist acts on. */
    bool bHideRedirectors = true;

    /** Report only assets that no level reaches. */
    bool bOnlyUnusedAssets = false;

    FLevelUsageResolveOptions ResolveOptions;
};

// Exported: CountByVerdict is defined out of line, so the Editor module cannot link without this.
struct ASSETUSAGEAUDITCORE_API FAssetUsageAuditResult
{
    TArray<FAssetUsageRow> Rows;
    FAssetUsageReportHeader Header;
    FAssetUsageGraphStats GraphStats;
    FLevelUsageStats LevelStats;

    int32 CountByVerdict(EAssetUsageVerdict Verdict) const;
};

/**
 * Runs the whole analysis: graph, level reachability, indirect scan, verdicts.
 *
 * Free of UI and of the editor asset pipeline, so the Slate panel and a future commandlet can
 * call exactly the same code and cannot drift apart.
 */
class ASSETUSAGEAUDITCORE_API FAssetUsageAuditor
{
public:
    static FAssetUsageAuditResult Run(IAssetRegistry& AssetRegistry, FAssetUsageAuditRequest& Request);

    /**
     * Classify one asset.
     *
     * Pure and exposed for testing: the ordering of these rules is the difference between a tool
     * people trust and one that tells an artist to delete the GameMode.
     *
     * @param bReachableFromLevel  A level's traversal reached this asset.
     * @param bHasReferencers      Something in the graph depends on it.
     * @param bFoundInConfigOrSource  A path literal in Config/ or Source/ named it.
     * @param bIsBlindSpot         Its type is one the registry cannot track reliably, e.g. FMOD.
     */
    static EAssetUsageVerdict ClassifyVerdict(bool bReachableFromLevel, bool bHasReferencers, bool bFoundInConfigOrSource, bool bIsBlindSpot);

    /**
     * True for asset types whose real usage the Asset Registry cannot see.
     *
     * FMOD is the concrete case on this project: it resolves events by string path through the
     * FMOD Studio runtime, entirely outside the UObject reference graph, so "no referencers"
     * carries no information at all for an FMOD asset.
     */
    static bool IsRegistryBlindSpot(const FAssetUsageNode& Node);
};
