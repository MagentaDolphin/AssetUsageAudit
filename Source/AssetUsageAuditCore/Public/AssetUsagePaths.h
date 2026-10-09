// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"

/**
 * Package-path predicates and conversions.
 *
 * Pure string work, no registry and no engine state, so it is unit-testable and cheap enough to
 * call inside the per-asset sweep.
 *
 * Note on OFPA: these helpers only ever *classify* a path. They never try to derive a level's
 * external-actor folder by string building, and they never try to recover the owning level from
 * an external actor path. Both are real traps - Content Bundles inject /CB/<Guid>/ and External
 * Data Layers inject /EDL/<UID>/ between the folder and the level path, and plugins can register
 * additional roots through delegates. Producing those paths is ULevel::GetExternalActorsPaths's
 * job; recovering ownership is avoided entirely by traversing forward from the level.
 */
namespace AssetUsagePaths
{
    /** True for a package under an __ExternalActors__ or __ExternalObjects__ root. */
    ASSETUSAGEAUDITCORE_API bool IsExternalPackage(FName PackageName);
    ASSETUSAGEAUDITCORE_API bool IsExternalPackage(FStringView PackagePath);

    /** True for /Script/... - code, not an asset. Cannot be exported or reported unused. */
    ASSETUSAGEAUDITCORE_API bool IsScriptPackage(FName PackageName);
    ASSETUSAGEAUDITCORE_API bool IsScriptPackage(FStringView PackagePath);

    /** True for /Engine/... or /Temp/... - not project content. */
    ASSETUSAGEAUDITCORE_API bool IsEngineOrTempPackage(FStringView PackagePath);

    /**
     * True when the package sits under any of the given exclusion prefixes.
     *
     * Prefixes may be given in either package form ("/Game/3rdParty") or content-relative form
     * ("Content/3rdParty"), because the UI shows users the latter and settings files tend to
     * accumulate both. Matching is case-insensitive and boundary-aware, so "/Game/Art" does not
     * exclude "/Game/ArtSource".
     */
    ASSETUSAGEAUDITCORE_API bool IsPathExcluded(FName PackageName, const TArray<FString>& ExcludedPrefixes);
    ASSETUSAGEAUDITCORE_API bool IsPathExcluded(FStringView PackagePath, const TArray<FString>& ExcludedPrefixes);

    /**
     * Normalise an exclusion prefix to package form with no trailing slash.
     * "Content/3rdParty/"  -> "/Game/3rdParty"
     * "/Game/3rdParty"     -> "/Game/3rdParty"
     * "Content"            -> "/Game"
     */
    ASSETUSAGEAUDITCORE_API FString NormalizeExclusionPrefix(const FString& Prefix);

    /**
     * Package name to a path relative to the project root, as the report requires.
     * "/Game/Art/SM_Rock" -> "Content/Art/SM_Rock.uasset"
     *
     * @param bIsLevel  Chooses the .umap extension over .uasset.
     * @return Empty for packages with no project-relative form, e.g. /Script or /Engine.
     */
    ASSETUSAGEAUDITCORE_API FString ToProjectRelativePath(FName PackageName, bool bIsLevel);

    /**
     * Can this folder be used as a Migrate destination?
     *
     * Mirrors the two checks UAssetToolsImpl makes, and exists because it makes them *after* being
     * called and reports the refusal to the Output Log alone - so an impossible export otherwise
     * presents as a button that does nothing.
     *
     * The engine's rules:
     *   1. the path must end in /Content/                (MigratePackages_ReportConfirmed);
     *   2. the folder above it must hold a .uproject, or exactly one .uplugin
     *      (FPackageMigrationImpl::GetMountPointRootPath) - that is where the destination mount
     *      point comes from, and without it Migrate aborts.
     *
     * Lives in Core despite serving an editor-only feature: it is filesystem and string work with
     * no AssetTools involved, so it belongs with the other path predicates and can be tested
     * without an editor module.
     *
     * @return Reason the folder is unusable, or an empty string when it is fine.
     */
    ASSETUSAGEAUDITCORE_API FString ValidateMigrateDestination(const FString& Directory);
}
