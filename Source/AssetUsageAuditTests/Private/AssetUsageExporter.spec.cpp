// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageExporter.h"
#include "AssetExportNaming.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(AssetUsageExporterSpec,
    "AssetUsageAudit.Exporter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    FString ScratchDir;

    /** Create a throwaway folder under Saved/ so a failed run never touches project content. */
    void MakeScratchDir(const TCHAR* Suffix)
    {
        ScratchDir = FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetUsageAuditTests"), Suffix));

        IFileManager::Get().DeleteDirectory(*ScratchDir, false, true);
        IFileManager::Get().MakeDirectory(*ScratchDir, true);
    }

    void RemoveScratchDir()
    {
        if (!ScratchDir.IsEmpty())
        {
            IFileManager::Get().DeleteDirectory(*ScratchDir, false, true);
        }
    }

    void WriteStubFile(const FString& FileName)
    {
        FFileHelper::SaveStringToFile(TEXT("stub"), *FPaths::Combine(ScratchDir, FileName));
    }

    bool FileExistsInScratch(const FString& FileName) const
    {
        return IFileManager::Get().FileExists(*FPaths::Combine(ScratchDir, FileName));
    }

END_DEFINE_SPEC(AssetUsageExporterSpec)

void AssetUsageExporterSpec::Define()
{
    Describe("ResolvePackageFilePath", [this]()
    {
        It("returns empty for a package that does not exist", [this]()
        {
            const FString Path = FAssetUsageExporter::ResolvePackageFilePath(
                FName(TEXT("/Game/ThisPackageDoesNotExist_AssetUsageAuditTest")));

            TestTrue(TEXT("no file for a missing package"), Path.IsEmpty());
        });

        It("returns empty for a script package rather than inventing a path", [this]()
        {
            // Script packages are code. Handing back a plausible path would make the exporter
            // report a copy it never performed.
            const FString Path = FAssetUsageExporter::ResolvePackageFilePath(FName(TEXT("/Script/Engine")));

            TestTrue(TEXT("no file for a script package"), Path.IsEmpty());
        });

        It("returns empty for a malformed package name", [this]()
        {
            TestTrue(TEXT("empty name"), FAssetUsageExporter::ResolvePackageFilePath(FName()).IsEmpty());
            TestTrue(TEXT("not a long package name"), FAssetUsageExporter::ResolvePackageFilePath(FName(TEXT("NotAPackage"))).IsEmpty());
        });
    });

    Describe("ExportPackageFiles", [this]()
    {
        AfterEach([this]()
        {
            RemoveScratchDir();
        });

        It("fails cleanly when no target directory is given", [this]()
        {
            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory.Reset();

            // The exporter logs this at Error level on purpose - it is a setup mistake the user
            // must see in the Output Log. The automation framework treats any logged Error during
            // a test as a failure, so the expected message has to be declared.
            AddExpectedError(TEXT("Export aborted: no target directory was given"), EAutomationExpectedErrorFlags::Contains, 1);

            const FAssetUsageExporter::FResult Result =
                FAssetUsageExporter::ExportPackageFiles({ FName(TEXT("/Game/Whatever")) }, Options);

            TestFalse(TEXT("not successful"), Result.bSuccess);
            TestTrue(TEXT("reports why"), Result.Errors.Num() > 0);
            TestEqual(TEXT("copied nothing"), Result.FilesCopied, 0);
        });

        It("creates the target directory when it is missing", [this]()
        {
            MakeScratchDir(TEXT("CreatesDir"));

            const FString Nested = FPaths::Combine(ScratchDir, TEXT("Nested/Deeper"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = Nested;

            FAssetUsageExporter::ExportPackageFiles({}, Options);

            TestTrue(TEXT("directory created"), IFileManager::Get().DirectoryExists(*Nested));
        });

        It("counts a package with no file on disk without calling it an error", [this]()
        {
            // A script package or an unsaved asset is an ordinary outcome, not a failure. Folding
            // it into Errors would bury real copy failures in noise.
            MakeScratchDir(TEXT("MissingOnDisk"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bIncludeExternalPackages = false;

            const FAssetUsageExporter::FResult Result = FAssetUsageExporter::ExportPackageFiles(
                { FName(TEXT("/Game/NoSuchAsset_AssetUsageAuditTest")) }, Options);

            TestEqual(TEXT("counted as missing"), Result.FilesMissingOnDisk, 1);
            TestEqual(TEXT("no errors"), Result.Errors.Num(), 0);
            TestEqual(TEXT("nothing copied"), Result.FilesCopied, 0);
        });

        It("reports progress with a total that matches the request", [this]()
        {
            MakeScratchDir(TEXT("Progress"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bIncludeExternalPackages = false;

            int32 SeenTotal = INDEX_NONE;
            int32 Calls = 0;

            Options.OnProgress = [&SeenTotal, &Calls](int32 Done, int32 Total)
            {
                SeenTotal = Total;
                ++Calls;
                return true;
            };

            const TArray<FName> Packages = {
                FName(TEXT("/Game/A_AssetUsageAuditTest")),
                FName(TEXT("/Game/B_AssetUsageAuditTest")),
                FName(TEXT("/Game/C_AssetUsageAuditTest"))
            };

            FAssetUsageExporter::ExportPackageFiles(Packages, Options);

            TestEqual(TEXT("total matches"), SeenTotal, 3);
            TestEqual(TEXT("one call per package"), Calls, 3);
        });

        It("stops when the progress callback asks it to", [this]()
        {
            MakeScratchDir(TEXT("Cancel"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bIncludeExternalPackages = false;

            int32 Calls = 0;
            Options.OnProgress = [&Calls](int32, int32)
            {
                ++Calls;
                return false;  // cancel immediately
            };

            const TArray<FName> Packages = {
                FName(TEXT("/Game/A_AssetUsageAuditTest")),
                FName(TEXT("/Game/B_AssetUsageAuditTest")),
                FName(TEXT("/Game/C_AssetUsageAuditTest"))
            };

            const FAssetUsageExporter::FResult Result = FAssetUsageExporter::ExportPackageFiles(Packages, Options);

            TestTrue(TEXT("flagged as cancelled"), Result.bCancelled);
            TestFalse(TEXT("not reported successful"), Result.bSuccess);
            TestEqual(TEXT("stopped at the first item"), Calls, 1);
        });

        It("de-duplicates a package listed twice", [this]()
        {
            MakeScratchDir(TEXT("Dedup"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bIncludeExternalPackages = false;

            int32 Calls = 0;
            Options.OnProgress = [&Calls](int32, int32) { ++Calls; return true; };

            const FName Same(TEXT("/Game/Duplicate_AssetUsageAuditTest"));
            FAssetUsageExporter::ExportPackageFiles({ Same, Same, Same }, Options);

            TestEqual(TEXT("processed once"), Calls, 1);
        });
    });

    Describe("Collision policy as the exporter applies it", [this]()
    {
        AfterEach([this]()
        {
            RemoveScratchDir();
        });

        It("picks an indexed name against real files on disk", [this]()
        {
            // The exporter feeds a filesystem predicate into ResolveCollision. This checks the two
            // sides agree: a file that exists on disk must be treated as taken.
            MakeScratchDir(TEXT("Collision"));

            WriteStubFile(TEXT("SM_Rock.uasset"));
            WriteStubFile(TEXT("SM_Rock_1.uasset"));

            IFileManager& FileManager = IFileManager::Get();
            const FString Dir = ScratchDir;

            const FString Resolved = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock.uasset"),
                EExportCollisionPolicy::Index,
                [&FileManager, &Dir](const FString& Candidate)
                {
                    return FileManager.FileExists(*FPaths::Combine(Dir, Candidate));
                });

            TestEqual(TEXT("skips both taken names"), Resolved, FString(TEXT("SM_Rock_2.uasset")));
        });

        It("keeps the original name when the folder is empty", [this]()
        {
            MakeScratchDir(TEXT("NoCollision"));

            IFileManager& FileManager = IFileManager::Get();
            const FString Dir = ScratchDir;

            const FString Resolved = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock.uasset"),
                EExportCollisionPolicy::Index,
                [&FileManager, &Dir](const FString& Candidate)
                {
                    return FileManager.FileExists(*FPaths::Combine(Dir, Candidate));
                });

            TestEqual(TEXT("unchanged"), Resolved, FString(TEXT("SM_Rock.uasset")));
        });
    });

    Describe("FResult::Summarise", [this]()
    {
        It("always states how many files were copied", [this]()
        {
            FAssetUsageExporter::FResult Result;
            Result.FilesCopied = 7;

            TestTrue(TEXT("mentions the count"), Result.Summarise().Contains(TEXT("7 copied")));
        });

        It("mentions renames, overwrites and cancellation when they happened", [this]()
        {
            FAssetUsageExporter::FResult Result;
            Result.FilesCopied = 3;
            Result.FilesRenamed = 2;
            Result.FilesOverwritten = 1;
            Result.bCancelled = true;

            const FString Summary = Result.Summarise();

            TestTrue(TEXT("renames"), Summary.Contains(TEXT("renamed")));
            TestTrue(TEXT("overwrites"), Summary.Contains(TEXT("overwritten")));
            TestTrue(TEXT("cancellation"), Summary.Contains(TEXT("cancelled")));
        });

        It("stays quiet about categories that did not occur", [this]()
        {
            FAssetUsageExporter::FResult Result;
            Result.FilesCopied = 1;

            const FString Summary = Result.Summarise();

            TestFalse(TEXT("no rename noise"), Summary.Contains(TEXT("renamed")));
            TestFalse(TEXT("no failure noise"), Summary.Contains(TEXT("failed")));
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
