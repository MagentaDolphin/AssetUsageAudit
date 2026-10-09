// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageReportWriter.h"
#include "AssetUsageAuditCoreModule.h"
#include "AssetUsageAuditor.h"
#include "AssetUsageAuditTypes.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    /** Column order is shared by the CSV header and the row writer so they cannot drift. */
    const TCHAR* CsvColumns[] = {
        TEXT("Type"),
        TEXT("GeneratedClass"),
        TEXT("Name"),
        TEXT("PathFromProjectRoot"),
        TEXT("Verdict"),
        TEXT("LevelCount"),
        TEXT("Levels"),
        TEXT("HardRefs"),
        TEXT("SoftRefs"),
        TEXT("Provenance"),
        TEXT("Route"),
        TEXT("ProvenanceDetail")
    };

    FString JoinNames(const TArray<FName>& Names, const FString& Separator)
    {
        TArray<FString> Strings;
        Strings.Reserve(Names.Num());

        for (const FName& Name : Names)
        {
            Strings.Add(Name.ToString());
        }

        return FString::Join(Strings, *Separator);
    }
}

FString FAssetUsageReportWriter::EscapeCsvField(const FString& Field, TCHAR Delimiter)
{
    const bool bNeedsQuoting =
        Field.Contains(FString(1, &Delimiter), ESearchCase::CaseSensitive) ||
        Field.Contains(TEXT("\""), ESearchCase::CaseSensitive) ||
        Field.Contains(TEXT("\n"), ESearchCase::CaseSensitive) ||
        Field.Contains(TEXT("\r"), ESearchCase::CaseSensitive);

    if (!bNeedsQuoting)
    {
        return Field;
    }

    FString Escaped = Field;
    Escaped.ReplaceInline(TEXT("\""), TEXT("\"\""), ESearchCase::CaseSensitive);

    return FString::Printf(TEXT("\"%s\""), *Escaped);
}

FString FAssetUsageReportWriter::BuildCsv(const FAssetUsageAuditResult& AuditResult, const FOptions& Options)
{
    const FAssetUsageReportHeader& Header = AuditResult.Header;
    const TCHAR Delimiter = Options.CsvDelimiter;

    FString Output;
    Output.Reserve(AuditResult.Rows.Num() * 160 + 2048);

    // Provenance of the report itself, as '#' comment lines. Excel shows them as ordinary rows,
    // which is fine and is much better than a report nobody can date or attribute to a filter set.
    Output += FString::Printf(TEXT("# Asset Usage Audit report\n"));
    Output += FString::Printf(TEXT("# Generated: %s\n"), *Header.GeneratedAt.ToIso8601());
    Output += FString::Printf(TEXT("# Project: %s\n"), *Header.ProjectName);
    Output += FString::Printf(TEXT("# Engine: %s\n"), *Header.EngineVersion);
    Output += FString::Printf(TEXT("# Tool: %s\n"), *Header.ToolVersion);
    Output += FString::Printf(TEXT("# Levels scanned: %d\n"), Header.LevelsScanned);
    Output += FString::Printf(TEXT("# Assets scanned: %d\n"), Header.AssetsScanned);
    Output += FString::Printf(TEXT("# Rows reported: %d\n"), AuditResult.Rows.Num());
    Output += FString::Printf(TEXT("# Scan duration: %.2f s\n"), Header.ScanDurationSeconds);

    for (const FString& Filter : Header.AppliedFilters)
    {
        Output += FString::Printf(TEXT("# Filter: %s\n"), *Filter);
    }

    Output += FString::Printf(TEXT("# Verdict counts: UsedOnLevel=%d UsedByAssetsOnly=%d ReferencedFromConfigOrSource=%d Unreferenced=%d Unknown=%d\n"),
        AuditResult.CountByVerdict(EAssetUsageVerdict::UsedOnLevel),
        AuditResult.CountByVerdict(EAssetUsageVerdict::UsedByAssetsOnly),
        AuditResult.CountByVerdict(EAssetUsageVerdict::ReferencedFromConfigOrSource),
        AuditResult.CountByVerdict(EAssetUsageVerdict::Unreferenced),
        AuditResult.CountByVerdict(EAssetUsageVerdict::Unknown));

    Output += TEXT("# Unknown means the registry cannot see this asset's references (FMOD, DataTable rows,\n");
    Output += TEXT("#   runtime-built paths). It does NOT mean unused. Never delete on the strength of it.\n");

    for (int32 ColumnIndex = 0; ColumnIndex < UE_ARRAY_COUNT(CsvColumns); ++ColumnIndex)
    {
        if (ColumnIndex > 0)
        {
            Output.AppendChar(Delimiter);
        }

        Output += CsvColumns[ColumnIndex];
    }

    Output += TEXT("\n");

    for (const FAssetUsageRow& Row : AuditResult.Rows)
    {
        TArray<FString, TInlineAllocator<16>> Fields;

        Fields.Add(Row.ClassPath.IsNull() ? FString() : Row.ClassPath.ToString());
        Fields.Add(Row.GeneratedClassPath.IsNull() ? FString() : Row.GeneratedClassPath.ToString());
        Fields.Add(Row.AssetName.ToString());
        Fields.Add(Row.PathFromProjectRoot);
        Fields.Add(LexToString(Row.Verdict));
        Fields.Add(FString::FromInt(Row.Levels.Num()));
        Fields.Add(JoinNames(Row.Levels, Options.CsvMultiValueSeparator));
        Fields.Add(FString::FromInt(Row.HardReferenceCount));
        Fields.Add(FString::FromInt(Row.SoftReferenceCount));
        Fields.Add(ProvenanceToString(Row.Provenance));
        Fields.Add(Row.Route);
        Fields.Add(Row.ProvenanceDetail);

        checkf(Fields.Num() == UE_ARRAY_COUNT(CsvColumns), TEXT("CSV row width must match the header."));

        for (int32 FieldIndex = 0; FieldIndex < Fields.Num(); ++FieldIndex)
        {
            if (FieldIndex > 0)
            {
                Output.AppendChar(Delimiter);
            }

            Output += EscapeCsvField(Fields[FieldIndex], Delimiter);
        }

        Output += TEXT("\n");
    }

    return Output;
}

FString FAssetUsageReportWriter::BuildJson(const FAssetUsageAuditResult& AuditResult)
{
    const FAssetUsageReportHeader& Header = AuditResult.Header;

    FString Output;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);

    Writer->WriteObjectStart();

    Writer->WriteObjectStart(TEXT("header"));
    Writer->WriteValue(TEXT("generatedAt"), Header.GeneratedAt.ToIso8601());
    Writer->WriteValue(TEXT("project"), Header.ProjectName);
    Writer->WriteValue(TEXT("engineVersion"), Header.EngineVersion);
    Writer->WriteValue(TEXT("toolVersion"), Header.ToolVersion);
    Writer->WriteValue(TEXT("levelsScanned"), Header.LevelsScanned);
    Writer->WriteValue(TEXT("assetsScanned"), Header.AssetsScanned);
    Writer->WriteValue(TEXT("rowsReported"), AuditResult.Rows.Num());
    Writer->WriteValue(TEXT("scanDurationSeconds"), Header.ScanDurationSeconds);

    Writer->WriteArrayStart(TEXT("appliedFilters"));
    for (const FString& Filter : Header.AppliedFilters)
    {
        Writer->WriteValue(Filter);
    }
    Writer->WriteArrayEnd();

    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("verdictCounts"));
    Writer->WriteValue(TEXT("usedOnLevel"), AuditResult.CountByVerdict(EAssetUsageVerdict::UsedOnLevel));
    Writer->WriteValue(TEXT("usedByAssetsOnly"), AuditResult.CountByVerdict(EAssetUsageVerdict::UsedByAssetsOnly));
    Writer->WriteValue(TEXT("referencedFromConfigOrSource"), AuditResult.CountByVerdict(EAssetUsageVerdict::ReferencedFromConfigOrSource));
    Writer->WriteValue(TEXT("unreferenced"), AuditResult.CountByVerdict(EAssetUsageVerdict::Unreferenced));
    Writer->WriteValue(TEXT("unknown"), AuditResult.CountByVerdict(EAssetUsageVerdict::Unknown));
    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("graphStats"));
    Writer->WriteValue(TEXT("packages"), AuditResult.GraphStats.NumPackages);
    Writer->WriteValue(TEXT("edges"), AuditResult.GraphStats.NumEdges);
    Writer->WriteValue(TEXT("levels"), AuditResult.GraphStats.NumLevels);
    Writer->WriteValue(TEXT("externalPackages"), AuditResult.GraphStats.NumExternalPackages);
    Writer->WriteValue(TEXT("enumerateSeconds"), AuditResult.GraphStats.EnumerateSeconds);
    Writer->WriteValue(TEXT("dependencySeconds"), AuditResult.GraphStats.DependencySeconds);
    Writer->WriteObjectEnd();

    Writer->WriteArrayStart(TEXT("assets"));

    for (const FAssetUsageRow& Row : AuditResult.Rows)
    {
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("name"), Row.AssetName.ToString());
        Writer->WriteValue(TEXT("package"), Row.PackageName.ToString());
        Writer->WriteValue(TEXT("path"), Row.PathFromProjectRoot);
        Writer->WriteValue(TEXT("type"), Row.ClassPath.IsNull() ? FString() : Row.ClassPath.ToString());

        if (!Row.GeneratedClassPath.IsNull())
        {
            Writer->WriteValue(TEXT("generatedClass"), Row.GeneratedClassPath.ToString());
        }

        Writer->WriteValue(TEXT("verdict"), LexToString(Row.Verdict));
        Writer->WriteValue(TEXT("provenance"), ProvenanceToString(Row.Provenance));
        Writer->WriteValue(TEXT("hardReferenceCount"), Row.HardReferenceCount);
        Writer->WriteValue(TEXT("softReferenceCount"), Row.SoftReferenceCount);

        Writer->WriteArrayStart(TEXT("levels"));
        for (const FName& Level : Row.Levels)
        {
            Writer->WriteValue(Level.ToString());
        }
        Writer->WriteArrayEnd();

        if (!Row.Route.IsEmpty())
        {
            Writer->WriteValue(TEXT("route"), Row.Route);
        }

        if (!Row.ProvenanceDetail.IsEmpty())
        {
            Writer->WriteValue(TEXT("provenanceDetail"), Row.ProvenanceDetail);
        }

        Writer->WriteObjectEnd();
    }

    Writer->WriteArrayEnd();
    Writer->WriteObjectEnd();
    Writer->Close();

    return Output;
}

FAssetUsageReportWriter::FResult FAssetUsageReportWriter::Write(const FAssetUsageAuditResult& AuditResult, const FOptions& Options)
{
    FResult Result;

    if (Options.OutputDirectory.IsEmpty())
    {
        Result.ErrorMessage = TEXT("Output directory is empty.");
        return Result;
    }

    if (!IFileManager::Get().MakeDirectory(*Options.OutputDirectory, true))
    {
        if (!IFileManager::Get().DirectoryExists(*Options.OutputDirectory))
        {
            Result.ErrorMessage = FString::Printf(TEXT("Could not create output directory '%s'."), *Options.OutputDirectory);
            return Result;
        }
    }

    const FString BaseName = Options.BaseFileName.IsEmpty() ? TEXT("AssetUsageReport") : Options.BaseFileName;

    bool bAnyWritten = false;

    if (Options.bWriteJson)
    {
        const FString JsonPath = FPaths::Combine(Options.OutputDirectory, BaseName + TEXT(".json"));

        if (FFileHelper::SaveStringToFile(BuildJson(AuditResult), *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            Result.JsonPath = JsonPath;
            bAnyWritten = true;
        }
        else
        {
            Result.ErrorMessage = FString::Printf(TEXT("Failed to write '%s'."), *JsonPath);
            return Result;
        }
    }

    if (Options.bWriteCsv)
    {
        const FString CsvPath = FPaths::Combine(Options.OutputDirectory, BaseName + TEXT(".csv"));

        // Excel needs the BOM to read UTF-8; without it Cyrillic asset names come out mangled.
        const FFileHelper::EEncodingOptions Encoding = Options.bCsvUtf8Bom
            ? FFileHelper::EEncodingOptions::ForceUTF8
            : FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM;

        if (FFileHelper::SaveStringToFile(BuildCsv(AuditResult, Options), *CsvPath, Encoding))
        {
            Result.CsvPath = CsvPath;
            bAnyWritten = true;
        }
        else
        {
            Result.ErrorMessage = FString::Printf(TEXT("Failed to write '%s'."), *CsvPath);
            return Result;
        }
    }

    Result.bSuccess = bAnyWritten;

    if (!bAnyWritten)
    {
        Result.ErrorMessage = TEXT("Both JSON and CSV output were disabled.");
    }
    else
    {
        UE_LOG(LogAssetUsageAudit, Log, TEXT("Report written: %d rows. JSON '%s', CSV '%s'."),
            AuditResult.Rows.Num(), *Result.JsonPath, *Result.CsvPath);
    }

    return Result;
}
