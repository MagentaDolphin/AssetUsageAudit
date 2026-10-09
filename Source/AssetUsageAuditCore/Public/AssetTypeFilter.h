// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"

class IAssetRegistry;
struct FAssetUsageNode;

/**
 * Named groups of asset classes, so an artist picks "VFX" rather than typing a class path.
 */
enum class EAssetTypePreset : uint8
{
    StaticMesh,
    SkeletalMesh,
    Material,
    Texture,
    VFX,
    Sound,
    Blueprint,
    Level,
    DataAsset,
    Animation
};

ASSETUSAGEAUDITCORE_API const TCHAR* LexToString(EAssetTypePreset Preset);

/** Every preset, in the order the UI should list them. */
ASSETUSAGEAUDITCORE_API TArray<EAssetTypePreset> GetAllAssetTypePresets();

/** Class paths a preset stands for, before subclass expansion. */
ASSETUSAGEAUDITCORE_API TArray<FTopLevelAssetPath> GetPresetClassPaths(EAssetTypePreset Preset);

/**
 * Matches assets by class, with the two things that make class filtering work in this project.
 *
 * 1. Subclass expansion via GetDerivedClassNames, so "Material" also matches MaterialInstanceConstant.
 *
 * 2. Blueprint awareness. A BP asset's own class is always /Script/Engine.Blueprint, so filtering on
 *    class alone finds no Blueprints at all - fatal in a project whose gameplay is entirely Blueprints.
 *    The gameplay class lives in the GeneratedClass registry tag, captured per node during the sweep,
 *    and a node matches when EITHER its class or its generated class is in the expanded set.
 */
class ASSETUSAGEAUDITCORE_API FAssetTypeFilter
{
public:
    /** Empty filter matches everything. */
    FAssetTypeFilter() = default;

    void AddPreset(EAssetTypePreset Preset);

    /** Raw class path, e.g. "/Script/Engine.StaticMesh" or a short name like "StaticMesh". */
    void AddRawClass(const FString& ClassPathOrName);

    /**
     * Expand every added class to include its subclasses.
     * Must be called after the Add* calls and before Matches.
     */
    void Compile(IAssetRegistry& AssetRegistry);

    bool IsEmpty() const
    {
        return RequestedClasses.IsEmpty();
    }

    bool Matches(const FAssetUsageNode& Node) const;

    /** Human-readable description for the report header. */
    FString Describe() const;

    /** Raw class strings that could not be resolved to a real class. */
    TConstArrayView<FString> GetUnresolvedClasses() const
    {
        return UnresolvedClasses;
    }

private:
    TArray<FTopLevelAssetPath> RequestedClasses;
    TArray<FString> RequestedDescriptions;
    TArray<FString> UnresolvedClasses;

    TSet<FTopLevelAssetPath> ExpandedClasses;
    bool bCompiled = false;
};
