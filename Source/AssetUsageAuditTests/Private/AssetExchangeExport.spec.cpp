// MagentaDolphin 2026. Asset Usage Audit.

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
 * The second export mode: convert assets to interchange formats instead of copying .uasset files.
 *
 * Format selection is a pure function over a class path and a map, so it is tested directly. The
 * conversion itself needs a loaded UObject and a registered UExporter, so those cases run against
 * real project content and warn rather than fail when the content is not there.
 */
BEGIN_DEFINE_SPEC(AssetExchangeExportSpec,
    "AssetUsageAudit.ExchangeExport",
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

    /** First /Game asset of the given class, as an export item. Sorted for reproducibility. */
    static bool FindItemOfClass(UClass* Class, FAssetUsageExporter::FExportItem& OutItem)
    {
        FARFilter Filter;
        Filter.ClassPaths.Add(Class->GetClassPathName());
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.bIncludeOnlyOnDiskAssets = true;

        TArray<FAssetData> Found;

        ReadyRegistry().EnumerateAssets(Filter, [&Found](const FAssetData& AssetData)
        {
            Found.Add(AssetData);
            return Found.Num() < 64;
        });

        if (Found.IsEmpty())
        {
            return false;
        }

        Found.Sort([](const FAssetData& A, const FAssetData& B)
        {
            return A.PackageName.LexicalLess(B.PackageName);
        });

        OutItem.PackageName = Found[0].PackageName;
        OutItem.AssetName = Found[0].AssetName;
        OutItem.ClassPath = Found[0].AssetClassPath;
        return true;
    }

    int32 CountFilesInScratch(const TCHAR* Wildcard = TEXT("*.*")) const
    {
        TArray<FString> Found;
        IFileManager::Get().FindFilesRecursive(Found, *ScratchDir, Wildcard, true, false);
        return Found.Num();
    }

END_DEFINE_SPEC(AssetExchangeExportSpec)

void AssetExchangeExportSpec::Define()
{
    Describe("FindFormatForClass", [this]()
    {
        It("matches a class named directly in the map", [this]()
        {
            const TMap<FString, FString> Map = { { TEXT("StaticMesh"), TEXT("fbx") } };

            TestEqual(TEXT("direct hit"),
                FAssetUsageExporter::FindFormatForClass(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh")), Map),
                FString(TEXT("fbx")));
        });

        It("returns nothing for a class with no entry", [this]()
        {
            const TMap<FString, FString> Map = { { TEXT("StaticMesh"), TEXT("fbx") } };

            TestTrue(TEXT("no format"),
                FAssetUsageExporter::FindFormatForClass(
                    FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")), Map).IsEmpty());
        });

        It("returns nothing for an invalid class path rather than guessing", [this]()
        {
            TestTrue(TEXT("no format"),
                FAssetUsageExporter::FindFormatForClass(FTopLevelAssetPath(), FAssetUsageExporter::GetDefaultFormatByClass()).IsEmpty());
        });

        It("inherits a format from a base class", [this]()
        {
            // One "Texture" entry has to cover Texture2D, or every texture subclass needs its own
            // line in the settings. Requires the class to be findable, which Engine classes are.
            const TMap<FString, FString> Map = { { TEXT("Texture"), TEXT("png") } };

            TestEqual(TEXT("Texture2D inherits from Texture"),
                FAssetUsageExporter::FindFormatForClass(UTexture2D::StaticClass()->GetClassPathName(), Map),
                FString(TEXT("png")));
        });

        It("prefers the exact entry over an inherited one", [this]()
        {
            const TMap<FString, FString> Map = {
                { TEXT("Texture"),   TEXT("hdr") },
                { TEXT("Texture2D"), TEXT("png") }
            };

            TestEqual(TEXT("exact wins"),
                FAssetUsageExporter::FindFormatForClass(UTexture2D::StaticClass()->GetClassPathName(), Map),
                FString(TEXT("png")));
        });
    });

    Describe("The default format map", [this]()
    {
        It("covers the asset types artists actually export", [this]()
        {
            const TMap<FString, FString> Defaults = FAssetUsageExporter::GetDefaultFormatByClass();

            TestEqual(TEXT("static meshes as fbx"), Defaults.FindRef(TEXT("StaticMesh")), FString(TEXT("fbx")));
            TestEqual(TEXT("textures as png"), Defaults.FindRef(TEXT("Texture2D")), FString(TEXT("png")));
            TestEqual(TEXT("sounds as wav"), Defaults.FindRef(TEXT("SoundWave")), FString(TEXT("wav")));
        });

        It("leaves out Blueprint, which has no interchange format", [this]()
        {
            // A Blueprint entry would promise a conversion the engine cannot perform. Skipping it
            // with a counted reason is honest; producing a .t3d text dump labelled "exported" is not.
            const TMap<FString, FString> Defaults = FAssetUsageExporter::GetDefaultFormatByClass();

            TestFalse(TEXT("no Blueprint entry"), Defaults.Contains(TEXT("Blueprint")));
        });
    });

    Describe("Converting without a usable target", [this]()
    {
        It("fails cleanly when no target directory is given", [this]()
        {
            AddExpectedError(TEXT("Conversion aborted: no target directory was given"), EAutomationExpectedErrorFlags::Contains, 1);

            FAssetUsageExporter::FExchangeOptions Options;

            FAssetUsageExporter::FExportItem Item;
            Item.PackageName = FName(TEXT("/Game/Whatever"));
            Item.AssetName = FName(TEXT("Whatever"));

            const FAssetUsageExporter::FExchangeResult Result =
                FAssetUsageExporter::ExportConvertedAssets({ Item }, Options);

            TestFalse(TEXT("not successful"), Result.bSuccess);
            TestEqual(TEXT("wrote nothing"), Result.FilesWritten, 0);
        });

        It("counts an asset with no configured format without loading it", [this]()
        {
            MakeScratchDir(TEXT("NoFormat"));

            FAssetUsageExporter::FExchangeOptions Options;
            Options.TargetDirectory = ScratchDir;

            FAssetUsageExporter::FExportItem Item;
            Item.PackageName = FName(TEXT("/Game/NoSuchAsset_AssetUsageAuditTest"));
            Item.AssetName = FName(TEXT("NoSuchAsset_AssetUsageAuditTest"));
            Item.ClassPath = FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint"));

            const FAssetUsageExporter::FExchangeResult Result =
                FAssetUsageExporter::ExportConvertedAssets({ Item }, Options);

            TestEqual(TEXT("skipped for want of a format"), Result.SkippedNoFormat, 1);
            TestEqual(TEXT("never attempted a load"), Result.SkippedNotLoaded, 0);
            TestEqual(TEXT("no errors"), Result.Errors.Num(), 0);

            RemoveScratchDir();
        });

        It("stops when the progress callback asks it to", [this]()
        {
            MakeScratchDir(TEXT("ConvertCancel"));

            FAssetUsageExporter::FExchangeOptions Options;
            Options.TargetDirectory = ScratchDir;

            int32 Calls = 0;
            Options.OnProgress = [&Calls](int32, int32) { ++Calls; return false; };

            FAssetUsageExporter::FExportItem Item;
            Item.PackageName = FName(TEXT("/Game/A_AssetUsageAuditTest"));
            Item.AssetName = FName(TEXT("A_AssetUsageAuditTest"));

            const FAssetUsageExporter::FExchangeResult Result =
                FAssetUsageExporter::ExportConvertedAssets({ Item, Item }, Options);

            TestTrue(TEXT("flagged as cancelled"), Result.bCancelled);
            TestFalse(TEXT("not reported successful"), Result.bSuccess);
            TestEqual(TEXT("stopped at the first item"), Calls, 1);

            RemoveScratchDir();
        });
    });

    Describe("Converting real project content", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("writes a PNG for a texture", [this]()
        {
            FAssetUsageExporter::FExportItem Item;
            if (!FindItemOfClass(UTexture2D::StaticClass(), Item))
            {
                AddWarning(TEXT("No Texture2D found under /Game; skipping."));
                return;
            }

            AddInfo(FString::Printf(TEXT("Converting '%s'."), *Item.PackageName.ToString()));

            MakeScratchDir(TEXT("ConvertTexture"));

            FAssetUsageExporter::FExchangeOptions Options;
            Options.TargetDirectory = ScratchDir;

            const FAssetUsageExporter::FExchangeResult Result =
                FAssetUsageExporter::ExportConvertedAssets({ Item }, Options);

            TestTrue(TEXT("succeeded"), Result.bSuccess);
            TestEqual(TEXT("one file written"), Result.FilesWritten, 1);

            const FString Expected = FPaths::Combine(ScratchDir, Item.AssetName.ToString() + TEXT(".png"));
            TestTrue(TEXT("the png exists"), IFileManager::Get().FileExists(*Expected));

            // An exporter can create the file and write nothing into it; a zero-byte PNG would pass
            // an existence check and be useless to whoever opens the folder.
            TestTrue(TEXT("the png is not empty"), IFileManager::Get().FileSize(*Expected) > 0);
        });

        It("writes an FBX for a static mesh", [this]()
        {
            FAssetUsageExporter::FExportItem Item;
            if (!FindItemOfClass(UStaticMesh::StaticClass(), Item))
            {
                AddWarning(TEXT("No StaticMesh found under /Game; skipping."));
                return;
            }

            AddInfo(FString::Printf(TEXT("Converting '%s'."), *Item.PackageName.ToString()));

            MakeScratchDir(TEXT("ConvertMesh"));

            FAssetUsageExporter::FExchangeOptions Options;
            Options.TargetDirectory = ScratchDir;

            const FAssetUsageExporter::FExchangeResult Result =
                FAssetUsageExporter::ExportConvertedAssets({ Item }, Options);

            TestTrue(TEXT("succeeded"), Result.bSuccess);
            TestEqual(TEXT("one file written"), Result.FilesWritten, 1);

            const FString Expected = FPaths::Combine(ScratchDir, Item.AssetName.ToString() + TEXT(".fbx"));
            TestTrue(TEXT("the fbx exists"), IFileManager::Get().FileExists(*Expected));
            TestTrue(TEXT("the fbx is not empty"), IFileManager::Get().FileSize(*Expected) > 0);
        });

        It("indexes the second conversion of the same asset", [this]()
        {
            FAssetUsageExporter::FExportItem Item;
            if (!FindItemOfClass(UTexture2D::StaticClass(), Item))
            {
                AddWarning(TEXT("No Texture2D found under /Game; skipping."));
                return;
            }

            MakeScratchDir(TEXT("ConvertIndex"));

            FAssetUsageExporter::FExchangeOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.CollisionPolicy = EExportCollisionPolicy::Index;

            FAssetUsageExporter::ExportConvertedAssets({ Item }, Options);
            const FAssetUsageExporter::FExchangeResult Second =
                FAssetUsageExporter::ExportConvertedAssets({ Item }, Options);

            TestEqual(TEXT("second run renamed"), Second.FilesRenamed, 1);
            TestEqual(TEXT("two files on disk"), CountFilesInScratch(TEXT("*.png")), 2);

            const FString Indexed = FPaths::Combine(ScratchDir, Item.AssetName.ToString() + TEXT("_1.png"));
            TestTrue(TEXT("_1 present"), IFileManager::Get().FileExists(*Indexed));
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
