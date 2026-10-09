// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageAuditEditorModule.h"

#include "AssetUsageAuditSettings.h"
#include "AssetUsageAuditor.h"
#include "AssetUsageReportWriter.h"
#include "SAssetImportDialog.h"
#include "SAssetUsageAuditPanel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopedSlowTask.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

DEFINE_LOG_CATEGORY(LogAssetUsageAuditEditor);

#define LOCTEXT_NAMESPACE "FAssetUsageAuditEditorModule"

const FName FAssetUsageAuditEditorModule::PanelTabId(TEXT("AssetUsageAudit"));

void FAssetUsageAuditEditorModule::StartupModule()
{
    RegisterConsoleCommands();
    RegisterTabSpawner();

    // ToolMenus may not exist yet at PostEngineInit; the callback fires once it does. Registering
    // directly as well would double up the entry.
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAssetUsageAuditEditorModule::RegisterMenus));
}

void FAssetUsageAuditEditorModule::ShutdownModule()
{
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);

    UnregisterTabSpawner();
    UnregisterConsoleCommands();
}

void FAssetUsageAuditEditorModule::RegisterTabSpawner()
{
    FGlobalTabmanager::Get()
        ->RegisterNomadTabSpawner(
            PanelTabId,
            FOnSpawnTab::CreateRaw(this, &FAssetUsageAuditEditorModule::SpawnPanelTab))
        .SetDisplayName(LOCTEXT("PanelTabTitle", "Asset Usage Audit"))
        .SetTooltipText(LOCTEXT("PanelTabTooltip", "Find which assets each level uses, and which are used nowhere."))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"));
}

void FAssetUsageAuditEditorModule::UnregisterTabSpawner()
{
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PanelTabId);
}

TSharedRef<SDockTab> FAssetUsageAuditEditorModule::SpawnPanelTab(const FSpawnTabArgs& Args)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SAssetUsageAuditPanel)
        ];
}

void FAssetUsageAuditEditorModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);

    UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
    if (!ToolsMenu)
    {
        return;
    }

    FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("Audit"), LOCTEXT("AuditSection", "Audit"));

    Section.AddMenuEntry(
        TEXT("OpenAssetUsageAudit"),
        LOCTEXT("OpenPanel", "Asset Usage Audit"),
        LOCTEXT("OpenPanelTip", "Find which assets each level uses, and which are used nowhere."),
        FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FGlobalTabmanager::Get()->TryInvokeTab(FAssetUsageAuditEditorModule::PanelTabId);
        })));

    // Import sits beside the audit rather than inside its panel: it is the reverse direction and
    // needs no audit result, so requiring a sweep first would be pure ceremony.
    Section.AddMenuEntry(
        TEXT("ImportAssetsFromFolder"),
        LOCTEXT("OpenImport", "Import Assets from Folder..."),
        LOCTEXT("OpenImportTip", "Read a folder of exported .uasset files and bring the chosen ones back into this project."),
        FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            ShowAssetImportDialog();
        })));
}

void FAssetUsageAuditEditorModule::RegisterConsoleCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();

    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AssetUsageAudit.Run"),
        TEXT("Audit asset usage across levels and write a JSON + CSV report. Optional args: level package names to restrict the sweep."),
        FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
        {
            ExecuteAudit(Args, /*bOnlyUnused=*/false);
        }),
        ECVF_Default));

    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AssetUsageAudit.OpenPanel"),
        TEXT("Open the Asset Usage Audit panel. Same as Tools > Asset Usage Audit."),
        FConsoleCommandDelegate::CreateLambda([]()
        {
            if (FGlobalTabmanager::Get()->TryInvokeTab(PanelTabId).IsValid())
            {
                UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Asset Usage Audit panel opened."));
            }
            else
            {
                UE_LOG(LogAssetUsageAuditEditor, Warning, TEXT("Could not open the Asset Usage Audit panel."));
            }
        }),
        ECVF_Default));

    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AssetUsageAudit.FindUnused"),
        TEXT("Report only assets that no level reaches. Read the Verdict column: Unknown is not the same as unused."),
        FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
        {
            ExecuteAudit(Args, /*bOnlyUnused=*/true);
        }),
        ECVF_Default));
}

void FAssetUsageAuditEditorModule::UnregisterConsoleCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();

    for (IConsoleObject* Command : ConsoleCommands)
    {
        if (Command)
        {
            ConsoleManager.UnregisterConsoleObject(Command);
        }
    }

    ConsoleCommands.Reset();
}

void FAssetUsageAuditEditorModule::ExecuteAudit(const TArray<FString>& Args, bool bOnlyUnused)
{
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    // A sweep started mid-scan silently reports a fraction of the project, so the scan must finish
    // first. Block rather than refuse: on a cold start the registry is nearly always still
    // scanning, and refusing made the command unusable from -ExecCmds, where there is no human to
    // retry. WaitForCompletion is synchronous and behaves the same in the editor and a commandlet.
    if (AssetRegistry.IsLoadingAssets())
    {
        UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Asset Registry is still scanning; waiting for it to finish before auditing."));
        AssetRegistry.WaitForCompletion();
        UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Asset Registry scan complete; starting the audit."));
    }

    const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get();
    check(Settings);

    FAssetUsageAuditRequest Request;
    Request.IncludePackagePaths = Settings->IncludedPackagePaths;
    Request.ExcludePackagePaths = Settings->ExcludedPackagePaths;
    Request.bScanIndirectReferences = Settings->bScanIndirectReferences;
    Request.bHideExternalPackages = Settings->bHideExternalPackages;
    Request.bOnlyUnusedAssets = bOnlyUnused;

    for (const FString& Arg : Args)
    {
        Request.LevelPackages.Add(FName(*Arg));
    }

    FScopedSlowTask SlowTask(100.0f, LOCTEXT("AuditingAssets", "Auditing asset usage..."));
    SlowTask.MakeDialog(/*bShowCancelButton=*/true);

    bool bCancelled = false;

    Request.ResolveOptions.ShouldAbort = [&SlowTask, &bCancelled]()
    {
        if (SlowTask.ShouldCancel())
        {
            bCancelled = true;
            return true;
        }
        return false;
    };

    Request.ResolveOptions.OnLevelProgress = [&SlowTask](int32 LevelOrdinal, int32 TotalLevels)
    {
        if (TotalLevels > 0 && LevelOrdinal % 16 == 0)
        {
            SlowTask.EnterProgressFrame(
                16.0f * 100.0f / static_cast<float>(TotalLevels),
                FText::Format(LOCTEXT("AuditingLevel", "Level {0} of {1}"), LevelOrdinal, TotalLevels));
        }
    };

    const FAssetUsageAuditResult Result = FAssetUsageAuditor::Run(AssetRegistry, Request);

    if (bCancelled)
    {
        UE_LOG(LogAssetUsageAuditEditor, Warning,
            TEXT("Audit cancelled. Partial results were discarded - a partial sweep understates usage and must not be reported."));
        return;
    }

    FAssetUsageReportWriter::FOptions WriteOptions;
    WriteOptions.OutputDirectory = Settings->GetResolvedExportDirectory();
    WriteOptions.BaseFileName = FString::Printf(TEXT("AssetUsageReport_%s"), *Result.Header.GeneratedAt.ToString(TEXT("%Y%m%d_%H%M%S")));

    const FAssetUsageReportWriter::FResult WriteResult = FAssetUsageReportWriter::Write(Result, WriteOptions);

    if (!WriteResult.bSuccess)
    {
        UE_LOG(LogAssetUsageAuditEditor, Error, TEXT("Report was not written: %s"), *WriteResult.ErrorMessage);
        return;
    }

    UE_LOG(LogAssetUsageAuditEditor, Display,
        TEXT("Audit finished in %.2fs. %d rows across %d levels (%d packages, %d edges, %d OFPA packages).\n")
        TEXT("  UsedOnLevel=%d  UsedByAssetsOnly=%d  ReferencedFromConfigOrSource=%d  Unreferenced=%d  Unknown=%d\n")
        TEXT("  JSON: %s\n  CSV:  %s"),
        Result.Header.ScanDurationSeconds,
        Result.Rows.Num(),
        Result.Header.LevelsScanned,
        Result.GraphStats.NumPackages,
        Result.GraphStats.NumEdges,
        Result.GraphStats.NumExternalPackages,
        Result.CountByVerdict(EAssetUsageVerdict::UsedOnLevel),
        Result.CountByVerdict(EAssetUsageVerdict::UsedByAssetsOnly),
        Result.CountByVerdict(EAssetUsageVerdict::ReferencedFromConfigOrSource),
        Result.CountByVerdict(EAssetUsageVerdict::Unreferenced),
        Result.CountByVerdict(EAssetUsageVerdict::Unknown),
        *WriteResult.JsonPath,
        *WriteResult.CsvPath);

    // The canary. If a sweep of an OFPA level yields almost nothing, the dependency query has
    // regressed to Hard-only and every external actor has silently vanished from the result.
    if (Result.GraphStats.NumExternalPackages > 0 && Result.LevelStats.AssetsReachable < Result.GraphStats.NumExternalPackages / 4)
    {
        UE_LOG(LogAssetUsageAuditEditor, Warning,
            TEXT("Only %d assets were reachable although the project has %d One File Per Actor packages. ")
            TEXT("This is the signature of a Hard-only dependency query dropping every external actor. Check the traversal query."),
            Result.LevelStats.AssetsReachable, Result.GraphStats.NumExternalPackages);
    }
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAssetUsageAuditEditorModule, AssetUsageAuditEditor)
