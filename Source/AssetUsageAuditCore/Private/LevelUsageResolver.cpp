// MagentaDolphin 2026. Asset Usage Audit.

#include "LevelUsageResolver.h"

#include "AssetUsageAuditCoreModule.h"
#include "AssetUsageGraph.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Algo/Reverse.h"
#include "Engine/Level.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/StringBuilder.h"

FLevelUsageResolver::FLevelUsageResolver(const FAssetUsageGraph& InGraph, IAssetRegistry& InAssetRegistry)
    : Graph(InGraph)
    , AssetRegistry(InAssetRegistry)
{
}

TArray<FName> FLevelUsageResult::GetLevelsForAsset(int32 AssetIndex) const
{
    TArray<FName> Out;

    for (int32 LevelIdx = 0; LevelIdx < LevelReachability.Num(); ++LevelIdx)
    {
        const TBitArray<>& Bits = LevelReachability[LevelIdx];
        if (Bits.IsValidIndex(AssetIndex) && Bits[AssetIndex])
        {
            Out.Add(LevelPackageNames[LevelIdx]);
        }
    }

    return Out;
}

FLevelUsageResult FLevelUsageResolver::Resolve(const FLevelUsageResolveOptions& Options)
{
    FLevelUsageResult Result;

    const int32 NumPackages = Graph.Num();
    if (NumPackages == 0)
    {
        return Result;
    }

    // Decide which levels to walk. An explicit list is resolved against the graph so a typo
    // surfaces as a warning rather than as a silently empty report.
    TArray<int32> LevelIndices;
    if (Options.LevelPackages.Num() > 0)
    {
        LevelIndices.Reserve(Options.LevelPackages.Num());
        for (FName LevelPackage : Options.LevelPackages)
        {
            const int32 Index = Graph.FindPackageIndex(LevelPackage);
            if (Index == INDEX_NONE)
            {
                UE_LOG(LogAssetUsageAudit, Warning, TEXT("Level '%s' is not in the graph; skipping."), *LevelPackage.ToString());
                continue;
            }

            if (!Graph.GetNode(Index).bIsLevel)
            {
                UE_LOG(LogAssetUsageAudit, Warning, TEXT("Package '%s' is not a World asset; skipping."), *LevelPackage.ToString());
                continue;
            }

            LevelIndices.Add(Index);
        }
    }
    else
    {
        LevelIndices = Graph.GetLevelIndices();
    }

    const int32 NumLevels = LevelIndices.Num();

    UE_LOG(LogAssetUsageAudit, Log, TEXT("Resolving %d level(s); %d requested explicitly, %d present in the graph."),
        NumLevels, Options.LevelPackages.Num(), Graph.GetLevelIndices().Num());

    Result.LevelPackageNames.Reserve(NumLevels);
    Result.LevelReachability.Reserve(NumLevels);
    Result.ReachableFromAnyLevel.Init(false, NumPackages);
    Result.Provenance.Init(EAssetUsageProvenance::None, NumPackages);
    Result.HardReferenceCounts.Init(0, NumPackages);
    Result.SoftReferenceCounts.Init(0, NumPackages);

    // Scratch buffers reused across every level so the BFS allocates nothing per level.
    TArray<int32> ScratchPredecessor;
    TArray<int32> ScratchQueue;
    ScratchPredecessor.SetNumUninitialized(NumPackages);
    ScratchQueue.Reserve(FMath::Min(NumPackages, 4096));

    const double TraversalStart = FPlatformTime::Seconds();

    for (int32 Ordinal = 0; Ordinal < NumLevels; ++Ordinal)
    {
        if (Options.ShouldAbort && Options.ShouldAbort())
        {
            UE_LOG(LogAssetUsageAudit, Warning, TEXT("Level sweep aborted after %d of %d levels."), Ordinal, NumLevels);
            break;
        }

        if (Options.OnLevelProgress)
        {
            Options.OnLevelProgress(Ordinal, NumLevels);
        }

        const int32 LevelIndex = LevelIndices[Ordinal];

        // The gatherer only reports external packages the registry has already seen. Without this
        // a level nobody opened this session can report zero actors, which is indistinguishable
        // from a level that genuinely has none.
        if (Options.bScanLevelAssetsFirst)
        {
            const double ScanStart = FPlatformTime::Seconds();
            ULevel::ScanLevelAssets(Graph.GetNode(LevelIndex).PackageName.ToString());
            Result.Stats.ScanLevelAssetsSeconds += FPlatformTime::Seconds() - ScanStart;
        }

        TBitArray<> Reachable;
        Reachable.Init(false, NumPackages);

        TraverseLevel(LevelIndex, Options, Reachable, Result, ScratchPredecessor, ScratchQueue);

        Result.LevelPackageNames.Add(Graph.GetNode(LevelIndex).PackageName);
        Result.LevelReachability.Add(MoveTemp(Reachable));
        ++Result.Stats.LevelsScanned;
    }

    Result.Stats.TraversalSeconds = FPlatformTime::Seconds() - TraversalStart - Result.Stats.ScanLevelAssetsSeconds;

    for (int32 Index = 0; Index < NumPackages; ++Index)
    {
        if (Result.ReachableFromAnyLevel[Index])
        {
            ++Result.Stats.AssetsReachable;
        }
    }

    return Result;
}

void FLevelUsageResolver::TraverseLevel(int32 LevelIndex, const FLevelUsageResolveOptions& Options, TBitArray<>& OutReachable, FLevelUsageResult& InOutResult, TArray<int32>& ScratchPredecessor, TArray<int32>& ScratchQueue)
{
    const int32 NumPackages = Graph.Num();

    if (Options.bRecordRoutes)
    {
        for (int32& Pred : ScratchPredecessor)
        {
            Pred = INDEX_NONE;
        }
    }

    ScratchQueue.Reset();

    const auto Enqueue = [&](int32 Index, int32 FromIndex, EAssetUsageProvenance EdgeProvenance)
    {
        if (!Graph.IsValidIndex(Index) || OutReachable[Index])
        {
            return;
        }

        OutReachable[Index] = true;
        InOutResult.ReachableFromAnyLevel[Index] = true;
        InOutResult.Provenance[Index] |= EdgeProvenance;

        if (Options.bRecordRoutes)
        {
            ScratchPredecessor[Index] = FromIndex;
        }

        ScratchQueue.Add(Index);
    };

    Enqueue(LevelIndex, INDEX_NONE, EAssetUsageProvenance::None);

    // Explicit OFPA seeds. Normally redundant with the gatherer's edges, but it costs one registry
    // filter per level and converts a stale-registry failure from wrong into merely slow.
    if (Options.bSeedExternalPackages)
    {
        TArray<int32> Seeds;
        GatherExternalPackageSeeds(LevelIndex, Seeds, InOutResult);

        for (int32 SeedIndex : Seeds)
        {
            Enqueue(SeedIndex, LevelIndex, EAssetUsageProvenance::ExternalActor | EAssetUsageProvenance::SoftReference);
        }
    }

    int32 Head = 0;
    while (Head < ScratchQueue.Num())
    {
        const int32 Current = ScratchQueue[Head++];

        for (const FAssetUsageEdge& Edge : Graph.GetDependencies(Current))
        {
            int32 Target = Edge.TargetIndex;
            if (!Graph.IsValidIndex(Target))
            {
                continue;
            }

            // A redirector reached on the way to a real asset must be followed through, or the
            // redirector is reported used and the asset behind it is reported garbage.
            EAssetUsageProvenance EdgeProvenance = EAssetUsageProvenance::None;
            if (Graph.GetNode(Target).bIsRedirector)
            {
                const int32 Resolved = Graph.ResolveRedirector(Target);
                if (Resolved != Target)
                {
                    // Mark the redirector itself reachable too - it is a real package on disk that
                    // the level does depend on, and hiding it would make the report disagree with
                    // what a migrate or a copy actually pulls.
                    Enqueue(Target, Current, EAssetUsageProvenance::Redirector);
                    ++InOutResult.Stats.RedirectorsResolved;
                    Target = Resolved;
                    EdgeProvenance |= EAssetUsageProvenance::Redirector;
                }
            }

            if (Edge.IsHard())
            {
                ++InOutResult.HardReferenceCounts[Target];
                EdgeProvenance |= EAssetUsageProvenance::HardReference;
            }
            else
            {
                ++InOutResult.SoftReferenceCounts[Target];
                EdgeProvenance |= EAssetUsageProvenance::SoftReference;
            }

            if (Edge.IsEditorOnly())
            {
                EdgeProvenance |= EAssetUsageProvenance::EditorOnly;
            }

            const FAssetUsageNode& TargetNode = Graph.GetNode(Target);
            if (TargetNode.bIsExternalPackage)
            {
                EdgeProvenance |= EAssetUsageProvenance::ExternalActor;
            }

            // Reaching another World is NOT automatically a sublevel. An earlier version assumed it
            // was, and on WP_Example that assumption pulled the whole of L_Other in through
            // BP_GameMode -> PDA_MenuConfig: 9994 of 18136 rows belonged to a
            // different map. A crossing only counts when the reference comes from the level package
            // itself or from one of its external actor packages, which is the shape a streaming
            // sublevel or a Level Instance actually has.
            bool bStopAtForeignLevel = false;

            if (TargetNode.bIsLevel && Target != LevelIndex)
            {
                const FAssetUsageNode& SourceNode = Graph.GetNode(Current);
                const bool bCrossingIsStructural = SourceNode.bIsLevel || SourceNode.bIsExternalPackage;

                if (bCrossingIsStructural)
                {
                    EdgeProvenance |= EAssetUsageProvenance::Sublevel;
                }
                else if (!Options.bTraverseIntoOtherLevels)
                {
                    // Record the foreign map as referenced - it genuinely is - but attribute its
                    // contents to itself. Its own sweep reports them.
                    bStopAtForeignLevel = true;
                }
            }

            // Accumulate provenance even when already visited: an asset reached by both a hard and
            // a soft path should report both, otherwise the columns depend on BFS visit order.
            if (OutReachable[Target])
            {
                InOutResult.Provenance[Target] |= EdgeProvenance;
                continue;
            }

            if (bStopAtForeignLevel)
            {
                // Mark it reachable without queueing it, so the BFS never expands its dependencies.
                OutReachable[Target] = true;
                InOutResult.ReachableFromAnyLevel[Target] = true;
                InOutResult.Provenance[Target] |= EdgeProvenance;

                if (Options.bRecordRoutes)
                {
                    ScratchPredecessor[Target] = Current;
                    InOutResult.Routes.FindOrAdd(Target, BuildRouteString(LevelIndex, Target, ScratchPredecessor, Options.MaxRouteHops));
                }

                ++InOutResult.Stats.ForeignLevelsNotExpanded;
                continue;
            }

            Enqueue(Target, Current, EdgeProvenance);
        }
    }

    if (Options.bRecordRoutes)
    {
        // Walk the queue, not the whole index space: the queue already holds exactly the nodes this
        // level reached. Scanning all packages per level would be 80k x 1207 iterations for nothing.
        for (int32 Index : ScratchQueue)
        {
            if (InOutResult.Routes.Contains(Index))
            {
                continue;
            }

            InOutResult.Routes.Add(Index, BuildRouteString(LevelIndex, Index, ScratchPredecessor, Options.MaxRouteHops));
        }
    }
}

void FLevelUsageResolver::GatherExternalPackageSeeds(int32 LevelIndex, TArray<int32>& OutSeeds, FLevelUsageResult& InOutResult) const
{
    OutSeeds.Reset();

    const FString LevelPackageName = Graph.GetNode(LevelIndex).PackageName.ToString();

    // The plural form runs registered path-provider delegates, so plugins that mount their own
    // external-actor roots are covered. Never hardcode "__ExternalActors__" here.
    TArray<FString> ExternalPaths = ULevel::GetExternalActorsPaths(LevelPackageName);
    ExternalPaths.Append(ULevel::GetExternalObjectsPaths(LevelPackageName));

    if (ExternalPaths.Num() == 0)
    {
        return;
    }

    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.bIncludeOnlyOnDiskAssets = true;
    Filter.PackagePaths.Reserve(ExternalPaths.Num());

    for (const FString& Path : ExternalPaths)
    {
        Filter.PackagePaths.Add(FName(*Path));
    }

    AssetRegistry.EnumerateAssets(Filter, [this, &OutSeeds, &InOutResult](const FAssetData& AssetData)
    {
        const int32 Index = Graph.FindPackageIndex(AssetData.PackageName);
        if (Index != INDEX_NONE)
        {
            OutSeeds.AddUnique(Index);
            ++InOutResult.Stats.ExternalPackagesSeeded;
        }
        return true;
    });
}

FString FLevelUsageResolver::BuildRouteString(int32 LevelIndex, int32 AssetIndex, const TArray<int32>& Predecessor, int32 MaxHops) const
{
    TArray<int32> Chain;

    int32 Current = AssetIndex;
    while (Current != INDEX_NONE && Chain.Num() < 1024)
    {
        Chain.Add(Current);
        if (Current == LevelIndex)
        {
            break;
        }
        Current = Predecessor.IsValidIndex(Current) ? Predecessor[Current] : INDEX_NONE;
    }

    Algo::Reverse(Chain);

    const auto NameAt = [this](int32 Index)
    {
        return Graph.GetNode(Index).AssetName.IsNone()
            ? Graph.GetNode(Index).PackageName.ToString()
            : Graph.GetNode(Index).AssetName.ToString();
    };

    if (MaxHops > 0 && Chain.Num() > MaxHops)
    {
        // Keep both ends: the level explains scope, the tail explains the asset. The middle is
        // where an artist stops reading anyway.
        const int32 HeadCount = MaxHops / 2;
        const int32 TailCount = MaxHops - HeadCount;

        TStringBuilder<512> Builder;
        for (int32 i = 0; i < HeadCount; ++i)
        {
            if (i > 0)
            {
                Builder << TEXT(" -> ");
            }
            Builder << NameAt(Chain[i]);
        }

        Builder << TEXT(" -> ... (") << (Chain.Num() - MaxHops) << TEXT(" more) -> ");

        for (int32 i = Chain.Num() - TailCount; i < Chain.Num(); ++i)
        {
            if (i > Chain.Num() - TailCount)
            {
                Builder << TEXT(" -> ");
            }
            Builder << NameAt(Chain[i]);
        }

        return Builder.ToString();
    }

    TStringBuilder<512> Builder;
    for (int32 i = 0; i < Chain.Num(); ++i)
    {
        if (i > 0)
        {
            Builder << TEXT(" -> ");
        }
        Builder << NameAt(Chain[i]);
    }

    return Builder.ToString();
}
