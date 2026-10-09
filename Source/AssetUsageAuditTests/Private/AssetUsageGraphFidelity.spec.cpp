// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageGraph.h"

#include "AssetUsageAuditTypes.h"
#include "AssetUsagePaths.h"
#include "LevelUsageResolver.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Does the graph agree with the Asset Registry it was built from?
 *
 * This is the automated form of the plan's "open Reference Viewer on ten assets and compare the
 * edges by eye". Reference Viewer is a drawing of IAssetRegistry::GetDependencies, so comparing
 * against the registry directly checks the same thing, on hundreds of assets instead of ten, and
 * keeps checking it after every future change.
 *
 * What it cannot check: whether the registry itself is right. A reference the registry never
 * recorded - an FMOD event resolved by string, a path built by concatenation - is invisible here
 * exactly as it is invisible in Reference Viewer. That limitation is the Unknown verdict's whole
 * reason for existing, and no test can close it.
 */
BEGIN_DEFINE_SPEC(AssetUsageGraphFidelitySpec,
    "AssetUsageAudit.GraphFidelity",
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
     * Build the graph the way the audit does.
     *
     * Restricted to a subtree so the spec costs a fraction of a second rather than sweeping 80k
     * packages; the invariants under test are per-edge and do not depend on scale.
     */
    static void BuildGraph(FAssetUsageGraph& OutGraph, const TCHAR* Root = TEXT("/Game"))
    {
        FAssetUsageGraphBuildOptions Options;
        Options.IncludePackagePaths.Add(Root);

        OutGraph.Build(ReadyRegistry(), Options);
    }

    /**
     * A stable sample of graph indices.
     *
     * Sampling by raw index is not reproducible: node indices are assigned in Asset Registry
     * enumeration order, which varies between runs even though the graph itself does not. Measured
     * on this project the graph is identical run to run - 84505 packages, 370722 edges - while an
     * index-strided sample compared 3879, 3847 and 3870 targets on three consecutive runs, because
     * index 97 was a different package each time.
     *
     * Sorting by package name first makes the sample the same every run, so a failure here can be
     * reproduced and a pass actually means something.
     */
    static TArray<int32> StableSample(const FAssetUsageGraph& Graph, int32 Stride = 97)
    {
        TArray<FName> Names;
        Names.Reserve(Graph.Num());

        for (int32 Index = 0; Index < Graph.Num(); ++Index)
        {
            Names.Add(Graph.GetNode(Index).PackageName);
        }

        Names.Sort(FNameLexicalLess());

        TArray<int32> Sample;
        Sample.Reserve(Names.Num() / FMath::Max(1, Stride) + 1);

        for (int32 Position = 0; Position < Names.Num(); Position += Stride)
        {
            const int32 Index = Graph.FindPackageIndex(Names[Position]);
            if (Index != INDEX_NONE)
            {
                Sample.Add(Index);
            }
        }

        return Sample;
    }

    /** Raw registry dependencies for a package, using the tool's mandated query. */
    static TSet<FName> RegistryDependencies(FName PackageName)
    {
        TArray<FName> Dependencies;
        ReadyRegistry().GetDependencies(
            PackageName,
            Dependencies,
            AssetUsageAudit::MakeTraversalCategory(),
            AssetUsageAudit::MakeTraversalQuery());

        return TSet<FName>(Dependencies);
    }

END_DEFINE_SPEC(AssetUsageGraphFidelitySpec)

void AssetUsageGraphFidelitySpec::Define()
{
    Describe("Edges against the registry", [this]()
    {
        It("stores every /Game dependency the registry reports", [this]()
        {
            FAssetUsageGraph Graph;
            BuildGraph(Graph);

            if (Graph.Num() == 0)
            {
                AddWarning(TEXT("The graph came back empty; skipping."));
                return;
            }

            // A sample rather than all 84k: this compares against a live registry call per package,
            // and the point is to catch a systematic edge-handling mistake, which shows up on the
            // first dozen or not at all.
            int32 Checked = 0;
            int32 Missing = 0;
            FString FirstMissing;

            for (int32 Index : StableSample(Graph))
            {
                const FAssetUsageNode& Node = Graph.GetNode(Index);
                const TSet<FName> Expected = RegistryDependencies(Node.PackageName);

                TSet<FName> InGraph;
                for (const FAssetUsageEdge& Edge : Graph.GetDependencies(Index))
                {
                    if (Graph.IsValidIndex(Edge.TargetIndex))
                    {
                        InGraph.Add(Graph.GetNode(Edge.TargetIndex).PackageName);
                    }
                }

                for (FName Dependency : Expected)
                {
                    // The graph only holds packages inside the swept roots, so a dependency it
                    // legitimately does not know about is not a defect. Anything under /Game is.
                    if (!Dependency.ToString().StartsWith(TEXT("/Game/")))
                    {
                        continue;
                    }

                    if (!InGraph.Contains(Dependency))
                    {
                        ++Missing;
                        if (FirstMissing.IsEmpty())
                        {
                            FirstMissing = FString::Printf(TEXT("%s -> %s"),
                                *Node.PackageName.ToString(), *Dependency.ToString());
                        }
                    }
                }

                ++Checked;
            }

            AddInfo(FString::Printf(TEXT("Compared %d packages against the registry."), Checked));

            TestTrue(TEXT("something was actually compared"), Checked > 0);
            TestEqual(*FString::Printf(TEXT("no /Game edge is missing (first: %s)"), *FirstMissing), Missing, 0);
        });

        It("invents no edge the registry does not report", [this]()
        {
            // The opposite direction, and the more dangerous one: a fabricated edge would make an
            // unused asset look used, which is the failure mode this tool must never have.
            FAssetUsageGraph Graph;
            BuildGraph(Graph);

            if (Graph.Num() == 0)
            {
                AddWarning(TEXT("The graph came back empty; skipping."));
                return;
            }

            int32 Extra = 0;
            FString FirstExtra;

            for (int32 Index : StableSample(Graph))
            {
                const FAssetUsageNode& Node = Graph.GetNode(Index);
                const TSet<FName> Expected = RegistryDependencies(Node.PackageName);

                for (const FAssetUsageEdge& Edge : Graph.GetDependencies(Index))
                {
                    if (!Graph.IsValidIndex(Edge.TargetIndex))
                    {
                        continue;
                    }

                    const FName Target = Graph.GetNode(Edge.TargetIndex).PackageName;
                    if (!Expected.Contains(Target))
                    {
                        ++Extra;
                        if (FirstExtra.IsEmpty())
                        {
                            FirstExtra = FString::Printf(TEXT("%s -> %s"),
                                *Node.PackageName.ToString(), *Target.ToString());
                        }
                    }
                }
            }

            TestEqual(*FString::Printf(TEXT("no invented edge (first: %s)"), *FirstExtra), Extra, 0);
        });

        It("agrees with the registry on which edges are hard", [this]()
        {
            // Hard versus soft is a report column, and the columns are what someone decides on.
            // A mislabelled edge is worse than a missing one: it reads as information.
            FAssetUsageGraph Graph;
            BuildGraph(Graph);

            if (Graph.Num() == 0)
            {
                AddWarning(TEXT("The graph came back empty; skipping."));
                return;
            }

            IAssetRegistry& Registry = ReadyRegistry();

            // Compare per TARGET, not per edge. The registry may report the same target twice under
            // different property masks - a hard reference and a soft one to the same package - and
            // the graph faithfully keeps both. Asking "is this edge hard" against a set that only
            // answers "is any edge to this target hard" then flags the soft twin as a mismatch.
            //
            // An earlier version of this spec did exactly that and failed intermittently: whether a
            // package with such a pair fell into the sample depended on the sample, so the same code
            // passed or failed run to run. The defect was here, not in the graph.
            int32 Compared = 0;
            int32 Disagreements = 0;
            TArray<FString> Details;

            for (int32 Index : StableSample(Graph))
            {
                const FAssetUsageNode& Node = Graph.GetNode(Index);

                TArray<FName> HardOnly;
                Registry.GetDependencies(
                    Node.PackageName,
                    HardOnly,
                    AssetUsageAudit::MakeTraversalCategory(),
                    UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard));

                const TSet<FName> RegistryHardTargets(HardOnly);

                // Targets the graph considers hard by at least one edge - the same question the
                // registry's hard-only query answers.
                TSet<FName> GraphHardTargets;
                TSet<FName> GraphAllTargets;

                for (const FAssetUsageEdge& Edge : Graph.GetDependencies(Index))
                {
                    if (!Graph.IsValidIndex(Edge.TargetIndex))
                    {
                        continue;
                    }

                    const FName Target = Graph.GetNode(Edge.TargetIndex).PackageName;
                    GraphAllTargets.Add(Target);

                    if (Edge.IsHard())
                    {
                        GraphHardTargets.Add(Target);
                    }
                }

                for (FName Target : GraphAllTargets)
                {
                    const bool bGraphSaysHard = GraphHardTargets.Contains(Target);
                    const bool bRegistrySaysHard = RegistryHardTargets.Contains(Target);

                    if (bGraphSaysHard != bRegistrySaysHard)
                    {
                        ++Disagreements;

                        if (Details.Num() < 5)
                        {
                            Details.Add(FString::Printf(
                                TEXT("%s -> %s: graph says %s, registry says %s"),
                                *Node.PackageName.ToString(),
                                *Target.ToString(),
                                bGraphSaysHard ? TEXT("hard") : TEXT("not hard"),
                                bRegistrySaysHard ? TEXT("hard") : TEXT("not hard")));
                        }
                    }

                    ++Compared;
                }
            }

            AddInfo(FString::Printf(TEXT("Compared the hard flag on %d dependency targets."), Compared));

            for (const FString& Detail : Details)
            {
                AddInfo(Detail);
            }

            TestTrue(TEXT("something was actually compared"), Compared > 0);
            TestEqual(TEXT("hard flags match the registry"), Disagreements, 0);
        });
    });

    Describe("The OFPA invariant, stated as a measurement", [this]()
    {
        It("finds external actor packages that a hard-only query would lose", [this]()
        {
            // The regression this guards is silent: a Hard-only query still returns a plausible
            // graph, just without any One File Per Actor package in it. Rather than trusting the
            // comment, measure the difference the query makes.
            IAssetRegistry& Registry = ReadyRegistry();

            FARFilter Filter;
            Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
            Filter.PackagePaths.Add(FName(TEXT("/Game")));
            Filter.bRecursivePaths = true;
            Filter.bIncludeOnlyOnDiskAssets = true;

            FName LevelWithExternals = NAME_None;
            int32 ExternalsUnderCorrectQuery = 0;

            Registry.EnumerateAssets(Filter, [&](const FAssetData& AssetData)
            {
                TArray<FName> All;
                Registry.GetDependencies(
                    AssetData.PackageName,
                    All,
                    AssetUsageAudit::MakeTraversalCategory(),
                    AssetUsageAudit::MakeTraversalQuery());

                int32 Externals = 0;
                for (FName Dependency : All)
                {
                    if (AssetUsagePaths::IsExternalPackage(Dependency))
                    {
                        ++Externals;
                    }
                }

                if (Externals > 0)
                {
                    LevelWithExternals = AssetData.PackageName;
                    ExternalsUnderCorrectQuery = Externals;
                    return false;
                }

                return true;
            });

            if (LevelWithExternals.IsNone())
            {
                AddWarning(TEXT("No level with external actor packages was found; skipping."));
                return;
            }

            TArray<FName> HardOnly;
            Registry.GetDependencies(
                LevelWithExternals,
                HardOnly,
                AssetUsageAudit::MakeTraversalCategory(),
                UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard));

            int32 ExternalsUnderHardQuery = 0;
            for (FName Dependency : HardOnly)
            {
                if (AssetUsagePaths::IsExternalPackage(Dependency))
                {
                    ++ExternalsUnderHardQuery;
                }
            }

            AddInfo(FString::Printf(
                TEXT("'%s': %d external packages with the tool's query, %d with a Hard-only query."),
                *LevelWithExternals.ToString(), ExternalsUnderCorrectQuery, ExternalsUnderHardQuery));

            TestTrue(TEXT("the tool's query finds external packages"), ExternalsUnderCorrectQuery > 0);

            // The engine emits these edges as Game|Build without Hard. If this ever becomes
            // non-zero the engine has changed and the surrounding comments need revisiting.
            TestEqual(TEXT("a Hard-only query finds none of them"), ExternalsUnderHardQuery, 0);
        });
    });

    Describe("Sublevels and their parent map", [this]()
    {
        It("attributes a sublevel's assets to the parent map as well", [this]()
        {
            // Plan item 4. A streaming sublevel is reached from the parent's own package, which is
            // a structural crossing, so the parent's sweep should see through it. Asserted rather
            // than assumed: the map-boundary rule refuses most crossings, and getting it slightly
            // wrong would silently drop every sublevel's contents from the parent.
            IAssetRegistry& Registry = ReadyRegistry();
            const FTopLevelAssetPath WorldClass = UWorld::StaticClass()->GetClassPathName();

            FARFilter Filter;
            Filter.ClassPaths.Add(WorldClass);
            Filter.PackagePaths.Add(FName(TEXT("/Game")));
            Filter.bRecursivePaths = true;
            Filter.bIncludeOnlyOnDiskAssets = true;

            FName Parent = NAME_None;
            FName Sublevel = NAME_None;

            Registry.EnumerateAssets(Filter, [&](const FAssetData& AssetData)
            {
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
                        if (Inner.AssetClassPath == WorldClass && Dependency != AssetData.PackageName)
                        {
                            Parent = AssetData.PackageName;
                            Sublevel = Dependency;
                            return false;
                        }
                    }
                }

                return true;
            });

            if (Parent.IsNone())
            {
                AddWarning(TEXT("No map referencing another map directly was found; skipping."));
                return;
            }

            AddInfo(FString::Printf(TEXT("Parent '%s' references '%s'."), *Parent.ToString(), *Sublevel.ToString()));

            FAssetUsageGraph Graph;
            BuildGraph(Graph);

            const int32 SublevelIndex = Graph.FindPackageIndex(Sublevel);
            if (SublevelIndex == INDEX_NONE)
            {
                AddWarning(TEXT("The sublevel is outside the built graph; skipping."));
                return;
            }

            FLevelUsageResolver Resolver(Graph, Registry);

            FLevelUsageResolveOptions Options;
            Options.LevelPackages = { Parent };
            Options.bRecordRoutes = false;

            const FLevelUsageResult Result = Resolver.Resolve(Options);

            TestTrue(TEXT("the parent's sweep reaches the sublevel"), Result.IsReachableFromAnyLevel(SublevelIndex));

            // Reaching the sublevel package is necessary but not sufficient - a foreign map is also
            // "reached", just not expanded. What matters is that its contents came too.
            TArray<FName> SublevelDependencies;
            Registry.GetDependencies(
                Sublevel,
                SublevelDependencies,
                AssetUsageAudit::MakeTraversalCategory(),
                AssetUsageAudit::MakeTraversalQuery());

            int32 ContentsChecked = 0;
            int32 ContentsReached = 0;

            for (FName Dependency : SublevelDependencies)
            {
                const int32 Index = Graph.FindPackageIndex(Dependency);
                if (Index == INDEX_NONE)
                {
                    continue;
                }

                ++ContentsChecked;
                if (Result.IsReachableFromAnyLevel(Index))
                {
                    ++ContentsReached;
                }
            }

            if (ContentsChecked == 0)
            {
                AddWarning(TEXT("The sublevel has no dependencies inside the graph; nothing to check."));
                return;
            }

            AddInfo(FString::Printf(TEXT("%d of %d sublevel dependencies reached from the parent."),
                ContentsReached, ContentsChecked));

            TestEqual(TEXT("the parent sees the sublevel's contents"), ContentsReached, ContentsChecked);
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
