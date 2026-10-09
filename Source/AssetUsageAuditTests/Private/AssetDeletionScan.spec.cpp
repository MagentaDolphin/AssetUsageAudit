// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetDeletionScan.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetUsageAuditTypes.h"
#include "AssetUsagePaths.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The refusals, and the referencer arithmetic behind the warning.
 *
 * This is the only part of the tool that leads to something being destroyed, so the tests are
 * written against the two claims the delete window makes to the user: "these will not be touched",
 * and "this many things still point at that". Both are decided here; the window only renders them.
 *
 * Nothing in this spec deletes anything. AssetDeletionScan cannot - it holds no delete code at all,
 * which is exactly why the analysis was split out of the Editor module.
 */
BEGIN_DEFINE_SPEC(AssetDeletionScanSpec,
    "AssetUsageAudit.DeletionScan",
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

    /** First map in /Game, chosen deterministically so a failure is reproducible. */
    static FName FindLevelPackage()
    {
        IAssetRegistry& Registry = ReadyRegistry();

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;
        Filter.bIncludeOnlyOnDiskAssets = true;
        Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());

        TArray<FName> Levels;
        Registry.EnumerateAssets(Filter, [&Levels](const FAssetData& AssetData)
        {
            Levels.Add(AssetData.PackageName);
            return Levels.Num() < 512;
        });

        if (Levels.IsEmpty())
        {
            return NAME_None;
        }

        Levels.Sort(FNameLexicalLess());
        return Levels[0];
    }

    /** First One File Per Actor package, if this project has any. */
    static FName FindExternalPackage()
    {
        IAssetRegistry& Registry = ReadyRegistry();

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game/__ExternalActors__")));
        Filter.bRecursivePaths = true;
        Filter.bIncludeOnlyOnDiskAssets = true;

        TArray<FName> Found;
        Registry.EnumerateAssets(Filter, [&Found](const FAssetData& AssetData)
        {
            Found.Add(AssetData.PackageName);
            return false;
        });

        return Found.IsEmpty() ? NAME_None : Found[0];
    }

    /**
     * An ordinary asset together with one package that references it.
     *
     * Both halves are needed to prove the delete-set exclusion: the referencer has to be a real one
     * for "referenced from outside" and "referenced from inside the set" to be the same edge seen
     * twice, which is the whole point of the check.
     */
    static FName FindReferencedAsset(FName& OutReferencer)
    {
        IAssetRegistry& Registry = ReadyRegistry();

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;
        Filter.bIncludeOnlyOnDiskAssets = true;

        TArray<FName> Candidates;
        Registry.EnumerateAssets(Filter, [&Candidates](const FAssetData& AssetData)
        {
            if (AssetData.IsRedirector() || AssetData.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
            {
                return true;
            }

            if (AssetUsagePaths::IsExternalPackage(AssetData.PackageName))
            {
                return true;
            }

            Candidates.Add(AssetData.PackageName);
            return Candidates.Num() < 4096;
        });

        Candidates.Sort(FNameLexicalLess());

        TArray<FName> Referencers;

        for (FName Candidate : Candidates)
        {
            Referencers.Reset();
            Registry.GetReferencers(Candidate, Referencers,
                AssetUsageAudit::MakeTraversalCategory(), AssetUsageAudit::MakeTraversalQuery());

            for (FName Referencer : Referencers)
            {
                if (Referencer != Candidate)
                {
                    OutReferencer = Referencer;
                    return Candidate;
                }
            }
        }

        return NAME_None;
    }

END_DEFINE_SPEC(AssetDeletionScanSpec)

void AssetDeletionScanSpec::Define()
{
    Describe("Refusing what must not be deleted", [this]()
    {
        It("refuses a map", [this]()
        {
            const FName Level = FindLevelPackage();

            if (Level.IsNone())
            {
                AddWarning(TEXT("This project has no maps; skipping."));
                return;
            }

            AssetDeletionScan::FStats Stats;
            const TArray<AssetDeletionScan::FCandidate> Candidates =
                AssetDeletionScan::Scan(ReadyRegistry(), { Level }, AssetDeletionScan::FOptions(), Stats);

            if (Candidates.Num() != 1)
            {
                AddError(TEXT("expected exactly one candidate"));
                return;
            }

            // The user's rule, and the one refusal that is policy rather than capability: the engine
            // would happily delete an unopened map.
            TestEqual(TEXT("refused as a level"),
                Candidates[0].Refusal, AssetDeletionScan::ERefusal::IsLevel);
            TestFalse(TEXT("not deletable"), Candidates[0].CanDelete());
            TestEqual(TEXT("counted as a refused level"), Stats.RefusedLevels, 1);
            TestEqual(TEXT("nothing deletable"), Stats.Deletable, 0);
        });

        It("refuses a One File Per Actor package", [this]()
        {
            const FName External = FindExternalPackage();

            if (External.IsNone())
            {
                AddWarning(TEXT("This project has no external actor packages; skipping."));
                return;
            }

            AssetDeletionScan::FStats Stats;
            const TArray<AssetDeletionScan::FCandidate> Candidates =
                AssetDeletionScan::Scan(ReadyRegistry(), { External }, AssetDeletionScan::FOptions(), Stats);

            if (Candidates.Num() != 1)
            {
                AddError(TEXT("expected exactly one candidate"));
                return;
            }

            // Deleting one of these is removing a placed actor from a map behind the level editor's
            // back - no undo, and the map keeps a reference to a package that no longer exists.
            TestEqual(TEXT("refused as external"),
                Candidates[0].Refusal, AssetDeletionScan::ERefusal::IsExternalPackage);
            TestEqual(TEXT("counted"), Stats.RefusedExternal, 1);
        });

        It("refuses engine content", [this]()
        {
            AssetDeletionScan::FStats Stats;
            const TArray<AssetDeletionScan::FCandidate> Candidates = AssetDeletionScan::Scan(
                ReadyRegistry(),
                { FName(TEXT("/Engine/BasicShapes/Cube")) },
                AssetDeletionScan::FOptions(),
                Stats);

            if (Candidates.Num() != 1)
            {
                AddError(TEXT("expected exactly one candidate"));
                return;
            }

            TestEqual(TEXT("refused as engine content"),
                Candidates[0].Refusal, AssetDeletionScan::ERefusal::NotProjectContent);
            TestEqual(TEXT("counted"), Stats.RefusedNotProjectContent, 1);
        });

        It("refuses a package the registry does not know", [this]()
        {
            AssetDeletionScan::FStats Stats;
            const TArray<AssetDeletionScan::FCandidate> Candidates = AssetDeletionScan::Scan(
                ReadyRegistry(),
                { FName(TEXT("/Game/AssetUsageAuditNoSuchPackage")) },
                AssetDeletionScan::FOptions(),
                Stats);

            if (Candidates.Num() != 1)
            {
                AddError(TEXT("expected exactly one candidate"));
                return;
            }

            TestEqual(TEXT("reported as missing"),
                Candidates[0].Refusal, AssetDeletionScan::ERefusal::Missing);
            TestEqual(TEXT("counted"), Stats.RefusedMissing, 1);
        });

        It("keeps refused rows in the result rather than dropping them", [this]()
        {
            const FName Level = FindLevelPackage();

            if (Level.IsNone())
            {
                AddWarning(TEXT("This project has no maps; skipping."));
                return;
            }

            const FName Missing(TEXT("/Game/AssetUsageAuditNoSuchPackage"));
            const FName Engine(TEXT("/Engine/BasicShapes/Cube"));

            AssetDeletionScan::FStats Stats;
            const TArray<AssetDeletionScan::FCandidate> Candidates = AssetDeletionScan::Scan(
                ReadyRegistry(), { Level, Missing, Engine }, AssetDeletionScan::FOptions(), Stats);

            // A refusal the user cannot see is indistinguishable from the tool losing their
            // selection, so every ticked package must come back with a reason attached.
            TestEqual(TEXT("all three returned"), Candidates.Num(), 3);
            TestEqual(TEXT("in the original order"), Candidates[0].PackageName, Level);
            TestEqual(TEXT("in the original order"), Candidates[1].PackageName, Missing);
            TestEqual(TEXT("in the original order"), Candidates[2].PackageName, Engine);

            TestEqual(TEXT("requested count matches"), Stats.Requested, 3);
            TestEqual(TEXT("none deletable"), Stats.Deletable, 0);
        });
    });

    Describe("Counting what still points at the asset", [this]()
    {
        It("lists a referencer that is not itself being deleted", [this]()
        {
            FName Referencer = NAME_None;
            const FName Asset = FindReferencedAsset(Referencer);

            if (Asset.IsNone())
            {
                AddWarning(TEXT("No referenced asset was found; skipping."));
                return;
            }

            AssetDeletionScan::FStats Stats;
            const TArray<AssetDeletionScan::FCandidate> Candidates =
                AssetDeletionScan::Scan(ReadyRegistry(), { Asset }, AssetDeletionScan::FOptions(), Stats);

            if (Candidates.Num() != 1 || !Candidates[0].CanDelete())
            {
                AddError(TEXT("expected one deletable candidate"));
                return;
            }

            TestTrue(TEXT("at least one outside referencer"), Candidates[0].OutsideReferencerCount > 0);
            TestTrue(TEXT("the known referencer is listed"),
                Candidates[0].OutsideReferencers.Contains(Referencer)
                || Candidates[0].OutsideReferencerCount > Candidates[0].OutsideReferencers.Num());
            TestEqual(TEXT("counted in the stats"), Stats.WithOutsideReferencers, 1);
        });

        It("does not count a referencer that is in the delete set", [this]()
        {
            FName Referencer = NAME_None;
            const FName Asset = FindReferencedAsset(Referencer);

            if (Asset.IsNone() || Referencer.IsNone())
            {
                AddWarning(TEXT("No referenced asset was found; skipping."));
                return;
            }

            AssetDeletionScan::FStats Before;
            const TArray<AssetDeletionScan::FCandidate> Alone =
                AssetDeletionScan::Scan(ReadyRegistry(), { Asset }, AssetDeletionScan::FOptions(), Before);

            AssetDeletionScan::FStats After;
            const TArray<AssetDeletionScan::FCandidate> Together =
                AssetDeletionScan::Scan(ReadyRegistry(), { Asset, Referencer }, AssetDeletionScan::FOptions(), After);

            const AssetDeletionScan::FCandidate* Widened =
                Together.FindByPredicate([Asset](const AssetDeletionScan::FCandidate& C) { return C.PackageName == Asset; });

            if (Alone.Num() != 1 || !Widened)
            {
                AddError(TEXT("expected the asset in both scans"));
                return;
            }

            // Deleting a Blueprint together with the mesh only it uses is a clean operation. If the
            // Blueprint were still counted, every sensible multi-asset delete would look dangerous
            // and the warning would stop meaning anything.
            TestTrue(TEXT("the count dropped once its referencer joined the set"),
                Widened->OutsideReferencerCount < Alone[0].OutsideReferencerCount);

            TestFalse(TEXT("the referencer is no longer listed"),
                Widened->OutsideReferencers.Contains(Referencer));
        });

        It("truncates the listed names but not the count", [this]()
        {
            FName Referencer = NAME_None;
            const FName Asset = FindReferencedAsset(Referencer);

            if (Asset.IsNone())
            {
                AddWarning(TEXT("No referenced asset was found; skipping."));
                return;
            }

            AssetDeletionScan::FOptions Options;
            Options.MaxReferencersListed = 1;

            AssetDeletionScan::FStats Stats;
            const TArray<AssetDeletionScan::FCandidate> Candidates =
                AssetDeletionScan::Scan(ReadyRegistry(), { Asset }, Options, Stats);

            if (Candidates.Num() != 1)
            {
                AddError(TEXT("expected one candidate"));
                return;
            }

            TestTrue(TEXT("at most one name kept"), Candidates[0].OutsideReferencers.Num() <= 1);
            TestTrue(TEXT("the count is not truncated with it"),
                Candidates[0].OutsideReferencerCount >= Candidates[0].OutsideReferencers.Num());
        });
    });

    Describe("Describing a deletable asset", [this]()
    {
        It("reports the read-only flag the way the filesystem does", [this]()
        {
            FName Referencer = NAME_None;
            const FName Asset = FindReferencedAsset(Referencer);

            if (Asset.IsNone())
            {
                AddWarning(TEXT("No suitable asset was found; skipping."));
                return;
            }

            AssetDeletionScan::FStats Stats;
            const TArray<AssetDeletionScan::FCandidate> Candidates =
                AssetDeletionScan::Scan(ReadyRegistry(), { Asset }, AssetDeletionScan::FOptions(), Stats);

            if (Candidates.Num() != 1 || !Candidates[0].CanDelete())
            {
                AddError(TEXT("expected one deletable candidate"));
                return;
            }

            FString Filename;
            if (!FPackageName::DoesPackageExist(Asset.ToString(), &Filename))
            {
                AddError(TEXT("the package has no file on disk"));
                return;
            }

            // Perforce-primary projects keep unopened files read-only, so read-only is the normal state of an unopened
            // file. Reporting it up front is the difference between a warning and a delete that
            // fails halfway through the selection.
            TestEqual(TEXT("matches the filesystem"),
                Candidates[0].bReadOnlyOnDisk, IFileManager::Get().IsReadOnly(*Filename));

            TestEqual(TEXT("read-only count agrees"),
                Stats.ReadOnly, Candidates[0].bReadOnlyOnDisk ? 1 : 0);

            AddInfo(FString::Printf(TEXT("%s: %d outside referencer(s), %d from levels, read-only %s."),
                *Asset.ToString(),
                Candidates[0].OutsideReferencerCount,
                Candidates[0].LevelReferencerCount,
                Candidates[0].bReadOnlyOnDisk ? TEXT("yes") : TEXT("no")));
        });

        It("fills in the name, class and project-relative path", [this]()
        {
            FName Referencer = NAME_None;
            const FName Asset = FindReferencedAsset(Referencer);

            if (Asset.IsNone())
            {
                AddWarning(TEXT("No suitable asset was found; skipping."));
                return;
            }

            AssetDeletionScan::FStats Stats;
            const TArray<AssetDeletionScan::FCandidate> Candidates =
                AssetDeletionScan::Scan(ReadyRegistry(), { Asset }, AssetDeletionScan::FOptions(), Stats);

            if (Candidates.Num() != 1)
            {
                AddError(TEXT("expected one candidate"));
                return;
            }

            // The window shows these three and nothing else per row, so an empty one is a blank
            // line in front of someone about to press Delete.
            TestFalse(TEXT("name filled"), Candidates[0].AssetName.IsNone());
            TestTrue(TEXT("class filled"), Candidates[0].ClassPath.IsValid());
            TestTrue(TEXT("path starts at Content/"),
                Candidates[0].PathFromProjectRoot.StartsWith(TEXT("Content/")));
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
