// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"
#include "AssetUsageAuditor.h"
#include "SAssetExportDialog.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

/** One row as the list view sees it. Shared so the list can hold it without copying. */
using FAssetUsageRowPtr = TSharedPtr<FAssetUsageRow>;

/**
 * A node in the results tree: either a level heading or an asset under one.
 *
 * The tree exists because "by level" needs to answer two questions at once - what is on this
 * location, and how do two locations compare - and a flat list filtered to one level can only
 * answer the first. In by-asset mode every node is an asset with no children, so the same widget
 * serves both directions and there is one code path to keep correct rather than two.
 */
struct FAuditTreeItem
{
    /** Set on a level heading. Mutually exclusive with Row. */
    FName LevelPackage;

    /** Set on an asset node. */
    FAssetUsageRowPtr Row;

    /** Assets under a level heading. Always empty for an asset node. */
    TArray<TSharedPtr<FAuditTreeItem>> Children;

    bool IsLevel() const
    {
        return !LevelPackage.IsNone();
    }
};

using FAuditTreeItemPtr = TSharedPtr<FAuditTreeItem>;

/**
 * Filter on how an asset is held.
 *
 * The interesting value is SoftOnly. An asset reached exclusively through soft references is
 * loaded on demand and nothing forces it to be present - that is where "it worked in the editor
 * and vanished in the build" comes from. Hard vs soft is a property of the edge, never of the
 * query (see AssetUsageAudit::MakeTraversalQuery), so this filters the result, not the sweep.
 */
enum class EReferenceStrengthFilter : uint8
{
    /** No constraint. */
    Any,

    /** At least one hard reference and no soft ones. */
    HardOnly,

    /** Held only by soft references. */
    SoftOnly,

    /** Both kinds present. */
    Mixed,

    /** Neither - reached only as a level seed, from config, or not reached at all. */
    None
};

/**
 * What "export ticked files" writes.
 *
 * Both modes were agreed, and they answer different questions. A copy is byte-exact and cheap, and
 * is what you want when the files are going into another Unreal project. A conversion is what you
 * want when they are going to someone who does not run Unreal at all - and it costs a full asset
 * load per file, so it is never the silent default.
 */
// EFileExportMode now lives in SAssetExportDialog.h: the dialog owns the mode switch, and keeping
// the definition here would make the two headers include each other.

/**
 * Result of widening a ticked set to its dependency closure.
 *
 * Carries the before and after counts because the ratio is the whole point of showing a dialog:
 * "40 ticked, 380 will be written" is the number that stops someone exporting half the project.
 */
struct FDependencyExpansion
{
    /** What the user ticked. */
    int32 SeedCount = 0;

    /** What will actually be written, seeds included. Equals SeedCount when the option is off. */
    int32 TotalCount = 0;

    /** Human-readable breakdown for the Output Log. */
    FString Detail;

    int32 AddedCount() const
    {
        return FMath::Max(0, TotalCount - SeedCount);
    }
};

/** Which question the user is asking. Both directions live in one window. */
enum class EAuditViewMode : uint8
{
    /** Rows are assets; the Levels column says where each is used. */
    ByAsset,

    /** Rows are grouped under the level that uses them. */
    ByLevel
};

/**
 * The audit window.
 *
 * Holds the last result, a filtered view of it, and the user's tick marks. Runs nothing itself -
 * every calculation goes through FAssetUsageAuditor in the Core module, so the panel and the
 * console commands cannot diverge.
 */
class SAssetUsageAuditPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAssetUsageAuditPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Persists the filter state. The tab is destroyed on close, which is when this fires. */
    virtual ~SAssetUsageAuditPanel() override;

private:
    // --- Running -----------------------------------------------------------------------------

    FReply OnRunClicked();
    bool CanRun() const;
    void RunAudit();

    // --- List --------------------------------------------------------------------------------

    TSharedRef<ITableRow> OnGenerateRow(FAuditTreeItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
    void OnGetChildren(FAuditTreeItemPtr Item, TArray<FAuditTreeItemPtr>& OutChildren);

    /**
     * Double-click reveals the asset in the Content Browser, the same as Browse To in the editor.
     *
     * Deliberately the FAssetData overload of SyncBrowserToObjects rather than the UObject one:
     * revealing a row must not load it. On this project a row can be a 200 MB mesh or a whole map,
     * and loading it to point at it would stall the editor for seconds with no visible reason.
     *
     * A level heading syncs to the .umap itself, which is what someone double-clicking a location
     * is asking for.
     */
    void OnItemDoubleClicked(FAuditTreeItemPtr Item);
    void RebuildFilteredRows();

    /**
     * Rebuild the tree from FilteredRows.
     *
     * By-asset mode produces one childless node per row; by-level mode groups rows under the levels
     * that use them. A row on several levels appears under each - deliberately, because "which
     * locations use this" is the other half of the question the tool answers.
     */
    void RebuildTree();

    /** Ticking a level heading ticks everything under it. */
    ECheckBoxState GetLevelCheckState(FAuditTreeItemPtr Item) const;
    void OnLevelCheckChanged(ECheckBoxState NewState, FAuditTreeItemPtr Item);

    /** Root nodes: assets in by-asset mode, levels in by-level mode. */
    TArray<FAuditTreeItemPtr> RootItems;

    // --- Filtering ---------------------------------------------------------------------------

    bool PassesFilters(const FAssetUsageRow& Row) const;
    void OnSearchTextChanged(const FText& NewText);

    TSharedRef<SWidget> MakeVerdictFilterMenu();
    TSharedRef<SWidget> MakeTypeFilterMenu();

    /**
     * Short class names a type preset stands for, expanded through subclasses.
     *
     * Expansion is the point: "Material" without it misses every MaterialInstanceConstant, which on
     * this project is most of what exists. The menu deals in short names because that is what
     * PassesFilters compares against, so the expanded class paths are reduced to their asset names.
     */
    TSet<FName> ResolvePresetTypeNames(enum class EAssetTypePreset Preset) const;

    // --- Persisted panel state ---------------------------------------------------------------

    /**
     * Restore the previous session's filters.
     *
     * Every value is validated rather than trusted: the ini is a text file a user can edit, and an
     * out-of-range enum read straight into a switch is a crash on startup of the editor.
     */
    void LoadUserSettings();

    /**
     * Persist the current filters.
     *
     * Called from the destructor, which fires when the tab is closed or the editor shuts down
     * normally. Saving on every filter change instead would mean an ini write per keystroke in the
     * search box, and SaveConfig is a synchronous disk write.
     *
     * Known consequence: an editor crash loses the session's filter changes. Accepted rather than
     * worked around - the state is cheap to recreate, and the alternative costs disk traffic on
     * every interaction.
     */
    void SaveUserSettings() const;
    TSharedRef<SWidget> MakeLevelFilterMenu();
    TSharedRef<SWidget> MakeReferenceFilterMenu();

    /**
     * Fill the level and type dropdowns from the Asset Registry.
     *
     * Deliberately NOT derived from the last result. Building them from result rows left both
     * menus empty until Run Audit had been pressed, which reads as a broken tool: you open the
     * panel, click "All levels", and nothing is there.
     *
     * It also matters for correctness of the workflow, not just for looks. The task is "assets of
     * a chosen type for a chosen location", so the level has to be pickable BEFORE the sweep -
     * a menu populated from results can only ever filter a sweep that already happened.
     *
     * Cached: enumerating the project costs a fraction of a second and the content does not change
     * under the user mid-session. Refreshed on demand and after each audit.
     */
    void RefreshAvailableFilters();

    /**
     * The chosen level scopes the sweep, so changing it invalidates the current result rather
     * than filtering it. Marks the result stale and tells the user to press Run Audit, instead
     * of showing figures that answer a different question than the one on screen.
     */
    void OnScopeChanged();

    /** True when the level scope changed after the last sweep. */
    bool bResultStale = false;

    /** Level packages offered by the level dropdown, sorted by name. */
    TArray<FName> AvailableLevels;

    /** Short type names offered by the type dropdown, including Blueprint generated classes. */
    TArray<FName> AvailableTypes;

    /** False until RefreshAvailableFilters has completed a full pass. */
    bool bAvailableFiltersReady = false;

    // --- Sorting -----------------------------------------------------------------------------

    EColumnSortMode::Type GetSortModeForColumn(FName ColumnId) const;
    void OnSortChanged(EColumnSortPriority::Type Priority, const FName& ColumnId, EColumnSortMode::Type NewMode);
    void ApplySorting();

    // --- Ticking assets for export -----------------------------------------------------------

    /**
     * Checked packages, keyed by package name rather than by row pointer.
     *
     * Deliberately NOT the list view's selection. Selection is rebuilt whenever the filter changes
     * or the audit re-runs, so using it would silently discard ticks the user had already made -
     * they would filter, tick forty assets, change the type filter and find their work gone.
     * Keying by FName means ticks survive both.
     */
    TSet<FName> CheckedPackages;

    ECheckBoxState GetRowCheckState(FAssetUsageRowPtr Item) const;
    void OnRowCheckChanged(ECheckBoxState NewState, FAssetUsageRowPtr Item);

    ECheckBoxState GetHeaderCheckState() const;
    void OnHeaderCheckChanged(ECheckBoxState NewState);

    FText GetSelectionSummaryText() const;

    // --- Export ------------------------------------------------------------------------------

    FReply OnExportReportClicked();
    FReply OnExportFilesClicked();
    bool HasCheckedAssets() const;

    // --- Deleting ----------------------------------------------------------------------------

    /**
     * Delete the ticked assets, after two confirmations.
     *
     * Sits beside Export because it acts on the same ticked set, and is the reason the audit is
     * worth running: the point of finding unused content is to be able to remove it. It is kept
     * visually apart and never becomes the default action.
     *
     * Levels are refused outright - see AssetDeletionScan::ERefusal::IsLevel. Everything else the
     * window explains before anything is touched, and the destruction itself is ObjectTools' code,
     * not ours.
     */
    FReply OnDeleteClicked();

    /**
     * Drop deleted packages from the result without re-running the sweep.
     *
     * A full re-run after a delete would cost minutes on this project, and the rows that remain are
     * still correct - what changed is that some assets are gone, plus the referencer counts of
     * whatever pointed at them. The result is marked stale so the numbers are not mistaken for a
     * fresh sweep.
     */
    void ForgetDeletedPackages(const TArray<FName>& DeletedPackages);

    /** Byte copy of the package files, expanding levels to their OFPA packages. */
    void RunCopyExport(const TArray<FAssetUsageRow>& Rows, const FAssetExportRequest& Request);

    /** Load and convert through UExporter. Separate function because the failure modes differ. */
    void RunConvertExport(const TArray<FAssetUsageRow>& Rows, const FAssetExportRequest& Request);

    /**
     * Hand the packages to IAssetTools::MigratePackages.
     *
     * The only export whose result opens in another project with references intact: Migrate writes
     * each package to the same package path under the destination Content folder, which is what a
     * .uasset's stored references need in order to resolve.
     *
     * Lives here rather than in Core because AssetTools is editor-only, and reports nothing back -
     * MigratePackages returns void and shows the engine's own report - so this cannot pretend to
     * count files the way the copy path does.
     */
    void RunMigrateExport(const TArray<FAssetUsageRow>& Rows, const FAssetExportRequest& Request);

    EFileExportMode ExportMode = EFileExportMode::CopyPackages;

    /** Rows currently ticked, resolved against the full result rather than the filtered view. */
    void GetCheckedRows(TArray<FAssetUsageRow>& OutRows) const;

    /**
     * Pull in everything the ticked assets reference, so an exported mesh arrives with its
     * materials and textures instead of opening pink.
     *
     * On by default. Ticking eleven meshes and receiving eleven unusable files is the more
     * surprising of the two behaviours, and the confirmation dialog states the real figure before
     * anything is written.
     */
    bool bIncludeDependencies = true;

    /** Seeds plus their dependency closure, or the seeds unchanged when the option is off. */
    TArray<FName> ExpandWithDependencies(const TArray<FName>& Seeds, struct FDependencyExpansion& OutExpansion) const;

    // --- State -------------------------------------------------------------------------------

    FAssetUsageAuditResult LastResult;
    TArray<FAssetUsageRowPtr> AllRows;
    TArray<FAssetUsageRowPtr> FilteredRows;

    TSharedPtr<STreeView<FAuditTreeItemPtr>> TreeView;
    TSharedPtr<SHeaderRow> HeaderRow;

    EAuditViewMode ViewMode = EAuditViewMode::ByAsset;

    FString SearchText;
    TSet<EAssetUsageVerdict> VisibleVerdicts;

    /**
     * Asset classes to show, e.g. StaticMesh, Texture2D, Blueprint.
     *
     * Only the asset's own class goes here. Blueprint generated classes are deliberately kept out:
     * folding them in produced a 2980-entry dropdown on this project, where someone looking for
     * "StaticMesh" had to scroll past thousands of BP_Something_C entries. The gameplay class is
     * a different question and gets its own text filter below.
     */
    TSet<FName> VisibleTypeNames;

    /**
     * Substring match against a Blueprint's generated gameplay class.
     *
     * A separate axis from the asset class: every Blueprint asset is /Script/Engine.Blueprint, so
     * "which Blueprints derive from something Pickup-shaped" cannot be answered by asset class.
     */
    FString GeneratedClassFilter;

    /** How the asset must be held for its row to show. */
    EReferenceStrengthFilter ReferenceStrength = EReferenceStrengthFilter::Any;

    /**
     * Provenance flags a row must carry, ANDed together.
     *
     * None means no constraint. Combining flags answers questions the columns alone cannot, e.g.
     * "editor-only AND reached through an external actor" - content placed in a level that will
     * not survive a cook.
     */
    EAssetUsageProvenance RequiredProvenance = EAssetUsageProvenance::None;

    /**
     * Levels the audit is scoped to. Empty means every level in the project.
     *
     * A set rather than a single name, and the same shape as VisibleTypeNames, because the two
     * menus do the same job and a checklist beside a radio list is a UI that has to be explained.
     * FAssetUsageAuditRequest::LevelPackages was already an array - the panel was the only thing
     * limiting this to one.
     */
    TSet<FName> LevelFilters;

    FName SortColumn;
    EColumnSortMode::Type SortMode = EColumnSortMode::None;

    bool bHasRun = false;
    FText StatusText;
};
