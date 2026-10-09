#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

ASSETUSAGEAUDITCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogAssetUsageAudit, Log, All);

/**
 * Analysis-only module. Deliberately links no UI and no editor framework:
 * every dependency here is available in a commandlet and in a standalone Program,
 * which is what keeps a future headless mode a packaging question rather than a rewrite.
 */
class FAssetUsageAuditCoreModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
