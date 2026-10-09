// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetUsageAuditTypes.h"
#include "AssetRegistry/AssetData.h"

class IAssetRegistry;

/**
 * One dependency edge, stored by dense index rather than by FName.
 *
 * 8 bytes. At this project's scale (80k packages, several hundred thousand edges) the difference
 * between this and a TMap<FName, TArray<FName>> is the difference between a tool that answers in
 * seconds and one nobody waits for.
 */
struct FAssetUsageEdge
{
    /** Dense index of the target package in FAssetUsageGraph. */
    int32 TargetIndex = INDEX_NONE;

    /** Edge properties as reported by the registry: Hard / Game / Build. */
    UE::AssetRegistry::EDependencyProperty Properties = UE::AssetRegistry::EDependencyProperty::None;

    bool IsHard() const
    {
        return EnumHasAnyFlags(Properties, UE::AssetRegistry::EDependencyProperty::Hard);
    }

    /** Lack of Hard is what the engine calls a soft dependency (AssetRegistryInterface.h:95). */
    bool IsSoft() const
    {
        return !IsHard();
    }

    /** Lack of Game means the edge is editor-only and will not survive a cook. */
    bool IsEditorOnly() const
    {
        return !EnumHasAnyFlags(Properties, UE::AssetRegistry::EDependencyProperty::Game);
    }
};

/**
 * Per-package facts captured once during the sweep, so later passes never re-query the registry.
 */
struct FAssetUsageNode
{
    FName PackageName;
    FName AssetName;
    FTopLevelAssetPath ClassPath;

    /**
     * Gameplay class behind a Blueprint, read from the GeneratedClass asset-registry tag.
     *
     * Needed because a BP asset's own ClassPath is always /Script/Engine.Blueprint. Filtering
     * ClassPaths alone finds zero Blueprints, which in this project means finding almost nothing.
     */
    FTopLevelAssetPath GeneratedClassPath;

    /** ClassPath is /Script/Engine.World. */
    bool bIsLevel = false;

    /** Package lives under __ExternalActors__ or __ExternalObjects__. */
    bool bIsExternalPackage = false;

    /** Asset is an ObjectRedirector and must be followed through, not reported. */
    bool bIsRedirector = false;
};

struct FAssetUsageGraphBuildOptions
{
    /** Roots to sweep. Defaults to /Game. */
    TArray<FString> IncludePackagePaths;

    /** Package path prefixes to skip entirely, e.g. Content/3rdParty. */
    TArray<FString> ExcludePackagePaths;

    /** Collect dependency edges. Off gives a much faster inventory-only pass. */
    bool bGatherDependencies = true;
};

struct FAssetUsageGraphStats
{
    int32 NumPackages = 0;
    int32 NumEdges = 0;
    int32 NumLevels = 0;
    int32 NumExternalPackages = 0;
    double EnumerateSeconds = 0.0;
    double DependencySeconds = 0.0;

    double TotalSeconds() const
    {
        return EnumerateSeconds + DependencySeconds;
    }
};

/**
 * Dense, immutable-after-build dependency graph over the project's packages.
 *
 * Edges are stored CSR-style: one flat FAssetUsageEdge array plus an offset table, so a node's
 * dependency list is a contiguous view with no per-node allocation.
 *
 * The graph deliberately stores dependencies in BOTH directions. Forward edges answer "what does
 * this level use"; reverse edges answer "is this asset referenced by anything at all", which is
 * what separates the Unreferenced verdict from UsedByAssetsOnly.
 */
class ASSETUSAGEAUDITCORE_API FAssetUsageGraph
{
public:
    /**
     * Sweep the registry and build the graph.
     *
     * The registry must already be populated. In a commandlet the AssetRegistry module gathers
     * synchronously on load; in the editor callers must wait for OnFilesLoaded first.
     */
    void Build(IAssetRegistry& AssetRegistry, const FAssetUsageGraphBuildOptions& Options);

    void Reset();

    int32 Num() const
    {
        return Nodes.Num();
    }

    bool IsValidIndex(int32 Index) const
    {
        return Nodes.IsValidIndex(Index);
    }

    /** INDEX_NONE when the package was not part of the sweep. */
    int32 FindPackageIndex(FName PackageName) const
    {
        const int32* Found = PackageToIndex.Find(PackageName);
        return Found ? *Found : INDEX_NONE;
    }

    const FAssetUsageNode& GetNode(int32 Index) const
    {
        return Nodes[Index];
    }

    TConstArrayView<FAssetUsageNode> GetNodes() const
    {
        return Nodes;
    }

    /** Packages this one depends on. */
    TConstArrayView<FAssetUsageEdge> GetDependencies(int32 Index) const;

    /** Packages that depend on this one. */
    TConstArrayView<FAssetUsageEdge> GetReferencers(int32 Index) const;

    /** Dense indices of every /Script/Engine.World package in the sweep. */
    TConstArrayView<int32> GetLevelIndices() const
    {
        return LevelIndices;
    }

    const FAssetUsageGraphStats& GetStats() const
    {
        return Stats;
    }

    /**
     * Follow a redirector chain to the asset it ultimately points at.
     * Returns Index unchanged when it is not a redirector. Cycle-safe.
     */
    int32 ResolveRedirector(int32 Index) const;

private:
    int32 AddOrFindPackage(FName PackageName);
    void BuildReverseEdges();

    TArray<FAssetUsageNode> Nodes;
    TMap<FName, int32> PackageToIndex;

    /** CSR forward edges: Dependencies[DependencyOffsets[i] .. DependencyOffsets[i+1]). */
    TArray<FAssetUsageEdge> Dependencies;
    TArray<int32> DependencyOffsets;

    TArray<FAssetUsageEdge> Referencers;
    TArray<int32> ReferencerOffsets;

    TArray<int32> LevelIndices;

    FAssetUsageGraphStats Stats;
};
