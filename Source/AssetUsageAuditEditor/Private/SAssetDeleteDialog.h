// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"

/** What the delete window ended up doing. Everything the panel needs to report afterwards. */
struct FAssetDeleteOutcome
{
    /** Packages handed to the window. */
    int32 Requested = 0;

    /** Refused by policy - levels, OFPA packages, engine content. Never reaches the engine. */
    int32 Refused = 0;

    /** Ticked and passed to ObjectTools. */
    int32 Attempted = 0;

    /**
     * Packages the registry can no longer find afterwards.
     *
     * Measured rather than taken from the engine's return value: ObjectTools reports a count of
     * deleted *objects*, and one package can hold several. Asking the registry what is actually
     * gone is the only figure that matches what the user will see in the Content Browser.
     */
    TArray<FName> DeletedPackages;

    /** True when the user closed or cancelled the window; nothing was touched. */
    bool bCancelled = true;
};

/**
 * Show the delete window: what is about to be destroyed, what still points at it, and why some
 * rows are refused.
 *
 * Modal, and deliberately heavier than the export window. Export is reversible by deleting the
 * output folder; this is not, and the window's job is to be read rather than clicked through -
 * hence the per-row referencer count, the explicit acknowledgement, and Cancel as the default.
 *
 * The window does not delete anything itself. Once confirmed it hands the assets to
 * ObjectTools::DeleteAssets with the engine's own confirmation enabled, so the familiar Delete
 * Assets dialog - referencer list, Force Delete, Replace References - appears exactly as it does
 * from the Content Browser. Reimplementing that would mean reimplementing reference replacement,
 * and getting it subtly wrong on someone's project.
 */
FAssetDeleteOutcome ShowAssetDeleteDialog(const TArray<FName>& Packages);
