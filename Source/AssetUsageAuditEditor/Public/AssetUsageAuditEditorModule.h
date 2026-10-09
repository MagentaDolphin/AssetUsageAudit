// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class IConsoleObject;
class SDockTab;
class FSpawnTabArgs;

ASSETUSAGEAUDITEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogAssetUsageAuditEditor, Log, All);

/**
 * Editor-side host: settings, console entry points and (from phase 3) the Slate panel.
 *
 * All analysis lives in AssetUsageAuditCore. This module only drives it and presents results,
 * so a commandlet can reach exactly the same code path without dragging UI in.
 */
class FAssetUsageAuditEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** Nomad tab id, also used as the invoke target from the Tools menu. */
    static const FName PanelTabId;

private:
    void RegisterConsoleCommands();
    void UnregisterConsoleCommands();

    void RegisterTabSpawner();
    void UnregisterTabSpawner();
    void RegisterMenus();

    TSharedRef<SDockTab> SpawnPanelTab(const FSpawnTabArgs& Args);

    /** Shared implementation behind every console entry point. */
    void ExecuteAudit(const TArray<FString>& Args, bool bOnlyUnused);

    TArray<IConsoleObject*> ConsoleCommands;
};
