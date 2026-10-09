// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetUsageAuditTypes.h"

/**
 * Collision-handling for export filenames.
 *
 * Deliberately a free function over strings with an injectable existence predicate: the rule
 * ("increment an existing index, do not append a second one") is fiddly on real asset names like
 * SM_Rock_02_v3, and it must be unit-testable without touching a filesystem or an editor.
 */
namespace AssetExportNaming
{
    /** Predicate answering "does this filename already exist in the target folder". */
    using FExistsPredicate = TFunctionRef<bool(const FString& /*FileName*/)>;

    /**
     * Split a base name into stem and trailing numeric index.
     *
     * "Foo"          -> {"Foo",         INDEX_NONE, 0}
     * "Foo_1"        -> {"Foo",         1,          1}
     * "Foo_007"      -> {"Foo",         7,          3}   <- padding width preserved
     * "SM_Rock_02_v3"-> {"SM_Rock_02_v3", INDEX_NONE, 0}  <- v3 is not a numeric suffix
     * "SM_Rock_02"   -> {"SM_Rock",     2,          2}
     * "Foo_"         -> {"Foo_",        INDEX_NONE, 0}   <- empty suffix is not an index
     *
     * @param BaseName      Filename without extension.
     * @param OutStem       Portion before the trailing _N, or the whole name when there is none.
     * @param OutIndex      Parsed index, or INDEX_NONE.
     * @param OutPadWidth   Digit count of the parsed index, so Foo_007 -> Foo_008 not Foo_8.
     */
    ASSETUSAGEAUDITCORE_API void SplitTrailingIndex(const FString& BaseName, FString& OutStem, int32& OutIndex, int32& OutPadWidth);

    /**
     * Compose a filename from stem, index and padding.
     * (Foo, INDEX_NONE, 0) -> "Foo"
     * (Foo, 1, 1)          -> "Foo_1"
     * (Foo, 8, 3)          -> "Foo_008"
     */
    ASSETUSAGEAUDITCORE_API FString ComposeIndexedName(const FString& Stem, int32 Index, int32 PadWidth);

    /**
     * Resolve a target filename under the given collision policy.
     *
     * Overwrite -> returns DesiredFileName unchanged.
     * Index     -> returns the first non-colliding name, incrementing any existing trailing index.
     *
     * @param DesiredFileName  Filename with extension, e.g. "SM_Rock.uasset".
     * @param Policy           Overwrite or Index.
     * @param Exists           Predicate over filenames (with extension) in the target folder.
     * @return Filename with extension that does not collide, or DesiredFileName under Overwrite.
     */
    ASSETUSAGEAUDITCORE_API FString ResolveCollision(const FString& DesiredFileName, EExportCollisionPolicy Policy, FExistsPredicate Exists);
}
