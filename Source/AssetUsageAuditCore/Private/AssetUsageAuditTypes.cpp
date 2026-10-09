// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageAuditTypes.h"

const TCHAR* LexToString(EAssetUsageVerdict Verdict)
{
    switch (Verdict)
    {
        case EAssetUsageVerdict::UsedOnLevel:
            return TEXT("UsedOnLevel");
        case EAssetUsageVerdict::UsedByAssetsOnly:
            return TEXT("UsedByAssetsOnly");
        case EAssetUsageVerdict::ReferencedFromConfigOrSource:
            return TEXT("ReferencedFromConfigOrSource");
        case EAssetUsageVerdict::Unreferenced:
            return TEXT("Unreferenced");
        case EAssetUsageVerdict::Unknown:
            return TEXT("Unknown");
    }

    return TEXT("Unknown");
}

TArray<EAssetUsageVerdict> GetAllAssetUsageVerdicts()
{
    // Most actionable first: this order is what the filter menu shows, and "used on a level" is the
    // question people open the tool with.
    return {
        EAssetUsageVerdict::UsedOnLevel,
        EAssetUsageVerdict::UsedByAssetsOnly,
        EAssetUsageVerdict::ReferencedFromConfigOrSource,
        EAssetUsageVerdict::Unreferenced,
        EAssetUsageVerdict::Unknown
    };
}

const TCHAR* LexToString(EExportLayout Layout)
{
    switch (Layout)
    {
        case EExportLayout::Flat:           return TEXT("Flat");
        case EExportLayout::MirrorTree:     return TEXT("MirrorTree");
        case EExportLayout::FolderPerAsset: return TEXT("FolderPerAsset");
        case EExportLayout::Migrate:        return TEXT("Migrate");
    }

    return TEXT("Flat");
}

FString ProvenanceToString(EAssetUsageProvenance Provenance)
{
    if (Provenance == EAssetUsageProvenance::None)
    {
        return TEXT("None");
    }

    TArray<FString> Parts;

    auto Append = [&Parts, Provenance](EAssetUsageProvenance Flag, const TCHAR* Label)
    {
        if (EnumHasAnyFlags(Provenance, Flag))
        {
            Parts.Add(Label);
        }
    };

    Append(EAssetUsageProvenance::HardReference, TEXT("Hard"));
    Append(EAssetUsageProvenance::SoftReference, TEXT("Soft"));
    Append(EAssetUsageProvenance::ExternalActor, TEXT("ExternalActor"));
    Append(EAssetUsageProvenance::Sublevel, TEXT("Sublevel"));
    Append(EAssetUsageProvenance::ConfigFile, TEXT("Config"));
    Append(EAssetUsageProvenance::SourceFile, TEXT("Source"));
    Append(EAssetUsageProvenance::EditorOnly, TEXT("EditorOnly"));
    Append(EAssetUsageProvenance::Redirector, TEXT("Redirector"));

    return FString::Join(Parts, TEXT("|"));
}
