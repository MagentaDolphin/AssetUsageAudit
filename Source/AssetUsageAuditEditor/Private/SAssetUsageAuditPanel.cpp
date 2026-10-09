// MagentaDolphin 2026. Asset Usage Audit.

#include "SAssetUsageAuditPanel.h"

#include "AssetDependencyClosure.h"
#include "AssetTypeFilter.h"
#include "AssetExportLayout.h"
#include "AssetExportManifest.h"
#include "AssetUsageAuditEditorModule.h"
#include "AssetUsageAuditSettings.h"
#include "AssetUsageAuditUserSettings.h"
#include "AssetUsageExporter.h"
#include "AssetUsagePaths.h"
#include "AssetUsageReportWriter.h"
#include "SAssetDeleteDialog.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Algo/AllOf.h"
#include "AssetToolsModule.h"
#include "DesktopPlatformModule.h"
#include "IAssetTools.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"

#define LOCTEXT_NAMESPACE "SAssetUsageAuditPanel"

namespace AuditColumns
{
    static const FName Check("Check");
    static const FName Type("Type");
    static const FName Name("Name");
    static const FName Path("Path");
    static const FName Verdict("Verdict");
    static const FName Levels("Levels");
    static const FName Hard("Hard");
    static const FName Soft("Soft");
    static const FName Provenance("Provenance");
    static const FName Route("Route");
}

namespace
{
    /** Colour-code the verdict so a 45k-row list is scannable rather than a wall of text. */
    FSlateColor VerdictColour(EAssetUsageVerdict Verdict)
    {
        switch (Verdict)
        {
            case EAssetUsageVerdict::UsedOnLevel:
                return FSlateColor(FLinearColor(0.35f, 0.78f, 0.35f));
            case EAssetUsageVerdict::UsedByAssetsOnly:
                return FSlateColor(FLinearColor(0.55f, 0.72f, 0.90f));
            case EAssetUsageVerdict::ReferencedFromConfigOrSource:
                return FSlateColor(FLinearColor(0.85f, 0.75f, 0.35f));
            case EAssetUsageVerdict::Unreferenced:
                return FSlateColor(FLinearColor(0.90f, 0.45f, 0.40f));
            case EAssetUsageVerdict::Unknown:
                return FSlateColor(FLinearColor(0.65f, 0.65f, 0.65f));
        }

        return FSlateColor::UseForeground();
    }

    /** Short class label for the Type column: "/Script/Engine.StaticMesh" -> "StaticMesh". */
    FString ShortTypeName(const FTopLevelAssetPath& ClassPath)
    {
        if (ClassPath.IsNull())
        {
            return FString();
        }

        return ClassPath.GetAssetName().ToString();
    }
}

/** One row widget. Kept in this file: it has no use outside the panel. */
class SAssetUsageRowWidget : public SMultiColumnTableRow<FAuditTreeItemPtr>
{
public:
    SLATE_BEGIN_ARGS(SAssetUsageRowWidget) {}
        SLATE_ARGUMENT(FAssetUsageRowPtr, Item)
        SLATE_EVENT(FOnCheckStateChanged, OnCheckChanged)
        SLATE_ATTRIBUTE(ECheckBoxState, CheckState)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
    {
        Item = InArgs._Item;
        OnCheckChanged = InArgs._OnCheckChanged;
        CheckState = InArgs._CheckState;

        SMultiColumnTableRow<FAuditTreeItemPtr>::Construct(FSuperRowType::FArguments(), InOwnerTable);
    }

    virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
    {
        if (!Item.IsValid())
        {
            return SNullWidget::NullWidget;
        }

        const auto TextCell = [](const FString& Text, const FSlateColor& Colour = FSlateColor::UseForeground())
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.0f, 0.0f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Text))
                    .ColorAndOpacity(Colour)
                    .ToolTipText(FText::FromString(Text))
                ];
        };

        if (ColumnName == AuditColumns::Check)
        {
            return SNew(SBox)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                [
                    SNew(SCheckBox)
                    .IsChecked(CheckState)
                    .OnCheckStateChanged(OnCheckChanged)
                ];
        }

        if (ColumnName == AuditColumns::Type)
        {
            // Asset class, matching the type dropdown and PassesFilters exactly. Showing the
            // gameplay class here instead would mean a row labelled "BP_Pickup_Child_C" is caught
            // by the "Blueprint" filter - visibly inconsistent, and the sort of thing that makes
            // someone distrust the whole table.
            //
            // The gameplay class is the more interesting fact for an artist, so it goes in the
            // tooltip rather than being dropped. The report keeps it as its own column.
            const FString Label = ShortTypeName(Item->ClassPath);

            FString Tooltip = Item->ClassPath.ToString();
            if (!Item->GeneratedClassPath.IsNull())
            {
                Tooltip += FString::Printf(TEXT("\nGameplay class: %s"), *Item->GeneratedClassPath.ToString());
            }

            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.0f, 0.0f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Label))
                    .ToolTipText(FText::FromString(Tooltip))
                ];
        }

        if (ColumnName == AuditColumns::Name)
        {
            return TextCell(Item->AssetName.ToString());
        }

        if (ColumnName == AuditColumns::Path)
        {
            return TextCell(Item->PathFromProjectRoot);
        }

        if (ColumnName == AuditColumns::Verdict)
        {
            return TextCell(LexToString(Item->Verdict), VerdictColour(Item->Verdict));
        }

        if (ColumnName == AuditColumns::Levels)
        {
            // The count is what fits; the full list goes in the tooltip.
            TArray<FString> LevelNames;
            LevelNames.Reserve(Item->Levels.Num());
            for (const FName& Level : Item->Levels)
            {
                LevelNames.Add(Level.ToString());
            }

            const FText Tooltip = LevelNames.IsEmpty()
                ? LOCTEXT("NoLevels", "Not reached by any level.")
                : FText::FromString(FString::Join(LevelNames, TEXT("\n")));

            return SNew(SBox)
                .VAlign(VAlign_Center)
                .HAlign(HAlign_Right)
                .Padding(FMargin(4.0f, 0.0f))
                [
                    SNew(STextBlock)
                    .Text(FText::AsNumber(Item->Levels.Num()))
                    .ToolTipText(Tooltip)
                ];
        }

        if (ColumnName == AuditColumns::Hard)
        {
            return TextCell(FString::FromInt(Item->HardReferenceCount));
        }

        if (ColumnName == AuditColumns::Soft)
        {
            return TextCell(FString::FromInt(Item->SoftReferenceCount));
        }

        if (ColumnName == AuditColumns::Provenance)
        {
            return TextCell(ProvenanceToString(Item->Provenance));
        }

        if (ColumnName == AuditColumns::Route)
        {
            const FString RouteText = Item->Route.IsEmpty() ? Item->ProvenanceDetail : Item->Route;
            return TextCell(RouteText);
        }

        return SNullWidget::NullWidget;
    }

private:
    FAssetUsageRowPtr Item;
    FOnCheckStateChanged OnCheckChanged;
    TAttribute<ECheckBoxState> CheckState;
};

void SAssetUsageAuditPanel::Construct(const FArguments& InArgs)
{
    // Everything visible until the user narrows it down.
    VisibleVerdicts = TSet<EAssetUsageVerdict>(GetAllAssetUsageVerdicts());

    // Restore after the defaults are in place, so a config that resolves to nothing leaves the
    // panel usable rather than empty. Must also happen before the widget tree is built: the
    // segmented controls and checkboxes read these fields for their initial state.
    LoadUserSettings();

    StatusText = LOCTEXT("NotRunYet", "Press Run Audit to scan the project.");

    HeaderRow = SNew(SHeaderRow)

        + SHeaderRow::Column(AuditColumns::Check)
        .DefaultLabel(FText::GetEmpty())
        .FixedWidth(28.0f)
        .HAlignHeader(HAlign_Center)
        [
            SNew(SCheckBox)
            .IsChecked(this, &SAssetUsageAuditPanel::GetHeaderCheckState)
            .OnCheckStateChanged(this, &SAssetUsageAuditPanel::OnHeaderCheckChanged)
            .ToolTipText(LOCTEXT("CheckAllTip", "Tick or clear every row currently shown by the filters."))
        ]

        + SHeaderRow::Column(AuditColumns::Type)
        .DefaultLabel(LOCTEXT("ColType", "Type"))
        .FillWidth(0.12f)
        .SortMode(this, &SAssetUsageAuditPanel::GetSortModeForColumn, AuditColumns::Type)
        .OnSort(this, &SAssetUsageAuditPanel::OnSortChanged)

        + SHeaderRow::Column(AuditColumns::Name)
        .DefaultLabel(LOCTEXT("ColName", "Name"))
        .FillWidth(0.16f)
        .SortMode(this, &SAssetUsageAuditPanel::GetSortModeForColumn, AuditColumns::Name)
        .OnSort(this, &SAssetUsageAuditPanel::OnSortChanged)

        + SHeaderRow::Column(AuditColumns::Path)
        .DefaultLabel(LOCTEXT("ColPath", "Path from project root"))
        .FillWidth(0.26f)
        .SortMode(this, &SAssetUsageAuditPanel::GetSortModeForColumn, AuditColumns::Path)
        .OnSort(this, &SAssetUsageAuditPanel::OnSortChanged)

        + SHeaderRow::Column(AuditColumns::Verdict)
        .DefaultLabel(LOCTEXT("ColVerdict", "Verdict"))
        .FillWidth(0.13f)
        .SortMode(this, &SAssetUsageAuditPanel::GetSortModeForColumn, AuditColumns::Verdict)
        .OnSort(this, &SAssetUsageAuditPanel::OnSortChanged)

        + SHeaderRow::Column(AuditColumns::Levels)
        .DefaultLabel(LOCTEXT("ColLevels", "Levels"))
        .FixedWidth(60.0f)
        .HAlignHeader(HAlign_Right)
        .SortMode(this, &SAssetUsageAuditPanel::GetSortModeForColumn, AuditColumns::Levels)
        .OnSort(this, &SAssetUsageAuditPanel::OnSortChanged)

        + SHeaderRow::Column(AuditColumns::Hard)
        .DefaultLabel(LOCTEXT("ColHard", "Hard"))
        .FixedWidth(50.0f)
        .SortMode(this, &SAssetUsageAuditPanel::GetSortModeForColumn, AuditColumns::Hard)
        .OnSort(this, &SAssetUsageAuditPanel::OnSortChanged)

        + SHeaderRow::Column(AuditColumns::Soft)
        .DefaultLabel(LOCTEXT("ColSoft", "Soft"))
        .FixedWidth(50.0f)
        .SortMode(this, &SAssetUsageAuditPanel::GetSortModeForColumn, AuditColumns::Soft)
        .OnSort(this, &SAssetUsageAuditPanel::OnSortChanged)

        + SHeaderRow::Column(AuditColumns::Provenance)
        .DefaultLabel(LOCTEXT("ColProvenance", "Provenance"))
        .FillWidth(0.13f)

        + SHeaderRow::Column(AuditColumns::Route)
        .DefaultLabel(LOCTEXT("ColRoute", "Route"))
        .FillWidth(0.20f);

    ChildSlot
    [
        SNew(SVerticalBox)

        // --- Toolbar -------------------------------------------------------------------------
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.0f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(2.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("RunAudit", "Run Audit"))
                .ToolTipText(LOCTEXT("RunAuditTip", "Sweep the Asset Registry and work out which assets each level uses."))
                .OnClicked(this, &SAssetUsageAuditPanel::OnRunClicked)
                .IsEnabled(this, &SAssetUsageAuditPanel::CanRun)
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(8.0f, 0.0f, 2.0f, 0.0f)
            [
                SNew(SSegmentedControl<EAuditViewMode>)
                .Value_Lambda([this]() { return ViewMode; })
                .OnValueChanged_Lambda([this](EAuditViewMode NewMode)
                {
                    ViewMode = NewMode;
                    RebuildFilteredRows();
                })
                + SSegmentedControl<EAuditViewMode>::Slot(EAuditViewMode::ByAsset)
                    .Text(LOCTEXT("ByAsset", "By asset"))
                    .ToolTip(LOCTEXT("ByAssetTip", "List assets; the Levels column says where each one is used."))
                + SSegmentedControl<EAuditViewMode>::Slot(EAuditViewMode::ByLevel)
                    .Text(LOCTEXT("ByLevel", "By level"))
                    .ToolTip(LOCTEXT("ByLevelTip", "Group the results under the levels that use them. Leave the level scope on \"All levels\" to compare locations side by side."))
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(8.0f, 0.0f, 2.0f, 0.0f)
            [
                SNew(SComboButton)
                .OnGetMenuContent(this, &SAssetUsageAuditPanel::MakeLevelFilterMenu)
                .ButtonContent()
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]()
                    {
                        if (LevelFilters.IsEmpty())
                        {
                            return LOCTEXT("AllLevels", "All levels");
                        }

                        // Name the level when there is one, count them when there are several:
                        // a button reading "WP_Example" is worth more than one reading "1 level".
                        if (LevelFilters.Num() == 1)
                        {
                            return FText::FromString(FPackageName::GetShortName(*LevelFilters.CreateConstIterator()));
                        }

                        return FText::Format(LOCTEXT("NLevels", "{0} levels"), FText::AsNumber(LevelFilters.Num()));
                    })
                ]
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(2.0f, 0.0f)
            [
                SNew(SComboButton)
                .OnGetMenuContent(this, &SAssetUsageAuditPanel::MakeTypeFilterMenu)
                .ButtonContent()
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]()
                    {
                        return VisibleTypeNames.IsEmpty()
                            ? LOCTEXT("AllTypes", "All types")
                            : FText::Format(LOCTEXT("NTypes", "{0} types"), FText::AsNumber(VisibleTypeNames.Num()));
                    })
                ]
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(2.0f, 0.0f)
            [
                SNew(SComboButton)
                .OnGetMenuContent(this, &SAssetUsageAuditPanel::MakeVerdictFilterMenu)
                .ButtonContent()
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]()
                    {
                        return FText::Format(LOCTEXT("NVerdicts", "Verdicts ({0}/5)"), FText::AsNumber(VisibleVerdicts.Num()));
                    })
                ]
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(2.0f, 0.0f)
            [
                SNew(SComboButton)
                .OnGetMenuContent(this, &SAssetUsageAuditPanel::MakeReferenceFilterMenu)
                .ButtonContent()
                [
                    SNew(STextBlock)
                    // Label states the active constraint so a filtered view is never mistaken for
                    // the whole result - the mistake that makes someone delete a "missing" asset.
                    .Text_Lambda([this]()
                    {
                        const bool bHasProvenance = RequiredProvenance != EAssetUsageProvenance::None;

                        if (ReferenceStrength == EReferenceStrengthFilter::Any && !bHasProvenance)
                        {
                            return LOCTEXT("RefsAny", "References");
                        }

                        FText StrengthLabel;
                        switch (ReferenceStrength)
                        {
                            case EReferenceStrengthFilter::HardOnly: StrengthLabel = LOCTEXT("RefsHard", "Hard only"); break;
                            case EReferenceStrengthFilter::SoftOnly: StrengthLabel = LOCTEXT("RefsSoft", "Soft only"); break;
                            case EReferenceStrengthFilter::Mixed:    StrengthLabel = LOCTEXT("RefsMixed", "Mixed"); break;
                            case EReferenceStrengthFilter::None:     StrengthLabel = LOCTEXT("RefsNone", "No edges"); break;
                            default:                                 StrengthLabel = LOCTEXT("RefsAnyShort", "Any"); break;
                        }

                        if (!bHasProvenance)
                        {
                            return FText::Format(LOCTEXT("RefsStrengthOnly", "Refs: {0}"), StrengthLabel);
                        }

                        return FText::Format(
                            LOCTEXT("RefsStrengthAndProv", "Refs: {0} +{1}"),
                            StrengthLabel,
                            FText::FromString(ProvenanceToString(RequiredProvenance)));
                    })
                ]
            ]

            // Gameplay class is a text box, not a dropdown: there are thousands of them on this
            // project, which is exactly why they were taken out of the type menu.
            + SHorizontalBox::Slot()
            .FillWidth(0.5f)
            .VAlign(VAlign_Center)
            .Padding(8.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(SSearchBox)
                .HintText(LOCTEXT("ClassHint", "Gameplay class, e.g. Pickup"))
                .ToolTipText(LOCTEXT("ClassTip", "Matches the Blueprint's generated class. Rows that are not Blueprints are hidden while this is set."))
                .OnTextChanged_Lambda([this](const FText& NewText)
                {
                    GeneratedClassFilter = NewText.ToString();
                    RebuildFilteredRows();
                })
            ]

            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            .VAlign(VAlign_Center)
            .Padding(8.0f, 0.0f)
            [
                SNew(SSearchBox)
                .HintText(LOCTEXT("SearchHint", "Filter by name or path"))
                .OnTextChanged(this, &SAssetUsageAuditPanel::OnSearchTextChanged)
            ]
        ]

        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SSeparator)
        ]

        // --- List ----------------------------------------------------------------------------
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SAssignNew(TreeView, STreeView<FAuditTreeItemPtr>)
            .TreeItemsSource(&RootItems)
            .OnGenerateRow(this, &SAssetUsageAuditPanel::OnGenerateRow)
            .OnGetChildren(this, &SAssetUsageAuditPanel::OnGetChildren)
            .OnMouseButtonDoubleClick(this, &SAssetUsageAuditPanel::OnItemDoubleClicked)
            .SelectionMode(ESelectionMode::Multi)
            .HeaderRow(HeaderRow)
        ]

        // --- Status --------------------------------------------------------------------------
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.0f)
        [
            SNew(STextBlock)
            .Text_Lambda([this]() { return StatusText; })
        ]

        // --- Export bar ----------------------------------------------------------------------
        //
        // Below the results, not above them. The top strip is for narrowing down what you are
        // looking at; exporting acts on what you have already found, and reads left to right as
        // "N ticked -> export". Keeping it at the top put an action button between two filters.
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SSeparator)
        ]

        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.0f, 4.0f, 4.0f, 4.0f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(2.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(this, &SAssetUsageAuditPanel::GetSelectionSummaryText)
            ]

            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SNullWidget::NullWidget
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(2.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("ExportReport", "Export report (JSON + CSV)"))
                .ToolTipText(LOCTEXT("ExportReportTip", "Write the current, filtered result as a JSON and a CSV file."))
                .OnClicked(this, &SAssetUsageAuditPanel::OnExportReportClicked)
                .IsEnabled_Lambda([this]() { return bHasRun; })
            ]

            // Mode, layout, destination and the dependency switch all moved into the export window.
            // They were four controls competing with the filters for the same strip of toolbar, and
            // they are only ever touched at the moment of exporting - which is exactly when a window
            // can show what the choices will produce, instead of leaving the user to guess.
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(2.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("ExportFilesEllipsis", "Export ticked assets..."))
                .ToolTipText(LOCTEXT("ExportFilesTip", "Open the export window: choose copy or convert, the folder layout and the destination."))
                .OnClicked(this, &SAssetUsageAuditPanel::OnExportFilesClicked)
                .IsEnabled(this, &SAssetUsageAuditPanel::HasCheckedAssets)
            ]

            // Delete sits last and behind a separator, deliberately. It acts on the same ticked
            // set as Export, so it belongs on this bar, but it is the only irreversible action in
            // the tool and must never be the button someone reaches by muscle memory.
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(8.0f, 2.0f, 2.0f, 2.0f)
            [
                SNew(SSeparator)
                .Orientation(Orient_Vertical)
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(2.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("DeleteTicked", "Delete ticked assets..."))
                .ToolTipText(LOCTEXT("DeleteTickedTip",
                    "Permanently delete the ticked assets from the project. Levels are never deleted. "
                    "Shows what still references them before anything is removed."))
                .ButtonColorAndOpacity(FLinearColor(0.55f, 0.16f, 0.16f))
                .OnClicked(this, &SAssetUsageAuditPanel::OnDeleteClicked)
                .IsEnabled(this, &SAssetUsageAuditPanel::HasCheckedAssets)
            ]
        ]
    ];
}

// --- Running ---------------------------------------------------------------------------------

bool SAssetUsageAuditPanel::CanRun() const
{
    return true;
}

FReply SAssetUsageAuditPanel::OnRunClicked()
{
    RunAudit();
    return FReply::Handled();
}

void SAssetUsageAuditPanel::RunAudit()
{
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    if (AssetRegistry.IsLoadingAssets())
    {
        StatusText = LOCTEXT("WaitingForRegistry", "Asset Registry is still scanning; waiting for it to finish...");
        AssetRegistry.WaitForCompletion();
    }

    const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get();

    FAssetUsageAuditRequest Request;
    if (Settings)
    {
        Request.IncludePackagePaths = Settings->IncludedPackagePaths;
        Request.ExcludePackagePaths = Settings->ExcludedPackagePaths;
        Request.bScanIndirectReferences = Settings->bScanIndirectReferences;
        Request.bHideExternalPackages = Settings->bHideExternalPackages;
    }

    // Scope the sweep to the chosen levels. Previously this was never set, so the panel always
    // swept all 1072 levels and the level dropdown could only filter the result afterwards -
    // which is not the question the task asks ("assets of type X for location Y").
    //
    // An empty set means every level, which is what the resolver already does with an empty array.
    for (FName Level : LevelFilters)
    {
        Request.LevelPackages.Add(Level);
    }

    // Sorted so two runs over the same selection produce the same report ordering, and so the
    // header line in the log is comparable between runs.
    Request.LevelPackages.Sort(FNameLexicalLess());

    FScopedSlowTask SlowTask(100.0f, LOCTEXT("Auditing", "Auditing asset usage..."));
    SlowTask.MakeDialog(true);

    Request.ResolveOptions.OnLevelProgress = [&SlowTask](int32 Current, int32 Total)
    {
        if (Total > 0 && Current % 16 == 0)
        {
            SlowTask.EnterProgressFrame(16.0f * 100.0f / static_cast<float>(Total));
        }
    };

    Request.ResolveOptions.ShouldAbort = [&SlowTask]()
    {
        return SlowTask.ShouldCancel();
    };

    UE_LOG(LogAssetUsageAuditEditor, Log,
        TEXT("Panel: starting audit. Include=[%s] Exclude=[%s] IndirectScan=%s HideExternal=%s"),
        Request.IncludePackagePaths.IsEmpty() ? TEXT("/Game") : *FString::Join(Request.IncludePackagePaths, TEXT(", ")),
        Request.ExcludePackagePaths.IsEmpty() ? TEXT("none") : *FString::Join(Request.ExcludePackagePaths, TEXT(", ")),
        Request.bScanIndirectReferences ? TEXT("on") : TEXT("off"),
        Request.bHideExternalPackages ? TEXT("on") : TEXT("off"));

    LastResult = FAssetUsageAuditor::Run(AssetRegistry, Request);

    // Logged as well as shown on screen: when someone reports "the numbers look wrong", the status
    // line is already gone and the settings that produced them are not recoverable from a
    // screenshot. The pair above and below is what makes a report reproducible after the fact.
    UE_LOG(LogAssetUsageAuditEditor, Log,
        TEXT("Panel: audit finished in %.2fs. %d rows - OnLevel %d, AssetsOnly %d, Config/Source %d, Unreferenced %d, Unknown %d."),
        LastResult.Header.ScanDurationSeconds,
        LastResult.Rows.Num(),
        LastResult.CountByVerdict(EAssetUsageVerdict::UsedOnLevel),
        LastResult.CountByVerdict(EAssetUsageVerdict::UsedByAssetsOnly),
        LastResult.CountByVerdict(EAssetUsageVerdict::ReferencedFromConfigOrSource),
        LastResult.CountByVerdict(EAssetUsageVerdict::Unreferenced),
        LastResult.CountByVerdict(EAssetUsageVerdict::Unknown));

    AllRows.Reset(LastResult.Rows.Num());
    for (const FAssetUsageRow& Row : LastResult.Rows)
    {
        AllRows.Add(MakeShared<FAssetUsageRow>(Row));
    }

    bHasRun = true;

    // Ticks are keyed by package name, so they survive a re-run. Drop only the ones whose asset
    // no longer exists, rather than silently clearing the user's whole selection.
    if (!CheckedPackages.IsEmpty())
    {
        TSet<FName> StillPresent;
        for (const FAssetUsageRowPtr& Row : AllRows)
        {
            if (CheckedPackages.Contains(Row->PackageName))
            {
                StillPresent.Add(Row->PackageName);
            }
        }

        CheckedPackages = MoveTemp(StillPresent);
    }

    RebuildFilteredRows();

    StatusText = FText::Format(
        LOCTEXT("AuditDone", "{0} rows in {1} s. On level: {2}  |  Assets only: {3}  |  Config/Source: {4}  |  Unreferenced: {5}  |  Unknown: {6}"),
        FText::AsNumber(LastResult.Rows.Num()),
        FText::AsNumber(FMath::RoundToFloat(LastResult.Header.ScanDurationSeconds * 100.0f) / 100.0f),
        FText::AsNumber(LastResult.CountByVerdict(EAssetUsageVerdict::UsedOnLevel)),
        FText::AsNumber(LastResult.CountByVerdict(EAssetUsageVerdict::UsedByAssetsOnly)),
        FText::AsNumber(LastResult.CountByVerdict(EAssetUsageVerdict::ReferencedFromConfigOrSource)),
        FText::AsNumber(LastResult.CountByVerdict(EAssetUsageVerdict::Unreferenced)),
        FText::AsNumber(LastResult.CountByVerdict(EAssetUsageVerdict::Unknown)));
}

/**
 * A level heading in by-level mode.
 *
 * Deliberately not the asset row widget with blank cells: a heading answers a different question
 * ("this location, this many assets") and filling the asset columns with empty space made the two
 * kinds of line hard to tell apart at a glance.
 */
class SAuditLevelRowWidget : public SMultiColumnTableRow<FAuditTreeItemPtr>
{
public:
    SLATE_BEGIN_ARGS(SAuditLevelRowWidget) {}
        SLATE_ARGUMENT(FAuditTreeItemPtr, Item)
        SLATE_EVENT(FOnCheckStateChanged, OnCheckChanged)
        SLATE_ATTRIBUTE(ECheckBoxState, CheckState)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
    {
        Item = InArgs._Item;
        OnCheckChanged = InArgs._OnCheckChanged;
        CheckState = InArgs._CheckState;

        SMultiColumnTableRow<FAuditTreeItemPtr>::Construct(FSuperRowType::FArguments(), InOwnerTable);
    }

    virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
    {
        if (!Item.IsValid())
        {
            return SNullWidget::NullWidget;
        }

        if (ColumnName == AuditColumns::Check)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .HAlign(HAlign_Center)
                [
                    SNew(SCheckBox)
                    .IsChecked(CheckState)
                    .OnCheckStateChanged(OnCheckChanged)
                    .ToolTipText(LOCTEXT("LevelCheckTip", "Tick every asset shown under this level."))
                ];
        }

        if (ColumnName == AuditColumns::Name)
        {
            const FString LevelPath = Item->LevelPackage.ToString();

            return SNew(SHorizontalBox)

                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(SExpanderArrow, SharedThis(this))
                ]

                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(FPackageName::GetShortName(LevelPath)))
                    .ToolTipText(FText::FromString(LevelPath))
                    .Font(FAppStyle::GetFontStyle("BoldFont"))
                ];
        }

        if (ColumnName == AuditColumns::Path)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.0f, 0.0f))
                [
                    SNew(STextBlock)
                    .Text(FText::Format(
                        LOCTEXT("LevelAssetCount", "{0} asset(s)"),
                        FText::AsNumber(Item->Children.Num())))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ];
        }

        return SNullWidget::NullWidget;
    }

private:
    FAuditTreeItemPtr Item;
    FOnCheckStateChanged OnCheckChanged;
    TAttribute<ECheckBoxState> CheckState;
};

// --- Tree ------------------------------------------------------------------------------------

TSharedRef<ITableRow> SAssetUsageAuditPanel::OnGenerateRow(FAuditTreeItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
    if (Item.IsValid() && Item->IsLevel())
    {
        return SNew(SAuditLevelRowWidget, OwnerTable)
            .Item(Item)
            .CheckState(TAttribute<ECheckBoxState>::CreateLambda([this, Item]() { return GetLevelCheckState(Item); }))
            .OnCheckChanged(FOnCheckStateChanged::CreateLambda([this, Item](ECheckBoxState NewState)
            {
                OnLevelCheckChanged(NewState, Item);
            }));
    }

    const FAssetUsageRowPtr Row = Item.IsValid() ? Item->Row : nullptr;

    return SNew(SAssetUsageRowWidget, OwnerTable)
        .Item(Row)
        .CheckState(TAttribute<ECheckBoxState>::CreateLambda([this, Row]() { return GetRowCheckState(Row); }))
        .OnCheckChanged(FOnCheckStateChanged::CreateLambda([this, Row](ECheckBoxState NewState)
        {
            OnRowCheckChanged(NewState, Row);
        }));
}

void SAssetUsageAuditPanel::OnGetChildren(FAuditTreeItemPtr Item, TArray<FAuditTreeItemPtr>& OutChildren)
{
    if (Item.IsValid())
    {
        OutChildren = Item->Children;
    }
}

void SAssetUsageAuditPanel::OnItemDoubleClicked(FAuditTreeItemPtr Item)
{
    if (!Item.IsValid())
    {
        return;
    }

    const FName PackageName = Item->IsLevel()
        ? Item->LevelPackage
        : (Item->Row.IsValid() ? Item->Row->PackageName : NAME_None);

    if (PackageName.IsNone())
    {
        return;
    }

    if (!GEditor)
    {
        UE_LOG(LogAssetUsageAuditEditor, Warning, TEXT("Cannot reveal '%s': no editor engine."), *PackageName.ToString());
        return;
    }

    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    TArray<FAssetData> Assets;
    Registry.GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyOnDiskAssets=*/true);

    // A package can hold several assets. Sync to the one the row is actually about; falling back to
    // the whole package would reveal a redirector or a secondary object and look like the wrong row
    // was selected.
    if (!Item->IsLevel() && Item->Row.IsValid())
    {
        const FName WantedAsset = Item->Row->AssetName;

        Assets.RemoveAll([WantedAsset](const FAssetData& Candidate)
        {
            return !WantedAsset.IsNone() && Candidate.AssetName != WantedAsset;
        });
    }

    if (Assets.IsEmpty())
    {
        // Ordinary for a package the registry knows only as a dependency target, and for anything
        // that has no file on disk. Say so rather than silently doing nothing on a double-click.
        StatusText = FText::Format(
            LOCTEXT("RevealMissing", "'{0}' has no asset in the Content Browser - it may have been deleted or never saved."),
            FText::FromName(PackageName));

        UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Browse To: no on-disk asset for package '%s'."), *PackageName.ToString());
        return;
    }

    GEditor->SyncBrowserToObjects(Assets);
}

ECheckBoxState SAssetUsageAuditPanel::GetLevelCheckState(FAuditTreeItemPtr Item) const
{
    if (!Item.IsValid() || Item->Children.IsEmpty())
    {
        return ECheckBoxState::Unchecked;
    }

    int32 Ticked = 0;

    for (const FAuditTreeItemPtr& Child : Item->Children)
    {
        if (Child.IsValid() && Child->Row.IsValid() && CheckedPackages.Contains(Child->Row->PackageName))
        {
            ++Ticked;
        }
    }

    if (Ticked == 0)
    {
        return ECheckBoxState::Unchecked;
    }

    // Undetermined rather than Checked for a partial selection: showing a full tick over a level
    // where half the assets are ticked would misreport what the export is about to do.
    return Ticked == Item->Children.Num() ? ECheckBoxState::Checked : ECheckBoxState::Undetermined;
}

void SAssetUsageAuditPanel::OnLevelCheckChanged(ECheckBoxState NewState, FAuditTreeItemPtr Item)
{
    if (!Item.IsValid())
    {
        return;
    }

    const bool bTick = (NewState == ECheckBoxState::Checked);

    for (const FAuditTreeItemPtr& Child : Item->Children)
    {
        if (!Child.IsValid() || !Child->Row.IsValid())
        {
            continue;
        }

        if (bTick)
        {
            CheckedPackages.Add(Child->Row->PackageName);
        }
        else
        {
            CheckedPackages.Remove(Child->Row->PackageName);
        }
    }
}

void SAssetUsageAuditPanel::RebuildTree()
{
    RootItems.Reset();

    if (ViewMode == EAuditViewMode::ByAsset)
    {
        RootItems.Reserve(FilteredRows.Num());

        for (const FAssetUsageRowPtr& Row : FilteredRows)
        {
            FAuditTreeItemPtr Node = MakeShared<FAuditTreeItem>();
            Node->Row = Row;
            RootItems.Add(MoveTemp(Node));
        }

        return;
    }

    // By level. Group in one pass, preserving the sorted order of FilteredRows inside each level so
    // the column sort still means something after grouping.
    TMap<FName, FAuditTreeItemPtr> LevelNodes;

    for (const FAssetUsageRowPtr& Row : FilteredRows)
    {
        if (!Row.IsValid())
        {
            continue;
        }

        for (FName LevelPackage : Row->Levels)
        {
            // A chosen scope narrows the tree to those levels; without a scope every level that uses
            // the asset gets a heading, which is what makes comparing locations possible.
            if (!LevelFilters.IsEmpty() && !LevelFilters.Contains(LevelPackage))
            {
                continue;
            }

            FAuditTreeItemPtr& LevelNode = LevelNodes.FindOrAdd(LevelPackage);
            if (!LevelNode.IsValid())
            {
                LevelNode = MakeShared<FAuditTreeItem>();
                LevelNode->LevelPackage = LevelPackage;
            }

            FAuditTreeItemPtr Child = MakeShared<FAuditTreeItem>();
            Child->Row = Row;
            LevelNode->Children.Add(MoveTemp(Child));
        }
    }

    LevelNodes.GenerateValueArray(RootItems);

    // Biggest first: on a full sweep this is 1072 headings, and the levels worth looking at are the
    // ones with content. Alphabetical would bury them.
    RootItems.Sort([](const FAuditTreeItemPtr& A, const FAuditTreeItemPtr& B)
    {
        if (A->Children.Num() != B->Children.Num())
        {
            return A->Children.Num() > B->Children.Num();
        }
        return A->LevelPackage.LexicalLess(B->LevelPackage);
    });

    // Expand automatically only when there is exactly one level to look at. Expanding 1072 headings
    // would render tens of thousands of rows and defeat the point of grouping them.
    if (RootItems.Num() == 1 && TreeView.IsValid())
    {
        TreeView->SetItemExpansion(RootItems[0], true);
    }
}

void SAssetUsageAuditPanel::RebuildFilteredRows()
{
    FilteredRows.Reset();

    for (const FAssetUsageRowPtr& Row : AllRows)
    {
        if (Row.IsValid() && PassesFilters(*Row))
        {
            FilteredRows.Add(Row);
        }
    }

    ApplySorting();

    RebuildTree();

    if (TreeView.IsValid())
    {
        TreeView->RequestTreeRefresh();
    }
}

// --- Filtering -------------------------------------------------------------------------------

bool SAssetUsageAuditPanel::PassesFilters(const FAssetUsageRow& Row) const
{
    if (!VisibleVerdicts.Contains(Row.Verdict))
    {
        return false;
    }

    // Asset class only, matching what the dropdown offers. A Blueprint reads as "Blueprint" here;
    // asking "which of these is a StaticMeshActor" is the generated-class filter below, because
    // mixing the two axes is what made the type menu unusable.
    if (!VisibleTypeNames.IsEmpty())
    {
        if (!VisibleTypeNames.Contains(FName(*ShortTypeName(Row.ClassPath))))
        {
            return false;
        }
    }

    // Substring rather than an exact list: gameplay classes number in the thousands, so this is a
    // text box, and "Pickup" usefully matches BP_Pickup_Child_C and BP_Pickup_Master alike.
    if (!GeneratedClassFilter.IsEmpty())
    {
        if (Row.GeneratedClassPath.IsNull())
        {
            return false;
        }

        if (!ShortTypeName(Row.GeneratedClassPath).Contains(GeneratedClassFilter, ESearchCase::IgnoreCase))
        {
            return false;
        }
    }

    if (ReferenceStrength != EReferenceStrengthFilter::Any)
    {
        const bool bHasHard = Row.HardReferenceCount > 0;
        const bool bHasSoft = Row.SoftReferenceCount > 0;

        bool bStrengthMatches = false;
        switch (ReferenceStrength)
        {
            case EReferenceStrengthFilter::HardOnly:
                bStrengthMatches = bHasHard && !bHasSoft;
                break;
            case EReferenceStrengthFilter::SoftOnly:
                bStrengthMatches = bHasSoft && !bHasHard;
                break;
            case EReferenceStrengthFilter::Mixed:
                bStrengthMatches = bHasHard && bHasSoft;
                break;
            case EReferenceStrengthFilter::None:
                bStrengthMatches = !bHasHard && !bHasSoft;
                break;
            default:
                bStrengthMatches = true;
                break;
        }

        if (!bStrengthMatches)
        {
            return false;
        }
    }

    // ANDed, not ORed: ticking two flags asks for rows carrying both, which is what makes
    // combinations like "editor-only AND through an external actor" expressible.
    if (RequiredProvenance != EAssetUsageProvenance::None)
    {
        if (!EnumHasAllFlags(Row.Provenance, RequiredProvenance))
        {
            return false;
        }
    }

    // Any of the chosen levels is enough. Requiring all of them would answer "what do these
    // locations share", which is a different and much rarer question than "what is on these".
    if (!LevelFilters.IsEmpty())
    {
        bool bOnAnyChosenLevel = false;
        for (FName Level : Row.Levels)
        {
            if (LevelFilters.Contains(Level))
            {
                bOnAnyChosenLevel = true;
                break;
            }
        }

        if (!bOnAnyChosenLevel)
        {
            return false;
        }
    }

    // By-level groups rows under level headings, so a row belonging to no level has nowhere to sit.
    // Before the tree existed this mode showed nothing at all until a level was picked; grouping
    // makes the unpicked case the useful one - every level at once, which is what comparing two
    // locations requires.
    if (ViewMode == EAuditViewMode::ByLevel && Row.Levels.IsEmpty())
    {
        return false;
    }

    if (!SearchText.IsEmpty())
    {
        const bool bMatches =
            Row.AssetName.ToString().Contains(SearchText, ESearchCase::IgnoreCase) ||
            Row.PathFromProjectRoot.Contains(SearchText, ESearchCase::IgnoreCase);

        if (!bMatches)
        {
            return false;
        }
    }

    return true;
}

void SAssetUsageAuditPanel::OnSearchTextChanged(const FText& NewText)
{
    SearchText = NewText.ToString();
    RebuildFilteredRows();
}

TSharedRef<SWidget> SAssetUsageAuditPanel::MakeVerdictFilterMenu()
{
    FMenuBuilder MenuBuilder(false, nullptr);

    const TArray<EAssetUsageVerdict> AllVerdicts = GetAllAssetUsageVerdicts();

    for (EAssetUsageVerdict Verdict : AllVerdicts)
    {
        MenuBuilder.AddMenuEntry(
            FText::FromString(LexToString(Verdict)),
            Verdict == EAssetUsageVerdict::Unknown
                ? LOCTEXT("UnknownTip", "The registry cannot see this asset's references (FMOD, DataTable rows, runtime-built paths). Unknown does NOT mean unused.")
                : FText::GetEmpty(),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([this, Verdict]()
                {
                    if (VisibleVerdicts.Contains(Verdict))
                    {
                        VisibleVerdicts.Remove(Verdict);
                    }
                    else
                    {
                        VisibleVerdicts.Add(Verdict);
                    }
                    RebuildFilteredRows();
                }),
                FCanExecuteAction(),
                FIsActionChecked::CreateLambda([this, Verdict]() { return VisibleVerdicts.Contains(Verdict); })),
            NAME_None,
            EUserInterfaceActionType::ToggleButton);
    }

    return MenuBuilder.MakeWidget();
}

void SAssetUsageAuditPanel::RefreshAvailableFilters()
{
    AvailableLevels.Reset();
    AvailableTypes.Reset();
    bAvailableFiltersReady = false;

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    if (AssetRegistry.IsLoadingAssets())
    {
        // Never block the UI thread on a full project scan just to populate a dropdown. The menu
        // shows an explanatory entry instead, and the next open retries.
        UE_LOG(LogAssetUsageAuditEditor, Log,
            TEXT("Filter lists not built yet: the Asset Registry is still scanning."));
        return;
    }

    const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get();
    const TArray<FString> ExcludedPaths = Settings ? Settings->ExcludedPackagePaths : TArray<FString>();

    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.bIncludeOnlyOnDiskAssets = true;

    if (Settings && !Settings->IncludedPackagePaths.IsEmpty())
    {
        for (const FString& Path : Settings->IncludedPackagePaths)
        {
            Filter.PackagePaths.Add(FName(*Path));
        }
    }
    else
    {
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
    }

    FARCompiledFilter CompiledFilter;
    AssetRegistry.CompileFilter(Filter, CompiledFilter);

    const FTopLevelAssetPath WorldClassPath = UWorld::StaticClass()->GetClassPathName();
    static const FName GeneratedClassTag(TEXT("GeneratedClass"));

    TSet<FName> TypeSet;
    const double StartTime = FPlatformTime::Seconds();

    AssetRegistry.EnumerateAssets(CompiledFilter, [&](const FAssetData& AssetData)
    {
        const FName PackageName = AssetData.PackageName;

        if (AssetUsagePaths::IsPathExcluded(PackageName, ExcludedPaths))
        {
            return true;
        }

        // OFPA packages are machine-named hashes. Offering 16117 of them in a dropdown would bury
        // the levels a person is actually looking for.
        if (AssetUsagePaths::IsExternalPackage(PackageName))
        {
            return true;
        }

        if (AssetData.AssetClassPath == WorldClassPath)
        {
            AvailableLevels.Add(PackageName);
        }

        // Asset class only - deliberately NOT the Blueprint's generated class.
        //
        // Folding generated classes in here produced 2980 entries on this project, because every
        // Blueprint contributes its own. A dropdown that long is no more usable than an empty one:
        // someone looking for "StaticMesh" has to scroll past BP_Pickup_Child_C and its thousand
        // siblings. Asset class alone is a bounded list of real asset types.
        //
        // Filtering by gameplay class is a different question and gets its own text box; the type
        // presets in FAssetTypeFilter still expand through GetDerivedClassNames + the GeneratedClass
        // tag, so nothing is lost from the analysis - only from this menu.
        const FString AssetTypeName = ShortTypeName(AssetData.AssetClassPath);
        if (!AssetTypeName.IsEmpty())
        {
            TypeSet.Add(FName(*AssetTypeName));
        }

        return true;
    }, UE::AssetRegistry::EEnumerateAssetsFlags::OnlyOnDiskAssets);

    AvailableLevels.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

    AvailableTypes = TypeSet.Array();
    AvailableTypes.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

    bAvailableFiltersReady = true;

    UE_LOG(LogAssetUsageAuditEditor, Log,
        TEXT("Filter lists built in %.2fs: %d levels, %d asset types."),
        FPlatformTime::Seconds() - StartTime, AvailableLevels.Num(), AvailableTypes.Num());
}

SAssetUsageAuditPanel::~SAssetUsageAuditPanel()
{
    SaveUserSettings();
}

void SAssetUsageAuditPanel::LoadUserSettings()
{
    const UAssetUsageAuditUserSettings* Saved = UAssetUsageAuditUserSettings::Get();
    if (!Saved)
    {
        return;
    }

    for (const FString& LevelPackage : Saved->LastLevelPackages)
    {
        if (!LevelPackage.IsEmpty())
        {
            LevelFilters.Add(FName(*LevelPackage));
        }
    }
    GeneratedClassFilter = Saved->GeneratedClassFilter;
    bIncludeDependencies = Saved->bIncludeDependencies;

    for (const FString& TypeName : Saved->VisibleTypeNames)
    {
        VisibleTypeNames.Add(FName(*TypeName));
    }

    // Verdicts are stored by name, so an enum that gains a value does not silently reinterpret an
    // old index. An unrecognised name is dropped rather than guessed at.
    if (Saved->VisibleVerdictNames.Num() > 0)
    {
        TSet<EAssetUsageVerdict> Restored;

        for (const FString& VerdictName : Saved->VisibleVerdictNames)
        {
            for (EAssetUsageVerdict Verdict : GetAllAssetUsageVerdicts())
            {
                if (VerdictName.Equals(LexToString(Verdict), ESearchCase::IgnoreCase))
                {
                    Restored.Add(Verdict);
                    break;
                }
            }
        }

        // An empty restored set would hide every row with no way to tell why, so the default
        // "everything visible" wins over a config file that resolved to nothing.
        if (Restored.Num() > 0)
        {
            VisibleVerdicts = MoveTemp(Restored);
        }
    }

    // Integers are validated rather than cast: an out-of-range value in a hand-edited ini would
    // otherwise become an invalid enum and take a branch nobody wrote.
    if (Saved->ViewMode >= 0 && Saved->ViewMode <= static_cast<int32>(EAuditViewMode::ByLevel))
    {
        ViewMode = static_cast<EAuditViewMode>(Saved->ViewMode);
    }

    if (Saved->ExportMode >= 0 && Saved->ExportMode <= static_cast<int32>(EFileExportMode::ConvertToExchangeFormat))
    {
        ExportMode = static_cast<EFileExportMode>(Saved->ExportMode);
    }

    if (Saved->ReferenceStrength >= 0 && Saved->ReferenceStrength <= static_cast<int32>(EReferenceStrengthFilter::None))
    {
        ReferenceStrength = static_cast<EReferenceStrengthFilter>(Saved->ReferenceStrength);
    }

    RequiredProvenance = static_cast<EAssetUsageProvenance>(
        static_cast<uint8>(Saved->RequiredProvenance & 0xFF));
}

void SAssetUsageAuditPanel::SaveUserSettings() const
{
    UAssetUsageAuditUserSettings* Saved = UAssetUsageAuditUserSettings::Get();
    if (!Saved)
    {
        return;
    }

    Saved->LastLevelPackages.Reset();
    for (FName Level : LevelFilters)
    {
        Saved->LastLevelPackages.Add(Level.ToString());
    }
    Saved->GeneratedClassFilter = GeneratedClassFilter;
    Saved->bIncludeDependencies = bIncludeDependencies;

    Saved->VisibleTypeNames.Reset();
    for (FName TypeName : VisibleTypeNames)
    {
        Saved->VisibleTypeNames.Add(TypeName.ToString());
    }

    Saved->VisibleVerdictNames.Reset();
    for (EAssetUsageVerdict Verdict : VisibleVerdicts)
    {
        Saved->VisibleVerdictNames.Add(LexToString(Verdict));
    }

    Saved->ViewMode = static_cast<int32>(ViewMode);
    Saved->ExportMode = static_cast<int32>(ExportMode);
    Saved->ReferenceStrength = static_cast<int32>(ReferenceStrength);
    Saved->RequiredProvenance = static_cast<int32>(RequiredProvenance);

    Saved->SaveConfig();
}

TSet<FName> SAssetUsageAuditPanel::ResolvePresetTypeNames(EAssetTypePreset Preset) const
{
    TSet<FName> Out;

    IAssetRegistry* Registry = IAssetRegistry::Get();
    if (!Registry)
    {
        return Out;
    }

    const TArray<FTopLevelAssetPath> Seeds = GetPresetClassPaths(Preset);
    if (Seeds.IsEmpty())
    {
        return Out;
    }

    TSet<FTopLevelAssetPath> Derived;
    Registry->GetDerivedClassNames(Seeds, TSet<FTopLevelAssetPath>(), Derived);

    // GetDerivedClassNames returns strict descendants in some builds and includes the seeds in
    // others. Adding the seeds explicitly makes the result the same either way - the same reason
    // FAssetTypeFilter::Compile does it.
    for (const FTopLevelAssetPath& ClassPath : Seeds)
    {
        Out.Add(ClassPath.GetAssetName());
    }

    for (const FTopLevelAssetPath& ClassPath : Derived)
    {
        Out.Add(ClassPath.GetAssetName());
    }

    return Out;
}

TSharedRef<SWidget> SAssetUsageAuditPanel::MakeTypeFilterMenu()
{
    if (!bAvailableFiltersReady)
    {
        RefreshAvailableFilters();
    }

    // Counts come from the last result when there is one, so the menu reads as
    // "StaticMesh (1204)" after a run and simply "StaticMesh" before it.
    // Keyed on asset class, the same axis the menu lists and PassesFilters matches. Keying these
    // on the generated class instead would silently show "Blueprint (0)" next to thousands of
    // Blueprint rows, because the counts and the entries would be on different axes.
    TMap<FName, int32> TypeCounts;
    for (const FAssetUsageRowPtr& Row : AllRows)
    {
        const FName TypeName(*ShortTypeName(Row->ClassPath));
        if (!TypeName.IsNone())
        {
            ++TypeCounts.FindOrAdd(TypeName);
        }
    }

    // Offer every type in the project, not only the ones the last sweep happened to return.
    TArray<TPair<FName, int32>> Sorted;
    Sorted.Reserve(AvailableTypes.Num());
    for (const FName& TypeName : AvailableTypes)
    {
        Sorted.Emplace(TypeName, TypeCounts.FindRef(TypeName));
    }

    // Types present in the result first, by count; then the rest alphabetically.
    Sorted.Sort([](const TPair<FName, int32>& A, const TPair<FName, int32>& B)
    {
        if (A.Value != B.Value)
        {
            return A.Value > B.Value;
        }
        return A.Key.LexicalLess(B.Key);
    });

    FMenuBuilder MenuBuilder(false, nullptr);

    if (Sorted.IsEmpty())
    {
        MenuBuilder.AddWidget(
            SNew(STextBlock)
            .Text(bAvailableFiltersReady
                ? LOCTEXT("NoTypesFound", "No asset types found in the scanned paths.")
                : LOCTEXT("TypesNotReady", "Asset Registry is still scanning; reopen this menu shortly."))
            .ColorAndOpacity(FSlateColor::UseSubduedForeground()),
            FText::GetEmpty());

        return MenuBuilder.MakeWidget();
    }

    MenuBuilder.AddMenuEntry(
        LOCTEXT("ClearTypes", "Show all types"),
        FText::GetEmpty(),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this]()
        {
            VisibleTypeNames.Reset();
            RebuildFilteredRows();
        })));

    // Presets first. The agreed requirement was "presets plus raw classes", and an artist looking
    // for VFX should not have to know that it means NiagaraSystem and not ParticleSystem. The raw
    // list stays below for anything a preset does not cover.
    MenuBuilder.BeginSection(TEXT("Presets"), LOCTEXT("PresetSection", "Presets"));
    {
        const TSet<FName> Available(AvailableTypes);

        for (EAssetTypePreset Preset : GetAllAssetTypePresets())
        {
            // Resolve to the short names this menu deals in, expanded through subclasses: without
            // expansion "Material" would miss every MaterialInstanceConstant, which is most of what
            // a project actually contains.
            TSet<FName> PresetTypes = ResolvePresetTypeNames(Preset);

            // Only offer what the project has. A preset that selects nothing looks broken, and this
            // project has no skeletal meshes at all - offering the entry would invite exactly that
            // conclusion.
            TSet<FName> Present;
            for (FName TypeName : PresetTypes)
            {
                if (Available.Contains(TypeName))
                {
                    Present.Add(TypeName);
                }
            }

            if (Present.IsEmpty())
            {
                continue;
            }

            const FText Label = FText::Format(
                LOCTEXT("PresetEntry", "{0}  ({1} class(es))"),
                FText::FromString(LexToString(Preset)),
                FText::AsNumber(Present.Num()));

            MenuBuilder.AddMenuEntry(
                Label,
                FText::Format(
                    LOCTEXT("PresetEntryTip", "Select the {0} classes present in this project. Adds to the current selection."),
                    FText::FromString(LexToString(Preset))),
                FSlateIcon(),
                FUIAction(
                    FExecuteAction::CreateLambda([this, Present]()
                    {
                        // Toggle as a group: clicking a fully selected preset clears it, which is
                        // the only way to undo it without hunting its classes in the raw list.
                        const bool bAllSelected = Algo::AllOf(Present, [this](FName TypeName)
                        {
                            return VisibleTypeNames.Contains(TypeName);
                        });

                        for (FName TypeName : Present)
                        {
                            if (bAllSelected)
                            {
                                VisibleTypeNames.Remove(TypeName);
                            }
                            else
                            {
                                VisibleTypeNames.Add(TypeName);
                            }
                        }

                        RebuildFilteredRows();
                    }),
                    FCanExecuteAction(),
                    FIsActionChecked::CreateLambda([this, Present]()
                    {
                        return Algo::AllOf(Present, [this](FName TypeName)
                        {
                            return VisibleTypeNames.Contains(TypeName);
                        });
                    })),
                NAME_None,
                EUserInterfaceActionType::Check);
        }
    }
    MenuBuilder.EndSection();

    MenuBuilder.BeginSection(TEXT("RawClasses"), LOCTEXT("RawClassSection", "All classes"));

    for (const TPair<FName, int32>& Entry : Sorted)
    {
        const FName TypeName = Entry.Key;

        MenuBuilder.AddMenuEntry(
            FText::Format(LOCTEXT("TypeEntry", "{0}  ({1})"), FText::FromName(TypeName), FText::AsNumber(Entry.Value)),
            FText::GetEmpty(),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([this, TypeName]()
                {
                    if (VisibleTypeNames.Contains(TypeName))
                    {
                        VisibleTypeNames.Remove(TypeName);
                    }
                    else
                    {
                        VisibleTypeNames.Add(TypeName);
                    }
                    RebuildFilteredRows();
                }),
                FCanExecuteAction(),
                FIsActionChecked::CreateLambda([this, TypeName]() { return VisibleTypeNames.Contains(TypeName); })),
            NAME_None,
            EUserInterfaceActionType::ToggleButton);
    }

    MenuBuilder.EndSection();

    return SNew(SBox)
        .MaxDesiredHeight(600.0f)
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            [
                MenuBuilder.MakeWidget()
            ]
        ];
}

void SAssetUsageAuditPanel::OnScopeChanged()
{
    // Deliberately does NOT re-run. Scope is chosen from a dropdown, and "All levels" means 1072
    // levels on this project - kicking that off because someone browsed the list would be a nasty
    // surprise. Mark the result stale, say so, and let them press Run.
    if (!bHasRun)
    {
        return;
    }

    bResultStale = true;

    // Name the scope rather than just saying it changed: the whole point of the message is that the
    // rows on screen answer a different question than the one now selected.
    FText ScopeText;
    if (LevelFilters.IsEmpty())
    {
        ScopeText = LOCTEXT("ScopeAll", "all levels");
    }
    else if (LevelFilters.Num() == 1)
    {
        ScopeText = FText::FromString(FPackageName::GetShortName(*LevelFilters.CreateConstIterator()));
    }
    else
    {
        ScopeText = FText::Format(LOCTEXT("ScopeNLevels", "{0} levels"), FText::AsNumber(LevelFilters.Num()));
    }

    StatusText = FText::Format(
        LOCTEXT("ScopeChanged", "Scope changed to {0}. Press Run Audit to refresh - the rows below are from the previous scope."),
        ScopeText);

    FString ScopeDescription;
    if (LevelFilters.IsEmpty())
    {
        ScopeDescription = TEXT("all levels");
    }
    else
    {
        TArray<FString> Names;
        Names.Reserve(LevelFilters.Num());
        for (FName Level : LevelFilters)
        {
            Names.Add(Level.ToString());
        }
        Names.Sort();
        ScopeDescription = FString::Join(Names, TEXT(", "));
    }
    UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Panel: audit scope changed to '%s'; previous result marked stale."), *ScopeDescription);
}

TSharedRef<SWidget> SAssetUsageAuditPanel::MakeReferenceFilterMenu()
{
    FMenuBuilder MenuBuilder(false, nullptr);

    const auto AddStrength = [this, &MenuBuilder](EReferenceStrengthFilter Value, const FText& Label, const FText& Tooltip)
    {
        MenuBuilder.AddMenuEntry(
            Label,
            Tooltip,
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([this, Value]()
                {
                    ReferenceStrength = Value;
                    RebuildFilteredRows();
                }),
                FCanExecuteAction(),
                FIsActionChecked::CreateLambda([this, Value]() { return ReferenceStrength == Value; })),
            NAME_None,
            EUserInterfaceActionType::RadioButton);
    };

    MenuBuilder.BeginSection("Strength", LOCTEXT("StrengthSection", "How the asset is held"));

    AddStrength(EReferenceStrengthFilter::Any,
        LOCTEXT("StrengthAny", "Any"),
        LOCTEXT("StrengthAnyTip", "No constraint on reference strength."));

    AddStrength(EReferenceStrengthFilter::HardOnly,
        LOCTEXT("StrengthHard", "Hard only"),
        LOCTEXT("StrengthHardTip", "Held by hard references and no soft ones. Always loaded with its referencer."));

    AddStrength(EReferenceStrengthFilter::SoftOnly,
        LOCTEXT("StrengthSoft", "Soft only"),
        LOCTEXT("StrengthSoftTip", "Held ONLY by soft references - loaded on demand, nothing forces it to be present. This is where missing-in-build problems come from, and where every external actor edge lands."));

    AddStrength(EReferenceStrengthFilter::Mixed,
        LOCTEXT("StrengthMixed", "Mixed"),
        LOCTEXT("StrengthMixedTip", "Both hard and soft references point at it."));

    AddStrength(EReferenceStrengthFilter::None,
        LOCTEXT("StrengthNone", "No incoming edges"),
        LOCTEXT("StrengthNoneTip", "Neither hard nor soft. A level itself, or an asset reached only from config or source."));

    MenuBuilder.EndSection();

    MenuBuilder.BeginSection("Provenance", LOCTEXT("ProvenanceSection", "Must be reached via (all ticked)"));

    const auto AddProvenance = [this, &MenuBuilder](EAssetUsageProvenance Flag, const FText& Label, const FText& Tooltip)
    {
        MenuBuilder.AddMenuEntry(
            Label,
            Tooltip,
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([this, Flag]()
                {
                    if (EnumHasAnyFlags(RequiredProvenance, Flag))
                    {
                        EnumRemoveFlags(RequiredProvenance, Flag);
                    }
                    else
                    {
                        EnumAddFlags(RequiredProvenance, Flag);
                    }
                    RebuildFilteredRows();
                }),
                FCanExecuteAction(),
                FIsActionChecked::CreateLambda([this, Flag]() { return EnumHasAnyFlags(RequiredProvenance, Flag); })),
            NAME_None,
            EUserInterfaceActionType::ToggleButton);
    };

    AddProvenance(EAssetUsageProvenance::ExternalActor,
        LOCTEXT("ProvExternal", "External actor (OFPA)"),
        LOCTEXT("ProvExternalTip", "Reached through a One File Per Actor package - the asset is placed in a level rather than referenced by another asset."));

    AddProvenance(EAssetUsageProvenance::Sublevel,
        LOCTEXT("ProvSublevel", "Sublevel or Level Instance"),
        LOCTEXT("ProvSublevelTip", "Reached by crossing into a streaming sublevel or a Level Instance."));

    AddProvenance(EAssetUsageProvenance::EditorOnly,
        LOCTEXT("ProvEditorOnly", "Editor-only edge"),
        LOCTEXT("ProvEditorOnlyTip", "At least one incoming edge lacks the Game property, so that path will not survive a cook."));

    AddProvenance(EAssetUsageProvenance::Redirector,
        LOCTEXT("ProvRedirector", "Through a redirector"),
        LOCTEXT("ProvRedirectorTip", "The path to this asset passes through an ObjectRedirector left by a rename or move."));

    AddProvenance(EAssetUsageProvenance::ConfigFile,
        LOCTEXT("ProvConfig", "Named in Config"),
        LOCTEXT("ProvConfigTip", "A /Game path literal in Config/ names this asset. No asset references it."));

    AddProvenance(EAssetUsageProvenance::SourceFile,
        LOCTEXT("ProvSource", "Named in Source"),
        LOCTEXT("ProvSourceTip", "A /Game path literal in Source/ names this asset."));

    MenuBuilder.EndSection();

    MenuBuilder.AddSeparator();

    MenuBuilder.AddMenuEntry(
        LOCTEXT("ClearRefFilters", "Clear reference filters"),
        FText::GetEmpty(),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this]()
        {
            ReferenceStrength = EReferenceStrengthFilter::Any;
            RequiredProvenance = EAssetUsageProvenance::None;
            RebuildFilteredRows();
        })));

    return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> SAssetUsageAuditPanel::MakeLevelFilterMenu()
{
    if (!bAvailableFiltersReady)
    {
        RefreshAvailableFilters();
    }

    FMenuBuilder MenuBuilder(true, nullptr);

    if (AvailableLevels.IsEmpty())
    {
        MenuBuilder.AddWidget(
            SNew(STextBlock)
            .Text(bAvailableFiltersReady
                ? LOCTEXT("NoLevelsFound", "No levels found in the scanned paths.")
                : LOCTEXT("LevelsNotReady", "Asset Registry is still scanning; reopen this menu shortly."))
            .ColorAndOpacity(FSlateColor::UseSubduedForeground()),
            FText::GetEmpty());

        return MenuBuilder.MakeWidget();
    }

    // Choosing levels SCOPES THE NEXT SWEEP, it does not merely filter the last one. Auditing all
    // 1072 levels to then hide 1071 of them would be both slow and misleading: an asset's verdict
    // depends on which levels were considered.
    //
    // A checklist rather than a single pick, because FAssetUsageAuditRequest::LevelPackages has
    // always been an array and the tree groups by level: "compare these three locations" is a
    // question the resolver could already answer and the UI could not ask.
    MenuBuilder.AddMenuEntry(
        LOCTEXT("AllLevelsEntry", "All levels"),
        LOCTEXT("AllLevelsTip", "Audit every level in the project. Slower, and the level column then lists every level that reaches each asset."),
        FSlateIcon(),
        FUIAction(
            FExecuteAction::CreateLambda([this]()
            {
                LevelFilters.Reset();
                OnScopeChanged();
            }),
            FCanExecuteAction(),
            // Checked when nothing is ticked: an empty set means no restriction, which is the same
            // thing "All levels" says.
            FIsActionChecked::CreateLambda([this]() { return LevelFilters.IsEmpty(); })),
        NAME_None,
        EUserInterfaceActionType::RadioButton);

    MenuBuilder.AddSeparator();

    for (const FName& Level : AvailableLevels)
    {
        MenuBuilder.AddMenuEntry(
            FText::FromString(FPackageName::GetShortName(Level)),
            FText::Format(LOCTEXT("LevelEntryTip", "Include {0} in the sweep. Press Run Audit to apply."), FText::FromName(Level)),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([this, Level]()
                {
                    if (LevelFilters.Contains(Level))
                    {
                        LevelFilters.Remove(Level);
                    }
                    else
                    {
                        LevelFilters.Add(Level);
                    }
                    OnScopeChanged();
                }),
                FCanExecuteAction(),
                FIsActionChecked::CreateLambda([this, Level]() { return LevelFilters.Contains(Level); })),
            NAME_None,
            EUserInterfaceActionType::ToggleButton);
    }

    return SNew(SBox)
        .MaxDesiredHeight(600.0f)
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            [
                MenuBuilder.MakeWidget()
            ]
        ];
}

// --- Sorting ---------------------------------------------------------------------------------

EColumnSortMode::Type SAssetUsageAuditPanel::GetSortModeForColumn(FName ColumnId) const
{
    return SortColumn == ColumnId ? SortMode : EColumnSortMode::None;
}

void SAssetUsageAuditPanel::OnSortChanged(EColumnSortPriority::Type Priority, const FName& ColumnId, EColumnSortMode::Type NewMode)
{
    SortColumn = ColumnId;
    SortMode = NewMode;
    ApplySorting();

    // The tree holds its own nodes, so sorting FilteredRows underneath it changes nothing on screen
    // until the nodes are rebuilt. A plain refresh here would look like the sort was ignored.
    RebuildTree();

    if (TreeView.IsValid())
    {
        TreeView->RequestTreeRefresh();
    }
}

void SAssetUsageAuditPanel::ApplySorting()
{
    if (SortColumn.IsNone() || SortMode == EColumnSortMode::None)
    {
        return;
    }

    const bool bAscending = SortMode == EColumnSortMode::Ascending;

    FilteredRows.Sort([this, bAscending](const FAssetUsageRowPtr& A, const FAssetUsageRowPtr& B)
    {
        if (!A.IsValid() || !B.IsValid())
        {
            return false;
        }

        auto Compare = [bAscending](auto Left, auto Right)
        {
            return bAscending ? Left < Right : Right < Left;
        };

        if (SortColumn == AuditColumns::Name)
        {
            return Compare(A->AssetName.ToString(), B->AssetName.ToString());
        }
        if (SortColumn == AuditColumns::Path)
        {
            return Compare(A->PathFromProjectRoot, B->PathFromProjectRoot);
        }
        if (SortColumn == AuditColumns::Type)
        {
            return Compare(ShortTypeName(A->ClassPath), ShortTypeName(B->ClassPath));
        }
        if (SortColumn == AuditColumns::Verdict)
        {
            return Compare(static_cast<uint8>(A->Verdict), static_cast<uint8>(B->Verdict));
        }
        if (SortColumn == AuditColumns::Levels)
        {
            return Compare(A->Levels.Num(), B->Levels.Num());
        }
        if (SortColumn == AuditColumns::Hard)
        {
            return Compare(A->HardReferenceCount, B->HardReferenceCount);
        }
        if (SortColumn == AuditColumns::Soft)
        {
            return Compare(A->SoftReferenceCount, B->SoftReferenceCount);
        }

        return false;
    });
}

// --- Ticking ---------------------------------------------------------------------------------

ECheckBoxState SAssetUsageAuditPanel::GetRowCheckState(FAssetUsageRowPtr Item) const
{
    if (!Item.IsValid())
    {
        return ECheckBoxState::Unchecked;
    }

    return CheckedPackages.Contains(Item->PackageName) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SAssetUsageAuditPanel::OnRowCheckChanged(ECheckBoxState NewState, FAssetUsageRowPtr Item)
{
    if (!Item.IsValid())
    {
        return;
    }

    if (NewState == ECheckBoxState::Checked)
    {
        CheckedPackages.Add(Item->PackageName);
    }
    else
    {
        CheckedPackages.Remove(Item->PackageName);
    }
}

ECheckBoxState SAssetUsageAuditPanel::GetHeaderCheckState() const
{
    if (FilteredRows.IsEmpty())
    {
        return ECheckBoxState::Unchecked;
    }

    int32 CheckedCount = 0;
    for (const FAssetUsageRowPtr& Row : FilteredRows)
    {
        if (Row.IsValid() && CheckedPackages.Contains(Row->PackageName))
        {
            ++CheckedCount;
        }
    }

    if (CheckedCount == 0)
    {
        return ECheckBoxState::Unchecked;
    }

    return CheckedCount == FilteredRows.Num() ? ECheckBoxState::Checked : ECheckBoxState::Undetermined;
}

void SAssetUsageAuditPanel::OnHeaderCheckChanged(ECheckBoxState NewState)
{
    // Acts on the filtered view only: ticking "all" while a filter is active must not silently
    // select 45000 rows the user cannot see.
    const bool bCheck = NewState != ECheckBoxState::Unchecked;

    for (const FAssetUsageRowPtr& Row : FilteredRows)
    {
        if (!Row.IsValid())
        {
            continue;
        }

        if (bCheck)
        {
            CheckedPackages.Add(Row->PackageName);
        }
        else
        {
            CheckedPackages.Remove(Row->PackageName);
        }
    }
}

FText SAssetUsageAuditPanel::GetSelectionSummaryText() const
{
    return FText::Format(
        LOCTEXT("SelectionSummary", "Showing {0} of {1} rows  |  {2} ticked"),
        FText::AsNumber(FilteredRows.Num()),
        FText::AsNumber(AllRows.Num()),
        FText::AsNumber(CheckedPackages.Num()));
}

TArray<FName> SAssetUsageAuditPanel::ExpandWithDependencies(const TArray<FName>& Seeds, FDependencyExpansion& OutExpansion) const
{
    OutExpansion = FDependencyExpansion();
    OutExpansion.SeedCount = Seeds.Num();
    OutExpansion.TotalCount = Seeds.Num();

    if (!bIncludeDependencies || Seeds.IsEmpty())
    {
        return Seeds;
    }

    IAssetRegistry* Registry = IAssetRegistry::Get();
    if (!Registry)
    {
        // Not fatal: exporting exactly what was ticked is a worse answer, not a wrong one. Say so
        // rather than silently returning a narrower set than the checkbox promised.
        UE_LOG(LogAssetUsageAuditEditor, Warning,
            TEXT("Asset Registry unavailable; exporting only the ticked assets without their dependencies."));
        return Seeds;
    }

    const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get();

    AssetDependencyClosure::FOptions ClosureOptions;
    if (Settings)
    {
        // Same exclusions as the audit. An export must not quietly pull in content the report was
        // told to ignore.
        ClosureOptions.ExcludePackagePaths = Settings->ExcludedPackagePaths;
    }

    AssetDependencyClosure::FStats Stats;
    TArray<FName> Expanded = AssetDependencyClosure::Gather(*Registry, Seeds, ClosureOptions, Stats);

    OutExpansion.TotalCount = Expanded.Num();
    OutExpansion.Detail = Stats.Summarise();

    return Expanded;
}

bool SAssetUsageAuditPanel::HasCheckedAssets() const
{
    return CheckedPackages.Num() > 0;
}

void SAssetUsageAuditPanel::GetCheckedRows(TArray<FAssetUsageRow>& OutRows) const
{
    OutRows.Reset();

    for (const FAssetUsageRowPtr& Row : AllRows)
    {
        if (Row.IsValid() && CheckedPackages.Contains(Row->PackageName))
        {
            OutRows.Add(*Row);
        }
    }
}

// --- Export ----------------------------------------------------------------------------------

FReply SAssetUsageAuditPanel::OnExportReportClicked()
{
    const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get();
    const FString DefaultDir = Settings ? Settings->GetResolvedExportDirectory() : FPaths::ProjectSavedDir();

    FString ChosenDir;
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();

    // No picker means no destination. Falling through with an empty ChosenDir would previously
    // have handed the writer a blank path and produced no file with no explanation.
    if (!DesktopPlatform)
    {
        StatusText = LOCTEXT("NoPicker", "Could not open a folder picker (DesktopPlatform is unavailable).");
        UE_LOG(LogAssetUsageAuditEditor, Error, TEXT("Report export: DesktopPlatform module unavailable; cannot ask for a destination folder."));
        return FReply::Handled();
    }

    const void* ParentHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (!DesktopPlatform->OpenDirectoryDialog(ParentHandle, LOCTEXT("PickReportDir", "Choose a folder for the report").ToString(), DefaultDir, ChosenDir))
    {
        UE_LOG(LogAssetUsageAuditEditor, Verbose, TEXT("Report export cancelled by the user at the folder picker."));
        return FReply::Handled();
    }

    // Report what is on screen, not the raw result: the filters ARE the user's question, and a
    // report that quietly widens them would not match what they are looking at.
    FAssetUsageAuditResult Filtered = LastResult;
    Filtered.Rows.Reset();
    for (const FAssetUsageRowPtr& Row : FilteredRows)
    {
        if (Row.IsValid())
        {
            Filtered.Rows.Add(*Row);
        }
    }

    Filtered.Header.AppliedFilters.Add(FString::Printf(TEXT("UI filter: %d of %d rows shown"), FilteredRows.Num(), AllRows.Num()));

    FAssetUsageReportWriter::FOptions Options;
    Options.OutputDirectory = ChosenDir;
    Options.BaseFileName = FString::Printf(TEXT("AssetUsageReport_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

    const FAssetUsageReportWriter::FResult WriteResult = FAssetUsageReportWriter::Write(Filtered, Options);

    if (WriteResult.bSuccess)
    {
        StatusText = FText::Format(LOCTEXT("ReportWritten", "Report written: {0}"), FText::FromString(WriteResult.CsvPath));
    }
    else
    {
        StatusText = FText::Format(LOCTEXT("ReportFailed", "Report failed: {0}"), FText::FromString(WriteResult.ErrorMessage));
    }

    return FReply::Handled();
}

// The mode label, the button label and the mode menu all lived here. They are gone rather than
// left unused: the export window now owns every one of those choices, and a second place that
// still knew how to phrase them would drift out of step with it.

FReply SAssetUsageAuditPanel::OnExportFilesClicked()
{
    TArray<FAssetUsageRow> Rows;
    GetCheckedRows(Rows);

    if (Rows.IsEmpty())
    {
        return FReply::Handled();
    }

    const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get();

    // Seed the dialog from the project defaults and from whatever this user chose last time. The
    // project settings are the team's starting point; the dialog is where one person deviates from
    // it for one export, without editing a version-controlled ini to do so.
    FAssetExportRequest Request;
    Request.Mode = ExportMode;
    Request.bIncludeDependencies = bIncludeDependencies;
    Request.TargetDirectory = Settings ? Settings->GetResolvedExportDirectory() : FPaths::ProjectSavedDir();
    Request.Layout = Settings ? Settings->ExportLayout : EExportLayout::Flat;
    Request.bGroupDependenciesByType = Settings && Settings->bGroupDependenciesByType;
    Request.CollisionPolicy = (Settings && Settings->bOverwriteExistingFiles)
        ? EExportCollisionPolicy::Overwrite
        : EExportCollisionPolicy::Index;

    if (!ShowAssetExportDialog(Request, Rows.Num()))
    {
        return FReply::Handled();
    }

    // Remember the two the panel owns, so the next export opens where this one left off.
    ExportMode = Request.Mode;
    bIncludeDependencies = Request.bIncludeDependencies;
    SaveUserSettings();

    UE_LOG(LogAssetUsageAuditEditor, Log,
        TEXT("Export requested: %d ticked, mode %s, layout %s, dependencies %s, target '%s'."),
        Rows.Num(),
        Request.Mode == EFileExportMode::CopyPackages ? TEXT("copy") : TEXT("convert"),
        LexToString(Request.Layout),
        Request.bIncludeDependencies ? TEXT("on") : TEXT("off"),
        *Request.TargetDirectory);

    // Migrate is checked before the mode. It is not a way of arranging copied files - it is the
    // engine performing the whole transfer itself, so neither the copy nor the convert path applies.
    if (Request.Layout == EExportLayout::Migrate)
    {
        RunMigrateExport(Rows, Request);
    }
    else if (Request.Mode == EFileExportMode::CopyPackages)
    {
        RunCopyExport(Rows, Request);
    }
    else
    {
        RunConvertExport(Rows, Request);
    }

    return FReply::Handled();
}

// --- Deleting ----------------------------------------------------------------------------------

FReply SAssetUsageAuditPanel::OnDeleteClicked()
{
    TArray<FAssetUsageRow> Rows;
    GetCheckedRows(Rows);

    if (Rows.IsEmpty())
    {
        return FReply::Handled();
    }

    TArray<FName> Packages;
    Packages.Reserve(Rows.Num());

    for (const FAssetUsageRow& Row : Rows)
    {
        Packages.Add(Row.PackageName);
    }

    // Deliberately NOT widened with the dependency closure, unlike export. Export widens so that a
    // mesh arrives with its materials; widening a delete the same way would destroy content nobody
    // ticked, and a single shared texture would drag half the project along with it. Delete acts on
    // exactly what was ticked, and never on more.
    const FAssetDeleteOutcome Outcome = ShowAssetDeleteDialog(Packages);

    if (Outcome.bCancelled)
    {
        StatusText = LOCTEXT("DeleteCancelled", "Delete cancelled - nothing was changed.");
        return FReply::Handled();
    }

    ForgetDeletedPackages(Outcome.DeletedPackages);

    StatusText = FText::Format(
        LOCTEXT("DeleteFinished",
            "Deleted {0} of {1} ticked asset(s); {2} were refused. Reference counts elsewhere in this result are now out of date - run the audit again."),
        FText::AsNumber(Outcome.DeletedPackages.Num()),
        FText::AsNumber(Outcome.Requested),
        FText::AsNumber(Outcome.Refused));

    return FReply::Handled();
}

void SAssetUsageAuditPanel::ForgetDeletedPackages(const TArray<FName>& DeletedPackages)
{
    if (DeletedPackages.IsEmpty())
    {
        return;
    }

    const TSet<FName> Gone(DeletedPackages);

    LastResult.Rows.RemoveAll([&Gone](const FAssetUsageRow& Row)
    {
        return Gone.Contains(Row.PackageName);
    });

    AllRows.RemoveAll([&Gone](const FAssetUsageRowPtr& Row)
    {
        return Row.IsValid() && Gone.Contains(Row->PackageName);
    });

    for (FName Package : DeletedPackages)
    {
        CheckedPackages.Remove(Package);
    }

    // Removing the rows is honest but incomplete: everything that referenced them still carries the
    // reference counts it had before, and an asset that was UsedByAssetsOnly through a now-deleted
    // Blueprint has a different verdict than the one on screen. Marking the result stale says so,
    // rather than leaving figures that look freshly measured.
    bResultStale = true;

    RebuildFilteredRows();
}

void SAssetUsageAuditPanel::RunMigrateExport(const TArray<FAssetUsageRow>& Rows, const FAssetExportRequest& Request)
{
    TArray<FName> Packages;
    Packages.Reserve(Rows.Num());
    for (const FAssetUsageRow& Row : Rows)
    {
        Packages.Add(Row.PackageName);
    }

    // Checked here as well as in the dialog. Migrate validates the destination only after it has
    // been called, and reports the refusal to the Output Log alone - which is exactly how this
    // arrived as "the Export button does nothing".
    const FText DestinationProblem = ValidateMigrateDestination(Request.TargetDirectory);
    if (!DestinationProblem.IsEmpty())
    {
        StatusText = DestinationProblem;

        UE_LOG(LogAssetUsageAuditEditor, Error,
            TEXT("Migrate refused: '%s' - %s"), *Request.TargetDirectory, *DestinationProblem.ToString());

        FMessageDialog::Open(EAppMsgType::Ok, DestinationProblem);
        return;
    }

    // Only the ticked packages are handed over. Migrate walks the dependency graph itself, and
    // feeding it our own closure as well would make it re-walk from every dependency - the same
    // result, reached slowly, with the engine's report listing assets the user never chose.
    FMigrationOptions Options;
    Options.bPrompt = false;
    Options.bIgnoreDependencies = !Request.bIncludeDependencies;
    Options.AssetConflict = (Request.CollisionPolicy == EExportCollisionPolicy::Overwrite)
        ? EAssetMigrationConflict::Overwrite
        : EAssetMigrationConflict::Skip;

    // The naming policy has no equivalent here. Migrate must land each package at its original
    // package path or the references it exists to preserve would break, so "keep both under a new
    // name" is not on offer - the closest honest mapping is skip.
    if (Request.CollisionPolicy == EExportCollisionPolicy::Index)
    {
        UE_LOG(LogAssetUsageAuditEditor, Log,
            TEXT("Migrate: the Keep-both naming policy does not apply; existing assets at the destination will be skipped."));
    }

    if (Options.bIgnoreDependencies)
    {
        // Engine comment on the flag, worth repeating where someone can act on it: with dependencies
        // ignored, a level's One File Per Actor packages are not migrated either, so the level
        // arrives empty rather than merely unreferenced.
        const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
            LOCTEXT("MigrateNoDeps",
                "Migrate without dependencies?\n\n"
                "The assets will arrive with their references unresolved, and any level among them "
                "will arrive empty - Unreal skips the One File Per Actor packages when dependencies "
                "are ignored.\n\n"
                "Tick \"Include referenced assets\" unless you know the destination already has them."));

        if (Answer != EAppReturnType::Yes)
        {
            UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Migrate cancelled at the no-dependencies warning."));
            return;
        }
    }

    UE_LOG(LogAssetUsageAuditEditor, Log,
        TEXT("Migrating %d ticked package(s) to '%s'. Dependencies %s, conflicts %s."),
        Packages.Num(),
        *Request.TargetDirectory,
        Options.bIgnoreDependencies ? TEXT("ignored") : TEXT("included"),
        Options.AssetConflict == EAssetMigrationConflict::Overwrite ? TEXT("overwritten") : TEXT("skipped"));

    IAssetTools::Get().MigratePackages(Packages, Request.TargetDirectory, Options);

    // MigratePackages returns void and reports through its own notifications, so there is no count
    // to quote here. Saying "N files copied" would be an invention.
    StatusText = FText::Format(
        LOCTEXT("MigrateStarted", "Migrate handed {0} asset(s) to Unreal for transfer to {1}. Unreal reports the outcome itself - check the Output Log and any notification it raises."),
        FText::AsNumber(Packages.Num()),
        FText::FromString(Request.TargetDirectory));
}

void SAssetUsageAuditPanel::RunCopyExport(const TArray<FAssetUsageRow>& Rows, const FAssetExportRequest& Request)
{
    const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get();

    const FString& TargetDirectory = Request.TargetDirectory;

    TArray<FName> Seeds;
    Seeds.Reserve(Rows.Num());
    for (const FAssetUsageRow& Row : Rows)
    {
        Seeds.Add(Row.PackageName);
    }

    const EExportLayout Layout = Request.Layout;

    IAssetRegistry* Registry = IAssetRegistry::Get();

    // Folder-per-asset needs to know which seed pulled each dependency in, so it takes the slower
    // per-seed closure. The other layouts collapse to one list and use the cheap path.
    TArray<AssetDependencyClosure::FSeedClosure> Closures;
    FDependencyExpansion Expansion;

    if (Layout == EExportLayout::FolderPerAsset && Request.bIncludeDependencies && Registry)
    {
        AssetDependencyClosure::FOptions ClosureOptions;
        if (Settings)
        {
            ClosureOptions.ExcludePackagePaths = Settings->ExcludedPackagePaths;
        }

        AssetDependencyClosure::FStats ClosureStats;
        Closures = AssetDependencyClosure::GatherPerSeed(*Registry, Seeds, ClosureOptions, ClosureStats);

        Expansion.SeedCount = ClosureStats.SeedCount;
        Expansion.TotalCount = ClosureStats.TotalCount;
        Expansion.Detail = ClosureStats.Summarise();
    }
    else
    {
        const TArray<FName> PackageNames = ExpandWithDependencies(Seeds, Expansion);

        AssetDependencyClosure::FSeedClosure& Single = Closures.AddDefaulted_GetRef();
        Single.Packages = PackageNames;
    }

    AssetExportLayout::FOptions LayoutOptions;
    LayoutOptions.Layout = Layout;
    LayoutOptions.bGroupDependenciesByType = Request.bGroupDependenciesByType;

    AssetExportLayout::FStats LayoutStats;
    const TArray<AssetExportLayout::FGroup> LayoutGroups = Registry
        ? AssetExportLayout::BuildGroups(*Registry, Closures, LayoutOptions, LayoutStats)
        : TArray<AssetExportLayout::FGroup>();

    // Ask before writing several times more files than were ticked. Without this the folder simply
    // arrives much larger than expected, and the natural conclusion is that the tool is broken.
    //
    // Under folder-per-asset the number that matters is placements, not distinct packages: a shared
    // texture is written once per folder, so the file count can far exceed the asset count.
    if (LayoutStats.FilePlacements > Expansion.SeedCount)
    {
        const FText Detail = (Layout == EExportLayout::FolderPerAsset)
            ? FText::Format(
                LOCTEXT("CopyConfirmFolders",
                    "Write {0} files into {1} folders?\n\n"
                    "You ticked {2} assets. Each gets its own folder with the materials, textures and "
                    "other assets it references beside it, so anything shared is copied into every "
                    "folder that needs it - {3} distinct assets become {0} files."),
                FText::AsNumber(LayoutStats.FilePlacements),
                FText::AsNumber(LayoutStats.GroupCount),
                FText::AsNumber(Expansion.SeedCount),
                FText::AsNumber(LayoutStats.DistinctPackages))
            : FText::Format(
                LOCTEXT("CopyConfirmDeps",
                    "Copy {0} files?\n\n"
                    "You ticked {1}; the other {2} are materials, textures and other assets they reference. "
                    "Without those, the exported files open with missing content.\n\n"
                    "Untick \"Include dependencies\" to copy only what you ticked."),
                FText::AsNumber(LayoutStats.FilePlacements),
                FText::AsNumber(Expansion.SeedCount),
                FText::AsNumber(FMath::Max(0, LayoutStats.FilePlacements - Expansion.SeedCount)));

        if (FMessageDialog::Open(EAppMsgType::YesNo, Detail) != EAppReturnType::Yes)
        {
            UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Copy cancelled at the confirmation dialog."));
            return;
        }

        UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Dependency expansion: %s"), *Expansion.Detail);
    }

    FAssetUsageExporter::FOptions ExportOptions;
    ExportOptions.TargetDirectory = Request.TargetDirectory;
    ExportOptions.CollisionPolicy = Request.CollisionPolicy;
    // bFlatten is the exporter's mirror-the-tree switch, applied inside whatever folder a group
    // lands in. The folder-per-asset layout wants flat contents within each asset's own folder,
    // so only MirrorTree turns it off.
    ExportOptions.bFlatten = Request.Layout != EExportLayout::MirrorTree;
    ExportOptions.bRecordWrittenFiles = Request.bWriteManifest;

    UE_LOG(LogAssetUsageAuditEditor, Log,
        TEXT("Copying to '%s': layout %s, %d group(s), %d file(s) from %d distinct package(s)."),
        *TargetDirectory,
        LexToString(Layout),
        LayoutStats.GroupCount,
        LayoutStats.FilePlacements,
        LayoutStats.DistinctPackages);

    FScopedSlowTask SlowTask(static_cast<float>(LayoutStats.FilePlacements), LOCTEXT("Exporting", "Copying asset files..."));
    SlowTask.MakeDialog(true);

    // Returning false cancels; the exporter keeps whatever it has already copied.
    ExportOptions.OnProgress = [&SlowTask](int32, int32)
    {
        SlowTask.EnterProgressFrame(1.0f);
        return !SlowTask.ShouldCancel();
    };

    const FAssetUsageExporter::FResult ExportResult = FAssetUsageExporter::ExportPackageGroups(LayoutGroups, ExportOptions);

    // The manifest describes what was written, so it is written after the copy and only when the
    // copy produced something. Writing it first would leave a manifest describing files that a
    // cancelled or failed export never created.
    if (Request.bWriteManifest && Registry && ExportResult.FilesCopied > 0)
    {
        AssetExportManifest::FOptions ManifestOptions;
        ManifestOptions.TargetDirectory = Request.TargetDirectory;
        ManifestOptions.Layout = Request.Layout;
        ManifestOptions.CollisionPolicy = Request.CollisionPolicy;
        ManifestOptions.SeedPackages = TSet<FName>(Seeds);

        const AssetExportManifest::FResult ManifestResult =
            AssetExportManifest::Write(*Registry, ExportResult.WrittenFiles, ManifestOptions);

        if (!ManifestResult.bSuccess)
        {
            // Not fatal - the files are already on disk and are what the user asked for. Said out
            // loud all the same: silently skipping it would leave someone believing they can restore
            // the references later.
            UE_LOG(LogAssetUsageAuditEditor, Warning,
                TEXT("Files were exported but the dependency manifest was not written: %s"),
                *ManifestResult.ErrorMessage);
        }
    }

    StatusText = FText::Format(
        LOCTEXT("ExportDone", "Export to {0}: {1}"),
        FText::FromString(TargetDirectory),
        FText::FromString(ExportResult.Summarise()));

    if (!ExportResult.Errors.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            LOCTEXT("ExportHadFailures", "{0} file(s) could not be copied. See the Output Log for details."),
            FText::AsNumber(ExportResult.Errors.Num())));
    }
}

void SAssetUsageAuditPanel::RunConvertExport(const TArray<FAssetUsageRow>& Rows, const FAssetExportRequest& Request)
{
    const UAssetUsageAuditSettings* Settings = UAssetUsageAuditSettings::Get();

    const FString& TargetDirectory = Request.TargetDirectory;

    TArray<FName> Seeds;
    Seeds.Reserve(Rows.Num());

    // Rows carry the asset name and class already; dependencies do not, and asking the registry for
    // every package would be wasteful when most of them are right here.
    TMap<FName, const FAssetUsageRow*> RowByPackage;
    RowByPackage.Reserve(Rows.Num());

    for (const FAssetUsageRow& Row : Rows)
    {
        Seeds.Add(Row.PackageName);
        RowByPackage.Add(Row.PackageName, &Row);
    }

    // Honour the dialog's choice rather than the panel's remembered one: the user may have just
    // unticked dependencies for this export alone.
    FDependencyExpansion Expansion;
    const TArray<FName> Packages = Request.bIncludeDependencies
        ? ExpandWithDependencies(Seeds, Expansion)
        : Seeds;

    if (!Request.bIncludeDependencies)
    {
        Expansion.SeedCount = Seeds.Num();
        Expansion.TotalCount = Seeds.Num();
    }

    IAssetRegistry* Registry = IAssetRegistry::Get();

    TArray<FAssetUsageExporter::FExportItem> Items;
    Items.Reserve(Packages.Num());

    for (FName PackageName : Packages)
    {
        if (const FAssetUsageRow* const* Found = RowByPackage.Find(PackageName))
        {
            FAssetUsageExporter::FExportItem& Item = Items.AddDefaulted_GetRef();
            Item.PackageName = (*Found)->PackageName;
            Item.AssetName = (*Found)->AssetName;
            Item.ClassPath = (*Found)->ClassPath;
            continue;
        }

        if (!Registry)
        {
            continue;
        }

        // A package can hold several assets. Take them all: skipping the extras would drop content
        // silently, and the format lookup rejects anything unconvertible anyway.
        TArray<FAssetData> AssetsInPackage;
        Registry->GetAssetsByPackageName(PackageName, AssetsInPackage, /*bIncludeOnlyOnDiskAssets=*/true);

        for (const FAssetData& AssetData : AssetsInPackage)
        {
            FAssetUsageExporter::FExportItem& Item = Items.AddDefaulted_GetRef();
            Item.PackageName = AssetData.PackageName;
            Item.AssetName = AssetData.AssetName;
            Item.ClassPath = AssetData.AssetClassPath;
        }
    }

    FAssetUsageExporter::FExchangeOptions ConvertOptions;
    ConvertOptions.TargetDirectory = TargetDirectory;
    ConvertOptions.CollisionPolicy = Request.CollisionPolicy;
    ConvertOptions.bFlatten = Request.Layout != EExportLayout::MirrorTree;

    if (Settings)
    {
        ConvertOptions.FormatByClass = Settings->ExchangeFormatByClass;
    }

    // Warn before starting rather than after: conversion loads every asset, and on a few thousand
    // rows that is minutes of a blocked editor. Someone who ticked half the project and picked the
    // wrong mode should find that out here.
    const TMap<FString, FString>& EffectiveFormats = ConvertOptions.FormatByClass.Num() > 0
        ? ConvertOptions.FormatByClass
        : FAssetUsageExporter::GetDefaultFormatByClass();

    int32 Convertible = 0;
    for (const FAssetUsageExporter::FExportItem& Item : Items)
    {
        if (!FAssetUsageExporter::FindFormatForClass(Item.ClassPath, EffectiveFormats).IsEmpty())
        {
            ++Convertible;
        }
    }

    if (Convertible == 0)
    {
        StatusText = LOCTEXT("ConvertNothing", "None of the ticked assets have a configured exchange format.");
        UE_LOG(LogAssetUsageAuditEditor, Warning,
            TEXT("Conversion not started: none of the %d ticked assets map to a format. Check Project Settings > Asset Usage Audit > Exchange Format By Class."),
            Items.Num());
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("ConvertNothingDialog",
            "None of the ticked assets have a configured exchange format.\n\n"
            "Set one in Project Settings > Asset Usage Audit > Exchange Format By Class, or use the copy mode instead."));
        return;
    }

    // The count shown must be the expanded one, since that is what will actually be processed.
    // Saying "of N ticked" here after silently widening the set would be a lie of exactly the kind
    // that makes people stop trusting a number.
    const FText Origin = Expansion.AddedCount() > 0
        ? FText::Format(
            LOCTEXT("ConvertOriginExpanded", "{0} ticked plus {1} they reference"),
            FText::AsNumber(Expansion.SeedCount),
            FText::AsNumber(Expansion.AddedCount()))
        : FText::Format(LOCTEXT("ConvertOriginPlain", "{0} ticked"), FText::AsNumber(Expansion.SeedCount));

    const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo, FText::Format(
        LOCTEXT("ConvertConfirm",
            "Convert {0} of {1} assets ({2})?\n\n"
            "{3} have no configured format and will be skipped.\n\n"
            "Conversion loads every asset, so this can take several minutes and cannot be undone once files are written."),
        FText::AsNumber(Convertible),
        FText::AsNumber(Items.Num()),
        Origin,
        FText::AsNumber(Items.Num() - Convertible)));

    if (Answer != EAppReturnType::Yes)
    {
        UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Conversion cancelled at the confirmation dialog."));
        return;
    }

    UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Converting %d of %d ticked asset(s) into '%s'."), Convertible, Items.Num(), *TargetDirectory);

    FScopedSlowTask SlowTask(static_cast<float>(Items.Num()), LOCTEXT("Converting", "Converting assets..."));
    SlowTask.MakeDialog(true);

    ConvertOptions.OnProgress = [&SlowTask](int32, int32)
    {
        SlowTask.EnterProgressFrame(1.0f);
        return !SlowTask.ShouldCancel();
    };

    const FAssetUsageExporter::FExchangeResult ConvertResult = FAssetUsageExporter::ExportConvertedAssets(Items, ConvertOptions);

    StatusText = FText::Format(
        LOCTEXT("ConvertDone", "Conversion to {0}: {1}"),
        FText::FromString(TargetDirectory),
        FText::FromString(ConvertResult.Summarise()));

    if (!ConvertResult.Errors.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            LOCTEXT("ConvertHadFailures", "{0} asset(s) could not be converted. See the Output Log for details."),
            FText::AsNumber(ConvertResult.Errors.Num())));
    }
}

#undef LOCTEXT_NAMESPACE
