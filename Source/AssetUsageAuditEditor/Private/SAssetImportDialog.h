// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"

/**
 * Show the import window: pick a folder, review what it holds, tick what to bring in.
 *
 * Modal, like the export window and for the same reason - the list describes a folder on disk at a
 * moment in time, and letting the project change underneath it would make the "already exists" and
 * "missing dependency" columns lie.
 *
 * Blocks until the window closes. Nothing is written unless Import is pressed.
 */
void ShowAssetImportDialog();
