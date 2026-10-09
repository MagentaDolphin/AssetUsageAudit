// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetExportManifest.h"

#include "AssetUsageAuditCoreModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace AssetExportManifest
{
    FResult Write(IAssetRegistry& Registry, const TArray<FAssetUsageExporter::FWrittenFile>& WrittenFiles, const FOptions& Options)
    {
        FResult Result;

        if (Options.TargetDirectory.IsEmpty())
        {
            Result.ErrorMessage = TEXT("No target directory was given.");
            UE_LOG(LogAssetUsageAudit, Error, TEXT("Manifest not written: no target directory."));
            return Result;
        }

        if (WrittenFiles.IsEmpty())
        {
            // Deliberately an error. The usual cause is a caller that forgot bRecordWrittenFiles,
            // and a manifest listing nothing beside a folder full of assets would be taken at face
            // value by whatever reads it next.
            Result.ErrorMessage = TEXT("No written files were recorded; the exporter must run with bRecordWrittenFiles.");
            UE_LOG(LogAssetUsageAudit, Error,
                TEXT("Manifest not written: the exporter recorded no files. Set FOptions::bRecordWrittenFiles."));
            return Result;
        }

        if (Options.Layout == EExportLayout::Migrate)
        {
            Result.ErrorMessage = TEXT("Migrate preserves package paths itself; a manifest would be misleading.");
            UE_LOG(LogAssetUsageAudit, Warning,
                TEXT("Manifest not written: the Migrate layout keeps references intact, so there is nothing to restore."));
            return Result;
        }

        // Where each package ended up, so dependency entries can point at a file in this folder
        // rather than at a package path the reader has no way to resolve.
        TMap<FName, FString> PathByPackage;
        PathByPackage.Reserve(WrittenFiles.Num());

        for (const FAssetUsageExporter::FWrittenFile& Written : WrittenFiles)
        {
            // First writer wins. Under FolderPerAsset the same package is written into several
            // folders; the per-entry "file" field below is the authoritative one for each copy, and
            // this map only serves the "present in this export" test.
            if (!PathByPackage.Contains(Written.PackageName))
            {
                PathByPackage.Add(Written.PackageName, Written.RelativePath);
            }
        }

        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

        Root->SetNumberField(TEXT("schemaVersion"), SchemaVersion);
        Root->SetStringField(TEXT("tool"), TEXT("AssetUsageAudit"));
        Root->SetStringField(TEXT("generatedAt"), FDateTime::UtcNow().ToIso8601());
        Root->SetStringField(TEXT("project"), FApp::GetProjectName());
        Root->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
        Root->SetStringField(TEXT("layout"), LexToString(Options.Layout));
        Root->SetStringField(TEXT("collisionPolicy"),
            Options.CollisionPolicy == EExportCollisionPolicy::Overwrite ? TEXT("Overwrite") : TEXT("Index"));

        Root->SetStringField(TEXT("note"),
            TEXT("Paths are relative to this file's folder. 'package' is the original Unreal package name; ")
            TEXT("restoring an asset means putting its file back at that package path. 'dependencies' lists ")
            TEXT("the packages each asset referenced at export time, whether or not they are in this folder."));

        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Reserve(WrittenFiles.Num());

        TArray<FName> ScratchDependencies;

        for (const FAssetUsageExporter::FWrittenFile& Written : WrittenFiles)
        {
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();

            Entry->SetStringField(TEXT("package"), Written.PackageName.ToString());
            Entry->SetStringField(TEXT("file"), Written.RelativePath);
            Entry->SetBoolField(TEXT("seed"), Options.SeedPackages.Contains(Written.PackageName));

            // Asset name and class, when the registry knows the package. A package can hold several
            // assets; the first is representative and the class is only a hint for a reader.
            TArray<FAssetData> AssetsInPackage;
            Registry.GetAssetsByPackageName(Written.PackageName, AssetsInPackage, /*bIncludeOnlyOnDiskAssets=*/true);

            if (!AssetsInPackage.IsEmpty())
            {
                Entry->SetStringField(TEXT("asset"), AssetsInPackage[0].AssetName.ToString());
                Entry->SetStringField(TEXT("class"), AssetsInPackage[0].AssetClassPath.ToString());
            }

            // Direct dependencies only. The full closure is recoverable by walking these entries,
            // and storing it per asset would repeat most of the file for no extra information.
            ScratchDependencies.Reset();
            Registry.GetDependencies(
                Written.PackageName,
                ScratchDependencies,
                AssetUsageAudit::MakeTraversalCategory(),
                AssetUsageAudit::MakeTraversalQuery());

            TArray<TSharedPtr<FJsonValue>> Dependencies;
            Dependencies.Reserve(ScratchDependencies.Num());

            for (FName Dependency : ScratchDependencies)
            {
                if (Dependency.IsNone())
                {
                    continue;
                }

                TSharedRef<FJsonObject> DependencyEntry = MakeShared<FJsonObject>();
                DependencyEntry->SetStringField(TEXT("package"), Dependency.ToString());

                // Whether the dependency travelled with the export. A reader needs this to tell
                // "restore this link from a file here" from "this link points outside the folder".
                if (const FString* DependencyPath = PathByPackage.Find(Dependency))
                {
                    DependencyEntry->SetStringField(TEXT("file"), *DependencyPath);
                }

                Dependencies.Add(MakeShared<FJsonValueObject>(DependencyEntry));
                ++Result.EdgesRecorded;
            }

            Entry->SetArrayField(TEXT("dependencies"), Dependencies);
            Entries.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Root->SetArrayField(TEXT("assets"), Entries);

        FString Output;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);

        if (!FJsonSerializer::Serialize(Root, Writer))
        {
            Result.ErrorMessage = TEXT("Could not serialise the manifest.");
            UE_LOG(LogAssetUsageAudit, Error, TEXT("Manifest not written: serialisation failed."));
            return Result;
        }

        const FString FilePath = FPaths::Combine(Options.TargetDirectory, FileName);

        if (!FFileHelper::SaveStringToFile(Output, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            Result.ErrorMessage = FString::Printf(TEXT("Could not write '%s'."), *FilePath);
            UE_LOG(LogAssetUsageAudit, Error, TEXT("Manifest not written: could not save '%s'."), *FilePath);
            return Result;
        }

        Result.bSuccess = true;
        Result.FilePath = FilePath;
        Result.EntriesWritten = WrittenFiles.Num();

        UE_LOG(LogAssetUsageAudit, Log,
            TEXT("Manifest written: %d entries, %d dependency edges, '%s'."),
            Result.EntriesWritten, Result.EdgesRecorded, *FilePath);

        return Result;
    }
}
