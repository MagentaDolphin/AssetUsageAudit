// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageExporter.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Exporter tests against the project's real content.
 *
 * The sibling spec (AssetUsageAudit.Exporter) covers the exporter's logic with names that
 * deliberately do not exist, so it proves the bookkeeping and nothing else: until this file existed
 * the exporter had never copied a single real .uasset. The interesting failures - a Perforce
 * read-only source, an OFPA level whose external actors live under a stale duplicate folder, a
 * package whose file extension is .umap rather than .uasset - only appear on real files.
 *
 * Everything here discovers its subject from the Asset Registry rather than hardcoding a path, so
 * the spec keeps working after content moves, and warns instead of failing on a clone that has no
 * suitable asset.
 */
BEGIN_DEFINE_SPEC(AssetUsageExporterLiveSpec,
    "AssetUsageAudit.ExporterLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    FString ScratchDir;

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

    /**
     * A partially scanned registry answers "no such asset" indistinguishably from "no such asset
     * exists", which would turn a real failure into a silent skip.
     */
    static IAssetRegistry& ReadyRegistry()
    {
        IAssetRegistry& Registry = IAssetRegistry::GetChecked();
        if (Registry.IsLoadingAssets())
        {
            Registry.WaitForCompletion();
        }
        return Registry;
    }

    /** Files on disk under a package's file, or 0 when the package has no file. */
    static int64 FileSizeOf(FName PackageName)
    {
        const FString Path = FAssetUsageExporter::ResolvePackageFilePath(PackageName);
        return Path.IsEmpty() ? 0 : IFileManager::Get().FileSize(*Path);
    }

    static FString HashOf(const FString& FilePath)
    {
        FMD5Hash Hash = FMD5Hash::HashFile(*FilePath);
        return Hash.IsValid() ? LexToString(Hash) : FString();
    }

    /**
     * Any ordinary /Game asset that has a file on disk.
     *
     * Excludes levels (a .umap drags external actors in), OFPA packages, and redirectors. Sorted so
     * two runs on the same content pick the same asset and a failure is reproducible.
     */
    static FName FindOrdinaryAssetPackage()
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;
        Filter.bIncludeOnlyOnDiskAssets = true;

        TArray<FName> Candidates;

        ReadyRegistry().EnumerateAssets(Filter, [&Candidates](const FAssetData& AssetData)
        {
            // Compare class paths rather than calling GetClass(): GetClass() resolves the UClass and
            // returns null for anything not loaded, which would let levels through.
            if (AssetData.IsRedirector() || AssetData.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
            {
                return true;
            }

            const FString PackageName = AssetData.PackageName.ToString();
            if (PackageName.Contains(TEXT("__External")))
            {
                return true;
            }

            Candidates.Add(AssetData.PackageName);

            // A few hundred is plenty to sort; enumerating all 80k just to pick one is waste.
            return Candidates.Num() < 512;
        });

        if (Candidates.IsEmpty())
        {
            return NAME_None;
        }

        Candidates.Sort(FNameLexicalLess());

        for (FName Candidate : Candidates)
        {
            if (!FAssetUsageExporter::ResolvePackageFilePath(Candidate).IsEmpty())
            {
                return Candidate;
            }
        }

        return NAME_None;
    }

    /**
     * How many .uasset files live under a level's registered external roots.
     *
     * Uses the plural GetExternalActorsPaths / GetExternalObjectsPaths rather than a hand-built
     * "__ExternalActors__/..." string. This project contains a stale duplicate of exactly that
     * shape - Content/__ExternalActors__/levelinterior_station alongside the live
     * Content/__ExternalActors__/Interior_station/levelinterior_station - so a string-built path
     * would silently count the wrong folder.
     */
    static int32 CountExternalPackages(FName LevelPackage)
    {
        const FString LevelPackageStr = LevelPackage.ToString();

        TArray<FString> Roots = ULevel::GetExternalActorsPaths(LevelPackageStr);
        Roots.Append(ULevel::GetExternalObjectsPaths(LevelPackageStr));

        // The returned roots overlap: measured on this project, summing the per-root file counts
        // gives exactly twice the number of packages that exist. Deduplicate by absolute file path
        // rather than by package name, so this stays an independent filesystem measurement instead
        // of a restatement of the exporter's own bookkeeping.
        TSet<FString> UniqueFiles;

        for (const FString& Root : Roots)
        {
            FString RootDir;
            if (!FPackageName::TryConvertLongPackageNameToFilename(Root, RootDir))
            {
                continue;
            }

            TArray<FString> Found;
            IFileManager::Get().FindFilesRecursive(Found, *RootDir, TEXT("*.uasset"), true, false);

            for (const FString& File : Found)
            {
                UniqueFiles.Add(FPaths::ConvertRelativePathToFull(File));
            }
        }

        return UniqueFiles.Num();
    }

    /**
     * The OFPA level with the fewest external actors.
     *
     * Smallest on purpose: WP_Example owns roughly 16000 external packages and copying those in a test
     * would move gigabytes. Search stops as soon as something small enough turns up.
     */
    static FName FindSmallestOfpaLevel(int32& OutExternalCount)
    {
        FARFilter Filter;
        Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;
        Filter.bIncludeOnlyOnDiskAssets = true;

        FName Best = NAME_None;
        int32 BestCount = MAX_int32;

        ReadyRegistry().EnumerateAssets(Filter, [&Best, &BestCount](const FAssetData& AssetData)
        {
            const int32 Count = CountExternalPackages(AssetData.PackageName);
            if (Count > 0 && Count < BestCount)
            {
                BestCount = Count;
                Best = AssetData.PackageName;
            }

            // Good enough to prove the expansion works without a long copy.
            return BestCount > 40;
        });

        OutExternalCount = (Best.IsNone()) ? 0 : BestCount;
        return Best;
    }

    int32 CountFilesInScratch() const
    {
        TArray<FString> Found;
        IFileManager::Get().FindFilesRecursive(Found, *ScratchDir, TEXT("*.*"), true, false);
        return Found.Num();
    }

END_DEFINE_SPEC(AssetUsageExporterLiveSpec)

void AssetUsageExporterLiveSpec::Define()
{
    Describe("Copying a real asset", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("reproduces the source file byte for byte", [this]()
        {
            const FName Package = FindOrdinaryAssetPackage();
            if (Package.IsNone())
            {
                AddWarning(TEXT("No ordinary /Game asset with a file on disk was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("LiveCopy"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bIncludeExternalPackages = false;

            const FAssetUsageExporter::FResult Result =
                FAssetUsageExporter::ExportPackageFiles({ Package }, Options);

            TestTrue(TEXT("succeeded"), Result.bSuccess);
            TestEqual(TEXT("one file copied"), Result.FilesCopied, 1);
            TestEqual(TEXT("nothing missing"), Result.FilesMissingOnDisk, 0);

            const FString SourcePath = FAssetUsageExporter::ResolvePackageFilePath(Package);
            const FString DestPath = FPaths::Combine(ScratchDir, FPaths::GetCleanFilename(SourcePath));

            TestTrue(TEXT("destination exists"), IFileManager::Get().FileExists(*DestPath));
            TestEqual(TEXT("same size"), IFileManager::Get().FileSize(*DestPath), FileSizeOf(Package));
            TestEqual(TEXT("same content"), HashOf(DestPath), HashOf(SourcePath));
        });

        It("leaves the exported file writable even when the source is read-only", [this]()
        {
            // Perforce-primary projects keep unopened files read-only, so unopened files are read-only on disk and Windows
            // CopyFile carries the attribute across. A read-only export folder is useless to an
            // artist and breaks the exporter's own Overwrite policy on the next run.
            const FName Package = FindOrdinaryAssetPackage();
            if (Package.IsNone())
            {
                AddWarning(TEXT("No ordinary /Game asset with a file on disk was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("LiveReadOnly"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bIncludeExternalPackages = false;

            FAssetUsageExporter::ExportPackageFiles({ Package }, Options);

            const FString DestPath = FPaths::Combine(
                ScratchDir, FPaths::GetCleanFilename(FAssetUsageExporter::ResolvePackageFilePath(Package)));

            TestFalse(TEXT("destination is writable"), IFileManager::Get().IsReadOnly(*DestPath));
        });
    });

    Describe("Collision policy on real files", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("writes a second copy beside the first under the Index policy", [this]()
        {
            const FName Package = FindOrdinaryAssetPackage();
            if (Package.IsNone())
            {
                AddWarning(TEXT("No ordinary /Game asset with a file on disk was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("LiveIndex"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.CollisionPolicy = EExportCollisionPolicy::Index;
            Options.bIncludeExternalPackages = false;

            FAssetUsageExporter::ExportPackageFiles({ Package }, Options);
            const FAssetUsageExporter::FResult Second =
                FAssetUsageExporter::ExportPackageFiles({ Package }, Options);
            const FAssetUsageExporter::FResult Third =
                FAssetUsageExporter::ExportPackageFiles({ Package }, Options);

            TestEqual(TEXT("second run renamed"), Second.FilesRenamed, 1);
            TestEqual(TEXT("third run renamed"), Third.FilesRenamed, 1);
            TestEqual(TEXT("three files on disk"), CountFilesInScratch(), 3);

            const FString SourcePath = FAssetUsageExporter::ResolvePackageFilePath(Package);
            const FString Base = FPaths::GetBaseFilename(SourcePath);
            const FString Ext = FPaths::GetExtension(SourcePath, true);

            TestTrue(TEXT("_1 present"), IFileManager::Get().FileExists(*FPaths::Combine(ScratchDir, Base + TEXT("_1") + Ext)));
            TestTrue(TEXT("_2 present"), IFileManager::Get().FileExists(*FPaths::Combine(ScratchDir, Base + TEXT("_2") + Ext)));
        });

        It("replaces the previous file under the Overwrite policy", [this]()
        {
            const FName Package = FindOrdinaryAssetPackage();
            if (Package.IsNone())
            {
                AddWarning(TEXT("No ordinary /Game asset with a file on disk was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("LiveOverwrite"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.CollisionPolicy = EExportCollisionPolicy::Overwrite;
            Options.bIncludeExternalPackages = false;

            FAssetUsageExporter::ExportPackageFiles({ Package }, Options);
            const FAssetUsageExporter::FResult Second =
                FAssetUsageExporter::ExportPackageFiles({ Package }, Options);

            TestTrue(TEXT("second run succeeded"), Second.bSuccess);
            TestEqual(TEXT("counted as an overwrite"), Second.FilesOverwritten, 1);
            TestEqual(TEXT("nothing renamed"), Second.FilesRenamed, 0);
            TestEqual(TEXT("still one file"), CountFilesInScratch(), 1);
        });
    });

    Describe("Mirroring the package tree", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("places the file under its /Game path when flattening is off", [this]()
        {
            const FName Package = FindOrdinaryAssetPackage();
            if (Package.IsNone())
            {
                AddWarning(TEXT("No ordinary /Game asset with a file on disk was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("LiveTree"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bFlatten = false;
            Options.bIncludeExternalPackages = false;

            const FAssetUsageExporter::FResult Result =
                FAssetUsageExporter::ExportPackageFiles({ Package }, Options);

            TestEqual(TEXT("one file copied"), Result.FilesCopied, 1);

            FString Relative = FPaths::GetPath(Package.ToString());
            Relative.RemoveFromStart(TEXT("/"));

            const FString SourcePath = FAssetUsageExporter::ResolvePackageFilePath(Package);
            const FString Expected = FPaths::Combine(ScratchDir, Relative, FPaths::GetCleanFilename(SourcePath));

            TestTrue(TEXT("mirrored path exists"), IFileManager::Get().FileExists(*Expected));
        });
    });

    Describe("Exporting an OFPA level", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("brings every external actor package along with the map", [this]()
        {
            // The whole point of the tool: an OFPA map copied on its own opens empty, because its
            // actors are separate packages joined to it by a soft dependency edge.
            int32 ExpectedExternal = 0;
            const FName Level = FindSmallestOfpaLevel(ExpectedExternal);

            if (Level.IsNone())
            {
                AddWarning(TEXT("No level with external actor packages was found; skipping."));
                return;
            }

            AddInfo(FString::Printf(TEXT("Using level '%s' with %d external packages."), *Level.ToString(), ExpectedExternal));

            MakeScratchDir(TEXT("LiveOfpa"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bFlatten = false;  // keep the tree: external actor names are GUID-ish and long
            Options.bIncludeExternalPackages = true;

            const FAssetUsageExporter::FResult Result =
                FAssetUsageExporter::ExportPackageFiles({ Level }, Options);

            TestTrue(TEXT("succeeded"), Result.bSuccess);
            TestEqual(TEXT("expanded to every external package"), Result.ExternalPackagesCopied, ExpectedExternal);
            TestEqual(TEXT("copied the map plus its externals"), Result.FilesCopied, ExpectedExternal + 1);
            TestEqual(TEXT("files on disk match"), CountFilesInScratch(), ExpectedExternal + 1);

            const FString MapPath = FAssetUsageExporter::ResolvePackageFilePath(Level);
            TestTrue(TEXT("the map itself is a .umap"), MapPath.EndsWith(TEXT(".umap")));
        });

        It("copies only the map when external packages are switched off", [this]()
        {
            int32 ExpectedExternal = 0;
            const FName Level = FindSmallestOfpaLevel(ExpectedExternal);

            if (Level.IsNone())
            {
                AddWarning(TEXT("No level with external actor packages was found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("LiveOfpaOff"));

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bIncludeExternalPackages = false;

            const FAssetUsageExporter::FResult Result =
                FAssetUsageExporter::ExportPackageFiles({ Level }, Options);

            TestEqual(TEXT("just the map"), Result.FilesCopied, 1);
            TestEqual(TEXT("no externals expanded"), Result.ExternalPackagesCopied, 0);
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
