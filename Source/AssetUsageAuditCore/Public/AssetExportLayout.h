// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetDependencyClosure.h"
#include "AssetUsageAuditTypes.h"
#include "AssetUsageExporter.h"

class IAssetRegistry;

/**
 * Decides where each exported file goes.
 *
 * Deliberately separate from FAssetUsageExporter. The exporter's job is to copy a package to a
 * path and nothing else - it has no idea what a preset or a dependency is, and keeping it that way
 * is why its tests can run against invented package names. Everything that needs the Asset
 * Registry, the class hierarchy or the dependency closure lives here instead, and hands the
 * exporter a finished list of "these packages, into this subfolder".
 */
namespace AssetExportLayout
{
    /**
     * A set of packages destined for one subfolder of the export target.
     *
     * An alias, not a second struct: this namespace exists to produce exactly what the exporter
     * consumes, and two identical types would mean a conversion loop whose only job is to prove
     * they stayed identical.
     */
    using FGroup = FAssetUsageExporter::FExportGroup;

    struct FOptions
    {
        EExportLayout Layout = EExportLayout::Flat;

        /**
         * Sort each asset's dependencies into per-type subfolders: Texture/, Material/, and so on.
         *
         * Only meaningful with FolderPerAsset - there is no "each asset's dependencies" to sort in
         * the other layouts. BuildGroups ignores it rather than inventing a meaning.
         *
         * The seed itself stays at the root of its own folder. It is the subject of that folder;
         * filing it under StaticMesh/ next to its own dependencies would bury it.
         */
        bool bGroupDependenciesByType = false;
    };

    struct ASSETUSAGEAUDITCORE_API FStats
    {
        int32 GroupCount = 0;

        /** Distinct packages across every group. */
        int32 DistinctPackages = 0;

        /**
         * Total placements, counting a shared dependency once per folder it lands in.
         *
         * This, not DistinctPackages, is the number of files that will be written. Under
         * FolderPerAsset the two differ sharply and the user has to be told which one they are
         * looking at before the copy starts.
         */
        int32 FilePlacements = 0;

        /** Folder names that collided and were given a numeric suffix. */
        int32 RenamedFolders = 0;

        FString Summarise() const;
    };

    /**
     * Build the export groups.
     *
     * @param SeedClosures  For FolderPerAsset, one entry per ticked asset from GatherPerSeed. For
     *                      the other layouts only the union of Packages is used, so a single
     *                      closure covering everything is enough.
     */
    ASSETUSAGEAUDITCORE_API TArray<FGroup> BuildGroups(
        IAssetRegistry& Registry,
        const TArray<AssetDependencyClosure::FSeedClosure>& SeedClosures,
        const FOptions& Options,
        FStats& OutStats);

    /**
     * Map every class the type presets cover, and their subclasses, to a folder name.
     *
     * Folder names are the preset names verbatim - Texture, Material, StaticMesh - rather than a
     * second set of invented labels. The Types filter in the panel already shows those words, so
     * the folders a user gets match the words they filtered by.
     *
     * Built once per export: expanding the class hierarchy costs a registry call per preset.
     */
    ASSETUSAGEAUDITCORE_API TMap<FTopLevelAssetPath, FString> BuildTypeFolderMap(IAssetRegistry& Registry);

    /**
     * Folder name for one package, using a map from BuildTypeFolderMap.
     * Returns "Other" for anything no preset covers, never an empty string - an empty name would
     * silently put the file in the parent folder and look like the grouping had failed.
     */
    ASSETUSAGEAUDITCORE_API FString TypeFolderForPackage(
        IAssetRegistry& Registry,
        FName PackageName,
        const TMap<FTopLevelAssetPath, FString>& FolderByClass);
}
