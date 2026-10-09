// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetUsageAuditTypes.h"
#include "AssetUsageExporter.h"

class IAssetRegistry;

/**
 * The JSON written beside a reference-breaking export.
 *
 * Why it exists: references inside a .uasset are stored as full package names, so a copied file
 * only resolves when it sits at exactly that package path in the destination. The Flat and
 * FolderPerAsset layouts deliberately do not put it there - they arrange files for a person to read
 * and hand around - so the reference graph is lost the moment the files leave the project.
 *
 * The manifest records that graph next to the files, so a later import can rebuild it rather than
 * guess. It is written from what the exporter actually wrote, never from what it intended to write:
 * the two differ exactly when the collision policy renamed something, which is the case a manifest
 * has to get right.
 *
 * Migrate needs none of this - the engine preserves the paths itself - and asking for a manifest
 * there is treated as a caller mistake rather than silently producing a misleading file.
 */
namespace AssetExportManifest
{
    /** Bumped when the schema changes in a way a reader must notice. */
    inline constexpr int32 SchemaVersion = 1;

    /** Manifest filename, written into the export root. */
    inline const TCHAR* FileName = TEXT("AssetUsageAudit.manifest.json");

    struct FOptions
    {
        /** Export root. The manifest is written here and paths are relative to it. */
        FString TargetDirectory;

        /** Recorded so a reader knows why the paths look the way they do. */
        EExportLayout Layout = EExportLayout::Flat;

        EExportCollisionPolicy CollisionPolicy = EExportCollisionPolicy::Index;

        /**
         * Packages the user ticked, as opposed to those pulled in as dependencies.
         *
         * Kept apart because an importer needs to know which assets were the point and which came
         * along to make them work - "restore what I exported" and "restore everything in this
         * folder" are different requests.
         */
        TSet<FName> SeedPackages;
    };

    struct ASSETUSAGEAUDITCORE_API FResult
    {
        bool bSuccess = false;

        /** Absolute path of the manifest, empty on failure. */
        FString FilePath;

        int32 EntriesWritten = 0;

        /** Dependency edges recorded across all entries. */
        int32 EdgesRecorded = 0;

        FString ErrorMessage;
    };

    /**
     * Write the manifest.
     *
     * @param WrittenFiles  From FAssetUsageExporter::FResult::WrittenFiles, which requires the
     *                      exporter to have been run with bRecordWrittenFiles. An empty array is a
     *                      failure rather than an empty manifest: it almost always means the flag
     *                      was forgotten, and an empty manifest beside a full folder is worse than
     *                      no manifest at all.
     */
    ASSETUSAGEAUDITCORE_API FResult Write(
        IAssetRegistry& Registry,
        const TArray<FAssetUsageExporter::FWrittenFile>& WrittenFiles,
        const FOptions& Options);
}
