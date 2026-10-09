// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageGraph.h"
#include "AssetUsageAuditCoreModule.h"
#include "AssetUsagePaths.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectRedirector.h"

namespace
{
    /** Asset registry tag carrying a Blueprint's generated gameplay class. */
    const FName GeneratedClassTagName(TEXT("GeneratedClass"));

    /** Redirector chains are shallow in practice; this only guards against a malformed cycle. */
    constexpr int32 MaxRedirectorHops = 16;
}

void FAssetUsageGraph::Reset()
{
    Nodes.Reset();
    PackageToIndex.Reset();
    Dependencies.Reset();
    DependencyOffsets.Reset();
    Referencers.Reset();
    ReferencerOffsets.Reset();
    LevelIndices.Reset();
    Stats = FAssetUsageGraphStats();
}

int32 FAssetUsageGraph::AddOrFindPackage(FName PackageName)
{
    if (const int32* Existing = PackageToIndex.Find(PackageName))
    {
        return *Existing;
    }

    const int32 NewIndex = Nodes.Num();

    FAssetUsageNode& Node = Nodes.AddDefaulted_GetRef();
    Node.PackageName = PackageName;
    Node.bIsExternalPackage = AssetUsagePaths::IsExternalPackage(PackageName);

    PackageToIndex.Add(PackageName, NewIndex);

    return NewIndex;
}

void FAssetUsageGraph::Build(IAssetRegistry& AssetRegistry, const FAssetUsageGraphBuildOptions& Options)
{
    Reset();

    // Temporary caching makes repeated class-hierarchy and tag lookups cheap for the duration of
    // the sweep. GetDerivedClassNames in particular is documented as slow without it.
    AssetRegistry.SetTemporaryCachingMode(true);
    ON_SCOPE_EXIT
    {
        AssetRegistry.SetTemporaryCachingMode(false);
    };

    FARFilter Filter;
    Filter.bRecursivePaths = true;

    // On-disk only: unsaved in-memory assets have no dependency data yet, and including them
    // makes the result depend on which windows the user happens to have open.
    Filter.bIncludeOnlyOnDiskAssets = true;

    if (Options.IncludePackagePaths.IsEmpty())
    {
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
    }
    else
    {
        for (const FString& Path : Options.IncludePackagePaths)
        {
            Filter.PackagePaths.Add(FName(*Path));
        }
    }

    FARCompiledFilter CompiledFilter;
    AssetRegistry.CompileFilter(Filter, CompiledFilter);

    const FTopLevelAssetPath WorldClassPath = UWorld::StaticClass()->GetClassPathName();
    const FTopLevelAssetPath RedirectorClassPath = UObjectRedirector::StaticClass()->GetClassPathName();

    // --- Pass 1: inventory -------------------------------------------------------------------
    // EnumerateAssets rather than GetAssets: at 80k assets the latter materialises one giant
    // TArray<FAssetData> before we have looked at a single element.
    {
        const double StartTime = FPlatformTime::Seconds();

        AssetRegistry.EnumerateAssets(CompiledFilter, [this, &Options, &WorldClassPath, &RedirectorClassPath](const FAssetData& AssetData)
        {
            const FName PackageName = AssetData.PackageName;

            if (AssetUsagePaths::IsPathExcluded(PackageName, Options.ExcludePackagePaths))
            {
                return true;
            }

            const int32 Index = AddOrFindPackage(PackageName);
            FAssetUsageNode& Node = Nodes[Index];

            // A package can hold several assets; keep the first as representative but never let a
            // secondary asset downgrade a class we already recorded.
            if (Node.AssetName.IsNone())
            {
                Node.AssetName = AssetData.AssetName;
                Node.ClassPath = AssetData.AssetClassPath;
            }

            if (AssetData.AssetClassPath == WorldClassPath)
            {
                Node.bIsLevel = true;
                Node.ClassPath = WorldClassPath;
            }

            if (AssetData.AssetClassPath == RedirectorClassPath)
            {
                Node.bIsRedirector = true;
            }

            // The GeneratedClass tag is the only route from a Blueprint asset to its gameplay
            // class; the asset's own class is always /Script/Engine.Blueprint.
            FString GeneratedClass;
            if (AssetData.GetTagValue(GeneratedClassTagName, GeneratedClass) && !GeneratedClass.IsEmpty())
            {
                // The tag is stored in export-text form, e.g. BlueprintGeneratedClass'/Game/X/BP_Y.BP_Y_C'.
                // TrySetPath rather than the FStringView constructor: a malformed tag then leaves the
                // path empty instead of asserting mid-sweep.
                Node.GeneratedClassPath.TrySetPath(FPackageName::ExportTextPathToObjectPath(GeneratedClass));
            }

            return true;
        }, UE::AssetRegistry::EEnumerateAssetsFlags::OnlyOnDiskAssets);

        Stats.EnumerateSeconds = FPlatformTime::Seconds() - StartTime;
    }

    Stats.NumPackages = Nodes.Num();

    // --- Pass 2: dependency edges ------------------------------------------------------------
    DependencyOffsets.SetNumZeroed(Nodes.Num() + 1);

    if (Options.bGatherDependencies)
    {
        const double StartTime = FPlatformTime::Seconds();

        TArray<FAssetDependency> EdgeBuffer;

        // Node count grows as we discover dependency targets outside the swept paths (engine
        // content, plugin content). Index by position rather than caching Num().
        for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
        {
            DependencyOffsets[NodeIndex] = Dependencies.Num();

            EdgeBuffer.Reset();

            // The query that makes this tool correct. Package category, no requirement flags.
            // Never EDependencyQuery::Hard: map -> external actor edges carry Game|Build without
            // Hard, so a Hard query silently drops every OFPA actor in the project.
            AssetRegistry.GetDependencies(
                Nodes[NodeIndex].PackageName,
                EdgeBuffer,
                AssetUsageAudit::MakeTraversalCategory(),
                AssetUsageAudit::MakeTraversalQuery());

            for (const FAssetDependency& Dependency : EdgeBuffer)
            {
                const FName TargetPackage = Dependency.AssetId.PackageName;

                if (TargetPackage.IsNone() || TargetPackage == Nodes[NodeIndex].PackageName)
                {
                    continue;
                }

                if (AssetUsagePaths::IsPathExcluded(TargetPackage, Options.ExcludePackagePaths))
                {
                    continue;
                }

                // Script packages (/Script/Engine) are code, not assets. They inflate the graph
                // and can never be exported or reported as unused.
                if (AssetUsagePaths::IsScriptPackage(TargetPackage))
                {
                    continue;
                }

                FAssetUsageEdge& Edge = Dependencies.AddDefaulted_GetRef();
                Edge.TargetIndex = AddOrFindPackage(TargetPackage);
                Edge.Properties = Dependency.Properties;
            }

            // AddOrFindPackage above may have appended nodes; keep the offset table in step.
            if (DependencyOffsets.Num() < Nodes.Num() + 1)
            {
                DependencyOffsets.SetNumZeroed(Nodes.Num() + 1, EAllowShrinking::No);
            }
        }

        DependencyOffsets[Nodes.Num()] = Dependencies.Num();

        Stats.DependencySeconds = FPlatformTime::Seconds() - StartTime;
    }
    else
    {
        DependencyOffsets[Nodes.Num()] = 0;
    }

    // Nodes discovered during pass 2 have no inventory data. Fill in what we can from the path so
    // they are never reported as an unnamed blank row.
    for (FAssetUsageNode& Node : Nodes)
    {
        if (Node.AssetName.IsNone())
        {
            Node.AssetName = FName(*FPackageName::GetShortName(Node.PackageName));
        }
    }

    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        if (Nodes[NodeIndex].bIsLevel)
        {
            LevelIndices.Add(NodeIndex);
        }

        if (Nodes[NodeIndex].bIsExternalPackage)
        {
            ++Stats.NumExternalPackages;
        }
    }

    BuildReverseEdges();

    Stats.NumPackages = Nodes.Num();
    Stats.NumEdges = Dependencies.Num();
    Stats.NumLevels = LevelIndices.Num();

    UE_LOG(LogAssetUsageAudit, Log,
        TEXT("Graph built: %d packages, %d edges, %d levels, %d external packages. Enumerate %.2fs, dependencies %.2fs, total %.2fs."),
        Stats.NumPackages, Stats.NumEdges, Stats.NumLevels, Stats.NumExternalPackages,
        Stats.EnumerateSeconds, Stats.DependencySeconds, Stats.TotalSeconds());
}

void FAssetUsageGraph::BuildReverseEdges()
{
    Referencers.Reset();
    ReferencerOffsets.Reset();
    ReferencerOffsets.SetNumZeroed(Nodes.Num() + 1);

    if (Dependencies.IsEmpty())
    {
        return;
    }

    // Counting sort: one pass to size each bucket, one to fill. Avoids per-node TArray growth.
    for (const FAssetUsageEdge& Edge : Dependencies)
    {
        if (Nodes.IsValidIndex(Edge.TargetIndex))
        {
            ++ReferencerOffsets[Edge.TargetIndex + 1];
        }
    }

    for (int32 Index = 1; Index <= Nodes.Num(); ++Index)
    {
        ReferencerOffsets[Index] += ReferencerOffsets[Index - 1];
    }

    Referencers.SetNumUninitialized(Dependencies.Num());

    TArray<int32> FillCursor = ReferencerOffsets;

    for (int32 SourceIndex = 0; SourceIndex < Nodes.Num(); ++SourceIndex)
    {
        for (const FAssetUsageEdge& Edge : GetDependencies(SourceIndex))
        {
            if (!Nodes.IsValidIndex(Edge.TargetIndex))
            {
                continue;
            }

            FAssetUsageEdge& Reverse = Referencers[FillCursor[Edge.TargetIndex]++];
            Reverse.TargetIndex = SourceIndex;
            Reverse.Properties = Edge.Properties;
        }
    }
}

TConstArrayView<FAssetUsageEdge> FAssetUsageGraph::GetDependencies(int32 Index) const
{
    if (!Nodes.IsValidIndex(Index) || DependencyOffsets.Num() < Nodes.Num() + 1)
    {
        return TConstArrayView<FAssetUsageEdge>();
    }

    const int32 Begin = DependencyOffsets[Index];
    const int32 End = DependencyOffsets[Index + 1];

    if (Begin >= End)
    {
        return TConstArrayView<FAssetUsageEdge>();
    }

    return TConstArrayView<FAssetUsageEdge>(Dependencies.GetData() + Begin, End - Begin);
}

TConstArrayView<FAssetUsageEdge> FAssetUsageGraph::GetReferencers(int32 Index) const
{
    if (!Nodes.IsValidIndex(Index) || ReferencerOffsets.Num() < Nodes.Num() + 1)
    {
        return TConstArrayView<FAssetUsageEdge>();
    }

    const int32 Begin = ReferencerOffsets[Index];
    const int32 End = ReferencerOffsets[Index + 1];

    if (Begin >= End)
    {
        return TConstArrayView<FAssetUsageEdge>();
    }

    return TConstArrayView<FAssetUsageEdge>(Referencers.GetData() + Begin, End - Begin);
}

int32 FAssetUsageGraph::ResolveRedirector(int32 Index) const
{
    int32 Current = Index;

    for (int32 Hop = 0; Hop < MaxRedirectorHops; ++Hop)
    {
        if (!Nodes.IsValidIndex(Current) || !Nodes[Current].bIsRedirector)
        {
            return Current;
        }

        // A redirector package depends on exactly the asset it forwards to. Take the first
        // non-self dependency; there is no second candidate in a well-formed redirector.
        const TConstArrayView<FAssetUsageEdge> Forward = GetDependencies(Current);

        int32 Next = INDEX_NONE;
        for (const FAssetUsageEdge& Edge : Forward)
        {
            if (Edge.TargetIndex != Current && Nodes.IsValidIndex(Edge.TargetIndex))
            {
                Next = Edge.TargetIndex;
                break;
            }
        }

        if (Next == INDEX_NONE)
        {
            // Dangling redirector: points at something outside the sweep, or at nothing.
            return Current;
        }

        Current = Next;
    }

    UE_LOG(LogAssetUsageAudit, Warning,
        TEXT("Redirector chain from '%s' exceeded %d hops; treating it as unresolvable."),
        *Nodes[Index].PackageName.ToString(), MaxRedirectorHops);

    return Current;
}
