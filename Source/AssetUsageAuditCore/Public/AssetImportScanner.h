// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"

class IAssetRegistry;

/**
 * Reads a folder of exported .uasset files and works out where each one would have to go.
 *
 * The hard part is not copying files, it is knowing the package path to copy them to. A reference
 * inside a .uasset names its target by full package path, so an imported asset only resolves when
 * it is restored to the path it came from. Getting that path wrong does not fail loudly - it
 * produces an asset that loads with missing references, which is discovered much later.
 *
 * Four sources are tried in order, and the one used is reported per file so the UI can show it:
 *
 *   1. Our own manifest. Authoritative: we wrote it from what the exporter actually wrote.
 *   2. The package header. FPackageFileSummary carries "the package name the file was last saved
 *      with" - but the engine's own AssetHeaderPatcher notes it is not always serialised and falls
 *      back when it is empty or "None", so this cannot be trusted blindly either.
 *   3. The folder structure under the import root, which is exactly right for a MirrorTree export
 *      and a guess for anything else.
 *   4. Nothing. The file can still be imported, but only into a folder the user names, and its
 *      references will not resolve. Said plainly rather than papered over.
 */
namespace AssetImportScanner
{
    /** Where a candidate's target package path came from. Shown per row; not an implementation detail. */
    enum class EPackageNameSource : uint8
    {
        /** From AssetUsageAudit.manifest.json written beside the files. */
        Manifest,

        /** From FPackageFileSummary::PackageName inside the .uasset. */
        PackageHeader,

        /** Inferred from the file's position under the import root. Correct for a mirrored export. */
        FolderStructure,

        /** Not recoverable. Importing this file cannot restore its references. */
        Unresolved
    };

    ASSETUSAGEAUDITCORE_API const TCHAR* LexToString(EPackageNameSource Source);

    struct FCandidate
    {
        /** Absolute path of the file in the source folder. */
        FString SourceFile;

        /** Path relative to the import root, for display. */
        FString RelativePath;

        /** Package it would be restored to. None when Unresolved. */
        FName TargetPackage;

        EPackageNameSource NameSource = EPackageNameSource::Unresolved;

        /**
         * A package of that name already exists in this project.
         *
         * Never imported silently: overwriting is how an import tool destroys someone's work, and
         * the file on disk gives no hint that it was about to happen.
         */
        bool bTargetExists = false;

        /** Packages this asset referenced at export time. Only known from a manifest. */
        TArray<FName> Dependencies;

        /**
         * Dependencies that are neither in this folder nor already in the project.
         *
         * A non-zero count means the asset will import and then open with something missing.
         */
        int32 MissingDependencies = 0;
    };

    struct FOptions
    {
        /** Folder to read. Scanned recursively. */
        FString SourceDirectory;
    };

    struct ASSETUSAGEAUDITCORE_API FStats
    {
        bool bManifestFound = false;

        int32 FilesFound = 0;
        int32 FromManifest = 0;
        int32 FromPackageHeader = 0;
        int32 FromFolderStructure = 0;
        int32 Unresolved = 0;

        /** Candidates whose target already exists in the project. */
        int32 WouldOverwrite = 0;

        FString Summarise() const;
    };

    /**
     * Scan a folder for importable packages.
     *
     * Reads only package headers, never loads an asset: a folder can hold gigabytes, and the list
     * has to appear immediately for the user to tick through.
     */
    ASSETUSAGEAUDITCORE_API TArray<FCandidate> Scan(
        IAssetRegistry& Registry,
        const FOptions& Options,
        FStats& OutStats);

    /**
     * The package name a .uasset was saved with, or empty when the file does not carry one.
     *
     * Empty is a normal answer, not an error: the field is not always serialised. Callers must have
     * a fallback rather than treating an empty result as a corrupt file.
     */
    ASSETUSAGEAUDITCORE_API FString ReadPackageNameFromFile(const FString& FilePath);
}
