// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageExporter.h"

#include "AssetExportNaming.h"
#include "AssetUsageAuditCoreModule.h"
#include "AssetUsagePaths.h"
#include "AssetExportTask.h"
#include "Engine/Level.h"
#include "Exporters/Exporter.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

FString FAssetUsageExporter::FResult::Summarise() const
{
    TArray<FString> Parts;

    Parts.Add(FString::Printf(TEXT("%d copied"), FilesCopied));

    if (FilesRenamed > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d renamed to avoid a collision"), FilesRenamed));
    }

    if (FilesOverwritten > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d overwritten"), FilesOverwritten));
    }

    if (ExternalPackagesCopied > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d external actor packages"), ExternalPackagesCopied));
    }

    if (FilesMissingOnDisk > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d had no file on disk"), FilesMissingOnDisk));
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

FString FAssetUsageExporter::FExchangeResult::Summarise() const
{
    TArray<FString> Parts;

    Parts.Add(FString::Printf(TEXT("%d written"), FilesWritten));

    if (FilesRenamed > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d renamed to avoid a collision"), FilesRenamed));
    }

    // Each skip reason is reported separately on purpose. "17 skipped" tells a user nothing, while
    // "17 have no configured format" points straight at the settings they need to change.
    if (SkippedNoFormat > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d had no configured format"), SkippedNoFormat));
    }

    if (SkippedNoExporter > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d had no exporter for that format"), SkippedNoExporter));
    }

    if (SkippedNotLoaded > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d could not be loaded"), SkippedNotLoaded));
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

TMap<FString, FString> FAssetUsageExporter::GetDefaultFormatByClass()
{
    // Only classes the engine actually ships a UExporter for. A plausible-looking entry with no
    // exporter behind it turns "nothing came out" into a mystery, so anything unverified is left
    // out and reported as SkippedNoFormat instead.
    //
    // Keys are class short names and lookup walks up the hierarchy, so MaterialInterface covers
    // material instances and Texture covers every 2D texture subclass.
    return {
        { TEXT("StaticMesh"),     TEXT("fbx") },
        { TEXT("SkeletalMesh"),   TEXT("fbx") },
        { TEXT("AnimSequence"),   TEXT("fbx") },
        { TEXT("Texture2D"),      TEXT("png") },
        { TEXT("TextureCube"),    TEXT("hdr") },
        { TEXT("SoundWave"),      TEXT("wav") },
        { TEXT("DataTable"),      TEXT("csv") },
        { TEXT("CurveTable"),     TEXT("csv") },
        { TEXT("FontFace"),       TEXT("ttf") },
        { TEXT("World"),          TEXT("fbx") }
    };
}

FString FAssetUsageExporter::FindFormatForClass(const FTopLevelAssetPath& ClassPath, const TMap<FString, FString>& FormatByClass)
{
    if (!ClassPath.IsValid())
    {
        return FString();
    }

    const FString ShortName = ClassPath.GetAssetName().ToString();

    if (const FString* Direct = FormatByClass.Find(ShortName))
    {
        return *Direct;
    }

    // Walk the hierarchy so one entry covers a family. FindObject rather than LoadObject: a native
    // class is already loaded, and a Blueprint class we cannot find has no exporter anyway.
    UClass* Class = FindObject<UClass>(ClassPath);

    for (UClass* Super = Class ? Class->GetSuperClass() : nullptr; Super; Super = Super->GetSuperClass())
    {
        if (const FString* Found = FormatByClass.Find(Super->GetName()))
        {
            return *Found;
        }
    }

    return FString();
}

FAssetUsageExporter::FExchangeResult FAssetUsageExporter::ExportConvertedAssets(const TArray<FExportItem>& Items, const FExchangeOptions& Options)
{
    FExchangeResult Result;

    if (Options.TargetDirectory.IsEmpty())
    {
        Result.Errors.Add(TEXT("No target directory was given."));
        UE_LOG(LogAssetUsageAudit, Error, TEXT("Conversion aborted: no target directory was given."));
        return Result;
    }

    IFileManager& FileManager = IFileManager::Get();

    if (!FileManager.DirectoryExists(*Options.TargetDirectory)
        && !FileManager.MakeDirectory(*Options.TargetDirectory, /*Tree=*/true))
    {
        Result.Errors.Add(FString::Printf(TEXT("Could not create '%s'."), *Options.TargetDirectory));
        UE_LOG(LogAssetUsageAudit, Error, TEXT("Conversion aborted: could not create target directory '%s'."), *Options.TargetDirectory);
        return Result;
    }

    const TMap<FString, FString>& FormatByClass = Options.FormatByClass.Num() > 0
        ? Options.FormatByClass
        : GetDefaultFormatByClass();

    const int32 Total = Items.Num();
    int32 LoadedSinceCollect = 0;

    for (int32 Index = 0; Index < Total; ++Index)
    {
        if (Options.OnProgress && !Options.OnProgress(Index, Total))
        {
            Result.bCancelled = true;
            break;
        }

        const FExportItem& Item = Items[Index];

        const FString Extension = FindFormatForClass(Item.ClassPath, FormatByClass);
        if (Extension.IsEmpty())
        {
            // Decided before loading anything: there is no point paying to load a Blueprint just to
            // discover the engine cannot write it to a file.
            ++Result.SkippedNoFormat;
            continue;
        }

        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *Item.PackageName.ToString(), *Item.AssetName.ToString());

        UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath);
        if (!Asset)
        {
            ++Result.SkippedNotLoaded;
            UE_LOG(LogAssetUsageAudit, Warning, TEXT("Could not load '%s' for conversion."), *ObjectPath);
            continue;
        }

        FString DestinationDir = Options.TargetDirectory;

        if (!Options.bFlatten)
        {
            FString Relative = FPaths::GetPath(Item.PackageName.ToString());
            Relative.RemoveFromStart(TEXT("/"));
            DestinationDir = FPaths::Combine(Options.TargetDirectory, Relative);

            if (!FileManager.DirectoryExists(*DestinationDir) && !FileManager.MakeDirectory(*DestinationDir, true))
            {
                Result.Errors.Add(FString::Printf(TEXT("Could not create '%s'."), *DestinationDir));
                continue;
            }
        }

        const FString DesiredFileName = FString::Printf(TEXT("%s.%s"), *Item.AssetName.ToString(), *Extension);

        const FString FinalFileName = AssetExportNaming::ResolveCollision(
            DesiredFileName,
            Options.CollisionPolicy,
            [&FileManager, &DestinationDir](const FString& Candidate)
            {
                return FileManager.FileExists(*FPaths::Combine(DestinationDir, Candidate));
            });

        const FString DestinationPath = FPaths::Combine(DestinationDir, FinalFileName);

        if (FileManager.FileExists(*DestinationPath) && FileManager.IsReadOnly(*DestinationPath))
        {
            // IFileManager can report the flag but not change it; only IPlatformFile can.
            FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*DestinationPath, false);
        }

        // TStrongObjectPtr, not a bare NewObject: this loop runs garbage collection, and a task
        // rooted nowhere would be destroyed underneath the exporter.
        TStrongObjectPtr<UAssetExportTask> Task(NewObject<UAssetExportTask>());
        Task->Object = Asset;
        Task->Exporter = nullptr;  // let the engine match SupportedClass against the extension
        Task->Filename = DestinationPath;
        Task->bSelected = false;
        Task->bReplaceIdentical = true;
        Task->bPrompt = false;     // a modal dialog per asset would hang an unattended run
        Task->bAutomated = true;
        Task->bUseFileArchive = false;
        Task->bWriteEmptyFiles = false;

        const bool bRan = UExporter::RunAssetExportTask(Task.Get());

        // Both halves matter. RunAssetExportTask returns false when no exporter matched, and it can
        // also return true having filled Errors - reading only the bool reports a success that
        // produced nothing.
        if (!bRan)
        {
            if (Task->Errors.Num() > 0)
            {
                for (const FString& TaskError : Task->Errors)
                {
                    Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Item.AssetName.ToString(), *TaskError));
                }
            }
            else
            {
                ++Result.SkippedNoExporter;
            }
        }
        else if (Task->Errors.Num() > 0)
        {
            for (const FString& TaskError : Task->Errors)
            {
                Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Item.AssetName.ToString(), *TaskError));
            }
        }
        else if (!FileManager.FileExists(*DestinationPath))
        {
            // An exporter that reports success without producing a file is the one failure mode a
            // caller would never notice. Say so rather than counting a write that did not happen.
            Result.Errors.Add(FString::Printf(
                TEXT("%s: the exporter reported success but wrote no file at '%s'."),
                *Item.AssetName.ToString(), *DestinationPath));
        }
        else
        {
            ++Result.FilesWritten;

            if (FinalFileName != DesiredFileName)
            {
                ++Result.FilesRenamed;
            }
        }

        // Loading thousands of meshes without collecting exhausts memory long before the export
        // finishes. The task is released first so it is not what keeps the asset alive.
        Task.Reset();

        if (Options.CollectGarbageEvery > 0 && ++LoadedSinceCollect >= Options.CollectGarbageEvery)
        {
            LoadedSinceCollect = 0;
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
        }
    }

    Result.bSuccess = Result.Errors.IsEmpty() && !Result.bCancelled;

    UE_LOG(LogAssetUsageAudit, Log, TEXT("Conversion to '%s': %s"), *Options.TargetDirectory, *Result.Summarise());

    for (const FString& Error : Result.Errors)
    {
        UE_LOG(LogAssetUsageAudit, Warning, TEXT("  %s"), *Error);
    }

    return Result;
}

FString FAssetUsageExporter::ResolvePackageFilePath(FName PackageName)
{
    const FString PackageNameStr = PackageName.ToString();

    if (!FPackageName::IsValidLongPackageName(PackageNameStr))
    {
        return FString();
    }

    // A package is either an asset or a map and we do not know which from the name alone.
    // DoesPackageExist resolves the extension for us and also copes with mount points other
    // than /Game, which matters for plugin content.
    FString ResolvedFileName;
    if (FPackageName::DoesPackageExist(PackageNameStr, &ResolvedFileName))
    {
        return ResolvedFileName;
    }

    return FString();
}

FAssetUsageExporter::FResult FAssetUsageExporter::ExportPackageFiles(const TArray<FName>& PackageNames, const FOptions& Options)
{
    // One unnamed group: everything straight into the target, which is what this function has
    // always done. Kept as its own entry point because most callers and every existing spec want
    // exactly this and should not have to build a group to say so.
    FExportGroup Group;
    Group.Packages = PackageNames;

    return ExportPackageGroups({ MoveTemp(Group) }, Options);
}

FAssetUsageExporter::FResult FAssetUsageExporter::ExportPackageGroups(const TArray<FExportGroup>& Groups, const FOptions& Options)
{
    FResult Result;

    // Setup failures return before the summary log at the bottom of this function, so they must
    // log for themselves. These two are the most likely things to go wrong and were previously
    // the only failures that left no trace at all - the UI would say "see the Output Log" and the
    // Output Log would be empty.
    if (Options.TargetDirectory.IsEmpty())
    {
        Result.Errors.Add(TEXT("No target directory was given."));
        UE_LOG(LogAssetUsageAudit, Error, TEXT("Export aborted: no target directory was given."));
        return Result;
    }

    IFileManager& FileManager = IFileManager::Get();

    if (!FileManager.DirectoryExists(*Options.TargetDirectory))
    {
        if (!FileManager.MakeDirectory(*Options.TargetDirectory, /*Tree=*/true))
        {
            Result.Errors.Add(FString::Printf(TEXT("Could not create '%s'."), *Options.TargetDirectory));
            UE_LOG(LogAssetUsageAudit, Error,
                TEXT("Export aborted: could not create target directory '%s'. Check the path is valid and writable ")
                TEXT("- a Perforce-read-only parent or a non-existent drive letter both land here."),
                *Options.TargetDirectory);
            return Result;
        }
    }

    // Flatten the groups into one work list, remembering each entry's destination folder. Doing it
    // up front keeps the progress total honest: a caller showing a bar must not watch the maximum
    // creep upward as levels expand mid-copy.
    //
    // Deduplication is per group, not global. The same texture legitimately appears in several
    // groups under the folder-per-asset layout - that is the whole point of that layout - so a
    // global Seen set would silently give the first folder the texture and leave the rest without.
    TArray<FName> Expanded;
    TArray<FString> DestinationDirs;

    // Parallel to Expanded: true for entries this function added by expanding a level, rather than
    // entries the caller asked for. Counted at copy time, not here - reporting "N external actor
    // packages" for packages that were only queued would overstate a cancelled or failing run.
    TBitArray<> IsExternalExpansion;

    for (const FExportGroup& Group : Groups)
    {
        const FString GroupDir = Group.RelativeDir.IsEmpty()
            ? Options.TargetDirectory
            : FPaths::Combine(Options.TargetDirectory, Group.RelativeDir);

        const int32 FirstIndexInGroup = Expanded.Num();

        TSet<FName> Seen;
        Seen.Reserve(Group.Packages.Num());

        for (FName PackageName : Group.Packages)
        {
            if (!Seen.Contains(PackageName))
            {
                Seen.Add(PackageName);
                Expanded.Add(PackageName);
                DestinationDirs.Add(GroupDir);
                IsExternalExpansion.Add(false);
            }
        }

        if (!Options.bIncludeExternalPackages)
        {
            continue;
        }

        const int32 LastIndexInGroup = Expanded.Num();

        for (int32 Index = FirstIndexInGroup; Index < LastIndexInGroup; ++Index)
        {
            const FString PackageNameStr = Expanded[Index].ToString();

            // Only a World owns external packages. Asking for the paths of a non-level is harmless
            // but pointless, and on 45k rows it is not free.
            const FString FilePath = ResolvePackageFilePath(Expanded[Index]);
            if (!FilePath.EndsWith(FPackageName::GetMapPackageExtension()))
            {
                continue;
            }

            // Plural form: plugins register extra roots through delegates, and Content Bundles and
            // External Data Layers inject path segments a hand-built string would miss.
            TArray<FString> ExternalRoots = ULevel::GetExternalActorsPaths(PackageNameStr);
            ExternalRoots.Append(ULevel::GetExternalObjectsPaths(PackageNameStr));

            for (const FString& Root : ExternalRoots)
            {
                FString RootDir;
                if (!FPackageName::TryConvertLongPackageNameToFilename(Root, RootDir))
                {
                    continue;
                }

                TArray<FString> Found;
                FileManager.FindFilesRecursive(Found, *RootDir, TEXT("*.uasset"), /*Files=*/true, /*Directories=*/false);

                for (const FString& AbsFile : Found)
                {
                    FString ExternalPackageName;
                    if (FPackageName::TryConvertFilenameToLongPackageName(AbsFile, ExternalPackageName))
                    {
                        const FName AsName(*ExternalPackageName);
                        if (!Seen.Contains(AsName))
                        {
                            Seen.Add(AsName);
                            Expanded.Add(AsName);
                            DestinationDirs.Add(GroupDir);
                            IsExternalExpansion.Add(true);
                        }
                    }
                }
            }
        }
    }

    return CopyExpandedPackages(Expanded, DestinationDirs, IsExternalExpansion, Options, Result);
}

FAssetUsageExporter::FResult FAssetUsageExporter::CopyExpandedPackages(const TArray<FName>& Expanded, const TArray<FString>& DestinationDirs, const TBitArray<>& IsExternalExpansion, const FOptions& Options, FResult& Result)
{
    IFileManager& FileManager = IFileManager::Get();

    const int32 Total = Expanded.Num();

    for (int32 Index = 0; Index < Total; ++Index)
    {
        if (Options.OnProgress && !Options.OnProgress(Index, Total))
        {
            Result.bCancelled = true;
            break;
        }

        const FName PackageName = Expanded[Index];
        const FString SourcePath = ResolvePackageFilePath(PackageName);

        if (SourcePath.IsEmpty())
        {
            // Normal for script packages and for assets that exist only in memory. Counted, not
            // reported as an error, so a real failure stays visible in the Errors list.
            ++Result.FilesMissingOnDisk;
            continue;
        }

        // The group decided the folder; bFlatten only chooses whether to mirror the package path
        // beneath it. The two compose: a folder-per-asset export with mirroring off writes straight
        // into the asset's folder, which is what makes that layout readable.
        FString DestinationDir = DestinationDirs.IsValidIndex(Index)
            ? DestinationDirs[Index]
            : Options.TargetDirectory;

        if (!Options.bFlatten)
        {
            // Mirror the package path under the target, minus the leading mount point slash.
            FString Relative = FPaths::GetPath(PackageName.ToString());
            Relative.RemoveFromStart(TEXT("/"));
            DestinationDir = FPaths::Combine(DestinationDir, Relative);
        }

        if (DestinationDir != Options.TargetDirectory)
        {
            if (!FileManager.DirectoryExists(*DestinationDir) && !FileManager.MakeDirectory(*DestinationDir, true))
            {
                Result.Errors.Add(FString::Printf(TEXT("Could not create '%s'."), *DestinationDir));
                continue;
            }
        }

        const FString DesiredFileName = FPaths::GetCleanFilename(SourcePath);

        const FString FinalFileName = AssetExportNaming::ResolveCollision(
            DesiredFileName,
            Options.CollisionPolicy,
            [&FileManager, &DestinationDir](const FString& Candidate)
            {
                return FileManager.FileExists(*FPaths::Combine(DestinationDir, Candidate));
            });

        const FString DestinationPath = FPaths::Combine(DestinationDir, FinalFileName);

        const bool bWasPresent = FileManager.FileExists(*DestinationPath);

        // Perforce-primary repositories keep unopened files read-only, so the source .uasset is very often read-only on
        // disk. Windows CopyFile carries that attribute across, which would make the export folder
        // read-only too - and then an Overwrite-policy re-export fails on its own previous output.
        // Clear the flag on both ends of the problem: before writing over an existing file, and
        // after writing a new one. The destination is outside source control by definition.
        if (bWasPresent && FileManager.IsReadOnly(*DestinationPath))
        {
            // IFileManager can report the flag but not change it; only IPlatformFile can.
            FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*DestinationPath, false);
        }

        if (FileManager.Copy(*DestinationPath, *SourcePath, /*bReplace=*/true) != COPY_OK)
        {
            Result.Errors.Add(FString::Printf(TEXT("Failed to copy '%s' to '%s'."), *PackageName.ToString(), *DestinationPath));
            continue;
        }

        if (FileManager.IsReadOnly(*DestinationPath))
        {
            // IFileManager can report the flag but not change it; only IPlatformFile can.
            FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*DestinationPath, false);
        }

        ++Result.FilesCopied;

        if (Options.bRecordWrittenFiles)
        {
            // Relative to the export root, so the manifest stays valid if the folder is moved or
            // renamed - which is the normal fate of an export folder.
            FString Relative = DestinationPath;
            FPaths::MakePathRelativeTo(Relative, *(Options.TargetDirectory / TEXT("")));

            FWrittenFile& Written = Result.WrittenFiles.AddDefaulted_GetRef();
            Written.PackageName = PackageName;
            Written.RelativePath = MoveTemp(Relative);
        }

        if (IsExternalExpansion.IsValidIndex(Index) && IsExternalExpansion[Index])
        {
            ++Result.ExternalPackagesCopied;
        }

        if (FinalFileName != DesiredFileName)
        {
            ++Result.FilesRenamed;
        }
        else if (bWasPresent)
        {
            ++Result.FilesOverwritten;
        }
    }

    Result.bSuccess = Result.Errors.IsEmpty() && !Result.bCancelled;

    UE_LOG(LogAssetUsageAudit, Log, TEXT("Export to '%s': %s"), *Options.TargetDirectory, *Result.Summarise());

    for (const FString& Error : Result.Errors)
    {
        UE_LOG(LogAssetUsageAudit, Warning, TEXT("  %s"), *Error);
    }

    return Result;
}
