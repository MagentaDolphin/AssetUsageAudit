// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "UObject/TopLevelAssetPath.h"

class IAssetRegistry;

/**
 * What would happen if this set of packages were deleted - worked out before anything is touched.
 *
 * Deletion is the one operation in this tool that destroys work, and the audit's own verdicts are
 * not a safe basis for it on their own: Unknown means "the registry cannot see it", not "unused",
 * and the whole point of the five-state verdict is that the tool never presents a guess as a fact.
 * So the scan answers a narrower, checkable question - who still points at this, right now, in the
 * registry - and leaves the judgement to the person reading it.
 *
 * Pure registry and filesystem work. It loads nothing, deletes nothing, and is deliberately
 * separate from the code that performs the delete: the analysis has to be testable without an
 * editor, and the actual removal is ObjectTools' job in the Editor module, not ours.
 */
namespace AssetDeletionScan
{
    /**
     * Why a ticked package will not be deleted.
     *
     * Refusals are policy, not failures, and each one is shown with its reason. Silently dropping a
     * row from the delete set would be worse than refusing loudly - the user ticked it and is owed
     * an explanation.
     */
    enum class ERefusal : uint8
    {
        /** No objection. */
        None,

        /**
         * The package is a map.
         *
         * Refused by decision, not by capability. A level is the unit the whole tool measures usage
         * against, and deleting one silently invalidates every other row in the result. The engine
         * also refuses to delete a level that is currently open, which would make the outcome depend
         * on which map the user happens to have loaded.
         */
        IsLevel,

        /**
         * A One File Per Actor package - an actor or object belonging to some level.
         *
         * Deleting one is not deleting an asset, it is deleting a placed actor out of a map behind
         * the level editor's back. That belongs in the level editor, with its undo.
         */
        IsExternalPackage,

        /** /Engine, /Script or /Temp. Not this project's to delete. */
        NotProjectContent,

        /** The registry has no asset under this package name; nothing to delete. */
        Missing
    };

    ASSETUSAGEAUDITCORE_API const TCHAR* LexToString(ERefusal Refusal);

    /** One line of explanation, suitable for showing beside the row. */
    ASSETUSAGEAUDITCORE_API FText DescribeRefusal(ERefusal Refusal);

    /** One ticked package and what stands in the way of deleting it. */
    struct FCandidate
    {
        FName PackageName;
        FName AssetName;
        FTopLevelAssetPath ClassPath;

        /** Content/... form, as the report shows it. */
        FString PathFromProjectRoot;

        ERefusal Refusal = ERefusal::None;

        /**
         * Packages that reference this one and are NOT themselves being deleted.
         *
         * The exclusion matters: deleting a Blueprint together with the mesh only it uses is a
         * clean operation, and listing the Blueprint as a blocker would make every sensible
         * multi-asset delete look dangerous. Truncated to FOptions::MaxReferencersListed - the list
         * is there to be read, and forty names is not read.
         */
        TArray<FName> OutsideReferencers;

        /** Full count before truncation. */
        int32 OutsideReferencerCount = 0;

        /** How many of those referencers are maps or OFPA packages - i.e. it is placed in a level. */
        int32 LevelReferencerCount = 0;

        /**
         * The file exists on disk and is marked read-only.
         *
         * Worth surfacing here rather than discovering it as a failure afterwards: this project is
         * Perforce-primary and unopened files are read-only by default, so a delete attempted
         * without checking out first fails per-file, halfway through.
         */
        bool bReadOnlyOnDisk = false;

        bool CanDelete() const
        {
            return Refusal == ERefusal::None;
        }
    };

    struct FOptions
    {
        /** How many referencer names to keep per candidate. The count is always exact. */
        int32 MaxReferencersListed = 8;
    };

    // Exported: Summarise is defined out of line, so the Editor module cannot link without this.
    struct ASSETUSAGEAUDITCORE_API FStats
    {
        int32 Requested = 0;
        int32 Deletable = 0;

        int32 RefusedLevels = 0;
        int32 RefusedExternal = 0;
        int32 RefusedNotProjectContent = 0;
        int32 RefusedMissing = 0;

        /** Deletable candidates that something outside the set still points at. */
        int32 WithOutsideReferencers = 0;

        /** Deletable candidates whose file is read-only on disk. */
        int32 ReadOnly = 0;

        FString Summarise() const;
    };

    /**
     * Classify the packages and collect their outside referencers.
     *
     * Order is preserved, refusals included, so the caller can show the whole ticked set with a
     * reason against each row instead of a shorter list that quietly lost entries.
     *
     * Referencers are queried with AssetUsageAudit::MakeTraversalQuery() for the same reason the
     * audit uses it: a Hard-only query misses every soft reference, and a soft reference still
     * breaks when its target disappears - it just breaks at runtime instead of at load.
     */
    ASSETUSAGEAUDITCORE_API TArray<FCandidate> Scan(
        IAssetRegistry& Registry,
        const TArray<FName>& Packages,
        const FOptions& Options,
        FStats& OutStats);
}
