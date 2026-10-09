// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetImportScanner.h"

#include "AssetExportManifest.h"
#include "AssetUsageAuditCoreModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/PackageFileSummary.h"

namespace AssetImportScanner
{
    const TCHAR* LexToString(EPackageNameSource Source)
    {
        switch (Source)
        {
            case EPackageNameSource::Manifest:        return TEXT("Manifest");
            case EPackageNameSource::PackageHeader:   return TEXT("Package header");
            case EPackageNameSource::FolderStructure: return TEXT("Folder structure");
            case EPackageNameSource::Unresolved:      return TEXT("Unresolved");
        }

        return TEXT("Unresolved");
    }

    FString FStats::Summarise() const
    {
        TArray<FString> Parts;

        Parts.Add(FString::Printf(TEXT("%d file(s)"), FilesFound));

        if (FromManifest > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d from the manifest"), FromManifest));
        }

        if (FromPackageHeader > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d from package headers"), FromPackageHeader));
        }

        if (FromFolderStructure > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d from the folder structure"), FromFolderStructure));
        }

        if (Unresolved > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d with no recoverable path"), Unresolved));
        }

        if (WouldOverwrite > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d would overwrite an existing asset"), WouldOverwrite));
        }

        return FString::Join(Parts, TEXT(", "));
    }

    FString ReadPackageNameFromFile(const FString& FilePath)
    {
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
        if (!Reader)
        {
            return FString();
        }

        FPackageFileSummary Summary;

        // Serialising the summary validates the magic number and version itself; a file that is not
        // a package leaves the tag unset rather than throwing.
        *Reader << Summary;

        if (Reader->IsError() || Summary.Tag != PACKAGE_FILE_TAG)
        {
            return FString();
        }

        // The engine's own AssetHeaderPatcher treats both of these as "absent" and falls back to the
        // file name, so neither is an error worth reporting - just an answer this function cannot
        // give for this file.
        if (Summary.PackageName.IsEmpty() || Summary.PackageName.Equals(TEXT("None")))
        {
            return FString();
        }

        return Summary.PackageName;
    }

    namespace Private
    {
        /** Read the manifest, mapping relative file path -> original package name. */
        bool LoadManifest(const FString& SourceDirectory, TMap<FString, FName>& OutPackageByRelativePath,
            TMap<FName, TArray<FName>>& OutDependenciesByPackage)
        {
            const FString ManifestPath = FPaths::Combine(SourceDirectory, AssetExportManifest::FileName);

            FString Text;
            if (!FFileHelper::LoadFileToString(Text, *ManifestPath))
            {
                return false;
            }

            TSharedPtr<FJsonObject> Root;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);

            if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
            {
                UE_LOG(LogAssetUsageAudit, Warning,
                    TEXT("Found '%s' but could not parse it; falling back to package headers."), *ManifestPath);
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
            if (!Root->TryGetArrayField(TEXT("assets"), Assets) || !Assets)
            {
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Assets)
            {
                const TSharedPtr<FJsonObject> Entry = Value->AsObject();
                if (!Entry.IsValid())
                {
                    continue;
                }

                FString PackageName;
                FString RelativeFile;

                if (!Entry->TryGetStringField(TEXT("package"), PackageName)
                    || !Entry->TryGetStringField(TEXT("file"), RelativeFile))
                {
                    continue;
                }

                // Normalise so a manifest written on Windows matches a scan using forward slashes.
                RelativeFile.ReplaceInline(TEXT("\\"), TEXT("/"));

                OutPackageByRelativePath.Add(RelativeFile, FName(*PackageName));

                const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
                if (Entry->TryGetArrayField(TEXT("dependencies"), Dependencies) && Dependencies)
                {
                    TArray<FName>& List = OutDependenciesByPackage.FindOrAdd(FName(*PackageName));

                    for (const TSharedPtr<FJsonValue>& DependencyValue : *Dependencies)
                    {
                        const TSharedPtr<FJsonObject> DependencyEntry = DependencyValue->AsObject();
                        FString DependencyPackage;

                        if (DependencyEntry.IsValid() && DependencyEntry->TryGetStringField(TEXT("package"), DependencyPackage))
                        {
                            List.AddUnique(FName(*DependencyPackage));
                        }
                    }
                }
            }

            return true;
        }

        /**
         * Package path implied by a file's position under the import root.
         *
         * Correct for a MirrorTree export, which writes Game/Art/SM_Rock.uasset, and a guess
         * for anything else. Returns None when the result would not be a valid package name.
         */
        FName PackageFromFolderStructure(const FString& RelativePath)
        {
            FString Stem = FPaths::Combine(FPaths::GetPath(RelativePath), FPaths::GetBaseFilename(RelativePath));
            Stem.ReplaceInline(TEXT("\\"), TEXT("/"));

            if (Stem.IsEmpty())
            {
                return NAME_None;
            }

            // The mirror layout writes the mount point as a plain folder: /Game/X becomes Game/X.
            // Anything else is treated as living under /Game, which is the only mount point an
            // import can safely target.
            FString PackagePath = Stem.StartsWith(TEXT("Game/"))
                ? TEXT("/") + Stem
                : TEXT("/Game/") + Stem;

            return FPackageName::IsValidLongPackageName(PackagePath) ? FName(*PackagePath) : NAME_None;
        }
    }

    TArray<FCandidate> Scan(IAssetRegistry& Registry, const FOptions& Options, FStats& OutStats)
    {
        OutStats = FStats();

        TArray<FCandidate> Candidates;

        if (Options.SourceDirectory.IsEmpty() || !IFileManager::Get().DirectoryExists(*Options.SourceDirectory))
        {
            UE_LOG(LogAssetUsageAudit, Error,
                TEXT("Import scan aborted: '%s' is not a folder."), *Options.SourceDirectory);
            return Candidates;
        }

        TMap<FString, FName> PackageByRelativePath;
        TMap<FName, TArray<FName>> DependenciesByPackage;

        OutStats.bManifestFound = Private::LoadManifest(Options.SourceDirectory, PackageByRelativePath, DependenciesByPackage);

        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *Options.SourceDirectory, TEXT("*.uasset"), true, false);

        TArray<FString> Maps;
        IFileManager::Get().FindFilesRecursive(Maps, *Options.SourceDirectory, TEXT("*.umap"), true, false);
        Files.Append(MoveTemp(Maps));

        Files.Sort();

        Candidates.Reserve(Files.Num());

        // Everything this folder can supply, so a dependency already travelling with the export is
        // not reported missing.
        TSet<FName> PackagesInFolder;
        PackagesInFolder.Reserve(Files.Num());

        for (const FString& AbsoluteFile : Files)
        {
            FString Relative = AbsoluteFile;
            FPaths::MakePathRelativeTo(Relative, *(Options.SourceDirectory / TEXT("")));
            Relative.ReplaceInline(TEXT("\\"), TEXT("/"));

            FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
            Candidate.SourceFile = AbsoluteFile;
            Candidate.RelativePath = Relative;

            if (const FName* FromManifest = PackageByRelativePath.Find(Relative))
            {
                Candidate.TargetPackage = *FromManifest;
                Candidate.NameSource = EPackageNameSource::Manifest;
                ++OutStats.FromManifest;
            }
            else
            {
                const FString FromHeader = ReadPackageNameFromFile(AbsoluteFile);

                if (!FromHeader.IsEmpty() && FPackageName::IsValidLongPackageName(FromHeader))
                {
                    Candidate.TargetPackage = FName(*FromHeader);
                    Candidate.NameSource = EPackageNameSource::PackageHeader;
                    ++OutStats.FromPackageHeader;
                }
                else
                {
                    const FName FromFolder = Private::PackageFromFolderStructure(Relative);

                    if (!FromFolder.IsNone())
                    {
                        Candidate.TargetPackage = FromFolder;
                        Candidate.NameSource = EPackageNameSource::FolderStructure;
                        ++OutStats.FromFolderStructure;
                    }
                    else
                    {
                        Candidate.NameSource = EPackageNameSource::Unresolved;
                        ++OutStats.Unresolved;
                    }
                }
            }

            if (!Candidate.TargetPackage.IsNone())
            {
                PackagesInFolder.Add(Candidate.TargetPackage);

                if (const TArray<FName>* Dependencies = DependenciesByPackage.Find(Candidate.TargetPackage))
                {
                    Candidate.Dependencies = *Dependencies;
                }
            }

            ++OutStats.FilesFound;
        }

        // Overwrite and missing-dependency checks need the complete folder contents, so they run
        // once everything has a target rather than while the list is still being built.
        for (FCandidate& Candidate : Candidates)
        {
            if (Candidate.TargetPackage.IsNone())
            {
                continue;
            }

            TArray<FAssetData> Existing;
            Registry.GetAssetsByPackageName(Candidate.TargetPackage, Existing, /*bIncludeOnlyOnDiskAssets=*/true);

            Candidate.bTargetExists = !Existing.IsEmpty();
            if (Candidate.bTargetExists)
            {
                ++OutStats.WouldOverwrite;
            }

            for (FName Dependency : Candidate.Dependencies)
            {
                if (PackagesInFolder.Contains(Dependency))
                {
                    continue;
                }

                // Already in the project counts as present: importing a mesh into the project it
                // came from does not need its textures brought along.
                TArray<FAssetData> InProject;
                Registry.GetAssetsByPackageName(Dependency, InProject, /*bIncludeOnlyOnDiskAssets=*/true);

                if (InProject.IsEmpty())
                {
                    ++Candidate.MissingDependencies;
                }
            }
        }

        UE_LOG(LogAssetUsageAudit, Log, TEXT("Import scan of '%s': %s%s"),
            *Options.SourceDirectory,
            *OutStats.Summarise(),
            OutStats.bManifestFound ? TEXT("") : TEXT(" (no manifest found)"));

        return Candidates;
    }
}
