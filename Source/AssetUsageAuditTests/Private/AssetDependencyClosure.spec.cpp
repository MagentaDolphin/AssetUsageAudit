// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetDependencyClosure.h"

#include "AssetUsageAuditTypes.h"
#include "AssetUsagePaths.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The dependency closure behind "export a mesh and get its materials too".
 *
 * Runs against the real registry, because the thing worth testing is whether the traversal finds
 * the edges the engine actually stores - a mock would only prove that a BFS visits nodes.
 */
BEGIN_DEFINE_SPEC(AssetDependencyClosureSpec,
    "AssetUsageAudit.DependencyClosure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    static IAssetRegistry& ReadyRegistry()
    {
        IAssetRegistry& Registry = IAssetRegistry::GetChecked();
        if (Registry.IsLoadingAssets())
        {
            Registry.WaitForCompletion();
        }
        return Registry;
    }

    /**
     * A /Game static mesh that actually references something, so the closure has work to do.
     *
     * Picking the first mesh found is not enough: a mesh with no material assigned would make the
     * "brings materials along" test pass vacuously.
     */
    static FName FindMeshWithDependencies(int32& OutDirectCount)
    {
        FARFilter Filter;
        Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.bIncludeOnlyOnDiskAssets = true;

        IAssetRegistry& Registry = ReadyRegistry();

        FName Best = NAME_None;
        int32 BestCount = 0;

        Registry.EnumerateAssets(Filter, [&Registry, &Best, &BestCount](const FAssetData& AssetData)
        {
            TArray<FName> Dependencies;
            Registry.GetDependencies(
                AssetData.PackageName,
                Dependencies,
                AssetUsageAudit::MakeTraversalCategory(),
                AssetUsageAudit::MakeTraversalQuery());

            int32 GameDependencies = 0;
            for (FName Dependency : Dependencies)
            {
                if (Dependency.ToString().StartsWith(TEXT("/Game/")))
                {
                    ++GameDependencies;
                }
            }

            if (GameDependencies > BestCount)
            {
                BestCount = GameDependencies;
                Best = AssetData.PackageName;
            }

            // Two /Game dependencies is enough to be a meaningful subject; stop hunting.
            return BestCount < 2;
        });

        OutDirectCount = BestCount;
        return Best;
    }

END_DEFINE_SPEC(AssetDependencyClosureSpec)

void AssetDependencyClosureSpec::Define()
{
    Describe("Degenerate input", [this]()
    {
        It("returns nothing for no seeds", [this]()
        {
            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(ReadyRegistry(), {}, {}, Stats);

            TestEqual(TEXT("empty"), Result.Num(), 0);
            TestEqual(TEXT("no seeds counted"), Stats.SeedCount, 0);
        });

        It("drops a malformed seed instead of passing it on", [this]()
        {
            // A name that is not a long package name has no file and no dependencies. Keeping it
            // would push the failure down into the exporter, which would report it as missing.
            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(
                ReadyRegistry(), { FName(TEXT("NotAPackage")), FName() }, {}, Stats);

            TestEqual(TEXT("nothing kept"), Result.Num(), 0);
        });

        It("keeps a seed that has no dependencies at all", [this]()
        {
            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(
                ReadyRegistry(), { FName(TEXT("/Game/NoSuchAsset_AssetUsageAuditTest")) }, {}, Stats);

            TestEqual(TEXT("the seed survives"), Result.Num(), 1);
            TestEqual(TEXT("counted as a seed"), Stats.SeedCount, 1);
        });

        It("de-duplicates a seed listed twice", [this]()
        {
            const FName Same(TEXT("/Game/NoSuchAsset_AssetUsageAuditTest"));

            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(ReadyRegistry(), { Same, Same, Same }, {}, Stats);

            TestEqual(TEXT("once"), Result.Num(), 1);
        });
    });

    Describe("Walking real dependencies", [this]()
    {
        It("brings a mesh's materials and textures along", [this]()
        {
            int32 DirectCount = 0;
            const FName Mesh = FindMeshWithDependencies(DirectCount);

            if (Mesh.IsNone())
            {
                AddWarning(TEXT("No /Game static mesh with dependencies was found; skipping."));
                return;
            }

            AddInfo(FString::Printf(TEXT("Using '%s' with %d direct /Game dependencies."), *Mesh.ToString(), DirectCount));

            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(ReadyRegistry(), { Mesh }, {}, Stats);

            // This is the whole point of the feature: exporting the seed alone produces a file that
            // opens with no material.
            TestTrue(TEXT("more than the seed came back"), Result.Num() > 1);
            TestEqual(TEXT("the seed is first"), Result[0], Mesh);
            TestEqual(TEXT("stats agree with the array"), Stats.TotalCount, Result.Num());
            TestTrue(TEXT("walked at least one hop"), Stats.DeepestHop >= 1);
        });

        It("leaves engine content out by default", [this]()
        {
            int32 DirectCount = 0;
            const FName Mesh = FindMeshWithDependencies(DirectCount);

            if (Mesh.IsNone())
            {
                AddWarning(TEXT("No /Game static mesh with dependencies was found; skipping."));
                return;
            }

            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(ReadyRegistry(), { Mesh }, {}, Stats);

            // The destination project ships /Engine itself; copying it there is at best redundant
            // and at worst overwrites engine files.
            for (FName Package : Result)
            {
                const FString AsString = Package.ToString();
                TestFalse(*FString::Printf(TEXT("no engine package: %s"), *AsString), AsString.StartsWith(TEXT("/Engine/")));
                TestFalse(*FString::Printf(TEXT("no script package: %s"), *AsString), AsString.StartsWith(TEXT("/Script/")));
            }
        });

        It("respects the exclusion list for dependencies but not for seeds", [this]()
        {
            int32 DirectCount = 0;
            const FName Mesh = FindMeshWithDependencies(DirectCount);

            if (Mesh.IsNone())
            {
                AddWarning(TEXT("No /Game static mesh with dependencies was found; skipping."));
                return;
            }

            // Excluding all of /Game must still return the ticked asset: the user asked for that
            // file by name. It is the traversal behind it that the exclusion list bounds.
            AssetDependencyClosure::FOptions Options;
            Options.ExcludePackagePaths.Add(TEXT("/Game"));

            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(ReadyRegistry(), { Mesh }, Options, Stats);

            TestEqual(TEXT("only the seed survives"), Result.Num(), 1);
            TestEqual(TEXT("and it is the seed"), Result[0], Mesh);
            TestTrue(TEXT("exclusions were counted"), Stats.SkippedExcluded > 0);
        });

        It("stops at the requested depth", [this]()
        {
            int32 DirectCount = 0;
            const FName Mesh = FindMeshWithDependencies(DirectCount);

            if (Mesh.IsNone())
            {
                AddWarning(TEXT("No /Game static mesh with dependencies was found; skipping."));
                return;
            }

            AssetDependencyClosure::FOptions Options;
            Options.MaxDepth = 1;

            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Limited = AssetDependencyClosure::Gather(ReadyRegistry(), { Mesh }, Options, Stats);

            TestTrue(TEXT("never went past one hop"), Stats.DeepestHop <= 1);

            AssetDependencyClosure::FStats FullStats;
            const TArray<FName> Full = AssetDependencyClosure::Gather(ReadyRegistry(), { Mesh }, {}, FullStats);

            TestTrue(TEXT("the limit removed something or the graph was one hop deep"),
                Limited.Num() <= Full.Num());
        });

        It("does not follow a map that an ordinary asset merely mentions", [this]()
        {
            // The defect this guards against: exporting WP_Example pulled in the whole of L_Other
            // through BP_GameMode -> PDA_MenuConfig, 9994 packages belonging to a
            // different location. FLevelUsageResolver already refused that crossing; the closure
            // did not, so the two disagreed about the same graph.
            IAssetRegistry& Registry = ReadyRegistry();

            FARFilter Filter;
            Filter.PackagePaths.Add(FName(TEXT("/Game")));
            Filter.bRecursivePaths = true;
            Filter.bIncludeOnlyOnDiskAssets = true;

            const FTopLevelAssetPath WorldClass = UWorld::StaticClass()->GetClassPathName();

            FName Culprit = NAME_None;
            FName ReferencedMap = NAME_None;

            Registry.EnumerateAssets(Filter, [&](const FAssetData& AssetData)
            {
                // The source must not itself be a level or an external actor package, or the
                // crossing would be structural and legitimately followed.
                if (AssetData.AssetClassPath == WorldClass
                    || AssetUsagePaths::IsExternalPackage(AssetData.PackageName))
                {
                    return true;
                }

                TArray<FName> Dependencies;
                Registry.GetDependencies(
                    AssetData.PackageName,
                    Dependencies,
                    AssetUsageAudit::MakeTraversalCategory(),
                    AssetUsageAudit::MakeTraversalQuery());

                for (FName Dependency : Dependencies)
                {
                    TArray<FAssetData> InPackage;
                    Registry.GetAssetsByPackageName(Dependency, InPackage, true);

                    for (const FAssetData& Inner : InPackage)
                    {
                        if (Inner.AssetClassPath == WorldClass)
                        {
                            Culprit = AssetData.PackageName;
                            ReferencedMap = Dependency;
                            return false;
                        }
                    }
                }

                return true;
            });

            if (Culprit.IsNone())
            {
                AddWarning(TEXT("No non-level asset referencing a map was found; skipping."));
                return;
            }

            AddInfo(FString::Printf(TEXT("'%s' references the map '%s'."), *Culprit.ToString(), *ReferencedMap.ToString()));

            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(ReadyRegistry(), { Culprit }, {}, Stats);

            TestFalse(TEXT("the foreign map is not in the closure"), Result.Contains(ReferencedMap));
            TestTrue(TEXT("and the refusal was counted"), Stats.ForeignLevelsSkipped > 0);
        });

        It("still expands a level that was ticked directly", [this]()
        {
            // The rule bounds foreign maps, not the seed. Someone who ticks a location is asking
            // for its contents - that is the task this tool exists for.
            IAssetRegistry& Registry = ReadyRegistry();

            FARFilter Filter;
            Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
            Filter.PackagePaths.Add(FName(TEXT("/Game")));
            Filter.bRecursivePaths = true;
            Filter.bIncludeOnlyOnDiskAssets = true;

            FName Level = NAME_None;

            Registry.EnumerateAssets(Filter, [&Registry, &Level](const FAssetData& AssetData)
            {
                TArray<FName> Dependencies;
                Registry.GetDependencies(
                    AssetData.PackageName,
                    Dependencies,
                    AssetUsageAudit::MakeTraversalCategory(),
                    AssetUsageAudit::MakeTraversalQuery());

                for (FName Dependency : Dependencies)
                {
                    if (Dependency.ToString().StartsWith(TEXT("/Game/")))
                    {
                        Level = AssetData.PackageName;
                        return false;
                    }
                }

                return true;
            });

            if (Level.IsNone())
            {
                AddWarning(TEXT("No /Game level with dependencies was found; skipping."));
                return;
            }

            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(ReadyRegistry(), { Level }, {}, Stats);

            TestTrue(TEXT("the level brought its content"), Result.Num() > 1);
            TestEqual(TEXT("the level is the seed"), Result[0], Level);
        });

        It("stops when the progress callback asks it to", [this]()
        {
            int32 DirectCount = 0;
            const FName Mesh = FindMeshWithDependencies(DirectCount);

            if (Mesh.IsNone())
            {
                AddWarning(TEXT("No /Game static mesh with dependencies was found; skipping."));
                return;
            }

            AssetDependencyClosure::FOptions Options;
            Options.OnProgress = [](int32, int32) { return false; };

            AssetDependencyClosure::FStats Stats;
            const TArray<FName> Result = AssetDependencyClosure::Gather(ReadyRegistry(), { Mesh }, Options, Stats);

            TestTrue(TEXT("flagged as stopped"), Stats.bStoppedEarly);

            // A cancelled walk still returns the seeds, so a caller that ignores the flag exports
            // what was ticked rather than nothing at all.
            TestEqual(TEXT("seeds survive cancellation"), Result.Num(), 1);
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
