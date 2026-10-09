// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"

class IAssetRegistry;

/**
 * Everything a set of packages needs in order to be usable somewhere else.
 *
 * Exists because "export the ticked meshes" without this produces meshes that open pink: the
 * package file of a StaticMesh holds no materials and no textures, only references to them. The
 * same applies to a Niagara system and its sprites, and to a Blueprint and everything it spawns.
 *
 * Deliberately a separate step rather than something FAssetUsageExporter does internally. The
 * exporter stays a dumb file copier that needs no Asset Registry and is testable without one, and
 * the caller keeps the chance to show "40 ticked, 380 will be written" before anything is copied -
 * which matters, because that ratio surprises people.
 *
 * This is NOT IAssetTools::MigratePackages. Migrate walks the same closure but also decides where
 * files go, prompts, and cannot be told to stop at a folder boundary. Here the traversal is bounded
 * by the same exclusion list the audit uses, and engine content is left out by default because the
 * destination project already has it.
 */
namespace AssetDependencyClosure
{
    struct FOptions
    {
        /**
         * Package prefixes not to descend into, in either /Game/... or Content/... form.
         * Normally the audit's own exclusion list, so an export cannot pull in content the report
         * deliberately ignores.
         */
        TArray<FString> ExcludePackagePaths;

        /**
         * Include /Engine and /Temp packages in the result.
         *
         * Off by default. A mesh using DefaultMaterial genuinely depends on /Engine content, but
         * copying it into the export folder is almost never what someone wants - the destination
         * project ships the same file, and overwriting it there is worse than useless.
         */
        bool bIncludeEnginePackages = false;

        /**
         * Stop after this many hops from a seed. Zero means no limit.
         *
         * A limit is a blunt instrument and changes the answer rather than just shortening it, so it
         * is off by default. It exists for the "just the materials, not the whole graph" case.
         */
        int32 MaxDepth = 0;

        /** Called as (Visited, Queued). Return false to stop; the partial result is still returned. */
        TFunction<bool(int32, int32)> OnProgress;
    };

    // Exported: Summarise is defined out of line, so the Editor module cannot link without this.
    struct ASSETUSAGEAUDITCORE_API FStats
    {
        /** Seeds that were valid package names to begin with. */
        int32 SeedCount = 0;

        /** Size of the returned array, seeds included. */
        int32 TotalCount = 0;

        /** Reached but dropped, per reason. Kept apart so a surprising result can be explained. */
        int32 SkippedScript = 0;
        int32 SkippedEngine = 0;
        int32 SkippedExcluded = 0;

        /**
         * Maps reached through an ordinary asset rather than through a level, and therefore not
         * followed - nor included.
         *
         * The same rule FLevelUsageResolver applies, and for the same measured reason: on WP_Example
         * the chain BP_GameMode -> PDA_MenuConfig -> L_Other drags in 9994
         * packages belonging to a different map. Without this the export dialog would quote an
         * honest number for a wrong set.
         *
         * Excluded rather than merely not expanded, unlike in the audit. The exporter expands any
         * .umap it is handed into its One File Per Actor packages, so including the map file would
         * pull the foreign level's contents back in through the exporter instead of the closure.
         */
        int32 ForeignLevelsSkipped = 0;

        /** Deepest hop count actually reached. Tells you whether MaxDepth did anything. */
        int32 DeepestHop = 0;

        bool bStoppedEarly = false;

        FString Summarise() const;
    };

    /**
     * Seeds plus everything they depend on, transitively.
     *
     * Seeds come first and in their original order, so a caller can still tell what was asked for.
     * The traversal uses AssetUsageAudit::MakeTraversalQuery() - package category, no requirements -
     * for the same reason the audit does: a Hard-only query silently drops every soft edge, which
     * includes every One File Per Actor package and every TSoftObjectPtr a Blueprint resolves.
     */
    /** One ticked asset and everything it pulls in, the seed first. */
    struct FSeedClosure
    {
        FName Seed;
        TArray<FName> Packages;
    };

    /**
     * Closure per seed, rather than one closure over all of them.
     *
     * Needed by the folder-per-asset layout: a texture shared by forty meshes has to appear in all
     * forty folders, and the flat Gather deliberately returns it once. Costs one traversal per
     * seed, so it is the slower call by construction - use Gather when a single list will do.
     */
    ASSETUSAGEAUDITCORE_API TArray<FSeedClosure> GatherPerSeed(
        IAssetRegistry& Registry,
        const TArray<FName>& Seeds,
        const FOptions& Options,
        FStats& OutStats);

    ASSETUSAGEAUDITCORE_API TArray<FName> Gather(
        IAssetRegistry& Registry,
        const TArray<FName>& Seeds,
        const FOptions& Options,
        FStats& OutStats);
}
