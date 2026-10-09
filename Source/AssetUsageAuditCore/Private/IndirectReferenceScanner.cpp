// MagentaDolphin 2026. Asset Usage Audit.

#include "IndirectReferenceScanner.h"
#include "AssetUsageAuditCoreModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/StringBuilder.h"

namespace
{
    const TCHAR* GameRootLiteral = TEXT("/Game/");

    /**
     * Characters that can legally continue a package path. Everything else ends the literal.
     *
     * Note '.' is included so that an object path like /Game/X/BP_Y.BP_Y_C is captured whole and
     * then trimmed below; stopping at the dot would work too but would silently accept the
     * malformed tail of a truncated literal.
     */
    bool IsPathChar(TCHAR Char)
    {
        return FChar::IsAlnum(Char) || Char == TEXT('_') || Char == TEXT('/') || Char == TEXT('.') || Char == TEXT('-');
    }
}

FString FIndirectReference::ToProvenanceString() const
{
    return FString::Printf(TEXT("%s:%d"), *FPaths::GetCleanFilename(SourceFile), LineNumber);
}

void FIndirectReferenceScanner::ExtractGamePathsFromLine(FStringView Line, TArray<FString>& OutPackageNames)
{
    const int32 RootLen = FCString::Strlen(GameRootLiteral);

    int32 SearchStart = 0;
    while (SearchStart < Line.Len())
    {
        const FStringView Remaining = Line.RightChop(SearchStart);
        const int32 Found = UE::String::FindFirst(Remaining, GameRootLiteral, ESearchCase::IgnoreCase);
        if (Found == INDEX_NONE)
        {
            return;
        }

        const int32 Start = SearchStart + Found;

        int32 End = Start + RootLen;
        while (End < Line.Len() && IsPathChar(Line[End]))
        {
            ++End;
        }

        FString Candidate(Line.Mid(Start, End - Start));
        SearchStart = End > Start ? End : Start + RootLen;

        // "/Game/" alone, or a trailing slash, is not a reference.
        if (Candidate.Len() <= RootLen)
        {
            continue;
        }

        while (Candidate.EndsWith(TEXT("/"), ESearchCase::CaseSensitive) || Candidate.EndsWith(TEXT("."), ESearchCase::CaseSensitive))
        {
            Candidate.LeftChopInline(1, EAllowShrinking::No);
        }

        // Object path -> package path. "/Game/X/BP_Y.BP_Y_C" and "/Game/X/T.T" both name package
        // "/Game/X/...". Take everything before the first dot after the last slash.
        int32 LastSlash = INDEX_NONE;
        Candidate.FindLastChar(TEXT('/'), LastSlash);

        int32 DotAfterSlash = INDEX_NONE;
        if (Candidate.FindChar(TEXT('.'), DotAfterSlash) && DotAfterSlash > LastSlash)
        {
            Candidate.LeftInline(DotAfterSlash, EAllowShrinking::No);
        }

        if (Candidate.Len() <= RootLen)
        {
            continue;
        }

        OutPackageNames.AddUnique(Candidate);
    }
}

FIndirectReferenceScanner::FOptions FIndirectReferenceScanner::MakeDefaultOptions()
{
    FOptions Options;

    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    Options.Directories.Add(ProjectDir / TEXT("Config"));
    Options.Directories.Add(ProjectDir / TEXT("Source"));

    return Options;
}

TArray<FIndirectReference> FIndirectReferenceScanner::Scan(const FOptions& Options)
{
    TArray<FIndirectReference> Results;

    IFileManager& FileManager = IFileManager::Get();

    for (const FString& Directory : Options.Directories)
    {
        if (!FileManager.DirectoryExists(*Directory))
        {
            UE_LOG(LogAssetUsageAudit, Verbose, TEXT("Indirect scan: directory '%s' does not exist, skipping."), *Directory);
            continue;
        }

        const bool bIsConfigDir = Directory.EndsWith(TEXT("Config"), ESearchCase::IgnoreCase);

        TArray<FString> FoundFiles;
        FileManager.FindFilesRecursive(FoundFiles, *Directory, TEXT("*.*"), true, false);

        for (const FString& FilePath : FoundFiles)
        {
            const FString Extension = FPaths::GetExtension(FilePath, true).ToLower();
            if (!Options.Extensions.Contains(Extension))
            {
                continue;
            }

            if (FileManager.FileSize(*FilePath) > Options.MaxFileSizeBytes)
            {
                UE_LOG(LogAssetUsageAudit, Verbose, TEXT("Indirect scan: skipping oversized file '%s'."), *FilePath);
                continue;
            }

            TArray<FString> Lines;
            if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
            {
                UE_LOG(LogAssetUsageAudit, Warning, TEXT("Indirect scan: could not read '%s'."), *FilePath);
                continue;
            }

            TArray<FString> PackageNames;
            for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
            {
                PackageNames.Reset();
                ExtractGamePathsFromLine(Lines[LineIndex], PackageNames);

                for (const FString& PackageName : PackageNames)
                {
                    FIndirectReference& Reference = Results.AddDefaulted_GetRef();
                    Reference.PackageName = FName(*PackageName);
                    Reference.SourceFile = FilePath;
                    Reference.LineNumber = LineIndex + 1;
                    Reference.bFromConfig = bIsConfigDir;
                }
            }
        }
    }

    UE_LOG(LogAssetUsageAudit, Log, TEXT("Indirect scan found %d /Game path literals in Config and Source."), Results.Num());

    return Results;
}
