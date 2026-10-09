// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetUsageAuditTypes.h"

/**
 * Copies package files out of the project.
 *
 * Deliberately a plain file copy rather than IAssetTools::MigratePackages. Migrate pulls the whole
 * dependency closure, which is emphatically not what someone asked for when they ticked eleven
 * meshes, and it lives in an editor-only module. A byte copy loads no UObject, needs no editor,
 * and gives exactly the files that were ticked.
 */
class ASSETUSAGEAUDITCORE_API FAssetUsageExporter
{
public:
    struct FOptions
    {
        /** Absolute destination directory. Created if missing. */
        FString TargetDirectory;

        /** What to do when a file of that name is already there. */
        EExportCollisionPolicy CollisionPolicy = EExportCollisionPolicy::Index;

        /**
         * Put every file directly in the target folder rather than mirroring the /Game tree.
         *
         * Flat is what people expect from "export to a folder" and is why the collision policy
         * exists at all - mirroring the tree makes collisions nearly impossible but hands back a
         * deep folder structure nobody asked for.
         */
        bool bFlatten = true;

        /**
         * Also copy the __ExternalActors__ / __ExternalObjects__ packages belonging to any exported
         * level. Without these an exported OFPA map opens empty in the destination project.
         */
        bool bIncludeExternalPackages = true;

        /**
         * Record where each package actually ended up, in FResult::WrittenFiles.
         *
         * Off by default because it costs a string per file. Required by the dependency manifest:
         * a manifest built from intended paths rather than written ones is wrong precisely when the
         * collision policy renamed something, which is when someone needs it most.
         */
        bool bRecordWrittenFiles = false;

        /** Called as (Done, Total). Return false to cancel; already-copied files are kept. */
        TFunction<bool(int32, int32)> OnProgress;
    };

    /** One copied file, as it was actually written. */
    struct FWrittenFile
    {
        FName PackageName;

        /** Path relative to FOptions::TargetDirectory, with the final on-disk filename. */
        FString RelativePath;
    };

    // Exported: Summarise is defined out of line, so the Editor module cannot link without this.
    struct ASSETUSAGEAUDITCORE_API FResult
    {
        /** Filled only when FOptions::bRecordWrittenFiles was set. */
        TArray<FWrittenFile> WrittenFiles;

        bool bSuccess = false;
        int32 FilesCopied = 0;
        int32 FilesRenamed = 0;
        int32 FilesOverwritten = 0;
        int32 FilesMissingOnDisk = 0;
        int32 ExternalPackagesCopied = 0;
        bool bCancelled = false;

        /** One line per failure, safe to show in a dialog. */
        TArray<FString> Errors;

        FString Summarise() const;
    };

    /**
     * A set of packages destined for one subfolder of the target.
     *
     * The exporter does not decide what the subfolder means - AssetExportLayout does, and hands
     * the result over already named. Keeping the split means this class still knows nothing about
     * presets, dependencies or the Asset Registry, which is why its specs can run on package names
     * that do not exist.
     */
    struct FExportGroup
    {
        /** Relative to FOptions::TargetDirectory. Empty writes straight into it. */
        FString RelativeDir;

        TArray<FName> Packages;
    };

    /**
     * Copy packages, each group into its own subfolder.
     *
     * Collision handling is per destination folder, not global: two groups may each hold a file
     * called SM_Rock.uasset without either being renamed, because they land in different folders.
     * That is the point of the folder-per-asset layout.
     */
    static FResult ExportPackageGroups(const TArray<FExportGroup>& Groups, const FOptions& Options);

    /** Single-group convenience: everything straight into the target. */
    static FResult ExportPackageFiles(const TArray<FName>& PackageNames, const FOptions& Options);

    // --- Exchange formats ------------------------------------------------------------------------

    /**
     * One asset to convert. Deliberately not FAssetUsageRow: conversion needs three fields and
     * taking the whole row would make this callable only from a finished audit.
     */
    struct FExportItem
    {
        FName PackageName;
        FName AssetName;

        /** Asset class, e.g. /Script/Engine.StaticMesh. Chooses the output format. */
        FTopLevelAssetPath ClassPath;
    };

    struct FExchangeOptions
    {
        FString TargetDirectory;

        EExportCollisionPolicy CollisionPolicy = EExportCollisionPolicy::Index;

        bool bFlatten = true;

        /**
         * Asset class short name -> file extension without the dot, e.g. {"StaticMesh", "fbx"}.
         *
         * Passed in rather than hardcoded so the Core module stays free of the settings object, and
         * so a class with no sensible exchange format is a configuration fact rather than a silent
         * omission. Lookup walks up the class hierarchy, so mapping MaterialInterface also covers
         * MaterialInstanceConstant.
         */
        TMap<FString, FString> FormatByClass;

        /**
         * Run garbage collection every N assets.
         *
         * Unlike a file copy, this path loads every UObject it touches. Exporting a few thousand
         * meshes without collecting will exhaust memory long before the export finishes. Zero
         * disables it.
         */
        int32 CollectGarbageEvery = 64;

        /** Called as (Done, Total). Return false to cancel; files already written are kept. */
        TFunction<bool(int32, int32)> OnProgress;
    };

    struct ASSETUSAGEAUDITCORE_API FExchangeResult
    {
        bool bSuccess = false;
        int32 FilesWritten = 0;
        int32 FilesRenamed = 0;

        /** No extension configured for the asset's class. Not an error - a deliberate omission. */
        int32 SkippedNoFormat = 0;

        /** The asset would not load. Counted separately from an exporter refusing to run. */
        int32 SkippedNotLoaded = 0;

        /** Loaded fine, but no UExporter is registered for that class and extension. */
        int32 SkippedNoExporter = 0;

        bool bCancelled = false;

        TArray<FString> Errors;

        FString Summarise() const;
    };

    /**
     * Convert assets to interchange formats (FBX, PNG, WAV, ...) via UAssetExportTask.
     *
     * Loads every asset, so it is orders of magnitude slower than ExportPackageFiles and must be
     * driven with a progress callback. bPrompt is forced false and bAutomated true: a modal file
     * dialog per asset would hang an unattended run, and this is the whole reason the engine has
     * those flags.
     */
    static FExchangeResult ExportConvertedAssets(const TArray<FExportItem>& Items, const FExchangeOptions& Options);

    /**
     * Extension for an asset class, following the class hierarchy upward.
     * Returns an empty string when nothing in the chain is mapped.
     */
    static FString FindFormatForClass(const FTopLevelAssetPath& ClassPath, const TMap<FString, FString>& FormatByClass);

    /** The mapping the tool ships with. Editor settings seed themselves from this. */
    static TMap<FString, FString> GetDefaultFormatByClass();

    /**
     * Resolve a package name to its file on disk, trying both asset and map extensions.
     * Returns an empty string when the package has no file (script packages, unsaved assets).
     */
    static FString ResolvePackageFilePath(FName PackageName);

private:
    /**
     * The copy loop, shared by every entry point.
     *
     * Takes the destination folder per entry rather than deriving it, so that deciding where a file
     * goes and actually writing it stay separate concerns. Result is passed by reference because
     * the caller has already recorded setup failures into it.
     */
    static FResult CopyExpandedPackages(
        const TArray<FName>& Expanded,
        const TArray<FString>& DestinationDirs,
        const TBitArray<>& IsExternalExpansion,
        const FOptions& Options,
        FResult& Result);
};
