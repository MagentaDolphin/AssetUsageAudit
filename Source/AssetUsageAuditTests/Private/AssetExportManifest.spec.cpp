// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetExportManifest.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(AssetExportManifestSpec,
    "AssetUsageAudit.ExportManifest",
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

    /** Parse the manifest the writer just produced. Null when it is missing or malformed. */
    TSharedPtr<FJsonObject> ReadManifest() const
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(ScratchDir, AssetExportManifest::FileName)))
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);

        return FJsonSerializer::Deserialize(Reader, Root) ? Root : nullptr;
    }

END_DEFINE_SPEC(AssetExportManifestSpec)

void AssetExportManifestSpec::Define()
{
    Describe("Refusing to write a misleading manifest", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("fails when the exporter recorded nothing", [this]()
        {
            // The usual cause is a caller that forgot bRecordWrittenFiles. An empty manifest beside
            // a folder full of assets would be believed by whatever reads it next.
            MakeScratchDir(TEXT("ManifestEmpty"));

            AddExpectedError(TEXT("the exporter recorded no files"), EAutomationExpectedErrorFlags::Contains, 1);

            AssetExportManifest::FOptions Options;
            Options.TargetDirectory = ScratchDir;

            const AssetExportManifest::FResult Result =
                AssetExportManifest::Write(ReadyRegistry(), {}, Options);

            TestFalse(TEXT("not successful"), Result.bSuccess);
            TestTrue(TEXT("says why"), !Result.ErrorMessage.IsEmpty());
            TestFalse(TEXT("wrote no file"), IFileManager::Get().FileExists(
                *FPaths::Combine(ScratchDir, AssetExportManifest::FileName)));
        });

        It("fails when no target directory is given", [this]()
        {
            AddExpectedError(TEXT("no target directory"), EAutomationExpectedErrorFlags::Contains, 1);

            AssetExportManifest::FOptions Options;

            TArray<FAssetUsageExporter::FWrittenFile> Written;
            Written.AddDefaulted_GetRef().PackageName = FName(TEXT("/Game/Whatever"));

            const AssetExportManifest::FResult Result =
                AssetExportManifest::Write(ReadyRegistry(), Written, Options);

            TestFalse(TEXT("not successful"), Result.bSuccess);
        });

        It("refuses under the Migrate layout", [this]()
        {
            // Migrate keeps package paths intact, so there is nothing to restore. A manifest there
            // would imply the references had been broken.
            MakeScratchDir(TEXT("ManifestMigrate"));

            AssetExportManifest::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.Layout = EExportLayout::Migrate;

            TArray<FAssetUsageExporter::FWrittenFile> Written;
            FAssetUsageExporter::FWrittenFile& Entry = Written.AddDefaulted_GetRef();
            Entry.PackageName = FName(TEXT("/Game/Whatever"));
            Entry.RelativePath = TEXT("Whatever.uasset");

            const AssetExportManifest::FResult Result =
                AssetExportManifest::Write(ReadyRegistry(), Written, Options);

            TestFalse(TEXT("not successful"), Result.bSuccess);
            TestFalse(TEXT("wrote no file"), IFileManager::Get().FileExists(
                *FPaths::Combine(ScratchDir, AssetExportManifest::FileName)));
        });
    });

    Describe("The manifest it writes", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("is valid JSON carrying the schema version and the layout", [this]()
        {
            MakeScratchDir(TEXT("ManifestBasic"));

            AssetExportManifest::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.Layout = EExportLayout::FolderPerAsset;
            Options.CollisionPolicy = EExportCollisionPolicy::Index;

            TArray<FAssetUsageExporter::FWrittenFile> Written;
            FAssetUsageExporter::FWrittenFile& Entry = Written.AddDefaulted_GetRef();
            Entry.PackageName = FName(TEXT("/Game/SomeAsset_AssetUsageAuditTest"));
            Entry.RelativePath = TEXT("SomeAsset/SomeAsset.uasset");

            const AssetExportManifest::FResult Result =
                AssetExportManifest::Write(ReadyRegistry(), Written, Options);

            TestTrue(TEXT("successful"), Result.bSuccess);

            const TSharedPtr<FJsonObject> Root = ReadManifest();
            TestTrue(TEXT("parses as JSON"), Root.IsValid());

            if (!Root.IsValid())
            {
                return;
            }

            TestEqual(TEXT("schema version"),
                static_cast<int32>(Root->GetNumberField(TEXT("schemaVersion"))),
                AssetExportManifest::SchemaVersion);

            TestEqual(TEXT("records the layout"), Root->GetStringField(TEXT("layout")), FString(TEXT("FolderPerAsset")));
            TestEqual(TEXT("records the naming policy"), Root->GetStringField(TEXT("collisionPolicy")), FString(TEXT("Index")));
            TestTrue(TEXT("has an assets array"), Root->HasField(TEXT("assets")));
        });

        It("records the path that was actually written, not the intended one", [this]()
        {
            // The case the manifest exists for: the collision policy renamed the file, so a manifest
            // built from the package name alone would point at a file that is not there.
            MakeScratchDir(TEXT("ManifestRenamed"));

            AssetExportManifest::FOptions Options;
            Options.TargetDirectory = ScratchDir;

            TArray<FAssetUsageExporter::FWrittenFile> Written;
            FAssetUsageExporter::FWrittenFile& Entry = Written.AddDefaulted_GetRef();
            Entry.PackageName = FName(TEXT("/Game/SM_Rock_AssetUsageAuditTest"));
            Entry.RelativePath = TEXT("SM_Rock_AssetUsageAuditTest_2.uasset");

            AssetExportManifest::Write(ReadyRegistry(), Written, Options);

            const TSharedPtr<FJsonObject> Root = ReadManifest();
            TestTrue(TEXT("parses"), Root.IsValid());

            if (!Root.IsValid())
            {
                return;
            }

            const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
            TestTrue(TEXT("has assets"), Root->TryGetArrayField(TEXT("assets"), Assets));

            if (!Assets || Assets->Num() != 1)
            {
                AddError(TEXT("expected exactly one asset entry"));
                return;
            }

            const TSharedPtr<FJsonObject> First = (*Assets)[0]->AsObject();
            TestEqual(TEXT("original package name"), First->GetStringField(TEXT("package")),
                FString(TEXT("/Game/SM_Rock_AssetUsageAuditTest")));
            TestEqual(TEXT("the renamed file"), First->GetStringField(TEXT("file")),
                FString(TEXT("SM_Rock_AssetUsageAuditTest_2.uasset")));
        });

        It("separates the assets that were ticked from those pulled in", [this]()
        {
            // "Restore what I exported" and "restore everything in this folder" are different
            // requests, and only the manifest can tell them apart after the fact.
            MakeScratchDir(TEXT("ManifestSeeds"));

            const FName SeedPackage(TEXT("/Game/Seed_AssetUsageAuditTest"));
            const FName DependencyPackage(TEXT("/Game/Dependency_AssetUsageAuditTest"));

            AssetExportManifest::FOptions Options;
            Options.TargetDirectory = ScratchDir;
            Options.SeedPackages = { SeedPackage };

            TArray<FAssetUsageExporter::FWrittenFile> Written;

            FAssetUsageExporter::FWrittenFile& SeedEntry = Written.AddDefaulted_GetRef();
            SeedEntry.PackageName = SeedPackage;
            SeedEntry.RelativePath = TEXT("Seed.uasset");

            FAssetUsageExporter::FWrittenFile& DependencyEntry = Written.AddDefaulted_GetRef();
            DependencyEntry.PackageName = DependencyPackage;
            DependencyEntry.RelativePath = TEXT("Dependency.uasset");

            AssetExportManifest::Write(ReadyRegistry(), Written, Options);

            const TSharedPtr<FJsonObject> Root = ReadManifest();
            if (!Root.IsValid())
            {
                AddError(TEXT("manifest did not parse"));
                return;
            }

            const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
            Root->TryGetArrayField(TEXT("assets"), Assets);

            if (!Assets || Assets->Num() != 2)
            {
                AddError(TEXT("expected two asset entries"));
                return;
            }

            int32 SeedCount = 0;
            for (const TSharedPtr<FJsonValue>& Value : *Assets)
            {
                if (Value->AsObject()->GetBoolField(TEXT("seed")))
                {
                    ++SeedCount;
                }
            }

            TestEqual(TEXT("exactly one entry is a seed"), SeedCount, 1);
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
