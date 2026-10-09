// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageAuditLibrary.h"

#include "AssetUsageAuditEditorModule.h"
#include "AssetUsageAuditSettings.h"
#include "AssetUsageAuditor.h"
#include "AssetUsageReportWriter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopedSlowTask.h"

namespace
{
    /**
     * The registry gathers asynchronously in the editor. Reading it mid-gather yields a partial
     * graph and therefore a confidently wrong "unused" list, so block until it is complete.
     * In a commandlet the module already gathers synchronously and this returns immediately.
     */
    IAssetRegistry& GetReadyAssetRegistry()
    {
        FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = Module.Get();

        if (AssetRegistry.IsLoadingAssets())
        {
            UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Asset Registry is still scanning; waiting before the audit."));
            AssetRegistry.WaitForCompletion();
        }

        return AssetRegistry;
    }

    /** Populate a request from the shared settings, so every entry point behaves the same. */
    FAssetUsageAuditRequest MakeRequestFromSettings()
    {
        FAssetUsageAuditRequest Request;

        if (const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get())
        {
            Request.IncludePackagePaths = Settings->IncludedPackagePaths;
            Request.ExcludePackagePaths = Settings->ExcludedPackagePaths;
            Request.bScanIndirectReferences = Settings->bScanIndirectReferences;
            Request.bHideExternalPackages = Settings->bHideExternalPackages;
        }

        return Request;
    }
}

bool UAssetUsageAuditLibrary::RunAudit(const TArray<FString>& LevelPackageNames, const FString& OutputDirectory, FString& OutReportPath)
{
    OutReportPath.Reset();

    IAssetRegistry& AssetRegistry = GetReadyAssetRegistry();

    FAssetUsageAuditRequest Request = MakeRequestFromSettings();

    Request.LevelPackages.Reserve(LevelPackageNames.Num());
    for (const FString& LevelName : LevelPackageNames)
    {
        Request.LevelPackages.Add(FName(*LevelName));
    }

    FScopedSlowTask SlowTask(100.0f, NSLOCTEXT("AssetUsageAudit", "RunningAudit", "Auditing asset usage..."));
    SlowTask.MakeDialog(true);

    Request.ResolveOptions.OnLevelProgress = [&SlowTask](int32 Current, int32 Total)
    {
        if (Total > 0)
        {
            SlowTask.EnterProgressFrame(100.0f / static_cast<float>(Total));
        }
    };

    Request.ResolveOptions.ShouldAbort = [&SlowTask]()
    {
        return SlowTask.ShouldCancel();
    };

    const FAssetUsageAuditResult Result = FAssetUsageAuditor::Run(AssetRegistry, Request);

    FAssetUsageReportWriter::FOptions WriteOptions;
    WriteOptions.OutputDirectory = OutputDirectory;

    if (WriteOptions.OutputDirectory.IsEmpty())
    {
        if (const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get())
        {
            WriteOptions.OutputDirectory = Settings->GetResolvedExportDirectory();
        }
    }

    const FAssetUsageReportWriter::FResult WriteResult = FAssetUsageReportWriter::Write(Result, WriteOptions);

    if (!WriteResult.bSuccess)
    {
        UE_LOG(LogAssetUsageAuditEditor, Error, TEXT("Failed to write the audit report: %s"), *WriteResult.ErrorMessage);
        return false;
    }

    OutReportPath = WriteResult.JsonPath;

    UE_LOG(LogAssetUsageAuditEditor, Log,
        TEXT("Audit complete: %d rows across %d levels. UsedOnLevel %d, UsedByAssetsOnly %d, Config/Source %d, Unreferenced %d, Unknown %d. Reports: %s | %s"),
        Result.Rows.Num(),
        Result.LevelStats.LevelsScanned,
        Result.CountByVerdict(EAssetUsageVerdict::UsedOnLevel),
        Result.CountByVerdict(EAssetUsageVerdict::UsedByAssetsOnly),
        Result.CountByVerdict(EAssetUsageVerdict::ReferencedFromConfigOrSource),
        Result.CountByVerdict(EAssetUsageVerdict::Unreferenced),
        Result.CountByVerdict(EAssetUsageVerdict::Unknown),
        *WriteResult.JsonPath,
        *WriteResult.CsvPath);

    return true;
}

TArray<FString> UAssetUsageAuditLibrary::GetAssetsUsedOnLevel(const FString& LevelPackageName)
{
    TArray<FString> Out;

    if (LevelPackageName.IsEmpty())
    {
        UE_LOG(LogAssetUsageAuditEditor, Warning, TEXT("GetAssetsUsedOnLevel called with an empty level name."));
        return Out;
    }

    IAssetRegistry& AssetRegistry = GetReadyAssetRegistry();

    FAssetUsageAuditRequest Request = MakeRequestFromSettings();
    Request.LevelPackages.Add(FName(*LevelPackageName));

    // Routes are the expensive part of the sweep and nothing here consumes them.
    Request.ResolveOptions.bRecordRoutes = false;

    const FAssetUsageAuditResult Result = FAssetUsageAuditor::Run(AssetRegistry, Request);

    Out.Reserve(Result.Rows.Num());
    for (const FAssetUsageRow& Row : Result.Rows)
    {
        if (Row.IsUsedOnAnyLevel())
        {
            Out.Add(Row.PackageName.ToString());
        }
    }

    return Out;
}

TArray<FString> UAssetUsageAuditLibrary::GetLevelsUsingAsset(const FString& AssetPackageName)
{
    TArray<FString> Out;

    if (AssetPackageName.IsEmpty())
    {
        UE_LOG(LogAssetUsageAuditEditor, Warning, TEXT("GetLevelsUsingAsset called with an empty asset name."));
        return Out;
    }

    IAssetRegistry& AssetRegistry = GetReadyAssetRegistry();

    // Every level must be swept: the question is which of them reach this asset, and that cannot
    // be answered from a subset.
    FAssetUsageAuditRequest Request = MakeRequestFromSettings();
    Request.ResolveOptions.bRecordRoutes = false;

    const FAssetUsageAuditResult Result = FAssetUsageAuditor::Run(AssetRegistry, Request);

    const FName Target(*AssetPackageName);
    for (const FAssetUsageRow& Row : Result.Rows)
    {
        if (Row.PackageName == Target)
        {
            Out.Reserve(Row.Levels.Num());
            for (FName Level : Row.Levels)
            {
                Out.Add(Level.ToString());
            }
            break;
        }
    }

    return Out;
}

float UAssetUsageAuditLibrary::MeasureFullSweepSeconds()
{
    IAssetRegistry& AssetRegistry = GetReadyAssetRegistry();

    FAssetUsageAuditRequest Request = MakeRequestFromSettings();

    const double StartTime = FPlatformTime::Seconds();
    const FAssetUsageAuditResult Result = FAssetUsageAuditor::Run(AssetRegistry, Request);
    const double Elapsed = FPlatformTime::Seconds() - StartTime;

    UE_LOG(LogAssetUsageAuditEditor, Log,
        TEXT("Full sweep timing: %.2fs total. Graph: enumerate %.2fs + dependencies %.2fs over %d packages and %d edges. ")
        TEXT("Levels: %d scanned, ScanLevelAssets %.2fs + traversal %.2fs, %d assets reachable, %d external packages seeded."),
        Elapsed,
        Result.GraphStats.EnumerateSeconds,
        Result.GraphStats.DependencySeconds,
        Result.GraphStats.NumPackages,
        Result.GraphStats.NumEdges,
        Result.LevelStats.LevelsScanned,
        Result.LevelStats.ScanLevelAssetsSeconds,
        Result.LevelStats.TraversalSeconds,
        Result.LevelStats.AssetsReachable,
        Result.LevelStats.ExternalPackagesSeeded);

    return static_cast<float>(Elapsed);
}
