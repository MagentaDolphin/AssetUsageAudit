// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetImportScanner.h"

#include "AssetExportManifest.h"
#include "AssetImporter.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetUsageAuditTypes.h"
#include "AssetUsageExporter.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Export, scan, import - the loop the manifest exists to close.
 *
 * The export side is covered elsewhere; what is unproven until here is that the folder an export
 * leaves behind can actually be read back. A manifest that cannot be consumed is decoration, and
 * nothing in the writer's own specs would notice.
 */
BEGIN_DEFINE_SPEC(AssetImportRoundTripSpec,
    "AssetUsageAudit.ImportRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    FString ScratchDir;

    /**
     * A package path this project cannot already contain.
     *
     * The importer writes into the running project's Content folder - that is its whole job - so
     * every spec that lets it write must aim somewhere no real asset lives, and clean up after.
     */
    static const TCHAR* ScratchPackageRoot() { return TEXT("/Game/AssetUsageAuditImportScratch"); }

    static FString ScratchContentDir()
    {
        return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetUsageAuditImportScratch"));
    }

    void MakeScratchDir(const TCHAR* Suffix)
    {
        ScratchDir = FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetUsageAuditTests"), Suffix));

        IFileManager::Get().DeleteDirectory(*ScratchDir, false, true);
        IFileManager::Get().MakeDirectory(*ScratchDir, true);
    }

    void CleanUp()
    {
        if (!ScratchDir.IsEmpty())
        {
            IFileManager::Get().DeleteDirectory(*ScratchDir, false, true);
        }

        // Always, even when the spec did not import: a half-finished run must not leave a stray
        // folder inside the project's content.
        IFileManager::Get().DeleteDirectory(*ScratchContentDir(), false, true);
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

    /** An ordinary /Game asset with at least one /Game dependency, chosen deterministically. */
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
            return Candidates.Num() < 2048;
        });

        Candidates.Sort(FNameLexicalLess());

        TArray<FName> Dependencies;

        for (FName Candidate : Candidates)
        {
            Dependencies.Reset();
            Registry.GetDependencies(Candidate, Dependencies,
                AssetUsageAudit::MakeTraversalCategory(), AssetUsageAudit::MakeTraversalQuery());

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

END_DEFINE_SPEC(AssetImportRoundTripSpec)

void AssetImportRoundTripSpec::Define()
{
    Describe("Reading back an export", [this]()
    {
        AfterEach([this]() { CleanUp(); });

        It("recovers every package path from the manifest", [this]()
        {
            FName Dependency = NAME_None;
            const FName Package = FindAssetWithGameDependency(Dependency);

            if (Package.IsNone())
            {
                AddWarning(TEXT("No /Game asset with a /Game dependency was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("RoundTripManifest"));

            // Flat on purpose: the layout that destroys the reference graph is the one the manifest
            // has to compensate for, so it is the only interesting case to read back.
            FAssetUsageExporter::FOptions ExportOptions;
            ExportOptions.TargetDirectory = ScratchDir;
            ExportOptions.bIncludeExternalPackages = false;
            ExportOptions.bRecordWrittenFiles = true;

            const FAssetUsageExporter::FResult ExportResult =
                FAssetUsageExporter::ExportPackageFiles({ Package, Dependency }, ExportOptions);

            TestEqual(TEXT("two files exported"), ExportResult.FilesCopied, 2);

            AssetExportManifest::FOptions ManifestOptions;
            ManifestOptions.TargetDirectory = ScratchDir;
            ManifestOptions.SeedPackages = { Package };

            const AssetExportManifest::FResult ManifestResult =
                AssetExportManifest::Write(ReadyRegistry(), ExportResult.WrittenFiles, ManifestOptions);

            TestTrue(TEXT("manifest written"), ManifestResult.bSuccess);

            AssetImportScanner::FOptions ScanOptions;
            ScanOptions.SourceDirectory = ScratchDir;

            AssetImportScanner::FStats ScanStats;
            const TArray<AssetImportScanner::FCandidate> Candidates =
                AssetImportScanner::Scan(ReadyRegistry(), ScanOptions, ScanStats);

            TestTrue(TEXT("manifest was found"), ScanStats.bManifestFound);
            TestEqual(TEXT("both files scanned"), ScanStats.FilesFound, 2);
            TestEqual(TEXT("both paths came from the manifest"), ScanStats.FromManifest, 2);
            TestEqual(TEXT("nothing unresolved"), ScanStats.Unresolved, 0);

            // They came from this project, so re-importing them would overwrite the originals.
            // The scanner has to say so before anyone ticks a box.
            TestEqual(TEXT("both flagged as already present"), ScanStats.WouldOverwrite, 2);

            bool bFoundOriginal = false;
            for (const AssetImportScanner::FCandidate& Candidate : Candidates)
            {
                if (Candidate.TargetPackage == Package)
                {
                    bFoundOriginal = true;
                    TestEqual(TEXT("source is the manifest"),
                        Candidate.NameSource, AssetImportScanner::EPackageNameSource::Manifest);
                }
            }

            TestTrue(TEXT("the exported asset is in the scan"), bFoundOriginal);
        });

        It("falls back to the package header when the manifest is missing", [this]()
        {
            // The manifest is an optional courtesy; a folder someone assembled by hand has none.
            // This is what makes the tool useful on an export it did not produce.
            FName Dependency = NAME_None;
            const FName Package = FindAssetWithGameDependency(Dependency);

            if (Package.IsNone())
            {
                AddWarning(TEXT("No suitable asset was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("RoundTripNoManifest"));

            FAssetUsageExporter::FOptions ExportOptions;
            ExportOptions.TargetDirectory = ScratchDir;
            ExportOptions.bIncludeExternalPackages = false;

            FAssetUsageExporter::ExportPackageFiles({ Package }, ExportOptions);

            AssetImportScanner::FOptions ScanOptions;
            ScanOptions.SourceDirectory = ScratchDir;

            AssetImportScanner::FStats ScanStats;
            const TArray<AssetImportScanner::FCandidate> Candidates =
                AssetImportScanner::Scan(ReadyRegistry(), ScanOptions, ScanStats);

            TestFalse(TEXT("no manifest"), ScanStats.bManifestFound);
            TestEqual(TEXT("one file"), ScanStats.FilesFound, 1);

            if (Candidates.Num() != 1)
            {
                AddError(TEXT("expected exactly one candidate"));
                return;
            }

            // The engine notes this field is not always serialised, so a fallback to the folder
            // structure is an acceptable answer too - what must not happen is Unresolved, which
            // would mean neither route worked on a file we just wrote ourselves.
            TestNotEqual(TEXT("a path was recovered"),
                Candidates[0].NameSource, AssetImportScanner::EPackageNameSource::Unresolved);

            AddInfo(FString::Printf(TEXT("Recovered '%s' via %s."),
                *Candidates[0].TargetPackage.ToString(),
                AssetImportScanner::LexToString(Candidates[0].NameSource)));
        });
    });

    Describe("Refusing to destroy existing work", [this]()
    {
        AfterEach([this]() { CleanUp(); });

        It("skips an asset that already exists rather than overwriting it", [this]()
        {
            FName Dependency = NAME_None;
            const FName Package = FindAssetWithGameDependency(Dependency);

            if (Package.IsNone())
            {
                AddWarning(TEXT("No suitable asset was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("ImportNoOverwrite"));

            FAssetUsageExporter::FOptions ExportOptions;
            ExportOptions.TargetDirectory = ScratchDir;
            ExportOptions.bIncludeExternalPackages = false;

            FAssetUsageExporter::ExportPackageFiles({ Package }, ExportOptions);

            AssetImportScanner::FOptions ScanOptions;
            ScanOptions.SourceDirectory = ScratchDir;

            AssetImportScanner::FStats ScanStats;
            const TArray<AssetImportScanner::FCandidate> Candidates =
                AssetImportScanner::Scan(ReadyRegistry(), ScanOptions, ScanStats);

            // Importing these back into the project they came from must be a no-op by default.
            // If this ever passes with FilesImported > 0, the tool has just overwritten real
            // content with a copy of itself.
            AssetImporter::FOptions ImportOptions;
            ImportOptions.bOverwriteExisting = false;

            const AssetImporter::FResult ImportResult =
                AssetImporter::Import(ReadyRegistry(), Candidates, ImportOptions);

            TestEqual(TEXT("nothing was written"), ImportResult.FilesImported, 0);
            TestTrue(TEXT("skipped or refused as loaded"),
                ImportResult.SkippedExisting + ImportResult.SkippedLoaded == Candidates.Num());
        });

        It("skips a candidate with no recoverable path and no fallback folder", [this]()
        {
            AssetImportScanner::FCandidate Candidate;
            Candidate.SourceFile = TEXT("D:/nowhere/Mystery.uasset");
            Candidate.RelativePath = TEXT("Mystery.uasset");
            Candidate.NameSource = AssetImportScanner::EPackageNameSource::Unresolved;

            AssetImporter::FOptions Options;

            const AssetImporter::FResult Result =
                AssetImporter::Import(ReadyRegistry(), { Candidate }, Options);

            TestEqual(TEXT("skipped"), Result.SkippedUnresolved, 1);
            TestEqual(TEXT("nothing written"), Result.FilesImported, 0);
            TestEqual(TEXT("not an error"), Result.Errors.Num(), 0);
        });
    });

    Describe("Importing into the project", [this]()
    {
        AfterEach([this]() { CleanUp(); });

        It("writes the file at the package path and makes it visible to the registry", [this]()
        {
            // The only spec that lets the importer write into this project's content, and it aims
            // at a folder no real asset uses. CleanUp removes it whatever happens.
            FName Dependency = NAME_None;
            const FName Package = FindAssetWithGameDependency(Dependency);

            if (Package.IsNone())
            {
                AddWarning(TEXT("No suitable asset was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("ImportWrite"));

            FAssetUsageExporter::FOptions ExportOptions;
            ExportOptions.TargetDirectory = ScratchDir;
            ExportOptions.bIncludeExternalPackages = false;

            const FAssetUsageExporter::FResult ExportResult =
                FAssetUsageExporter::ExportPackageFiles({ Package }, ExportOptions);

            if (ExportResult.FilesCopied != 1)
            {
                AddError(TEXT("the fixture export did not produce a file"));
                return;
            }

            TArray<FString> Files;
            IFileManager::Get().FindFilesRecursive(Files, *ScratchDir, TEXT("*.uasset"), true, false);

            if (Files.Num() != 1)
            {
                AddError(TEXT("expected one exported file"));
                return;
            }

            // Aimed at the scratch root rather than at its original path, so nothing real is at risk.
            const FName ScratchTarget(*FString::Printf(TEXT("%s/Imported"), ScratchPackageRoot()));

            AssetImportScanner::FCandidate Candidate;
            Candidate.SourceFile = Files[0];
            Candidate.RelativePath = FPaths::GetCleanFilename(Files[0]);
            Candidate.TargetPackage = ScratchTarget;
            Candidate.NameSource = AssetImportScanner::EPackageNameSource::Manifest;

            AssetImporter::FOptions ImportOptions;

            const AssetImporter::FResult ImportResult =
                AssetImporter::Import(ReadyRegistry(), { Candidate }, ImportOptions);

            TestEqual(TEXT("one file imported"), ImportResult.FilesImported, 1);
            TestTrue(TEXT("succeeded"), ImportResult.bSuccess);

            FString ExpectedFile;
            FPackageName::TryConvertLongPackageNameToFilename(
                ScratchTarget.ToString(), ExpectedFile, FPackageName::GetAssetPackageExtension());

            TestTrue(TEXT("the file is at the package path"), IFileManager::Get().FileExists(*ExpectedFile));

            // Copying is not enough: without the rescan the asset exists on disk and is invisible
            // in the Content Browser, which reads as an import that did nothing.
            TArray<FAssetData> Found;
            ReadyRegistry().GetAssetsByPackageName(ScratchTarget, Found, /*bIncludeOnlyOnDiskAssets=*/true);

            TestTrue(TEXT("the registry can see it"), Found.Num() > 0);
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
