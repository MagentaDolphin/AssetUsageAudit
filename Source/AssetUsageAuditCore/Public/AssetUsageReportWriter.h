// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"

struct FAssetUsageAuditResult;

/**
 * Writes the audit result as JSON and CSV.
 *
 * Both come from one in-memory result in a single call, so the two files can never disagree
 * about the same run. JSON is the machine-readable form (nested level lists, filter metadata);
 * CSV is the flat form a human opens in Excel.
 */
class ASSETUSAGEAUDITCORE_API FAssetUsageReportWriter
{
public:
    struct FOptions
    {
        /** Absolute directory to write into. Created if missing. */
        FString OutputDirectory;

        /** Base filename without extension; ".json" and ".csv" are appended. */
        FString BaseFileName = TEXT("AssetUsageReport");

        bool bWriteJson = true;
        bool bWriteCsv = true;

        /**
         * Write a UTF-8 BOM at the start of the CSV.
         *
         * Excel misreads UTF-8 without it and mangles every non-ASCII asset name, which on this
         * project means the Cyrillic folder and asset names come out as garbage.
         */
        bool bCsvUtf8Bom = true;

        /** CSV field separator. Semicolon suits locales where the comma is a decimal separator. */
        TCHAR CsvDelimiter = TEXT(';');

        /** Separator for multi-valued fields inside one CSV cell, e.g. the level list. */
        FString CsvMultiValueSeparator = TEXT("|");
    };

    struct FResult
    {
        bool bSuccess = false;
        FString JsonPath;
        FString CsvPath;
        FString ErrorMessage;
    };

    static FResult Write(const FAssetUsageAuditResult& AuditResult, const FOptions& Options);

    /** Serialize to a JSON string without touching disk. Exposed for tests. */
    static FString BuildJson(const FAssetUsageAuditResult& AuditResult);

    /** Serialize to a CSV string without touching disk. Exposed for tests. */
    static FString BuildCsv(const FAssetUsageAuditResult& AuditResult, const FOptions& Options);

    /**
     * Quote and escape one CSV field per RFC 4180.
     *
     * Exposed because this is where CSV writers usually break: a field containing the delimiter,
     * a quote or a newline must be quoted, and embedded quotes doubled.
     */
    static FString EscapeCsvField(const FString& Field, TCHAR Delimiter);
};
