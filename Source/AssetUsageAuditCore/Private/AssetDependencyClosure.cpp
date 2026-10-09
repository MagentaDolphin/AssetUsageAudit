// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetDependencyClosure.h"

#include "AssetUsageAuditCoreModule.h"
#include "AssetUsageAuditTypes.h"
#include "AssetUsagePaths.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"

namespace AssetDependencyClosure
{
    FString FStats::Summarise() const
    {
        TArray<FString> Parts;

        Parts.Add(FString::Printf(TEXT("%d seed(s) -> %d package(s)"), SeedCount, TotalCount));

        if (SkippedScript > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d script"), SkippedScript));
        }

        if (SkippedEngine > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d engine/temp"), SkippedEngine));
        }

        if (SkippedExcluded > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d excluded by path"), SkippedExcluded));
        }

        if (ForeignLevelsSkipped > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d other map(s) not followed"), ForeignLevelsSkipped));
        }

        Parts.Add(FString::Printf(TEXT("deepest hop %d"), DeepestHop));

        if (bStoppedEarly)
        {
            Parts.Add(TEXT("stopped early"));
        }

        return FString::Join(Parts, TEXT(", "));
    }

    TArray<FSeedClosure> GatherPerSeed(IAssetRegistry& Registry, const TArray<FName>& Seeds, const FOptions& Options, FStats& OutStats)
    {
        OutStats = FStats();

        TArray<FSeedClosure> Result;
        Result.Reserve(Seeds.Num());

        // Deduplicate seeds first: the same asset ticked twice would otherwise produce two identical
        // folders, and the second would be silently renamed by the collision policy.
        TSet<FName> SeenSeeds;
        SeenSeeds.Reserve(Seeds.Num());

        for (int32 Index = 0; Index < Seeds.Num(); ++Index)
        {
            const FName Seed = Seeds[Index];

            if (Seed.IsNone() || SeenSeeds.Contains(Seed))
            {
                continue;
            }

            SeenSeeds.Add(Seed);

            // Progress is reported per seed here, not per package: a caller showing a bar wants it
            // to advance once per ticked asset, and the inner traversals have wildly uneven sizes.
            if (Options.OnProgress && !Options.OnProgress(Index, Seeds.Num()))
            {
                OutStats.bStoppedEarly = true;
                break;
            }

            // Each seed gets its own traversal and its own visited set. That is the whole point:
            // sharing one set would give the first seed every shared dependency and leave the rest
            // with folders that are missing their textures.
            FOptions SeedOptions = Options;
            SeedOptions.OnProgress = nullptr;

            FStats SeedStats;
            FSeedClosure& Closure = Result.AddDefaulted_GetRef();
            Closure.Seed = Seed;
            Closure.Packages = Gather(Registry, { Seed }, SeedOptions, SeedStats);

            OutStats.SkippedScript += SeedStats.SkippedScript;
            OutStats.SkippedEngine += SeedStats.SkippedEngine;
            OutStats.SkippedExcluded += SeedStats.SkippedExcluded;
            OutStats.ForeignLevelsSkipped += SeedStats.ForeignLevelsSkipped;
            OutStats.DeepestHop = FMath::Max(OutStats.DeepestHop, SeedStats.DeepestHop);
            OutStats.TotalCount += SeedStats.TotalCount;
            ++OutStats.SeedCount;
        }

        UE_LOG(LogAssetUsageAudit, Log,
            TEXT("Per-seed closure: %d seed(s), %d package placement(s) counting duplicates."),
            OutStats.SeedCount, OutStats.TotalCount);

        return Result;
    }

    TArray<FName> Gather(IAssetRegistry& Registry, const TArray<FName>& Seeds, const FOptions& Options, FStats& OutStats)
    {
        OutStats = FStats();

        TArray<FName> Result;
        TSet<FName> Visited;

        // Hop count per queued entry, kept alongside the queue rather than in a map: the queue is
        // the only place an index is needed and a parallel array is one allocation instead of N.
        TArray<int32> Hops;

        Result.Reserve(Seeds.Num() * 4);
        Visited.Reserve(Seeds.Num() * 4);

        const auto ShouldKeep = [&Options, &OutStats](FName PackageName) -> bool
        {
            if (AssetUsagePaths::IsScriptPackage(PackageName))
            {
                ++OutStats.SkippedScript;
                return false;
            }

            const FString PackageStr = PackageName.ToString();

            if (!Options.bIncludeEnginePackages && AssetUsagePaths::IsEngineOrTempPackage(PackageStr))
            {
                ++OutStats.SkippedEngine;
                return false;
            }

            if (AssetUsagePaths::IsPathExcluded(PackageName, Options.ExcludePackagePaths))
            {
                ++OutStats.SkippedExcluded;
                return false;
            }

            return true;
        };

        // Which packages are maps, answered once each. The registry call allocates an FAssetData
        // array, and the same package is reached from many referencers, so the cache turns tens of
        // thousands of lookups into one per distinct package.
        TMap<FName, bool> LevelCache;

        const auto IsLevelPackage = [&Registry, &LevelCache](FName PackageName) -> bool
        {
            if (const bool* Cached = LevelCache.Find(PackageName))
            {
                return *Cached;
            }

            TArray<FAssetData> Assets;
            Registry.GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyOnDiskAssets=*/true);

            bool bIsLevel = false;
            for (const FAssetData& AssetData : Assets)
            {
                if (AssetData.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
                {
                    bIsLevel = true;
                    break;
                }
            }

            LevelCache.Add(PackageName, bIsLevel);
            return bIsLevel;
        };

        // Seeds are admitted without the path filters. Someone who ticked a row is entitled to the
        // file for that row even if it sits under an excluded folder; the filters bound what the
        // traversal drags in behind it, which is a different question.
        //
        // A seed that is itself a map is a level export: the user asked for that location, so its
        // contents are the point. Foreign maps found later are a different matter entirely.
        // Levels the closure has admitted, seeds first. A level may only be crossed into from
        // another level or from an external actor package, so this set has to grow as structural
        // crossings are accepted - otherwise a sublevel of a sublevel would be refused.
        TSet<FName> LevelsInResult;

        for (FName Seed : Seeds)
        {
            if (Seed.IsNone() || Visited.Contains(Seed))
            {
                continue;
            }

            if (!FPackageName::IsValidLongPackageName(Seed.ToString()))
            {
                continue;
            }

            Visited.Add(Seed);
            Result.Add(Seed);
            Hops.Add(0);
            ++OutStats.SeedCount;

            if (IsLevelPackage(Seed))
            {
                LevelsInResult.Add(Seed);
            }
        }

        TArray<FName> ScratchDependencies;

        for (int32 Head = 0; Head < Result.Num(); ++Head)
        {
            if (Options.OnProgress && !Options.OnProgress(Head, Result.Num()))
            {
                OutStats.bStoppedEarly = true;
                break;
            }

            const int32 CurrentHop = Hops[Head];

            if (Options.MaxDepth > 0 && CurrentHop >= Options.MaxDepth)
            {
                continue;
            }

            ScratchDependencies.Reset();

            // Package category, no requirements. Never Hard: in EDependencyQuery Soft is defined as
            // NotHard, so Hard|Soft matches nothing, and a Hard-only query drops every soft edge -
            // including every TSoftObjectPtr a Blueprint resolves at runtime and every OFPA package.
            Registry.GetDependencies(
                Result[Head],
                ScratchDependencies,
                AssetUsageAudit::MakeTraversalCategory(),
                AssetUsageAudit::MakeTraversalQuery());

            for (FName Dependency : ScratchDependencies)
            {
                if (Dependency.IsNone() || Visited.Contains(Dependency))
                {
                    continue;
                }

                // Marked visited even when dropped, so a package excluded by path is not re-tested
                // once per referencer. On a mesh library that is thousands of redundant string
                // comparisons.
                Visited.Add(Dependency);

                // The map boundary. Reaching another World is only legitimate when the reference
                // comes from a level or from one of its external actor packages - the shape a
                // streaming sublevel or a Level Instance actually has. A World named by an ordinary
                // asset is a different location that happens to be mentioned, and following it
                // exports someone else's map: measured on WP_Example, the chain
                // BP_GameMode -> PDA_MenuConfig -> L_Other accounts for 9994
                // packages. FLevelUsageResolver applies the same rule; the two must not disagree
                // about the same graph.
                if (IsLevelPackage(Dependency))
                {
                    const FName Source = Result[Head];
                    const bool bStructuralCrossing =
                        LevelsInResult.Contains(Source) || AssetUsagePaths::IsExternalPackage(Source);

                    if (!bStructuralCrossing)
                    {
                        ++OutStats.ForeignLevelsSkipped;

                        UE_LOG(LogAssetUsageAudit, Verbose,
                            TEXT("Not following '%s': reached from '%s', which is not a level or an external actor package."),
                            *Dependency.ToString(), *Source.ToString());

                        continue;
                    }

                    LevelsInResult.Add(Dependency);
                }

                if (!ShouldKeep(Dependency))
                {
                    continue;
                }

                Result.Add(Dependency);
                Hops.Add(CurrentHop + 1);

                OutStats.DeepestHop = FMath::Max(OutStats.DeepestHop, CurrentHop + 1);
            }
        }

        OutStats.TotalCount = Result.Num();

        UE_LOG(LogAssetUsageAudit, Log, TEXT("Dependency closure: %s"), *OutStats.Summarise());

        return Result;
    }
}
