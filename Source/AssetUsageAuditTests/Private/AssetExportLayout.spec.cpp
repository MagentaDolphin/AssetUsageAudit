// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetExportLayout.h"

#include "AssetDependencyClosure.h"
#include "AssetUsageExporter.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Where exported files land.
 *
 * The grouping logic is pure enough to test on invented package names; the type-folder lookup and
 * the end-to-end copy need real content and say so when it is missing.
 */
BEGIN_DEFINE_SPEC(AssetExportLayoutSpec,
    "AssetUsageAudit.ExportLayout",
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

    static IAssetRegistry& ReadyRegistry()
    {
        IAssetRegistry& Registry = IAssetRegistry::GetChecked();
        if (Registry.IsLoadingAssets())
        {
            Registry.WaitForCompletion();
        }
        return Registry;
    }

    /** A closure with a made-up seed and dependencies, for testing grouping alone. */
    static AssetDependencyClosure::FSeedClosure MakeClosure(const TCHAR* Seed, std::initializer_list<const TCHAR*> Dependencies)
    {
        AssetDependencyClosure::FSeedClosure Closure;
        Closure.Seed = FName(Seed);
        Closure.Packages.Add(Closure.Seed);

        for (const TCHAR* Dependency : Dependencies)
        {
            Closure.Packages.Add(FName(Dependency));
        }

        return Closure;
    }

    /** First /Game asset of a class, or NAME_None. */
    static FName FindPackageOfClass(UClass* Class)
    {
        FARFilter Filter;
        Filter.ClassPaths.Add(Class->GetClassPathName());
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.bIncludeOnlyOnDiskAssets = true;

        TArray<FName> Found;

        ReadyRegistry().EnumerateAssets(Filter, [&Found](const FAssetData& AssetData)
        {
            Found.Add(AssetData.PackageName);
            return Found.Num() < 64;
        });

        if (Found.IsEmpty())
        {
            return NAME_None;
        }

        Found.Sort(FNameLexicalLess());
        return Found[0];
    }

END_DEFINE_SPEC(AssetExportLayoutSpec)

void AssetExportLayoutSpec::Define()
{
    Describe("Flat and mirrored layouts", [this]()
    {
        It("collapses every closure into a single unnamed group", [this]()
        {
            const TArray<AssetDependencyClosure::FSeedClosure> Closures = {
                MakeClosure(TEXT("/Game/A"), { TEXT("/Game/Shared") }),
                MakeClosure(TEXT("/Game/B"), { TEXT("/Game/Shared") })
            };

            AssetExportLayout::FOptions Options;
            Options.Layout = EExportLayout::Flat;

            AssetExportLayout::FStats Stats;
            const TArray<AssetExportLayout::FGroup> Groups =
                AssetExportLayout::BuildGroups(ReadyRegistry(), Closures, Options, Stats);

            TestEqual(TEXT("one group"), Groups.Num(), 1);
            TestTrue(TEXT("writing into the target itself"), Groups[0].RelativeDir.IsEmpty());

            // The shared dependency appears once: a flat folder cannot hold it twice, and pretending
            // otherwise would make the file count disagree with what gets written.
            TestEqual(TEXT("three distinct packages"), Groups[0].Packages.Num(), 3);
            TestEqual(TEXT("stats agree"), Stats.FilePlacements, 3);
            TestEqual(TEXT("distinct count matches"), Stats.DistinctPackages, 3);
        });

        It("treats MirrorTree the same at this level", [this]()
        {
            // Mirroring is the exporter's job, applied per file inside whichever folder a group
            // names. The layout planner has nothing extra to do for it.
            const TArray<AssetDependencyClosure::FSeedClosure> Closures = {
                MakeClosure(TEXT("/Game/A"), { TEXT("/Game/Shared") })
            };

            AssetExportLayout::FOptions Options;
            Options.Layout = EExportLayout::MirrorTree;

            AssetExportLayout::FStats Stats;
            const TArray<AssetExportLayout::FGroup> Groups =
                AssetExportLayout::BuildGroups(ReadyRegistry(), Closures, Options, Stats);

            TestEqual(TEXT("one group"), Groups.Num(), 1);
            TestTrue(TEXT("no subfolder from the planner"), Groups[0].RelativeDir.IsEmpty());
        });
    });

    Describe("Folder per asset", [this]()
    {
        It("gives each seed its own folder named after the asset", [this]()
        {
            const TArray<AssetDependencyClosure::FSeedClosure> Closures = {
                MakeClosure(TEXT("/Game/Art/SM_Rock"), {}),
                MakeClosure(TEXT("/Game/Art/SM_Tree"), {})
            };

            AssetExportLayout::FOptions Options;
            Options.Layout = EExportLayout::FolderPerAsset;

            AssetExportLayout::FStats Stats;
            const TArray<AssetExportLayout::FGroup> Groups =
                AssetExportLayout::BuildGroups(ReadyRegistry(), Closures, Options, Stats);

            TestEqual(TEXT("one group per seed"), Groups.Num(), 2);
            TestEqual(TEXT("named after the asset, not the path"), Groups[0].RelativeDir, FString(TEXT("SM_Rock")));
            TestEqual(TEXT("second folder"), Groups[1].RelativeDir, FString(TEXT("SM_Tree")));
        });

        It("copies a shared dependency into every folder that needs it", [this]()
        {
            // The defining property of this layout: each folder must be self-contained, so a texture
            // used by two meshes is written twice. Deduplicating it would leave the second folder
            // opening with missing content.
            const TArray<AssetDependencyClosure::FSeedClosure> Closures = {
                MakeClosure(TEXT("/Game/Art/SM_Rock"), { TEXT("/Game/Art/T_Shared") }),
                MakeClosure(TEXT("/Game/Art/SM_Tree"), { TEXT("/Game/Art/T_Shared") })
            };

            AssetExportLayout::FOptions Options;
            Options.Layout = EExportLayout::FolderPerAsset;

            AssetExportLayout::FStats Stats;
            const TArray<AssetExportLayout::FGroup> Groups =
                AssetExportLayout::BuildGroups(ReadyRegistry(), Closures, Options, Stats);

            int32 SharedPlacements = 0;
            for (const AssetExportLayout::FGroup& Group : Groups)
            {
                SharedPlacements += Group.Packages.Contains(FName(TEXT("/Game/Art/T_Shared"))) ? 1 : 0;
            }

            TestEqual(TEXT("the texture lands in both folders"), SharedPlacements, 2);

            // Three distinct packages, four files written. This gap is what the export dialog has
            // to show, and the stats have to keep the two numbers apart to make that possible.
            TestEqual(TEXT("distinct packages"), Stats.DistinctPackages, 3);
            TestEqual(TEXT("file placements"), Stats.FilePlacements, 4);
        });

        It("makes folder names unique when two assets share a short name", [this]()
        {
            // /Game/A/SM_Rock and /Game/B/SM_Rock are different assets with the same name. Without
            // a suffix they would share one folder and interleave their dependencies.
            const TArray<AssetDependencyClosure::FSeedClosure> Closures = {
                MakeClosure(TEXT("/Game/A/SM_Rock"), {}),
                MakeClosure(TEXT("/Game/B/SM_Rock"), {})
            };

            AssetExportLayout::FOptions Options;
            Options.Layout = EExportLayout::FolderPerAsset;

            AssetExportLayout::FStats Stats;
            const TArray<AssetExportLayout::FGroup> Groups =
                AssetExportLayout::BuildGroups(ReadyRegistry(), Closures, Options, Stats);

            TestEqual(TEXT("two groups"), Groups.Num(), 2);
            TestNotEqual(TEXT("folders differ"), Groups[0].RelativeDir, Groups[1].RelativeDir);
            TestEqual(TEXT("the rename was counted"), Stats.RenamedFolders, 1);
        });

        It("keeps the seed at the root of its own folder when grouping by type", [this]()
        {
            const FName MeshPackage = FindPackageOfClass(UStaticMesh::StaticClass());
            if (MeshPackage.IsNone())
            {
                AddWarning(TEXT("No /Game StaticMesh found; skipping."));
                return;
            }

            AssetDependencyClosure::FSeedClosure Closure;
            Closure.Seed = MeshPackage;
            Closure.Packages.Add(MeshPackage);

            AssetExportLayout::FOptions Options;
            Options.Layout = EExportLayout::FolderPerAsset;
            Options.bGroupDependenciesByType = true;

            AssetExportLayout::FStats Stats;
            const TArray<AssetExportLayout::FGroup> Groups =
                AssetExportLayout::BuildGroups(ReadyRegistry(), Closure.Packages.IsEmpty()
                    ? TArray<AssetDependencyClosure::FSeedClosure>()
                    : TArray<AssetDependencyClosure::FSeedClosure>{ Closure },
                    Options, Stats);

            TestEqual(TEXT("one group"), Groups.Num(), 1);

            // The seed is the subject of its folder. Filing it under StaticMesh/ beside its own
            // dependencies would bury the thing the folder is named after.
            const FString Expected = FPackageName::GetShortName(MeshPackage.ToString());
            TestEqual(TEXT("seed sits at the folder root"), Groups[0].RelativeDir, Expected);
        });
    });

    Describe("Type folders", [this]()
    {
        It("names folders after the type presets, not invented labels", [this]()
        {
            const TMap<FTopLevelAssetPath, FString> FolderByClass = AssetExportLayout::BuildTypeFolderMap(ReadyRegistry());

            const FString* TextureFolder = FolderByClass.Find(UTexture2D::StaticClass()->GetClassPathName());

            if (!TextureFolder)
            {
                AddWarning(TEXT("Texture2D missing from the type folder map; skipping."));
                return;
            }

            // The Types filter in the panel shows "Texture"; the folder a user gets must match the
            // word they filtered by, or the two halves of the tool disagree.
            TestEqual(TEXT("Texture2D lands under Texture"), *TextureFolder, FString(TEXT("Texture")));
        });

        It("returns Other rather than an empty name for an unmapped package", [this]()
        {
            // An empty folder name would silently write into the parent folder and look like the
            // grouping had failed rather than like a type nobody mapped.
            const TMap<FTopLevelAssetPath, FString> FolderByClass = AssetExportLayout::BuildTypeFolderMap(ReadyRegistry());

            const FString Folder = AssetExportLayout::TypeFolderForPackage(
                ReadyRegistry(), FName(TEXT("/Game/NoSuchPackage_AssetUsageAuditTest")), FolderByClass);

            TestEqual(TEXT("falls back to Other"), Folder, FString(TEXT("Other")));
        });
    });

    Describe("Groups reaching the exporter", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("writes each group into its own subfolder", [this]()
        {
            const FName Package = FindPackageOfClass(UTexture2D::StaticClass());
            if (Package.IsNone())
            {
                AddWarning(TEXT("No /Game Texture2D found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("LayoutGroups"));

            // The same package in two groups: it must be written twice, once per folder, and
            // neither copy renamed - they do not collide because they are in different folders.
            FAssetUsageExporter::FExportGroup First;
            First.RelativeDir = TEXT("FolderOne");
            First.Packages.Add(Package);

            FAssetUsageExporter::FExportGroup Second;
            Second.RelativeDir = TEXT("FolderTwo");
            Second.Packages.Add(Package);

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bIncludeExternalPackages = false;

            const FAssetUsageExporter::FResult Result =
                FAssetUsageExporter::ExportPackageGroups({ First, Second }, Options);

            TestTrue(TEXT("succeeded"), Result.bSuccess);
            TestEqual(TEXT("copied once per group"), Result.FilesCopied, 2);
            TestEqual(TEXT("nothing renamed"), Result.FilesRenamed, 0);

            const FString FileName = FPaths::GetCleanFilename(FAssetUsageExporter::ResolvePackageFilePath(Package));

            TestTrue(TEXT("first folder"), IFileManager::Get().FileExists(*FPaths::Combine(ScratchDir, TEXT("FolderOne"), FileName)));
            TestTrue(TEXT("second folder"), IFileManager::Get().FileExists(*FPaths::Combine(ScratchDir, TEXT("FolderTwo"), FileName)));
        });

        It("still writes into the target when a group has no subfolder", [this]()
        {
            const FName Package = FindPackageOfClass(UTexture2D::StaticClass());
            if (Package.IsNone())
            {
                AddWarning(TEXT("No /Game Texture2D found; skipping."));
                return;
            }

            MakeScratchDir(TEXT("LayoutRoot"));

            FAssetUsageExporter::FExportGroup Group;
            Group.Packages.Add(Package);

            FAssetUsageExporter::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.bIncludeExternalPackages = false;

            const FAssetUsageExporter::FResult Result =
                FAssetUsageExporter::ExportPackageGroups({ Group }, Options);

            TestEqual(TEXT("one file"), Result.FilesCopied, 1);

            const FString FileName = FPaths::GetCleanFilename(FAssetUsageExporter::ResolvePackageFilePath(Package));
            TestTrue(TEXT("straight into the target"), IFileManager::Get().FileExists(*FPaths::Combine(ScratchDir, FileName)));
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
