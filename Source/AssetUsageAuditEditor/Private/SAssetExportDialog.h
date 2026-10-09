// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetUsageAuditTypes.h"

/** What "export the ticked files" does. */
enum class EFileExportMode : uint8
{
    /** Byte-exact copies of the package files. Loads nothing. */
    CopyPackages,

    /** Meshes to FBX, textures to PNG and so on, through the engine's exporters. */
    ConvertToExchangeFormat
};

/**
 * Everything the export dialog collects.
 *
 * Lives here rather than in the panel so the two headers do not have to include each other: the
 * panel needs the mode for its persisted state, and the dialog needs the whole request.
 */
struct FAssetExportRequest
{
    EFileExportMode Mode = EFileExportMode::CopyPackages;

    /** Absolute destination. Pre-filled from settings, editable in the dialog. */
    FString TargetDirectory;

    EExportLayout Layout = EExportLayout::Flat;

    /** Only meaningful with FolderPerAsset; the dialog disables it otherwise. */
    bool bGroupDependenciesByType = false;

    bool bIncludeDependencies = true;

    /**
     * Write AssetUsageAudit.manifest.json beside the exported files.
     *
     * On by default: the Flat and FolderPerAsset layouts destroy the reference graph by design, and
     * the manifest is the only record of what pointed at what. Meaningless under Migrate, where the
     * engine keeps the paths intact.
     */
    bool bWriteManifest = true;

    EExportCollisionPolicy CollisionPolicy = EExportCollisionPolicy::Index;
};

/**
 * Show the modal export dialog.
 *
 * Modal on purpose. The alternative - a dockable options panel - lets the ticked set change while
 * the options are open, so the figures on screen would describe a selection that no longer exists.
 *
 * @param InOutRequest  Seeded with the current defaults; overwritten with the user's choices.
 * @param TickedCount   Shown in the dialog so the user can see what they are about to act on.
 * @return true if the user pressed Export, false on Cancel or on closing the window.
 */
bool ShowAssetExportDialog(FAssetExportRequest& InOutRequest, int32 TickedCount);

/**
 * Why a folder cannot be used as a Migrate destination, or empty text when it can.
 *
 * Mirrors the two checks UAssetToolsImpl performs, because it performs them *after* being called
 * and reports the failure only to the Output Log - so an export that cannot possibly work would
 * otherwise look like a button that does nothing.
 *
 * The rules, from the engine:
 *   1. the path must end in /Content/  (MigratePackages_ReportConfirmed);
 *   2. the folder one level up must hold a .uproject, or exactly one .uplugin
 *      (FPackageMigrationImpl::GetMountPointRootPath) - that is how the destination's mount point
 *      is derived, and without it Migrate aborts.
 */
FText ValidateMigrateDestination(const FString& Directory);
