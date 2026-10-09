// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "Misc/AssetRegistryInterface.h"

// EExportLayout is a UENUM so the editor settings can expose it as a dropdown. It is the only
// reflected type in this module; everything else here stays plain C++ so the analysis has no
// reflection cost. Must be the last include, as UHT requires.
#include "AssetUsageAuditTypes.generated.h"

/**
 * Why an asset is considered used - or why we cannot tell.
 *
 * Deliberately five states, never a bool. The Asset Registry cannot see string-built paths,
 * OpenLevel(FName), DataTable row contents or FMOD event references; reporting those as
 * "unused" is how an audit tool causes a deletion incident. Unknown is a legitimate answer.
 */
enum class EAssetUsageVerdict : uint8
{
    /** Reachable from at least one level package. Columns say through which, and hard or soft. */
    UsedOnLevel,

    /** Has referencers, but no chain from any level reaches it. */
    UsedByAssetsOnly,

    /** Found by scanning Config/ and Source/ for /Game paths. Provenance carries file and line. */
    ReferencedFromConfigOrSource,

    /** GetReferencers returned nothing and no level reaches it. */
    Unreferenced,

    /** Falls into a known registry blind spot. Never present this as "safe to delete". */
    Unknown
};

ASSETUSAGEAUDITCORE_API const TCHAR* LexToString(EAssetUsageVerdict Verdict);

/**
 * Every verdict, in the order the UI should list them.
 *
 * Single source of truth: the menu, the report and the settings restore all need this list, and
 * three hand-written copies would drift the moment a sixth verdict is added.
 */
ASSETUSAGEAUDITCORE_API TArray<EAssetUsageVerdict> GetAllAssetUsageVerdicts();

/** How an asset reference was discovered. Bitmask - an asset can be reached several ways. */
enum class EAssetUsageProvenance : uint8
{
    None = 0,

    /** Direct hard dependency edge (EDependencyProperty::Hard present). */
    HardReference = 1 << 0,

    /** Soft dependency edge - the lack of Hard. Includes every external actor edge. */
    SoftReference = 1 << 1,

    /** Reached through an OFPA __ExternalActors__ / __ExternalObjects__ package. */
    ExternalActor = 1 << 2,

    /** Reached through ULevelStreaming::WorldAsset or ALevelInstance::WorldAsset. */
    Sublevel = 1 << 3,

    /** Path literal found in Config/*.ini. */
    ConfigFile = 1 << 4,

    /** Path literal found in Source/**.cpp|h. */
    SourceFile = 1 << 5,

    /** Edge is editor-only (EDependencyProperty::Game absent) - not shipped, but still a use. */
    EditorOnly = 1 << 6,

    /** Reached through a redirector that we resolved. */
    Redirector = 1 << 7
};

ENUM_CLASS_FLAGS(EAssetUsageProvenance);

ASSETUSAGEAUDITCORE_API FString ProvenanceToString(EAssetUsageProvenance Provenance);

/**
 * How exported files are arranged under the target folder.
 *
 * Deliberately an enum rather than a set of booleans. These are mutually exclusive: "mirror the
 * /Game tree" and "one folder per ticked asset" cannot both be true, and a pair of booleans would
 * make that contradiction expressible and then silently resolve it one way.
 */
UENUM()
enum class EExportLayout : uint8
{
    /** Everything straight into the target folder. This is why the collision policy exists. */
    Flat UMETA(DisplayName = "Flat - everything in one folder"),

    /** Mirror the package path, so /Game/Art/SM_Rock lands in Game/Art/SM_Rock.uasset. */
    MirrorTree UMETA(DisplayName = "Mirror the /Game folder tree"),

    /**
     * A subfolder per ticked asset, with that asset's dependencies beside it.
     *
     * Note the consequence: a texture shared by forty meshes is copied forty times, once into each
     * mesh's folder. That is the point - each folder is self-contained and can be handed over on
     * its own - but it means the file count exceeds the number of distinct assets, sometimes by a
     * lot. The export dialog states the real figure before anything is written.
     */
    FolderPerAsset,

    /**
     * Hand the packages to IAssetTools::MigratePackages, aimed at another project's Content folder.
     *
     * The only layout whose output opens in Unreal with its references intact. References inside a
     * .uasset are stored as full package names, so a file only resolves when it sits at exactly the
     * same package path in the destination; Migrate is the engine's own code for arranging that,
     * including the OFPA actor packages of a level.
     *
     * Not implemented in this module. AssetTools is editor-only, and the analysis here has to stay
     * runnable from a commandlet - the editor module dispatches this value to its own path. Anything
     * in Core that switches on the layout must therefore treat Migrate as "not mine".
     */
    Migrate
};

ASSETUSAGEAUDITCORE_API const TCHAR* LexToString(EExportLayout Layout);

/**
 * What to do when an exported file already exists in the target folder.
 */
enum class EExportCollisionPolicy : uint8
{
    /** Replace the existing file. */
    Overwrite,

    /** Append or increment a numeric suffix: Foo -> Foo_1, Foo_7 -> Foo_8. */
    Index
};

/**
 * One row of the audit result.
 */
struct ASSETUSAGEAUDITCORE_API FAssetUsageRow
{
    /** Package name, e.g. /Game/Art/SM_Rock. */
    FName PackageName;

    /** Asset name without path. */
    FName AssetName;

    /** Class path of the asset, e.g. /Script/Engine.StaticMesh. */
    FTopLevelAssetPath ClassPath;

    /**
     * For Blueprints, the generated gameplay class from the GeneratedClass tag.
     * A BP asset's ClassPath is always /Script/Engine.Blueprint, which is useless for filtering.
     */
    FTopLevelAssetPath GeneratedClassPath;

    /** Path relative to the project root, e.g. Content/Art/SM_Rock.uasset. */
    FString PathFromProjectRoot;

    EAssetUsageVerdict Verdict = EAssetUsageVerdict::Unknown;

    EAssetUsageProvenance Provenance = EAssetUsageProvenance::None;

    /** Levels this asset is reachable from. Includes both the sublevel and its parent map. */
    TArray<FName> Levels;

    /** How many incoming edges carried EDependencyProperty::Hard. */
    int32 HardReferenceCount = 0;

    /** How many incoming edges lacked Hard. */
    int32 SoftReferenceCount = 0;

    /**
     * Human-readable chain explaining the verdict, e.g.
     * "WP_Example -> __ExternalActors__/.../A2B -> BP_Rock -> SM_Rock".
     * A verdict without a route is unactionable for an artist.
     */
    FString Route;

    /** For ReferencedFromConfigOrSource - which file and line named this asset. */
    FString ProvenanceDetail;

    bool IsUsedOnAnyLevel() const
    {
        return Verdict == EAssetUsageVerdict::UsedOnLevel;
    }
};

/**
 * Metadata written into the report header so a stale report can never be mistaken for a fresh one.
 */
struct ASSETUSAGEAUDITCORE_API FAssetUsageReportHeader
{
    FDateTime GeneratedAt;
    FString EngineVersion;
    FString ToolVersion;
    FString ProjectName;

    /** Human-readable description of the filters that produced this result. */
    TArray<FString> AppliedFilters;

    int32 LevelsScanned = 0;
    int32 AssetsScanned = 0;
    double ScanDurationSeconds = 0.0;
};

/**
 * The dependency query this tool must always use.
 *
 * Package category, NoRequirements flags. NOT Hard.
 *
 * Two engine facts make this non-negotiable:
 *
 * 1. FExternalObjectAndActorDependencyGatherer (ExternalObjectAndActorDependencyGatherer.cpp:22)
 *    emits map -> external actor edges with property mask Game|Build. Hard is absent, and
 *    AssetRegistryInterface.h:95 states the lack of Hard *is* a soft dependency. Querying with
 *    Hard therefore drops every external actor - 16126 packages in this project.
 *
 * 2. In EDependencyQuery, Soft is literally defined as NotHard. So Hard|Soft means
 *    "require Hard AND require not-Hard" and matches nothing at all.
 *
 * Hard vs soft is a column in the report, never a filter on the query.
 */
namespace AssetUsageAudit
{
    inline UE::AssetRegistry::FDependencyQuery MakeTraversalQuery()
    {
        return UE::AssetRegistry::FDependencyQuery();
    }

    inline UE::AssetRegistry::EDependencyCategory MakeTraversalCategory()
    {
        return UE::AssetRegistry::EDependencyCategory::Package;
    }
}
