// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetImporter.h"

#include "AssetUsageAuditCoreModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace AssetImporter
{
    FString FResult::Summarise() const
    {
        TArray<FString> Parts;

        Parts.Add(FString::Printf(TEXT("%d imported"), FilesImported));

        // Each skip reason separately: "12 skipped" tells nobody what to do next, while "12 already
        // exist" and "12 are open in the editor" lead to different actions.
        if (SkippedExisting > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d already existed"), SkippedExisting));
        }

        if (SkippedLoaded > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d are open in the editor"), SkippedLoaded));
        }

        if (SkippedUnresolved > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d had no recoverable path"), SkippedUnresolved));
        }

        if (Errors.Num() > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d failed"), Errors.Num()));
        }

        if (bCancelled)
        {
            Parts.Add(TEXT("cancelled before finishing"));
        }

        return FString::Join(Parts, TEXT(", "));
    }

    namespace Private
    {
        /** Target package for a candidate, applying the fallback folder. None when it must be skipped. */
        FName ResolveTargetPackage(const AssetImportScanner::FCandidate& Candidate, const FOptions& Options)
        {
            if (!Candidate.TargetPackage.IsNone())
            {
                return Candidate.TargetPackage;
            }

            if (Options.UnresolvedDestinationPath.IsEmpty())
            {
                return NAME_None;
            }

            // Keep only the file's own name: the source folder structure meant nothing for this
            // candidate, which is why the path was unrecoverable in the first place.
            const FString AssetName = FPaths::GetBaseFilename(Candidate.SourceFile);

            FString Combined = Options.UnresolvedDestinationPath;
            if (!Combined.EndsWith(TEXT("/")))
            {
                Combined += TEXT("/");
            }
            Combined += AssetName;

            return FPackageName::IsValidLongPackageName(Combined) ? FName(*Combined) : NAME_None;
        }
    }

    FResult Import(IAssetRegistry& Registry, const TArray<AssetImportScanner::FCandidate>& Candidates, const FOptions& Options)
    {
        FResult Result;

        IFileManager& FileManager = IFileManager::Get();

        const int32 Total = Candidates.Num();

        for (int32 Index = 0; Index < Total; ++Index)
        {
            if (Options.OnProgress && !Options.OnProgress(Index, Total))
            {
                Result.bCancelled = true;
                break;
            }

            const AssetImportScanner::FCandidate& Candidate = Candidates[Index];

            const FName TargetPackage = Private::ResolveTargetPackage(Candidate, Options);
            if (TargetPackage.IsNone())
            {
                ++Result.SkippedUnresolved;
                continue;
            }

            // Refuse before touching the disk if the editor is holding this package. Replacing the
            // file underneath a loaded UPackage leaves the session with stale objects that get
            // saved back over the import - a corruption that surfaces much later than the import.
            if (FindPackage(nullptr, *TargetPackage.ToString()))
            {
                ++Result.SkippedLoaded;

                UE_LOG(LogAssetUsageAudit, Warning,
                    TEXT("Not importing '%s': the package is loaded in this editor session. Close the asset and re-run."),
                    *TargetPackage.ToString());
                continue;
            }

            // The extension has to match what the package actually is: a level saved as .uasset
            // will not be found by the engine, which looks for maps by their own extension.
            const bool bIsMap = Candidate.SourceFile.EndsWith(FPackageName::GetMapPackageExtension());

            FString DestinationPath;
            if (!FPackageName::TryConvertLongPackageNameToFilename(
                    TargetPackage.ToString(),
                    DestinationPath,
                    bIsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension()))
            {
                Result.Errors.Add(FString::Printf(
                    TEXT("'%s' does not map to a file path in this project."), *TargetPackage.ToString()));
                continue;
            }

            const bool bTargetOnDisk = FileManager.FileExists(*DestinationPath);

            if (bTargetOnDisk && !Options.bOverwriteExisting)
            {
                ++Result.SkippedExisting;
                continue;
            }

            const FString DestinationDir = FPaths::GetPath(DestinationPath);
            if (!FileManager.DirectoryExists(*DestinationDir) && !FileManager.MakeDirectory(*DestinationDir, true))
            {
                Result.Errors.Add(FString::Printf(TEXT("Could not create '%s'."), *DestinationDir));
                continue;
            }

            // Perforce-primary projects keep unopened files read-only, so an existing target is very likely read-only.
            // Without clearing the flag the copy fails with an error that reads like a permissions
            // problem rather than "the file is not checked out".
            if (bTargetOnDisk && FileManager.IsReadOnly(*DestinationPath))
            {
                FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*DestinationPath, false);
            }

            if (FileManager.Copy(*DestinationPath, *Candidate.SourceFile, /*bReplace=*/true) != COPY_OK)
            {
                Result.Errors.Add(FString::Printf(
                    TEXT("Failed to copy '%s' to '%s'."), *Candidate.SourceFile, *DestinationPath));
                continue;
            }

            ++Result.FilesImported;
            Result.ImportedFiles.Add(DestinationPath);
        }

        // Without this the files sit on disk and the Content Browser shows nothing, which reads as
        // an import that silently did nothing.
        if (!Result.ImportedFiles.IsEmpty())
        {
            Registry.ScanFilesSynchronous(Result.ImportedFiles, /*bForceRescan=*/true);
        }

        Result.bSuccess = Result.Errors.IsEmpty() && !Result.bCancelled;

        UE_LOG(LogAssetUsageAudit, Log, TEXT("Import: %s"), *Result.Summarise());

        for (const FString& Error : Result.Errors)
        {
            UE_LOG(LogAssetUsageAudit, Warning, TEXT("  %s"), *Error);
        }

        return Result;
    }
}
