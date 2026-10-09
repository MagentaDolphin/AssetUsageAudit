# Asset Usage Audit

[English](#english) · [Русский](#русский)

## English

### What it is

Asset Usage Audit is a source-code editor plugin for Unreal Engine 5.6. It answers two related questions:

- which project assets are used by which levels;
- which assets are not reached from any level.

The audit is transitive: it walks from maps through Blueprints and other assets instead of looking only at direct references. The graph includes hard and soft package references, streaming sublevels and Level Instances, and One File Per Actor packages under `__ExternalActors__` and `__ExternalObjects__`. Every result receives one of five explicit verdicts rather than a misleading used/unused boolean.

Selected rows can be written as a JSON/CSV report, copied as Unreal package files, converted through Unreal exporters to configured exchange formats, migrated into another Unreal project, or passed to Unreal's native deletion workflow after a safety review.

The plugin is version `0.1`, is authored by MagentaDolphin, and contains no content of its own.

### Requirements and installation

- Unreal Engine 5.6.
- Win64. Every module in `AssetUsageAudit.uplugin` has `PlatformAllowList: ["Win64"]`; no other platform is declared or claimed.
- A C++ Unreal project or another way to compile a source plugin. Prebuilt binaries are not stored in this repository.

Install it in a project:

1. Close Unreal Editor so its plugin DLLs are not locked.
2. Place the repository at `MyGame/Plugins/AssetUsageAudit`.
3. Regenerate the project's IDE files if the project workflow requires it.
4. Build the project's Editor target for Win64 Development.
5. Open the project. The descriptor has `EnabledByDefault: true`; if the host project overrides plugin activation, enable **Asset Usage Audit** in the Plugins window and restart the editor.

Example build command:

```bat
"D:\Epic\UE_5.6\Engine\Build\BatchFiles\Build.bat" ^
  MyGameEditor Win64 Development ^
  -project="D:\Projects\MyGame\MyGame.uproject" -waitmutex
```

The repository can also be attached as a submodule:

```bash
git submodule add https://github.com/MagentaDolphin/AssetUsageAudit Plugins/AssetUsageAudit
```

### Quick start

1. Open `Window → Tools → Asset Usage Audit`, or run `AssetUsageAudit.OpenPanel` in the Unreal console.
2. In **Level**, select one or more maps. This controls the audit scope; an empty selection means all levels.
3. Press **Run Audit**. The first run after editor startup waits until the Asset Registry has finished loading, because a partial registry would produce a false unused list.
4. Narrow the displayed result with **Types**, **Gameplay class**, **Verdicts**, **References**, or the name/path search field.
5. Tick the required rows. The header checkbox ticks or clears all rows currently visible through the filters.
6. Press **Export report (JSON + CSV)** for a report of the current filtered view, or **Export ticked assets...** to choose a file export mode.

Double-clicking an asset row browses to it in the Content Browser. The row is not loaded merely to locate it, which matters for large meshes and maps.

### Interface

#### Toolbar

| Control | Purpose |
|---|---|
| **Run Audit** | Builds the package graph and resolves level reachability. |
| **By asset / By level** | Changes the question without rerunning the audit. **By asset** lists assets and shows their levels. **By level** creates a tree with level headers and their assets; with all levels in scope, locations can be compared side by side. A level header ticks all children, and a partial child selection is shown as an indeterminate checkbox. |
| **Level** | Multi-select audit scope. No selected levels means all levels. Changing it marks the current result stale and does not start a potentially expensive run implicitly. |
| **Types** | Result filter. Presets are `StaticMesh`, `SkeletalMesh`, `Material`, `Texture`, `VFX`, `Sound`, `Blueprint`, `Level`, `DataAsset`, and `Animation`; raw asset classes follow. Presets expand through subclasses, so `Material` also catches material instances. Presets absent from the current project are not offered. Selecting an already fully selected preset clears it. |
| **Verdicts** | Result filter over all five verdicts. All verdicts are visible by default. |
| **References** | Result filter by incoming-edge strength (`Any`, `Hard only`, `Soft only`, `Mixed`, `No incoming edges`) and by required provenance flags: OFPA external actor, sublevel/Level Instance, editor-only edge, redirector, Config, and Source. Selected provenance flags are combined as requirements. |
| **Gameplay class** | Case-insensitive substring filter on a Blueprint's generated class. While non-empty it hides non-Blueprint rows. |
| **Search** | Case-insensitive filter by asset name or project-relative path. |

The distinction between scope and filters is intentional:

- **Level** changes what is traversed. The old result becomes stale and **Run Audit** must be pressed again.
- **Types**, **Gameplay class**, **Verdicts**, **References**, and **Search** only change which rows from the completed run are displayed. They do not alter graph traversal.

#### Columns

`Check · Type · Name · Path · Verdict · Levels · Hard · Soft · Provenance · Route`

- **Type** is the asset class, such as `Blueprint`. A Blueprint's gameplay class is kept separately and appears in its tooltip. This prevents the type filter and the displayed type from describing different axes.
- **Levels** is the number of levels that reach the asset.
- **Hard** and **Soft** are incoming reference counts. Strength is recorded on graph edges; it is never used to narrow the dependency query.
- **Provenance** can contain `Hard`, `Soft`, `ExternalActor`, `Sublevel`, `Config`, `Source`, `EditorOnly`, and `Redirector`.
- **Route** is a human-readable chain explaining a level-reachable verdict, for example `WP_Example → BP_Child → BP_Master → SM_Station`. A route is evidence, not a reconstructed guess.

### Verdicts

The exact `EAssetUsageVerdict` values are:

| Verdict | Meaning |
|---|---|
| `UsedOnLevel` | At least one selected level reaches the asset. The levels, route, provenance, and hard/soft columns explain how. |
| `UsedByAssetsOnly` | The asset has referencers, but no chain from the audited levels reaches it. |
| `ReferencedFromConfigOrSource` | A literal `/Game/...` package path was found while scanning project `Config/` or `Source/`; provenance identifies the file and line. |
| `Unreferenced` | The graph has no incoming package reference, no audited level reaches the asset, and it is not classified as a known registry blind spot. This is evidence about the Asset Registry, not a deletion guarantee. |
| `Unknown` | The asset belongs to a known blind spot where absence of Asset Registry edges is not meaningful. It must never be presented as safe to delete. |

#### `Unknown` is not “unused”

The Asset Registry cannot prove every runtime use. In particular, it cannot reliably see:

- paths assembled from strings in C++ or Blueprint;
- `OpenLevel(FName)` and comparable name-driven loading;
- plain path strings inside DataTable or CurveTable rows; consumers reference the whole table, not an independently traceable row;
- FMOD banks and events resolved by the FMOD runtime through string paths rather than UObject package edges.

The code therefore treats FMOD packages/classes, `DataTable`, and `CurveTable` as blind spots. Positive evidence still wins: a blind-spot asset demonstrably reached from a level is `UsedOnLevel`. Without positive evidence, it becomes `Unknown`, not `Unreferenced`.

When enabled, the indirect scanner supplements the registry by reading `.ini`, `.cpp`, `.h`, and `.cs` files under project `Config/` and `Source/`, extracting literal `/Game/...` paths, normalizing object/class suffixes, and recording file/line provenance. It cannot discover a path assembled dynamically at runtime.

The audit does not automatically propose or perform deletion based on a verdict. Manual deletion is available for explicitly ticked rows, but it has a separate safety scan, two confirmation stages, and repeats the warning that neither `Unknown` nor `Unreferenced` proves runtime non-use.

### Settings

Team settings are under `Project Settings → your project category → Asset Usage Audit`. `UAssetUsageAuditSettings` uses `config=Editor, defaultconfig`, so values are stored in `Config/DefaultEditor.ini` and can be version-controlled.

| Property | Default | Effect |
|---|---|---|
| `DefaultExportDirectory` | empty | Uses the absolute project `Saved/AssetUsageAudit` directory for reports and file export. |
| `bOverwriteExistingFiles` | `false` | The default collision policy is indexed names (`Foo_1`, `Foo_2`, and so on); `true` selects overwrite. |
| `ExchangeFormatByClass` | empty | Uses the built-in class-to-extension map. Lookup walks up the class hierarchy. |
| `ExportLayout` | `Flat` | Default file layout: `Flat`, `MirrorTree`, `FolderPerAsset`, or `Migrate`. |
| `bGroupDependenciesByType` | `false` | With `FolderPerAsset`, sorts dependencies into type-named subfolders; it has no effect on other layouts. |
| `ExcludedPackagePaths` | `Content/3rdParty`, `Content/StarterContent`, `Content/StarterBundle`, `Content/Megascans`, `Content/MSPresets` | Package/content-relative prefixes omitted from graph traversal and dependency expansion. |
| `IncludedPackagePaths` | empty | Empty means `/Game`; otherwise these roots define the graph input. |
| `bScanIndirectReferences` | `true` | Scans project Config and Source text files for literal `/Game` paths. |
| `bHideExternalPackages` | `true` | Hides OFPA plumbing rows while still traversing those packages. |

Do not exclude purchased content blindly. A third-party package may provide a GameMode, GameInstance, startup map, or another class named only by configuration; excluding the root would remove that package from the graph before the indirect reference can make it actionable.

The built-in conversion map is `StaticMesh → fbx`, `SkeletalMesh → fbx`, `AnimSequence → fbx`, `Texture2D → png`, `TextureCube → hdr`, `SoundWave → wav`, `DataTable → csv`, `CurveTable → csv`, `FontFace → ttf`, and `World → fbx`. An empty `ExchangeFormatByClass` selects this map. A custom map replaces it, and only entries backed by an engine `UExporter` can produce files.

Per-user panel state is separate in `Saved/Config/.../EditorPerProjectUserSettings.ini`: selected levels, visible types and verdicts, generated-class filter, view mode, export mode, reference filters, and the include-dependencies choice. It is saved when the tab closes. The search text is deliberately not persisted, so the panel cannot reopen looking empty because of a forgotten search.

The export dialog starts from team defaults plus the user's last mode/dependency choice, but canceling it writes nothing back.

### Console commands

The editor module registers exactly three console commands:

| Command | Purpose | Example |
|---|---|---|
| `AssetUsageAudit.OpenPanel` | Opens the dockable audit panel. | `AssetUsageAudit.OpenPanel` |
| `AssetUsageAudit.Run` | Audits all levels and writes JSON and CSV reports; optional arguments restrict the run to level package names. | `AssetUsageAudit.Run /Game/Maps/WP_Example /Game/Maps/L_Interior` |
| `AssetUsageAudit.FindUnused` | Writes a report excluding `UsedOnLevel` rows. The remaining set still includes distinct `UsedByAssetsOnly`, `ReferencedFromConfigOrSource`, `Unreferenced`, and `Unknown` verdicts. | `AssetUsageAudit.FindUnused /Game/Maps/WP_Example` |

With no level arguments, `Run` and `FindUnused` consider every level found in the configured graph roots. Both wait for the Asset Registry to finish scanning before they run. Reports use the configured output directory and a timestamped `AssetUsageReport_YYYYMMDD_HHMMSS` base name.

### Blueprint and Python API

`UAssetUsageAuditLibrary` is a `UBlueprintFunctionLibrary`; all four functions are `BlueprintCallable` in the **Asset Usage Audit** category and use the same settings and Core audit implementation as the panel.

| Function | Result |
|---|---|
| `RunAudit(LevelPackageNames, OutputDirectory, OutReportPath)` | Runs an audit, writes JSON and CSV, returns success, and outputs the absolute JSON path. Empty levels mean all; an empty output directory uses the configured default. |
| `GetAssetsUsedOnLevel(LevelPackageName)` | Returns package names whose verdict is `UsedOnLevel` for that level. |
| `GetLevelsUsingAsset(AssetPackageName)` | Audits all levels and returns the levels that reach the package. A streamed sublevel asset can be attributed to both the sublevel and its parent. |
| `MeasureFullSweepSeconds()` | Runs a full sweep, logs its breakdown, and returns elapsed seconds. It is a measurement entry point, not a cached estimate. |

In an Editor Utility Blueprint, add nodes from the **Asset Usage Audit** category and pass long package names such as `/Game/Maps/WP_Example`, not filesystem paths.

The reflected Python names use Unreal's snake_case convention:

```python
import unreal

success, json_path = unreal.AssetUsageAuditLibrary.run_audit(
    ["/Game/Maps/WP_Example"],
    "D:/AuditOutput"
)

assets = unreal.AssetUsageAuditLibrary.get_assets_used_on_level(
    "/Game/Maps/WP_Example"
)
levels = unreal.AssetUsageAuditLibrary.get_levels_using_asset(
    "/Game/Art/SM_Rock"
)
seconds = unreal.AssetUsageAuditLibrary.measure_full_sweep_seconds()
```

These calls are editor-facing: the library lives in `AssetUsageAuditEditor`, not in a runtime module.

### Reports

Every report write produces a pair from the same in-memory result:

- JSON for tools and scripts;
- CSV for spreadsheets.

The default destination is `Saved/AssetUsageAudit`. The panel asks for a directory and reports exactly the rows currently visible after UI filters; `RunAudit` and the console commands use their request/settings directly.

The JSON contains:

- a header with generation time, project, engine and tool versions, levels/assets/rows scanned, duration, and applied filters;
- counts for all five verdicts;
- graph statistics: packages, edges, levels, external packages, enumeration time, and dependency time;
- one object per asset with package/name/path/type, optional generated class, verdict, provenance, hard/soft counts, levels, and optional route/provenance detail.

The CSV is UTF-8 with BOM so Excel preserves non-ASCII names. It uses `;` between fields and `|` inside multi-value fields. Its columns are:

`Type · GeneratedClass · Name · PathFromProjectRoot · Verdict · LevelCount · Levels · HardRefs · SoftRefs · Provenance · Route · ProvenanceDetail`

Comment lines above the header record generation metadata, filters, verdict counts, scan duration, and the warning that `Unknown` is not unused. Read the verdict together with provenance and route; a count without the path that produced it is not enough to make a content decision.

### Exporting files

Press **Export ticked assets...** after ticking rows. A modal window keeps the selected set stable while you choose the mode, destination, folder layout, dependency behavior, manifest, and collision policy. Canceling changes neither files nor persisted choices.

#### Modes and layouts

The two modes are:

- **Copy `.uasset`**: byte-exact package copying without loading the assets. Exported maps also bring their discovered OFPA `.uasset` packages so the copied map is not empty.
- **Convert (FBX/PNG/WAV)**: loads each asset and runs `UAssetExportTask` with the configured extension. It performs garbage collection after every 64 loaded assets by default.

The four mutually exclusive layouts are:

| Layout | Behavior |
|---|---|
| `Flat` | Places every output file in one directory. Name collisions are handled by the selected policy. |
| `MirrorTree` | Recreates the package tree under the export root, for example `/Game/Art/SM_Rock` becomes `Game/Art/SM_Rock.uasset`. |
| `FolderPerAsset` | Creates one folder for each manually ticked asset and places its dependency closure beside it. A shared dependency is copied into every seed folder that needs it, so file placements can exceed distinct assets. |
| `Migrate` | Hands only the ticked packages to Unreal's `IAssetTools::MigratePackages`; Unreal builds the closure and restores packages at their original paths in another project. This is the only engine-managed option intended to preserve live Unreal references automatically. |

With `FolderPerAsset`, **Sort dependencies into type subfolders** can place dependencies under preset-named folders such as `Texture`, `Material`, and `StaticMesh`; the seed remains at its folder root. The checkbox stays visible but disabled for layouts where it cannot apply.

The collision policy is **Keep both** (indexed names) or **Overwrite**. For Migrate, renaming is impossible because changing a package path would defeat reference preservation; **Keep both** therefore maps to Unreal's **Skip** conflict policy.

#### A copied `.uasset` does not carry its dependencies

A package stores references as full package names such as `/Game/Art/T_Rock_D`, not as relative paths beside the copied file. Copying `SM_Rock.uasset` alone does not embed its materials or textures. Flat and per-asset layouts also change package placement, so their raw output is for review/handoff unless it is restored with path information.

Use **Migrate** for a direct Unreal-to-Unreal transfer with engine-managed paths. Its destination must:

1. end in a `Content` directory;
2. have either a `.uproject` or exactly one `.uplugin` one directory above it, allowing Unreal to derive the mount point.

`AssetUsagePaths::ValidateMigrateDestination` checks both conditions before enabling export. Switching to Migrate clears a prefilled destination if it is invalid. `MigratePackages` returns `void` and reports its own result through Unreal notifications and the Output Log, so the panel does not invent a copied-file count.

Migrate receives only the ticked packages; passing the plugin's already-expanded closure would make Unreal traverse redundant seeds. Disabling dependencies sets `bIgnoreDependencies`; Unreal then also omits a level's OFPA packages, so the panel asks for an additional confirmation because a migrated level can arrive empty.

#### Include referenced assets

**Include referenced assets** is on by default. For copy/convert layouts, the plugin computes each seed's transitive package dependency closure using the same package-category, no-requirements query as the audit. Before writing, the dialog shows the number of ticked seeds and actual file placements.

Traversal boundaries are deliberate:

- `/Script` is code and is never copied;
- `/Engine` and `/Temp` are excluded by default;
- project exclusion paths also bound dependency expansion;
- a manually ticked seed bypasses those path filters, because an explicit selection must still produce that file;
- a foreign map reached through an ordinary asset is not expanded; sublevels and Level Instances reached structurally from a level or external package are followed.

This closure is not `MigratePackages`: it is bounded by the audit exclusions and leaves destination placement under plugin control.

#### Dependency manifest

For non-Migrate layouts, **Write dependency manifest** is on by default and writes `AssetUsageAudit.manifest.json` at the export root. It records schema/tool/project/engine metadata, layout, collision policy, and for every written file:

- original package name;
- actual path relative to the export root after collision handling;
- asset/class hints when the registry provides them;
- whether the package was manually selected (`seed`) or arrived as a dependency;
- its direct package dependencies, including the relative file when that dependency is present in the same export.

The manifest is built from the exporter's recorded actual paths, not planned names. This matters precisely when a collision causes indexing. An empty recorded-file list is treated as an error rather than writing a plausible but false empty manifest. Migrate refuses manifest generation because it already preserves package paths.

#### Raw copies and conversion

Raw copy removes a read-only attribute from an existing destination before overwrite and from the copied file afterward. This prevents a read-only source attribute from making an exported handoff immutable or blocking the next overwrite.

Conversion uses the class-to-extension map and searches up the class hierarchy. A `Texture` mapping can therefore cover subclasses; only classes for which Unreal has an appropriate `UExporter` produce output. Blueprint has no built-in mapping by design. Materials, Blueprints, and Niagara assets have no default exchange format here.

Skipped conversions are reported separately as **no configured format**, **asset failed to load**, or **no exporter for that format**. Conversion loads assets and can be materially slower than byte copying; the repository makes no general performance promise for large batches.

### Importing from an external folder

Open `Window → Tools → Import Assets from Folder...`. This workflow recursively scans `.uasset` and `.umap` files without loading them, lists candidates, and lets you choose what to copy back into the current project.

The table shows the source file, target package, where that target came from, dependency status, and whether the target already exists. Target recovery uses four sources in descending confidence:

| Source | Meaning |
|---|---|
| `Manifest` | Exact original package and actual exported filename from `AssetUsageAudit.manifest.json`. |
| `Package header` | `FPackageFileSummary::PackageName`, when the package serialized a valid name. Unreal itself permits this field to be absent or `None`, so fallback is required. |
| `Folder structure` | Inferred from the file's location under the scan root. Exact for the plugin's mirrored layout and only a guess for other folders. |
| `Unresolved` | No target could be recovered. A user-supplied `/Game/...` fallback directory is required, and original references may remain broken. |

When a manifest is present, the scanner also reports dependencies that are neither in the import folder nor already registered in the project.

Import safety rules are strict because overwrite has no undo or backup:

- an existing target is skipped unless **Overwrite existing assets** is enabled explicitly;
- if selected rows would be overwritten, a separate confirmation states the count and consequence;
- a package already loaded in the editor is never overwritten, even when overwrite is enabled, because stale in-memory objects could later save over the imported file;
- an unresolved file is skipped unless a valid fallback package directory is supplied;
- invalid target paths and failed copies are reported individually.

After successful copies, `ScanFilesSynchronous(..., bForceRescan=true)` makes them visible to the Content Browser. The dialog rescans afterward, so newly imported targets appear as already present instead of inviting a duplicate second import.

### Deleting ticked assets

**Delete ticked assets...** acts on exactly the rows selected in the panel. It never expands the set with dependencies: expansion is helpful for export, but during deletion a shared texture could otherwise remove content nobody selected.

Deletion passes through two windows:

1. The plugin's review window lists every requested package with name, type, path, outside referencer count, and status. Deletable rows start checked. Referencers that are themselves in the delete set are excluded from the outside count; up to eight names are shown in the tooltip, while the count remains exact. Map/OFPA referencers are also identified as level references, and read-only files are flagged before deletion. The Delete button stays disabled until **I understand these files will be removed from the project** is checked.
2. The confirmed set is resolved again to fresh `FAssetData` and passed to `ObjectTools::DeleteAssets(..., bShowConfirmation=true)`. Unreal's native **Delete Assets** window then owns the final reference list, **Force Delete**, and **Replace References** behavior.

The plugin refuses these packages before the native window:

| Refusal | Reason |
|---|---|
| Level (`.umap`) | A level is the unit against which the audit measures usage; deleting it invalidates the rest of the result. Unreal also treats an open map specially. Delete maps deliberately from the Content Browser. |
| OFPA package under `__ExternalActors__` or `__ExternalObjects__` | It represents a placed actor/object belonging to a map. Remove it in the level editor, where the operation participates in the level workflow and undo. |
| `/Engine`, `/Script`, or `/Temp` | It is not deletable project content for this tool. |
| Missing package | The registry no longer has an asset under that package name. |

An outside referencer or read-only flag is a warning, not a silent omission: Unreal's native window makes the final decision. Source control is the recovery mechanism; the plugin creates no backup and offers no undo.

After deletion, removed rows disappear, but other rows retain reference counts and verdicts from the old graph. The panel marks the result stale and asks for another audit. `Unreferenced` still does not prove absence of dynamic string, DataTable-row, or FMOD use, and the deletion window repeats that warning.

### Architecture

| Module | Type | Responsibility |
|---|---|---|
| `AssetUsageAuditCore` | `UncookedOnly`, `Default` | Graph construction, transitive level resolution, OFPA handling, verdicts, indirect scan, report writer, dependency closure, file export/layout/manifest, import scan/copy, and pre-delete analysis. No Slate UI or editor asset pipeline. |
| `AssetUsageAuditEditor` | `Editor`, `PostEngineInit` | Dockable Slate panel, toolbar/menu registration, settings, console commands, Blueprint/Python library, export/import/delete dialogs, Migrate dispatch, and native deletion handoff. |
| `AssetUsageAuditTests` | `UncookedOnly`, `Default` | Automation Specs for Core behavior plus editor-context/live coverage, including Migrate. |

All three descriptor entries are Win64-only. `AssetUsageAuditCore` publicly depends on `Core`, `CoreUObject`, and `AssetRegistry`, and privately on runtime `Engine`, `Json`, and `Projects`. Its invariant is to remain free of `UnrealEd`, `AssetTools`, `ToolMenus`, `Slate`, `SlateCore`, and editor subsystems. `Engine` is permitted because `ULevel::GetExternalActorsPaths`, `GetExternalObjectsPaths`, and level asset scanning are runtime APIs available to commandlet-oriented code.

#### Correctness invariants

1. **Dependency traversal uses package category with `FDependencyQuery{}` (`NoRequirements`).** A hard-only query loses soft references and OFPA edges. `Soft` is a negated hard requirement, so combining required Hard and Soft is not “either kind.” Hard/soft remains report data, never a traversal filter.
2. **A map boundary is crossed only from a level already in the result or from one of its external packages.** This admits streaming sublevels and Level Instances without pulling an unrelated map and all of its content through an ordinary data/Blueprint reference. The internal `bTraverseIntoOtherLevels` option exists for callers that explicitly need the broader behavior.
3. **OFPA roots are obtained from `ULevel::GetExternalActorsPaths` and `GetExternalObjectsPaths`, never assembled as strings.** Content Bundles, External Data Layers, and plugin delegates can add path structure. Resolution proceeds forward from the map; it does not invent an external-package-to-map reverse edge.
4. **Blueprint asset class and generated gameplay class are separate.** A Blueprint asset's class is `/Script/Engine.Blueprint`; gameplay filtering uses the `GeneratedClass` registry tag and subclass expansion.
5. **`ScanLevelAssets` runs before level traversal by default.** Otherwise an unopened OFPA level can look empty because its external packages have not entered the registry yet.

The implementation keeps checkbox state in package-name sets rather than `SListView` selection, so filtering does not erase a user's ticked set. Scope changes do not auto-run. Reports and all public entry points call the same Core auditor. Conversion remains in Core because `UExporter` and `UAssetExportTask` are Engine APIs; concrete exporters are resolved at runtime and an unavailable exporter becomes an explicit skip reason. Migrate stays in Editor because `AssetTools` is editor-only.

### Tests

`Source/AssetUsageAuditTests/Private` contains 16 `*.spec.cpp` Automation Spec suites. Counting the `It(...)` definitions in those files gives 169 cases. The test prefix is `AssetUsageAudit`.

Build the editor target first, then run from a Windows command prompt:

```bat
"D:\Epic\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:\Projects\MyGame\MyGame.uproject" ^
  -ExecCmds="Automation RunTests AssetUsageAudit" ^
  -TestExit="Automation Test Queue Empty" ^
  -unattended -nopause -nosplash -stdout ^
  -abslog="D:\Projects\MyGame\Saved\Logs\AssetUsageAuditTests.log"
```

For a machine without a desktop/GPU session, add `-nullrhi` so Unreal does not initialize a graphics device:

```bat
"D:\Epic\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:\Projects\MyGame\MyGame.uproject" ^
  -ExecCmds="Automation RunTests AssetUsageAudit" ^
  -TestExit="Automation Test Queue Empty" ^
  -unattended -nopause -nosplash -stdout -nullrhi ^
  -abslog="D:\Projects\MyGame\Saved\Logs\AssetUsageAuditTests.log"
```

Some suites deliberately inspect real host-project content and skip individual checks with warnings if no suitable subject exists. In particular, `AssetUsageAudit.ExporterLive`, `AssetUsageAudit.ExchangeExport`, `AssetUsageAudit.GraphFidelity`, and `AssetUsageAudit.MigrateLive` search the Asset Registry for usable project assets/levels rather than relying on fixed package names. Review warnings to distinguish a genuinely exercised live check from a skipped one.

`GraphFidelity` compares a stable package-name sample of the plugin graph with raw Asset Registry answers: missing/extra edges, hard classification by target, real route transitions, the OFPA soft-edge invariant, and sublevel attribution. Comparing by package name avoids registry enumeration-order instability; hard state is compared per target because Unreal may return the same target through both hard and soft edges.

The previous host project recorded an unrelated `TNotNull` fatal with `-nullrhi` even when all plugins were disabled, and a missing default-material shader map with `-NoShaderCompile`. Those are host-project constraints, not successful flags for that project. The plugin repository contains no `.uproject`, so executing the suite always requires a UE 5.6 host project with the plugin installed.

Tests that intentionally emit `UE_LOG(Error)` declare the message with `AddExpectedError`; otherwise Unreal Automation correctly treats the log error as a failed test.

### Limitations

- This is an editor/source plugin, not a runtime gameplay system. It does not ship an in-game UI or runtime audit.
- The descriptor declares only Win64. Other platforms are neither packaged nor supported by the repository metadata.
- The repository has no host `.uproject`, prebuilt DLLs, commandlet implementation, badges, screenshots, or external documentation pages.
- Asset Registry evidence cannot cover dynamically constructed paths, name-driven level loads, arbitrary DataTable/CurveTable row strings, or FMOD's string-addressed events. `Unknown` and even `Unreferenced` require human review.
- The Config/Source scan finds literal `/Game` paths in supported text files; it is not a C++ or Blueprint data-flow analyzer.
- Flat and per-asset package copies do not rewrite internal package references. Use the manifest/import workflow for restoration, or Unreal Migrate for direct project transfer.
- Conversion depends on installed/registered Unreal exporters. Classes without a configured format are skipped; a configured extension without an exporter is reported separately. Large conversion batches load assets, and repository tests cover individual exports rather than a published large-volume performance target.
- Delete and overwrite operations have no plugin-level undo or backup. They rely on explicit confirmation and source control.
- Slate interaction is not covered by the Automation Specs; UI behavior should be smoke-tested in the editor after UI changes.
- Live suites can warn and skip when a host project has no suitable real content. A green run should be reviewed together with those warnings.
- The Core architecture is commandlet-oriented, but this repository does not provide a dedicated audit commandlet.

### License

This repository contains no `LICENSE` file. The source is published for portfolio demonstration and review; licensing terms for reuse, redistribution, or incorporation into another project must be requested from the author. No third-party license is implied here.

### Links

- Repository: https://github.com/MagentaDolphin/AssetUsageAudit
- Author: MagentaDolphin

---

## Русский

### Что это

Asset Usage Audit — редакторный плагин с исходным кодом для Unreal Engine 5.6. Он отвечает на два связанных вопроса:

- какие ассеты проекта используются на каких уровнях;
- какие ассеты не достижимы ни от одного уровня.

Аудит транзитивный: он идёт от карт через Blueprints и другие ассеты, а не ограничивается прямыми ссылками. В граф входят жёсткие и мягкие пакетные ссылки, стриминговые подуровни и Level Instances, а также пакеты One File Per Actor из `__ExternalActors__` и `__ExternalObjects__`. Каждая строка получает один из пяти явных вердиктов вместо обманчивого булева «используется / не используется».

Отмеченные строки можно сохранить в отчёт JSON/CSV, скопировать как пакетные файлы Unreal, преобразовать штатными экспортёрами Unreal в настроенные обменные форматы, перенести в другой Unreal-проект через Migrate или передать в штатный сценарий удаления Unreal после отдельной проверки безопасности.

Версия плагина — `0.1`, автор — MagentaDolphin; собственного контента в плагине нет.

### Требования и установка

- Unreal Engine 5.6.
- Win64. У каждого модуля в `AssetUsageAudit.uplugin` задан `PlatformAllowList: ["Win64"]`; другие платформы не заявлены.
- C++-проект Unreal либо другой способ собрать плагин из исходников. Готовые бинарники в репозитории не хранятся.

Установка в проект:

1. Закройте Unreal Editor, чтобы DLL плагина не были заблокированы.
2. Поместите репозиторий в `MyGame/Plugins/AssetUsageAudit`.
3. Если этого требует принятый в проекте процесс, перегенерируйте файлы IDE.
4. Соберите Editor-таргет проекта в конфигурации Win64 Development.
5. Откройте проект. В дескрипторе стоит `EnabledByDefault: true`; если проект переопределяет активацию плагинов, включите **Asset Usage Audit** в окне Plugins и перезапустите редактор.

Пример команды сборки:

```bat
"D:\Epic\UE_5.6\Engine\Build\BatchFiles\Build.bat" ^
  MyGameEditor Win64 Development ^
  -project="D:\Projects\MyGame\MyGame.uproject" -waitmutex
```

Репозиторий можно подключить как сабмодуль:

```bash
git submodule add https://github.com/MagentaDolphin/AssetUsageAudit Plugins/AssetUsageAudit
```

### Быстрый старт

1. Откройте `Window → Tools → Asset Usage Audit` либо выполните в консоли Unreal `AssetUsageAudit.OpenPanel`.
2. В **Level** выберите одну или несколько карт. Это область аудита; пустой выбор означает все уровни.
3. Нажмите **Run Audit**. Первый прогон после запуска редактора ждёт окончания загрузки Asset Registry: неполный реестр дал бы ложный список неиспользуемого.
4. Сузьте показанный результат через **Types**, **Gameplay class**, **Verdicts**, **References** или поиск по имени и пути.
5. Отметьте нужные строки. Галочка в заголовке отмечает или очищает все строки, которые сейчас видны после фильтров.
6. Нажмите **Export report (JSON + CSV)** для отчёта по текущему отфильтрованному виду либо **Export ticked assets...**, чтобы выбрать режим выгрузки файлов.

Двойной клик по строке показывает ассет в Content Browser. Ради навигации ассет не загружается — это важно для тяжёлых мешей и карт.

### Интерфейс

#### Тулбар

| Контрол | Назначение |
|---|---|
| **Run Audit** | Строит пакетный граф и вычисляет достижимость от уровней. |
| **By asset / By level** | Меняет представление без повторного аудита. **By asset** перечисляет ассеты и показывает их уровни. **By level** строит дерево из заголовков-уровней и их ассетов; при области «все уровни» локации можно сравнивать рядом. Галочка заголовка отмечает всех потомков, частичный выбор показан третьим состоянием. |
| **Level** | Множественный выбор области аудита. Пусто означает все уровни. Изменение помечает текущий результат устаревшим и не запускает потенциально долгий прогон само. |
| **Types** | Фильтр результата. Сначала идут пресеты `StaticMesh`, `SkeletalMesh`, `Material`, `Texture`, `VFX`, `Sound`, `Blueprint`, `Level`, `DataAsset`, `Animation`, затем сырые классы ассетов. Пресеты раскрываются по подклассам, поэтому `Material` включает material instances. Отсутствующие в проекте пресеты не показываются. Клик по уже полностью выбранному пресету снимает его целиком. |
| **Verdicts** | Фильтр результата по пяти вердиктам. По умолчанию видны все. |
| **References** | Фильтр результата по силе входящих рёбер (`Any`, `Hard only`, `Soft only`, `Mixed`, `No incoming edges`) и обязательным признакам происхождения: внешний актор OFPA, подуровень/Level Instance, editor-only ребро, redirector, Config, Source. Выбранные признаки применяются совместно. |
| **Gameplay class** | Регистронезависимый поиск подстроки в сгенерированном классе Blueprint. Непустое поле скрывает строки не-Blueprint. |
| **Search** | Регистронезависимый фильтр по имени ассета и пути относительно проекта. |

Разница области и фильтров принципиальна:

- **Level** меняет сам обход. Старый результат становится устаревшим, после чего нужно снова нажать **Run Audit**.
- **Types**, **Gameplay class**, **Verdicts**, **References** и **Search** лишь скрывают строки готового прогона. Граф они не меняют.

#### Колонки

`Check · Type · Name · Path · Verdict · Levels · Hard · Soft · Provenance · Route`

- **Type** показывает класс ассета, например `Blueprint`. Геймплейный класс Blueprint хранится отдельно и показан в тултипе. Так отображение и фильтр типов не смешивают две разные оси.
- **Levels** — число уровней, от которых достижим ассет.
- **Hard** и **Soft** — счётчики входящих ссылок. Сила хранится на рёбрах графа и никогда не сужает запрос зависимостей.
- **Provenance** может содержать `Hard`, `Soft`, `ExternalActor`, `Sublevel`, `Config`, `Source`, `EditorOnly`, `Redirector`.
- **Route** — читаемая цепочка, объясняющая достижимость от уровня, например `WP_Example → BP_Child → BP_Master → SM_Station`. Это цепочка реальных рёбер, а не догадка, собранная после прогона.

### Вердикты

Точный список значений `EAssetUsageVerdict`:

| Вердикт | Значение |
|---|---|
| `UsedOnLevel` | Ассет достижим хотя бы от одного выбранного уровня. Колонки уровней, маршрута, происхождения и hard/soft объясняют, как именно. |
| `UsedByAssetsOnly` | У ассета есть референсеры, но ни одна цепочка от проверяемых уровней до него не дошла. |
| `ReferencedFromConfigOrSource` | При сканировании проектных `Config/` или `Source/` найден буквальный пакетный путь `/Game/...`; в происхождении указаны файл и строка. |
| `Unreferenced` | У графа нет входящей пакетной ссылки, ни один проверенный уровень не достигает ассета, а сам ассет не относится к известной слепой зоне реестра. Это свидетельство Asset Registry, но не гарантия безопасного удаления. |
| `Unknown` | Ассет относится к известной слепой зоне, где отсутствие рёбер Asset Registry ничего не доказывает. Такой результат нельзя выдавать за разрешение на удаление. |

#### `Unknown` — это не «не используется»

Asset Registry не может доказать все рантайм-использования. В частности, он ненадёжно видит или совсем не видит:

- пути, собранные из строк в C++ или Blueprint;
- `OpenLevel(FName)` и похожую загрузку по имени;
- обычные строки-пути внутри строк DataTable или CurveTable: потребитель ссылается на таблицу целиком, а не на отдельно отслеживаемую строку;
- банки и события FMOD, которые рантайм FMOD разрешает по строковым путям вне пакетного UObject-графа.

Поэтому код считает слепыми зонами пакеты/классы FMOD, `DataTable` и `CurveTable`. Положительное свидетельство важнее: если такой ассет явно достижим от уровня, его вердикт — `UsedOnLevel`. При отсутствии положительного свидетельства получается `Unknown`, а не `Unreferenced`.

Если включён косвенный скан, плагин дополняет реестр: читает `.ini`, `.cpp`, `.h`, `.cs` в проектных `Config/` и `Source/`, извлекает буквальные пути `/Game/...`, нормализует суффиксы объектов/классов и сохраняет происхождение в виде файла и номера строки. Динамически собранный рантайм-путь так найти нельзя.

Сам аудит не предлагает и не запускает удаление автоматически по вердикту. Вручную удалить явно отмеченные строки можно, но для этого есть отдельный анализ безопасности, два этапа подтверждения и повторное предупреждение: ни `Unknown`, ни `Unreferenced` не доказывают отсутствие рантайм-использования.

### Настройки

Командные настройки находятся в `Project Settings → категория с именем проекта → Asset Usage Audit`. У `UAssetUsageAuditSettings` задано `config=Editor, defaultconfig`, поэтому значения попадают в `Config/DefaultEditor.ini` и могут храниться под контролем версий.

| Параметр | По умолчанию | Что делает |
|---|---|---|
| `DefaultExportDirectory` | пусто | Для отчётов и файлов используется абсолютный проектный путь `Saved/AssetUsageAudit`. |
| `bOverwriteExistingFiles` | `false` | По умолчанию коллизии разрешаются индексированием (`Foo_1`, `Foo_2` и далее); `true` выбирает перезапись. |
| `ExchangeFormatByClass` | пусто | Используется встроенная карта «класс → расширение». Поиск идёт вверх по иерархии классов. |
| `ExportLayout` | `Flat` | Раскладка по умолчанию: `Flat`, `MirrorTree`, `FolderPerAsset` или `Migrate`. |
| `bGroupDependenciesByType` | `false` | При `FolderPerAsset` раскладывает зависимости по подпапкам типов; для других раскладок не действует. |
| `ExcludedPackagePaths` | `Content/3rdParty`, `Content/StarterContent`, `Content/StarterBundle`, `Content/Megascans`, `Content/MSPresets` | Префиксы пакетов/контента, исключённые из графа и раскрытия зависимостей. |
| `IncludedPackagePaths` | пусто | Пусто означает `/Game`; иначе эти корни задают вход графа. |
| `bScanIndirectReferences` | `true` | Ищет буквальные пути `/Game` в текстовых файлах проектных Config и Source. |
| `bHideExternalPackages` | `true` | Скрывает служебные строки OFPA, но продолжает обходить эти пакеты. |

Не исключайте покупной контент вслепую. Сторонний пакет может содержать GameMode, GameInstance, стартовую карту или другой класс, названный только конфигурацией; исключение корня уберёт пакет из графа раньше, чем косвенная ссылка сможет сделать его видимым в отчёте.

Встроенная карта конвертации: `StaticMesh → fbx`, `SkeletalMesh → fbx`, `AnimSequence → fbx`, `Texture2D → png`, `TextureCube → hdr`, `SoundWave → wav`, `DataTable → csv`, `CurveTable → csv`, `FontFace → ttf`, `World → fbx`. Пустой `ExchangeFormatByClass` включает именно её. Пользовательская карта заменяет встроенную; файл получится только для сочетания, под которое движок зарегистрировал `UExporter`.

Персональное состояние панели хранится отдельно в `Saved/Config/.../EditorPerProjectUserSettings.ini`: выбранные уровни, видимые типы и вердикты, фильтр generated class, режим представления, режим выгрузки, фильтры ссылок и выбор зависимостей. Оно сохраняется при закрытии вкладки. Поисковая строка намеренно не восстанавливается, чтобы панель не открывалась пустой из-за забытого поиска.

Окно выгрузки начинает с командных умолчаний и последних персональных значений режима/зависимостей, но отмена ничего не записывает обратно.

### Консольные команды

Редакторный модуль регистрирует ровно три команды:

| Команда | Назначение | Пример |
|---|---|---|
| `AssetUsageAudit.OpenPanel` | Открывает докируемую панель аудита. | `AssetUsageAudit.OpenPanel` |
| `AssetUsageAudit.Run` | Проверяет все уровни и пишет JSON и CSV; необязательные аргументы ограничивают прогон пакетными именами уровней. | `AssetUsageAudit.Run /Game/Maps/WP_Example /Game/Maps/L_Interior` |
| `AssetUsageAudit.FindUnused` | Пишет отчёт без строк `UsedOnLevel`. В оставшемся наборе по-прежнему различаются `UsedByAssetsOnly`, `ReferencedFromConfigOrSource`, `Unreferenced`, `Unknown`. | `AssetUsageAudit.FindUnused /Game/Maps/WP_Example` |

Без аргументов уровней `Run` и `FindUnused` рассматривают все уровни, найденные в настроенных корнях графа. Обе команды ждут окончания загрузки Asset Registry. Отчёты используют настроенную папку и имя вида `AssetUsageReport_YYYYMMDD_HHMMSS`.

### Blueprint и Python API

`UAssetUsageAuditLibrary` наследует `UBlueprintFunctionLibrary`; все четыре функции имеют `BlueprintCallable`, категорию **Asset Usage Audit** и используют те же настройки и реализацию Core, что и панель.

| Функция | Результат |
|---|---|
| `RunAudit(LevelPackageNames, OutputDirectory, OutReportPath)` | Запускает аудит, пишет JSON и CSV, возвращает признак успеха и абсолютный путь JSON. Пустой список уровней означает все, пустая выходная папка — настроенное умолчание. |
| `GetAssetsUsedOnLevel(LevelPackageName)` | Возвращает пакетные имена строк с вердиктом `UsedOnLevel` для заданного уровня. |
| `GetLevelsUsingAsset(AssetPackageName)` | Проверяет все уровни и возвращает те, от которых достижим пакет. Ассет стримингового подуровня может быть приписан и подуровню, и родительской карте. |
| `MeasureFullSweepSeconds()` | Выполняет полный прогон, пишет разбивку в лог и возвращает прошедшие секунды. Это точка реального измерения, а не оценка из кэша. |

В Editor Utility Blueprint добавляйте ноды категории **Asset Usage Audit** и передавайте длинные пакетные имена вроде `/Game/Maps/WP_Example`, а не файловые пути.

В Python отражённые имена следуют принятому в Unreal `snake_case`:

```python
import unreal

success, json_path = unreal.AssetUsageAuditLibrary.run_audit(
    ["/Game/Maps/WP_Example"],
    "D:/AuditOutput"
)

assets = unreal.AssetUsageAuditLibrary.get_assets_used_on_level(
    "/Game/Maps/WP_Example"
)
levels = unreal.AssetUsageAuditLibrary.get_levels_using_asset(
    "/Game/Art/SM_Rock"
)
seconds = unreal.AssetUsageAuditLibrary.measure_full_sweep_seconds()
```

Эти вызовы относятся к редактору: библиотека находится в `AssetUsageAuditEditor`, а не в рантайм-модуле.

### Отчёты

Каждая запись отчёта создаёт пару файлов из одного результата в памяти:

- JSON — для инструментов и скриптов;
- CSV — для таблиц.

Папка по умолчанию — `Saved/AssetUsageAudit`. Панель предлагает выбрать директорию и сохраняет ровно строки текущего вида после UI-фильтров; `RunAudit` и консольные команды используют свои запросы и настройки напрямую.

В JSON входят:

- заголовок со временем генерации, проектом, версиями движка и инструмента, числами уровней/ассетов/строк, длительностью и применёнными фильтрами;
- счётчики всех пяти вердиктов;
- статистика графа: пакеты, рёбра, уровни, внешние пакеты, время перечисления и время получения зависимостей;
- объект каждого ассета с пакетом, именем, путём, типом, необязательным generated class, вердиктом, происхождением, hard/soft-счётчиками, уровнями и необязательными маршрутом/деталями происхождения.

CSV записывается в UTF-8 с BOM, чтобы Excel не ломал не-ASCII имена. Разделитель полей — `;`, значений внутри многозначного поля — `|`. Колонки:

`Type · GeneratedClass · Name · PathFromProjectRoot · Verdict · LevelCount · Levels · HardRefs · SoftRefs · Provenance · Route · ProvenanceDetail`

Строки-комментарии перед шапкой содержат метаданные генерации, фильтры, счётчики вердиктов, длительность и предупреждение, что `Unknown` не равен «не используется». Вердикт нужно читать вместе с происхождением и маршрутом: одного числа без объясняющей цепочки недостаточно для решения о контенте.

### Выгрузка файлов

После выбора строк нажмите **Export ticked assets...**. Модальное окно фиксирует набор на время выбора режима, папки, раскладки, поведения зависимостей, манифеста и политики коллизий. Отмена не меняет ни файлы, ни сохранённые значения.

#### Режимы и раскладки

Доступны два режима:

- **Copy `.uasset`** — побайтово копирует пакеты, не загружая ассеты. Для выбранной карты также добавляются найденные OFPA-пакеты `.uasset`, иначе копия карты оказалась бы пустой.
- **Convert (FBX/PNG/WAV)** — загружает каждый ассет и запускает `UAssetExportTask` с настроенным расширением. По умолчанию после каждых 64 загруженных ассетов выполняется сборка мусора.

Четыре взаимоисключающие раскладки:

| Раскладка | Поведение |
|---|---|
| `Flat` | Все выходные файлы лежат в одной папке. Совпадения имён разрешает выбранная политика. |
| `MirrorTree` | Под корнем выгрузки воспроизводится пакетное дерево, например `/Game/Art/SM_Rock` превращается в `Game/Art/SM_Rock.uasset`. |
| `FolderPerAsset` | Для каждого вручную отмеченного ассета создаётся своя папка, рядом кладётся его замыкание зависимостей. Общая зависимость копируется в каждую папку, где нужна, поэтому размещений файлов бывает больше, чем уникальных ассетов. |
| `Migrate` | Только отмеченные пакеты передаются штатному `IAssetTools::MigratePackages`; Unreal сам строит замыкание и восстанавливает исходные пути в другом проекте. Это единственный управляемый движком вариант, рассчитанный на автоматическое сохранение рабочих Unreal-ссылок. |

При `FolderPerAsset` галочка **Sort dependencies into type subfolders** раскладывает зависимости по папкам пресетов вроде `Texture`, `Material`, `StaticMesh`; сам исходный ассет остаётся в корне своей папки. Для неприменимых раскладок контрол остаётся видимым, но выключенным.

Политика совпадения имён — **Keep both** с индексом либо **Overwrite**. В Migrate переименование невозможно: новый пакетный путь разрушил бы ссылки, которые перенос должен сохранить. Поэтому **Keep both** отображается на штатную политику Unreal **Skip**.

#### Копия `.uasset` не переносит зависимости сама по себе

Пакет хранит ссылки как полные имена вроде `/Game/Art/T_Rock_D`, а не как относительные пути к соседним файлам. Одиночный `SM_Rock.uasset` не содержит внутри своих материалов и текстур. Раскладки Flat и FolderPerAsset ещё и меняют положение пакета, поэтому их сырой результат предназначен для ревью/передачи либо должен восстанавливаться с информацией о путях.

Для прямого переноса Unreal → Unreal с управляемыми движком путями используйте **Migrate**. Папка назначения обязана:

1. оканчиваться директорией `Content`;
2. иметь на уровень выше `.uproject` либо ровно один `.uplugin`, чтобы Unreal мог вывести точку монтирования.

`AssetUsagePaths::ValidateMigrateDestination` проверяет оба условия до включения кнопки выгрузки. При переключении на Migrate заранее подставленный непригодный путь очищается. `MigratePackages` возвращает `void` и сам сообщает результат через уведомления Unreal и Output Log, поэтому панель не выдумывает число скопированных файлов.

В Migrate передаются только отмеченные пакеты: если отдать ещё и уже раскрытое плагином замыкание, Unreal будет повторно обходить лишние исходные точки. Отключение зависимостей выставляет `bIgnoreDependencies`; при этом Unreal пропускает и OFPA-пакеты уровня, поэтому панель показывает дополнительное подтверждение — перенесённый уровень может приехать пустым.

#### Include referenced assets

**Include referenced assets** включено по умолчанию. Для Copy/Convert плагин строит транзитивное замыкание каждого исходного ассета тем же пакетным запросом без требований, что и основной аудит. До записи окно показывает число отмеченных исходников и реальное число размещений файлов.

Границы обхода заданы явно:

- `/Script` — код и никогда не копируется;
- `/Engine` и `/Temp` по умолчанию исключены;
- проектные исключения путей ограничивают и раскрытие зависимостей;
- вручную отмеченный исходник проходит мимо этих фильтров, потому что явный выбор обязан дать сам файл;
- чужая карта, достигнутая из обычного ассета, не раскрывается; подуровень или Level Instance, структурно достигнутый из уровня либо внешнего пакета, обходится.

Это не `MigratePackages`: замыкание ограничено исключениями аудита, а размещением управляет плагин.

#### Манифест зависимостей

Для всех раскладок кроме Migrate по умолчанию включено **Write dependency manifest**: в корне появляется `AssetUsageAudit.manifest.json`. В нём записаны версия схемы, инструмент, проект, движок, раскладка, политика коллизий и для каждого реально записанного файла:

- исходное пакетное имя;
- фактический путь относительно корня после разрешения коллизий;
- подсказки об ассете и классе, если они есть в реестре;
- был ли пакет отмечен вручную (`seed`) либо приехал зависимостью;
- его прямые пакетные зависимости и относительный файл для тех, что присутствуют в этой выгрузке.

Манифест строится по записанным экспортёром фактическим путям, а не по запланированным именам. Именно при коллизии и индексировании разница критична. Пустой список записанных файлов считается ошибкой: правдоподобный пустой манифест рядом с полной папкой опаснее отсутствующего. Для Migrate генерация манифеста запрещена, потому что движок уже сохраняет пакетные пути.

#### Побайтовая копия и конвертация

При перезаписи копирование снимает read-only с существующего назначения, а после записи — и с получившегося файла. Так read-only источника не превращает выгрузку в неизменяемую и не ломает следующий Overwrite.

Конвертация использует карту «класс → расширение» и поднимается вверх по иерархии. Запись для `Texture` поэтому может покрывать подклассы; результат появляется лишь при наличии подходящего `UExporter` в Unreal. Для Blueprint встроенной записи намеренно нет. У материалов, Blueprints и Niagara нет формата по умолчанию в этой карте.

Пропуски считаются раздельно: **нет настроенного формата**, **ассет не загрузился**, **для формата нет экспортёра**. Конвертация загружает ассеты и может быть ощутимо медленнее побайтового копирования; репозиторий не обещает универсальную производительность на больших пачках.

### Импорт из внешней папки

Откройте `Window → Tools → Import Assets from Folder...`. Этот сценарий рекурсивно сканирует `.uasset` и `.umap`, не загружая их, показывает кандидатов и позволяет выбрать, что вернуть в текущий проект.

В таблице видны исходный файл, целевой пакет, источник целевого пути, состояние зависимостей и наличие цели в проекте. Путь восстанавливается из четырёх источников по убыванию доверия:

| Источник | Значение |
|---|---|
| `Manifest` | Точный исходный пакет и фактическое имя выгруженного файла из `AssetUsageAudit.manifest.json`. |
| `Package header` | `FPackageFileSummary::PackageName`, если пакет сериализовал допустимое имя. Сам Unreal допускает отсутствие поля или `None`, поэтому откат необходим. |
| `Folder structure` | Вывод из положения файла под корнем сканирования. Точен для зеркальной раскладки плагина и остаётся догадкой для произвольной папки. |
| `Unresolved` | Путь восстановить не удалось. Нужна явно заданная запасная папка `/Game/...`, а исходные ссылки могут остаться сломанными. |

При наличии манифеста сканер также показывает зависимости, которых нет ни в импортируемой папке, ни в реестре текущего проекта.

Правила импорта строгие, потому что у перезаписи нет undo и резервной копии:

- существующая цель пропускается, пока явно не включено **Overwrite existing assets**;
- если выбранные строки будут перезаписаны, отдельное окно сообщает число и последствия;
- уже загруженный в редактор пакет не перезаписывается даже при включённом Overwrite: устаревшие объекты в памяти могли бы позже сохраниться поверх импортированного файла;
- файл без восстановленного пути пропускается, пока не указана допустимая запасная пакетная папка;
- неверные целевые пути и ошибки копирования выводятся отдельно.

После успешной копии `ScanFilesSynchronous(..., bForceRescan=true)` делает файлы видимыми в Content Browser. Затем окно сканирует папку заново, и только что импортированные цели уже отмечены как существующие — второй клик не выглядит приглашением создать дубль.

### Удаление отмеченных ассетов

**Delete ticked assets...** работает ровно по строкам, отмеченным в панели. Набор никогда не расширяется зависимостями: при выгрузке это полезно, а при удалении общая текстура могла бы снести контент, который никто не выбирал.

Удаление проходит через два окна:

1. Собственное окно плагина показывает каждый запрошенный пакет: имя, тип, путь, число внешних референсеров и статус. Допустимые строки уже отмечены. Референсеры, которые сами входят в набор удаления, не считаются внешними; в тултип попадает до восьми имён, а общий счётчик остаётся точным. Референсеры-карты и OFPA отдельно считаются ссылками с уровня, read-only файлы помечаются заранее. Кнопка Delete выключена, пока не отмечено **I understand these files will be removed from the project**.
2. Подтверждённый набор заново разрешается в свежие `FAssetData` и передаётся в `ObjectTools::DeleteAssets(..., bShowConfirmation=true)`. После этого штатное окно Unreal **Delete Assets** управляет окончательным списком ссылок, **Force Delete** и **Replace References**.

До штатного окна плагин запрещает:

| Отказ | Причина |
|---|---|
| Уровень (`.umap`) | Это единица, относительно которой аудит измеряет использование; удаление уровня обесценивает остальные строки результата. Unreal также особо обрабатывает открытую карту. Осознанно удаляйте карты из Content Browser. |
| OFPA-пакет из `__ExternalActors__` или `__ExternalObjects__` | Это размещённый объект/актор карты. Удалять его следует в редакторе уровня, где операция участвует в его рабочем процессе и undo. |
| `/Engine`, `/Script`, `/Temp` | Для этого инструмента это не удаляемый контент проекта. |
| Отсутствующий пакет | В реестре уже нет ассета с таким пакетным именем. |

Внешние референсеры и read-only — предупреждения, а не молчаливое исключение: последнее решение принимает штатное окно Unreal. Восстановление обеспечивает система контроля версий; плагин не создаёт резервную копию и не даёт собственного undo.

После удаления исчезнувшие строки убираются, но другие строки пока несут счётчики и вердикты из старого графа. Панель помечает результат устаревшим и просит повторить аудит. `Unreferenced` всё равно не доказывает отсутствия использования через динамическую строку, строку DataTable или FMOD; окно удаления повторяет это предупреждение.

### Архитектура

| Модуль | Тип | Ответственность |
|---|---|---|
| `AssetUsageAuditCore` | `UncookedOnly`, `Default` | Построение графа, транзитивная достижимость от уровней, OFPA, вердикты, косвенный скан, отчёты, замыкание зависимостей, раскладка/копирование/конвертация/манифест, скан и копирование импорта, предварительный анализ удаления. Никакого Slate UI и редакторного asset pipeline. |
| `AssetUsageAuditEditor` | `Editor`, `PostEngineInit` | Докируемая Slate-панель, меню и тулбар, настройки, консольные команды, Blueprint/Python-библиотека, окна выгрузки/импорта/удаления, вызов Migrate и передача в штатное удаление. |
| `AssetUsageAuditTests` | `UncookedOnly`, `Default` | Automation Specs для Core и проверки в editor context/на живом контенте, включая Migrate. |

Все три записи дескриптора разрешены только для Win64. `AssetUsageAuditCore` публично зависит от `Core`, `CoreUObject`, `AssetRegistry`, приватно — от рантайм-модулей `Engine`, `Json`, `Projects`. Его инвариант — не зависеть от `UnrealEd`, `AssetTools`, `ToolMenus`, `Slate`, `SlateCore` и editor subsystems. `Engine` допустим: `ULevel::GetExternalActorsPaths`, `GetExternalObjectsPaths` и скан ассетов уровня — рантайм-API, доступные коду, рассчитанному на commandlet.

#### Инварианты корректности

1. **Обход зависимостей использует пакетную категорию и `FDependencyQuery{}` (`NoRequirements`).** Hard-only теряет soft-ссылки и рёбра OFPA. `Soft` — отрицательное требование Hard, поэтому одновременное требование Hard и Soft не означает «любой из двух». Hard/soft остаётся данными отчёта и никогда не становится фильтром обхода.
2. **Граница карты пересекается только из уже принятого уровня либо его внешнего пакета.** Так проходят стриминговые подуровни и Level Instances, но обычная ссылка из data asset/Blueprint не затягивает чужую карту со всем содержимым. Внутренний параметр `bTraverseIntoOtherLevels` оставляет расширенный режим вызывающему коду, которому он действительно нужен.
3. **Корни OFPA берутся из `ULevel::GetExternalActorsPaths` и `GetExternalObjectsPaths`, а не собираются строками.** Content Bundles, External Data Layers и делегаты плагинов могут добавлять структуру пути. Обход идёт вперёд от карты и не придумывает обратное ребро от внешнего пакета к уровню.
4. **Класс Blueprint-ассета и сгенерированный геймплейный класс разделены.** Класс самого ассета — `/Script/Engine.Blueprint`; геймплейная фильтрация использует тег реестра `GeneratedClass` и раскрытие подклассов.
5. **До обхода уровня по умолчанию выполняется `ScanLevelAssets`.** Иначе не открывавшийся в сессии OFPA-уровень может выглядеть пустым: его внешние пакеты ещё не попали в реестр.

Состояние галочек хранится в множествах пакетных имён, а не в selection `SListView`, поэтому фильтрация не стирает выбор пользователя. Смена области не запускает аудит сама. Отчёты и все публичные точки входа зовут один Core-аудитор. Конвертация остаётся в Core, потому что `UExporter` и `UAssetExportTask` относятся к Engine API; конкретные экспортёры разрешаются в рантайме, а отсутствие даёт явную причину пропуска. Migrate находится в Editor, потому что `AssetTools` — editor-only.

### Тесты

В `Source/AssetUsageAuditTests/Private` находятся 16 Automation Spec-файлов `*.spec.cpp`. Подсчёт определений `It(...)` в них даёт 169 кейсов. Префикс тестов — `AssetUsageAudit`.

Сначала соберите Editor-таргет, затем запустите из командной строки Windows:

```bat
"D:\Epic\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:\Projects\MyGame\MyGame.uproject" ^
  -ExecCmds="Automation RunTests AssetUsageAudit" ^
  -TestExit="Automation Test Queue Empty" ^
  -unattended -nopause -nosplash -stdout ^
  -abslog="D:\Projects\MyGame\Saved\Logs\AssetUsageAuditTests.log"
```

На машине без рабочего стола/GPU-сессии добавьте `-nullrhi`, чтобы Unreal не инициализировал графическое устройство:

```bat
"D:\Epic\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:\Projects\MyGame\MyGame.uproject" ^
  -ExecCmds="Automation RunTests AssetUsageAudit" ^
  -TestExit="Automation Test Queue Empty" ^
  -unattended -nopause -nosplash -stdout -nullrhi ^
  -abslog="D:\Projects\MyGame\Saved\Logs\AssetUsageAuditTests.log"
```

Часть сьютов намеренно исследует настоящий контент хост-проекта и пропускает отдельные проверки с предупреждением, если подходящего объекта нет. В частности, `AssetUsageAudit.ExporterLive`, `AssetUsageAudit.ExchangeExport`, `AssetUsageAudit.GraphFidelity`, `AssetUsageAudit.MigrateLive` ищут пригодные ассеты/уровни через Asset Registry, а не полагаются на фиксированные имена пакетов. Предупреждения нужно читать: они отличают реально отработавшую live-проверку от пропущенной.

`GraphFidelity` сравнивает стабильную выборку по пакетным именам с сырым ответом Asset Registry: пропущенные и лишние рёбра, hard-классификацию по цели, реальные переходы Route, soft-инвариант OFPA и приписывание подуровня. Выборка по именам убирает нестабильность порядка перечисления реестра; hard сравнивается по цели, потому что Unreal может вернуть одну цель одновременно hard- и soft-ребром.

В прежнем хост-проекте был зафиксирован не связанный с плагином fatal `TNotNull` при `-nullrhi` даже с выключенными плагинами, а `-NoShaderCompile` приводил к отсутствующей shader map материала по умолчанию. Для того проекта эти флаги не давали успешного headless-запуска. В самом репозитории плагина нет `.uproject`, поэтому реальный прогон всегда требует хост-проект UE 5.6 с установленным плагином.

Тесты, которые намеренно вызывают `UE_LOG(Error)`, объявляют сообщение через `AddExpectedError`; иначе Automation Framework справедливо считает ошибку в логе падением теста.

### Ограничения

- Это редакторный плагин с исходниками, а не рантайм-система. В нём нет игрового UI и аудита в собранной игре.
- В дескрипторе заявлен только Win64. Другие платформы не собраны и не поддерживаются метаданными репозитория.
- В репозитории нет хостового `.uproject`, готовых DLL, отдельного commandlet, бейджей, скриншотов и внешних страниц документации.
- Свидетельства Asset Registry не покрывают динамически собранные пути, загрузку уровня по имени, произвольные строки DataTable/CurveTable и строковые события FMOD. `Unknown` и даже `Unreferenced` требуют проверки человеком.
- Скан Config/Source находит буквальные `/Game`-пути в поддерживаемых текстовых файлах; это не анализ потока данных C++ или Blueprint.
- Flat и FolderPerAsset не переписывают внутренние пакетные ссылки. Для восстановления используйте манифест и импорт, для прямого переноса проекта — штатный Unreal Migrate.
- Конвертация зависит от установленных и зарегистрированных экспортёров Unreal. Класс без настроенного формата пропускается; расширение без экспортёра считается отдельно. Большие партии загружают ассеты, а тесты репозитория проверяют отдельные выгрузки, но не публикуют универсальный замер большого объёма.
- У удаления и перезаписи нет собственного undo или резервной копии плагина. Защита — явные подтверждения и система контроля версий.
- Slate-взаимодействие не покрывается Automation Specs; после изменений UI нужен smoke-test в редакторе.
- Live-сьюты могут предупредить и пропустить проверку, если в хост-проекте нет подходящего настоящего контента. Зелёный прогон нужно читать вместе с предупреждениями.
- Архитектура Core рассчитана на использование из commandlet, но отдельного audit-commandlet в репозитории нет.

### Лицензия

Файла `LICENSE` в репозитории нет. Исходный код опубликован для демонстрации портфолио и ревью; условия повторного использования, распространения или включения в другой проект нужно запросить у автора. Никакая сторонняя лицензия здесь не подразумевается.

### Ссылки

- Репозиторий: https://github.com/MagentaDolphin/AssetUsageAudit
- Автор: MagentaDolphin
