// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetUsageAuditTypes.h"
#include "Containers/BitArray.h"

class FAssetUsageGraph;
class IAssetRegistry;

struct FLevelUsageResolveOptions
{
    /** Levels to analyse. Empty means every /Script/Engine.World package in the graph. */
    TArray<FName> LevelPackages;

    /**
     * Call ULevel::ScanLevelAssets before traversing each level.
     *
     * The external-actor dependency gatherer only reports packages the registry has already
     * scanned. Skipping this can silently yield an empty external-actor set on a level nobody
     * has opened this session - which looks exactly like a level that genuinely has no actors.
     */
    bool bScanLevelAssetsFirst = true;

    /**
     * Additionally seed the BFS from ULevel::GetExternalActorsPaths / GetExternalObjectsPaths.
     *
     * Belt and braces: the gatherer normally puts these edges in the graph already. Keeping the
     * explicit seed means a stale or partially-scanned registry degrades to a slower correct
     * answer instead of a fast wrong one, and it lets us tag ExternalActor provenance precisely.
     */
    bool bSeedExternalPackages = true;

    /**
     * Follow a reference into another map's contents.
     *
     * Off by default, and that default is load-bearing. A map does not only reference its own
     * sublevels: anything it can reach may name an unrelated map, and on this project it does.
     * WP_Example -> BP_GameMode -> PDA_MenuConfig -> L_Other drags in the whole
     * of L_Other, which measured at 9994 of WP_Example's 18136 rows - two thirds of the answer
     * was another level's content.
     *
     * With this off, a foreign map is still reported as referenced, but its contents are attributed
     * to that map alone. Genuine sublevels and Level Instances are unaffected: they are reached
     * from the level package itself or from one of its external actor packages, and that crossing
     * is always allowed. See ShouldCrossIntoLevel in the .cpp.
     */
    bool bTraverseIntoOtherLevels = false;

    /** Record a human-readable route for each asset. Costs one int32 array per BFS. */
    bool bRecordRoutes = true;

    /** Route strings longer than this many hops are elided in the middle. */
    int32 MaxRouteHops = 12;

    /** Optional progress sink, called once per level with (LevelIndex, TotalLevels). */
    TFunction<void(int32, int32)> OnLevelProgress;

    /** Return true to abort the sweep between levels. */
    TFunction<bool()> ShouldAbort;
};

struct FLevelUsageStats
{
    int32 LevelsScanned = 0;
    int32 AssetsReachable = 0;
    int32 ExternalPackagesSeeded = 0;
    int32 RedirectorsResolved = 0;

    /**
     * Foreign maps referenced but deliberately not expanded into.
     *
     * A non-zero value here is the amount of another level's content that would otherwise have
     * been attributed to this one. Worth surfacing: on WP_Example it was two thirds of the report.
     */
    int32 ForeignLevelsNotExpanded = 0;
    double ScanLevelAssetsSeconds = 0.0;
    double TraversalSeconds = 0.0;

    double TotalSeconds() const
    {
        return ScanLevelAssetsSeconds + TraversalSeconds;
    }
};

/**
 * Which levels reach which assets.
 *
 * Reachability is one TBitArray per level over dense package indices. At this project's scale
 * that is 1207 levels x 80608 bits, roughly 12 MB - cheap enough to hold both query directions
 * ("assets on this level" and "levels using this asset") without ever re-walking the graph.
 */
struct ASSETUSAGEAUDITCORE_API FLevelUsageResult
{
    /** Level package names, parallel to LevelReachability. */
    TArray<FName> LevelPackageNames;

    /** LevelReachability[L][A] - level L reaches asset A. */
    TArray<TBitArray<>> LevelReachability;

    /** Union across every level. The primary input to the UsedOnLevel verdict. */
    TBitArray<> ReachableFromAnyLevel;

    /** Accumulated provenance flags per asset index. */
    TArray<EAssetUsageProvenance> Provenance;

    /** Incoming edge counts per asset index, split by the Hard property. */
    TArray<int32> HardReferenceCounts;
    TArray<int32> SoftReferenceCounts;

    /** Route string per asset index, from the first level that reached it. Empty when not recorded. */
    TMap<int32, FString> Routes;

    FLevelUsageStats Stats;

    /** Levels that reach the given asset index. */
    TArray<FName> GetLevelsForAsset(int32 AssetIndex) const;

    bool IsReachableFromAnyLevel(int32 AssetIndex) const
    {
        return ReachableFromAnyLevel.IsValidIndex(AssetIndex) && ReachableFromAnyLevel[AssetIndex];
    }
};

/**
 * Forward traversal from level packages to everything they pull in.
 *
 * Direction matters. Going forward from the map is correct; going backward from an external actor
 * package to its owning map is not, because PackageDependencyData.cpp:57-96 deliberately strips
 * the UsedInGame flag off that reverse import by naming convention, so the AssetManager will not
 * drag a whole map in when something references one actor. Reconstructing ownership from the path
 * instead is possible but fragile - Content Bundles inject /CB/<Guid>/ and External Data Layers
 * inject /EDL/<UID>/ between the folder and the level path. Forward traversal sidesteps all of it.
 */
class ASSETUSAGEAUDITCORE_API FLevelUsageResolver
{
public:
    FLevelUsageResolver(const FAssetUsageGraph& InGraph, IAssetRegistry& InAssetRegistry);

    FLevelUsageResult Resolve(const FLevelUsageResolveOptions& Options);

private:
    /** BFS from one level. Marks OutReachable and accumulates provenance and counts. */
    void TraverseLevel(int32 LevelIndex, const FLevelUsageResolveOptions& Options, TBitArray<>& OutReachable, FLevelUsageResult& InOutResult, TArray<int32>& ScratchPredecessor, TArray<int32>& ScratchQueue);

    /** Dense indices of the __ExternalActors__ / __ExternalObjects__ packages owned by a level. */
    void GatherExternalPackageSeeds(int32 LevelIndex, TArray<int32>& OutSeeds, FLevelUsageResult& InOutResult) const;

    FString BuildRouteString(int32 LevelIndex, int32 AssetIndex, const TArray<int32>& Predecessor, int32 MaxHops) const;

    const FAssetUsageGraph& Graph;
    IAssetRegistry& AssetRegistry;
};
