// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageAuditUserSettings.h"

UAssetUsageAuditUserSettings* UAssetUsageAuditUserSettings::Get()
{
    return GetMutableDefault<UAssetUsageAuditUserSettings>();
}
