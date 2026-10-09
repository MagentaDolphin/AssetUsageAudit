// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetImportScanner.h"

class IAssetRegistry;

/**
 * Copies scanned files into the project at their recovered package paths.
 *
 * A byte copy is enough: a .uasset is self-contained and its references are resolved by package
 * path, so putting the file where the path says produces a working asset without loading anything.
 * That is also why the scanner's job - deciding the path - is the hard half and this is the easy one.
 *
 * What this refuses to do matters more than what it does. Importing is the one operation here that
 * can destroy work: writing over /Game/Art/SM_Rock replaces whatever the project had under that
 * name, with no undo and no trace. So an existing target is skipped unless overwriting was asked
 * for explicitly, and a package already loaded in the editor is never written over at all.
 */
namespace AssetImporter
{
    struct FOptions
    {
        /**
         * Replace assets that already exist at the target path.
         *
         * Off by default and deliberately awkward to turn on: the scanner reports the count up
         * front so the choice is made knowingly rather than discovered afterwards.
         */
        bool bOverwriteExisting = false;

        /**
         * Where to put files whose package path could not be recovered, e.g. "/Game/Imported".
         *
         * Empty means skip them. They can be imported, but their references will not resolve, so
         * putting them somewhere by default would quietly fill the project with broken assets.
         */
        FString UnresolvedDestinationPath;

        /** Called as (Done, Total). Return false to stop; files already written are kept. */
        TFunction<bool(int32, int32)> OnProgress;
    };

    struct ASSETUSAGEAUDITCORE_API FResult
    {
        bool bSuccess = false;

        int32 FilesImported = 0;

        /** Target existed and overwriting was not requested. */
        int32 SkippedExisting = 0;

        /** No recoverable package path and no fallback folder was given. */
        int32 SkippedUnresolved = 0;

        /**
         * Target package is loaded in this editor session.
         *
         * Counted separately because it is not a user choice to make: replacing the file under a
         * loaded package leaves the editor holding stale objects that will be saved back over the
         * new file. The asset has to be closed first.
         */
        int32 SkippedLoaded = 0;

        bool bCancelled = false;

        TArray<FString> Errors;

        /** Absolute paths written, for the Asset Registry rescan that makes them visible. */
        TArray<FString> ImportedFiles;

        FString Summarise() const;
    };

    /**
     * Import the given candidates.
     *
     * Only candidates the caller wants should be passed in - this applies no filtering of its own
     * beyond the safety rules above.
     */
    ASSETUSAGEAUDITCORE_API FResult Import(
        IAssetRegistry& Registry,
        const TArray<AssetImportScanner::FCandidate>& Candidates,
        const FOptions& Options);
}
