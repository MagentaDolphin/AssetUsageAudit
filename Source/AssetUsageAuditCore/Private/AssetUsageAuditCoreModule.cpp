#include "AssetUsageAuditCoreModule.h"

DEFINE_LOG_CATEGORY(LogAssetUsageAudit);

#define LOCTEXT_NAMESPACE "FAssetUsageAuditCoreModule"

void FAssetUsageAuditCoreModule::StartupModule()
{
}

void FAssetUsageAuditCoreModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAssetUsageAuditCoreModule, AssetUsageAuditCore)
