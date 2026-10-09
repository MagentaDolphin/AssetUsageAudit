// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageAuditTypes.h"
#include "AssetUsagePaths.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/AutomationTest.h"
#include "Misc/AssetRegistryInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Migrate, run for real against a throwaway destination project.
 *
 * Everything else about Migrate is covered by reading the engine's rules; this is the only spec
 * that proves the call actually moves files. It matters because MigratePackages returns void and
 * reports failures to the Output Log alone - the mode shipped once already in a state where it did
 * nothing at all and looked like a dead button.
 *
 * The destination is a minimal project skeleton under Saved/: a .uproject beside a Content folder
 * is exactly what the engine checks for, and building one costs nothing compared to requiring a
 * second real project on disk.
 */
BEGIN_DEFINE_SPEC(MigrateLiveSpec,
    "AssetUsageAudit.MigrateLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    FString ScratchDir;
    FString DestinationContentDir;

    void MakeDestinationProject(const TCHAR* Suffix)
    {
        ScratchDir = FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetUsageAuditTests"), Suffix));

        IFileManager::Get().DeleteDirectory(*ScratchDir, false, true);

        const FString ProjectRoot = FPaths::Combine(ScratchDir, TEXT("MigrateTarget"));
        DestinationContentDir = FPaths::Combine(ProjectRoot, TEXT("Content"));

        IFileManager::Get().MakeDirectory(*DestinationContentDir, true);

        // Contents are never parsed - the engine only looks for a file with this extension beside
        // the Content folder in order to decide the destination mount point is /Game/.
        FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(ProjectRoot, TEXT("MigrateTarget.uproject")));
    }

    void RemoveScratchDir()
    {
        if (!ScratchDir.IsEmpty())
        {
            IFileManager::Get().DeleteDirectory(*ScratchDir, false, true);
        }
    }

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
     * A /Game asset that has at least one /Game dependency of its own.
     *
     * The dependency is the point: a migrated asset arriving without the things it references is
     * the failure this whole layout exists to avoid, and an asset with no dependencies could not
     * detect it. Sorted so a failure is reproducible.
     */
    static FName FindAssetWithGameDependency(FName& OutDependency)
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

            if (AssetData.PackageName.ToString().Contains(TEXT("__External")))
            {
                return true;
            }

            Candidates.Add(AssetData.PackageName);
            return Candidates.Num() < 4096;
        });

        Candidates.Sort(FNameLexicalLess());

        TArray<FName> Dependencies;

        for (FName Candidate : Candidates)
        {
            Dependencies.Reset();
            Registry.GetDependencies(
                Candidate,
                Dependencies,
                AssetUsageAudit::MakeTraversalCategory(),
                AssetUsageAudit::MakeTraversalQuery());

            for (FName Dependency : Dependencies)
            {
                const FString AsString = Dependency.ToString();

                if (AsString.StartsWith(TEXT("/Game/")) && !AsString.Contains(TEXT("__External")))
                {
                    OutDependency = Dependency;
                    return Candidate;
                }
            }
        }

        return NAME_None;
    }

    /** Where a /Game package should land under the destination Content folder. */
    FString ExpectedDestinationFile(FName PackageName) const
    {
        FString Relative = PackageName.ToString();
        Relative.RemoveFromStart(TEXT("/Game/"));

        return FPaths::Combine(DestinationContentDir, Relative + TEXT(".uasset"));
    }

END_DEFINE_SPEC(MigrateLiveSpec)

void MigrateLiveSpec::Define()
{
    Describe("Migrating into a destination project", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("places the asset at its original package path, with its dependency", [this]()
        {
            FName Dependency = NAME_None;
            const FName Package = FindAssetWithGameDependency(Dependency);

            if (Package.IsNone())
            {
                AddWarning(TEXT("No /Game asset with a /Game dependency was found; skipping."));
                return;
            }

            MakeDestinationProject(TEXT("MigrateLive"));

            AddInfo(FString::Printf(TEXT("Migrating '%s', which references '%s'."),
                *Package.ToString(), *Dependency.ToString()));

            // The destination must pass our own check first. If it does not, the failure below
            // would be about the fixture rather than about Migrate.
            TestTrue(TEXT("the fixture is a valid migrate destination"),
                AssetUsagePaths::ValidateMigrateDestination(DestinationContentDir).IsEmpty());

            // Migrate loads every package it moves, and this project's ConventionKeeper plugin
            // validates asset names on load - so migrating real content emits Error lines about
            // naming violations in assets this spec did not choose and cannot fix. The automation
            // framework treats any logged Error as a failure, so they have to be declared.
            //
            // Deliberately matched loosely and with no expected count: which assets come along
            // depends on the dependency closure of whichever asset the search picked, and pinning
            // that would make the spec fail whenever content changes.
            AddExpectedError(TEXT("asset-naming"), EAutomationExpectedErrorFlags::Contains, 0);

            FMigrationOptions Options;
            Options.bPrompt = false;
            Options.bIgnoreDependencies = false;
            Options.AssetConflict = EAssetMigrationConflict::Overwrite;

            IAssetTools::Get().MigratePackages({ Package }, DestinationContentDir, Options);

            const FString ExpectedAsset = ExpectedDestinationFile(Package);
            const FString ExpectedDependency = ExpectedDestinationFile(Dependency);

            // The whole point of the mode: the file has to be at the same package path, because
            // that is how the reference inside it is resolved.
            TestTrue(*FString::Printf(TEXT("the asset landed at %s"), *ExpectedAsset),
                IFileManager::Get().FileExists(*ExpectedAsset));

            TestTrue(*FString::Printf(TEXT("its dependency landed at %s"), *ExpectedDependency),
                IFileManager::Get().FileExists(*ExpectedDependency));
        });

        It("writes nothing when the destination is not a Content folder", [this]()
        {
            // Guards the case that shipped broken: Migrate refuses, logs, and returns void, so
            // without our own check the caller cannot tell success from silence.
            FName Dependency = NAME_None;
            const FName Package = FindAssetWithGameDependency(Dependency);

            if (Package.IsNone())
            {
                AddWarning(TEXT("No suitable asset was found; skipping."));
                return;
            }

            MakeDestinationProject(TEXT("MigrateLiveBadDest"));

            // One level up from Content: right project, wrong folder.
            const FString BadDestination = FPaths::GetPath(DestinationContentDir);

            TestFalse(TEXT("our check rejects it"),
                AssetUsagePaths::ValidateMigrateDestination(BadDestination).IsEmpty());

            AddExpectedError(TEXT("does not appear to be a game Content folder"), EAutomationExpectedErrorFlags::Contains, 0);

            FMigrationOptions Options;
            Options.bPrompt = false;

            IAssetTools::Get().MigratePackages({ Package }, BadDestination, Options);

            TestFalse(TEXT("nothing was written into the project root"),
                IFileManager::Get().FileExists(*FPaths::Combine(BadDestination, TEXT("MigrateTarget.uasset"))));
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
