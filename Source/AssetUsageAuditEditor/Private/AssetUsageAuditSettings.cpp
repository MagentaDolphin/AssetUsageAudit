// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageAuditSettings.h"

#include "Misc/App.h"
#include "Misc/Paths.h"

UAssetUsageAuditSettings::UAssetUsageAuditSettings()
{
    // Bought content packs. Measured on this project: roughly two thirds of the 80k assets are
    // third-party, so leaving these in makes the first run unreadable for an artist looking for
    // their own content.
    //
    // Deliberately NOT excluded, despite being a bought pack: Content/a purchased UI pack. It supplies
    // BP_MenuGameInstance, which DefaultEngine.ini names as the project's GameInstance.
    // Excluding it would hide a load-bearing asset from every report.
    ExcludedPackagePaths = {
        TEXT("Content/3rdParty"),
        TEXT("Content/StarterContent"),
        TEXT("Content/StarterBundle"),
        TEXT("Content/Megascans"),
        TEXT("Content/MSPresets")
    };
}

const UAssetUsageAuditSettings* UAssetUsageAuditSettings::Get()
{
    return GetDefault<UAssetUsageAuditSettings>();
}

FString UAssetUsageAuditSettings::GetResolvedExportDirectory() const
{
    if (!DefaultExportDirectory.IsEmpty())
    {
        return DefaultExportDirectory;
    }

    return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetUsageAudit")));
}

FName UAssetUsageAuditSettings::GetCategoryName() const
{
    // House convention, matching ConventionKeeper: project tools group under the project's own
    // name in Project Settings rather than scattering across Plugins.
    // GetProjectName already returns const TCHAR*; dereferencing it would pass a single character.
    return FName(FApp::GetProjectName());
}
