// MagentaDolphin 2026. Asset Usage Audit.

#include "SAssetDeleteDialog.h"

#include "AssetDeletionScan.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetUsageAuditEditorModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "ObjectTools.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "AssetDeleteDialog"

namespace DeleteColumns
{
    static const FName Check(TEXT("Check"));
    static const FName Name(TEXT("Name"));
    static const FName Type(TEXT("Type"));
    static const FName Path(TEXT("Path"));
    static const FName Referencers(TEXT("Referencers"));
    static const FName Status(TEXT("Status"));
}

namespace
{
    /** Red enough to read as a warning, light enough to stay legible on the dark editor theme. */
    const FLinearColor DangerColor(0.95f, 0.35f, 0.30f);
    const FLinearColor CautionColor(0.95f, 0.72f, 0.25f);
    const FLinearColor MutedColor(0.6f, 0.6f, 0.6f);
}

/** One candidate plus its tick state. */
struct FDeleteRow
{
    AssetDeletionScan::FCandidate Candidate;

    /**
     * Ticked by default when nothing objects.
     *
     * The user already ticked these rows in the panel; re-ticking them here would be busywork.
     * What the window adds is the chance to *un*tick, having now been shown the referencers.
     */
    bool bChecked = false;
};

using FDeleteRowPtr = TSharedPtr<FDeleteRow>;

class SDeleteRowWidget : public SMultiColumnTableRow<FDeleteRowPtr>
{
public:
    SLATE_BEGIN_ARGS(SDeleteRowWidget) {}
        SLATE_ARGUMENT(FDeleteRowPtr, Row)
        SLATE_EVENT(FSimpleDelegate, OnCheckChanged)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
    {
        Row = InArgs._Row;
        OnCheckChanged = InArgs._OnCheckChanged;

        SMultiColumnTableRow<FDeleteRowPtr>::Construct(FSuperRowType::FArguments(), InOwnerTable);
    }

    virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
    {
        if (!Row.IsValid())
        {
            return SNullWidget::NullWidget;
        }

        const AssetDeletionScan::FCandidate& Candidate = Row->Candidate;
        const bool bCanDelete = Candidate.CanDelete();

        if (ColumnName == DeleteColumns::Check)
        {
            return SNew(SBox)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                [
                    SNew(SCheckBox)
                    .IsEnabled(bCanDelete)
                    .IsChecked_Lambda([this]()
                    {
                        return Row->bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
                    {
                        Row->bChecked = (NewState == ECheckBoxState::Checked);
                        OnCheckChanged.ExecuteIfBound();
                    })
                ];
        }

        if (ColumnName == DeleteColumns::Name)
        {
            return SNew(STextBlock)
                .Text(FText::FromName(Candidate.AssetName))
                .ColorAndOpacity(bCanDelete ? FSlateColor::UseForeground() : FSlateColor(MutedColor))
                .ToolTipText(FText::FromName(Candidate.PackageName));
        }

        if (ColumnName == DeleteColumns::Type)
        {
            return SNew(STextBlock)
                .Text(FText::FromString(Candidate.ClassPath.GetAssetName().ToString()))
                .ColorAndOpacity(FSlateColor(MutedColor));
        }

        if (ColumnName == DeleteColumns::Path)
        {
            return SNew(STextBlock)
                .Text(FText::FromString(Candidate.PathFromProjectRoot))
                .ColorAndOpacity(FSlateColor(MutedColor))
                .ToolTipText(FText::FromString(Candidate.PathFromProjectRoot));
        }

        if (ColumnName == DeleteColumns::Referencers)
        {
            if (Candidate.OutsideReferencerCount == 0)
            {
                return SNew(STextBlock)
                    .Text(LOCTEXT("NoReferencers", "none"))
                    .ColorAndOpacity(FSlateColor(MutedColor))
                    .ToolTipText(LOCTEXT("NoReferencersTip", "Nothing outside this delete set points at it, as far as the Asset Registry can see. Config files, C++ string paths and FMOD events are outside what it can see."));
            }

            // The names matter more than the number: "3" tells you to stop, the names tell you
            // whether stopping was right.
            FString Tooltip;
            for (FName Referencer : Candidate.OutsideReferencers)
            {
                Tooltip += Referencer.ToString() + TEXT("\n");
            }

            if (Candidate.OutsideReferencerCount > Candidate.OutsideReferencers.Num())
            {
                Tooltip += FString::Printf(TEXT("... and %d more"),
                    Candidate.OutsideReferencerCount - Candidate.OutsideReferencers.Num());
            }

            const FText Label = Candidate.LevelReferencerCount > 0
                ? FText::Format(
                    LOCTEXT("ReferencersWithLevels", "{0}  ({1} on levels)"),
                    FText::AsNumber(Candidate.OutsideReferencerCount),
                    FText::AsNumber(Candidate.LevelReferencerCount))
                : FText::AsNumber(Candidate.OutsideReferencerCount);

            return SNew(STextBlock)
                .Text(Label)
                .ColorAndOpacity(FSlateColor(CautionColor))
                .ToolTipText(FText::FromString(Tooltip));
        }

        if (ColumnName == DeleteColumns::Status)
        {
            if (!bCanDelete)
            {
                return SNew(STextBlock)
                    .Text(AssetDeletionScan::DescribeRefusal(Candidate.Refusal))
                    .ColorAndOpacity(FSlateColor(MutedColor));
            }

            if (Candidate.bReadOnlyOnDisk)
            {
                return SNew(STextBlock)
                    .Text(LOCTEXT("ReadOnly", "read-only on disk"))
                    .ColorAndOpacity(FSlateColor(CautionColor))
                    .ToolTipText(LOCTEXT("ReadOnlyTip", "The file is not writable. Perforce-primary projects keep unopened files read-only, so an unopened file is read-only and the delete will fail on it. Check it out first."));
            }

            return SNew(STextBlock)
                .Text(LOCTEXT("WillDelete", "will be deleted"))
                .ColorAndOpacity(FSlateColor(DangerColor));
        }

        return SNullWidget::NullWidget;
    }

private:
    FDeleteRowPtr Row;
    FSimpleDelegate OnCheckChanged;
};

/**
 * The window body.
 *
 * Owns the scan result and the tick state; deletes nothing until Delete is pressed, and reports
 * back through the outcome struct rather than mutating anything the panel owns.
 */
class SAssetDeleteDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAssetDeleteDialog) {}
        SLATE_ARGUMENT(TArray<FName>, Packages)
        SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs)
    {
        ParentWindow = InArgs._ParentWindow;

        FAssetRegistryModule& RegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& Registry = RegistryModule.Get();

        AssetDeletionScan::FOptions ScanOptions;

        const TArray<AssetDeletionScan::FCandidate> Candidates =
            AssetDeletionScan::Scan(Registry, InArgs._Packages, ScanOptions, Stats);

        for (const AssetDeletionScan::FCandidate& Candidate : Candidates)
        {
            FDeleteRowPtr NewRow = MakeShared<FDeleteRow>();
            NewRow->Candidate = Candidate;
            NewRow->bChecked = Candidate.CanDelete();
            Rows.Add(NewRow);
        }

        UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Delete scan: %s"), *Stats.Summarise());

        BuildHeaderRow();

        ChildSlot
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .Padding(12.0f)
            [
                SNew(SVerticalBox)

                // --- The headline ------------------------------------------------------------
                //
                // Stated as a consequence rather than as a question. "Delete 12 assets?" invites a
                // reflex click; naming what disappears does not.
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 6.0f)
                [
                    SNew(STextBlock)
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
                    .ColorAndOpacity(FSlateColor(DangerColor))
                    .Text(FText::Format(
                        LOCTEXT("Headline", "{0} asset file(s) will be deleted from this project."),
                        FText::AsNumber(Stats.Deletable)))
                ]

                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text(LOCTEXT("Explanation",
                        "This removes the files from disk. It is not an export and there is no undo - what protects you is source control, so make sure this content is submitted or checked out before continuing.\n\n"
                        "Bear in mind what the audit cannot see: assets named from C++ or Blueprint strings, rows inside DataTables, and FMOD events are invisible to the Asset Registry. An asset with no referencers here is not proof that nothing uses it."))
                ]

                // --- Reference warning -------------------------------------------------------
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(SBorder)
                    .BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
                    .Padding(8.0f)
                    .Visibility_Lambda([this]()
                    {
                        return Stats.WithOutsideReferencers > 0 ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .ColorAndOpacity(FSlateColor(CautionColor))
                        .Text(FText::Format(
                            LOCTEXT("ReferencerWarning",
                                "{0} of them are still referenced by something outside this selection. Deleting those leaves the referencing assets pointing at nothing.\n"
                                "Unreal's own Delete Assets window opens next and lists every reference, where you can replace them with another asset or force the delete."),
                            FText::AsNumber(Stats.WithOutsideReferencers)))
                    ]
                ]

                // --- Refusals ----------------------------------------------------------------
                //
                // Shown, not hidden. The user ticked those rows; a delete that quietly acts on
                // fewer assets than were ticked is how trust in a tool is lost.
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .ColorAndOpacity(FSlateColor(MutedColor))
                    .Visibility_Lambda([this]()
                    {
                        return (Stats.Requested - Stats.Deletable) > 0 ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    .Text_Lambda([this]() { return GetRefusalText(); })
                ]

                + SVerticalBox::Slot()
                .FillHeight(1.0f)
                [
                    SNew(SBox)
                    .MinDesiredHeight(240.0f)
                    [
                        SAssignNew(ListView, SListView<FDeleteRowPtr>)
                        .ListItemsSource(&Rows)
                        .SelectionMode(ESelectionMode::None)
                        .HeaderRow(HeaderRow)
                        .OnGenerateRow_Lambda([this](FDeleteRowPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
                        {
                            return SNew(SDeleteRowWidget, OwnerTable)
                                .Row(Item)
                                .OnCheckChanged(FSimpleDelegate::CreateLambda([this]() { Invalidate(EInvalidateWidgetReason::Paint); }));
                        })
                    ]
                ]

                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 8.0f)
                [
                    SNew(SSeparator)
                ]

                // --- Acknowledgement ---------------------------------------------------------
                //
                // A deliberate second action. The Delete button stays disabled until this is
                // ticked, which costs one click and removes the "I meant to press Cancel" class
                // of accident entirely.
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda([this]() { return bAcknowledged ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { bAcknowledged = (NewState == ECheckBoxState::Checked); })
                    [
                        SNew(STextBlock)
                        .Margin(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
                        .Text(LOCTEXT("Acknowledge", "I understand these files will be removed from the project."))
                    ]
                ]

                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SHorizontalBox)

                    + SHorizontalBox::Slot()
                    .VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .ColorAndOpacity(FSlateColor(MutedColor))
                        .Text_Lambda([this]()
                        {
                            return FText::Format(
                                LOCTEXT("Footer", "{0} ticked of {1} that can be deleted."),
                                FText::AsNumber(CountChecked()),
                                FText::AsNumber(Stats.Deletable));
                        })
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(4.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Cancel", "Cancel"))
                        .OnClicked(this, &SAssetDeleteDialog::OnCancel)
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(4.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text_Lambda([this]()
                        {
                            return FText::Format(
                                LOCTEXT("DeleteButton", "Delete {0} asset(s)"),
                                FText::AsNumber(CountChecked()));
                        })
                        .ToolTipText(LOCTEXT("DeleteButtonTip", "Hand the ticked assets to Unreal's delete, which will show its own reference window before anything is removed."))
                        .IsEnabled_Lambda([this]() { return bAcknowledged && CountChecked() > 0; })
                        .OnClicked(this, &SAssetDeleteDialog::OnDelete)
                    ]
                ]
            ]
        ];
    }

    bool WasAccepted() const
    {
        return bAccepted;
    }

    /** Packages the user confirmed, in the order they were listed. */
    TArray<FName> GetConfirmedPackages() const
    {
        TArray<FName> Result;

        for (const FDeleteRowPtr& Row : Rows)
        {
            if (Row->bChecked && Row->Candidate.CanDelete())
            {
                Result.Add(Row->Candidate.PackageName);
            }
        }

        return Result;
    }

    const AssetDeletionScan::FStats& GetStats() const
    {
        return Stats;
    }

private:
    void BuildHeaderRow()
    {
        HeaderRow = SNew(SHeaderRow)

            + SHeaderRow::Column(DeleteColumns::Check)
              .DefaultLabel(FText::GetEmpty())
              .FixedWidth(28.0f)
              [
                  SNew(SBox)
                  .HAlign(HAlign_Center)
                  .VAlign(VAlign_Center)
                  [
                      SNew(SCheckBox)
                      .IsChecked_Lambda([this]() { return GetHeaderCheckState(); })
                      .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { SetAllChecked(NewState == ECheckBoxState::Checked); })
                  ]
              ]

            + SHeaderRow::Column(DeleteColumns::Name)
              .DefaultLabel(LOCTEXT("ColName", "Asset"))
              .FillWidth(0.24f)

            + SHeaderRow::Column(DeleteColumns::Type)
              .DefaultLabel(LOCTEXT("ColType", "Type"))
              .FillWidth(0.13f)

            + SHeaderRow::Column(DeleteColumns::Path)
              .DefaultLabel(LOCTEXT("ColPath", "Path"))
              .FillWidth(0.33f)

            + SHeaderRow::Column(DeleteColumns::Referencers)
              .DefaultLabel(LOCTEXT("ColReferencers", "Referenced by"))
              .FillWidth(0.15f)

            + SHeaderRow::Column(DeleteColumns::Status)
              .DefaultLabel(LOCTEXT("ColStatus", "Status"))
              .FillWidth(0.15f);
    }

    ECheckBoxState GetHeaderCheckState() const
    {
        const int32 Checked = CountChecked();

        if (Checked == 0)
        {
            return ECheckBoxState::Unchecked;
        }

        return Checked == Stats.Deletable ? ECheckBoxState::Checked : ECheckBoxState::Undetermined;
    }

    void SetAllChecked(bool bNewChecked)
    {
        for (const FDeleteRowPtr& Row : Rows)
        {
            if (Row->Candidate.CanDelete())
            {
                Row->bChecked = bNewChecked;
            }
        }

        if (ListView.IsValid())
        {
            ListView->RequestListRefresh();
        }
    }

    int32 CountChecked() const
    {
        int32 Count = 0;

        for (const FDeleteRowPtr& Row : Rows)
        {
            if (Row->bChecked && Row->Candidate.CanDelete())
            {
                ++Count;
            }
        }

        return Count;
    }

    FText GetRefusalText() const
    {
        TArray<FString> Parts;

        if (Stats.RefusedLevels > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d level(s)"), Stats.RefusedLevels));
        }

        if (Stats.RefusedExternal > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d actor package(s)"), Stats.RefusedExternal));
        }

        if (Stats.RefusedNotProjectContent > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d outside project content"), Stats.RefusedNotProjectContent));
        }

        if (Stats.RefusedMissing > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d already gone"), Stats.RefusedMissing));
        }

        return FText::Format(
            LOCTEXT("RefusalSummary", "Kept out of the delete: {0}. They are listed below with a reason, and cannot be ticked."),
            FText::FromString(FString::Join(Parts, TEXT(", "))));
    }

    FReply OnDelete()
    {
        bAccepted = true;
        CloseWindow();
        return FReply::Handled();
    }

    FReply OnCancel()
    {
        bAccepted = false;
        CloseWindow();
        return FReply::Handled();
    }

    void CloseWindow()
    {
        if (ParentWindow.IsValid())
        {
            ParentWindow->RequestDestroyWindow();
        }
    }

    TArray<FDeleteRowPtr> Rows;
    AssetDeletionScan::FStats Stats;

    bool bAcknowledged = false;
    bool bAccepted = false;

    TSharedPtr<SWindow> ParentWindow;
    TSharedPtr<SListView<FDeleteRowPtr>> ListView;
    TSharedPtr<SHeaderRow> HeaderRow;
};

FAssetDeleteOutcome ShowAssetDeleteDialog(const TArray<FName>& Packages)
{
    FAssetDeleteOutcome Outcome;
    Outcome.Requested = Packages.Num();

    FAssetRegistryModule& RegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& Registry = RegistryModule.Get();

    // The engine refuses to delete while the registry is still discovering, and does so with a
    // transient notification that is easy to miss. Better to wait here than to have the delete
    // quietly do nothing.
    if (Registry.IsLoadingAssets())
    {
        Registry.WaitForCompletion();
    }

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(LOCTEXT("WindowTitle", "Delete ticked assets"))
        .ClientSize(FVector2D(940.0f, 620.0f))
        .SupportsMinimize(false)
        .SupportsMaximize(false);

    TSharedRef<SAssetDeleteDialog> Dialog = SNew(SAssetDeleteDialog)
        .Packages(Packages)
        .ParentWindow(Window);

    Window->SetContent(Dialog);

    FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());

    Outcome.Refused = Dialog->GetStats().Requested - Dialog->GetStats().Deletable;

    if (!Dialog->WasAccepted())
    {
        UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Delete cancelled; nothing was touched."));
        return Outcome;
    }

    const TArray<FName> Confirmed = Dialog->GetConfirmedPackages();
    Outcome.bCancelled = false;
    Outcome.Attempted = Confirmed.Num();

    if (Confirmed.Num() == 0)
    {
        return Outcome;
    }

    // Resolve to FAssetData at the last moment. The scan ran before the window opened and the
    // user may have spent a while reading it; a stale FAssetData would ask the engine to delete
    // something that has already moved.
    TArray<FAssetData> ToDelete;

    for (FName Package : Confirmed)
    {
        TArray<FAssetData> Assets;
        Registry.GetAssetsByPackageName(Package, Assets, /*bIncludeOnlyOnDiskAssets=*/true);
        ToDelete.Append(Assets);
    }

    if (ToDelete.Num() == 0)
    {
        UE_LOG(LogAssetUsageAuditEditor, Warning,
            TEXT("Nothing was deleted: none of the %d confirmed packages still hold an asset."), Confirmed.Num());
        return Outcome;
    }

    // The engine's own delete, with its confirmation left on. That window is where referencers are
    // listed and where Force Delete and Replace References live - the behaviour the request asked
    // to match, obtained by using it rather than by imitating it.
    const int32 ObjectsDeleted = ObjectTools::DeleteAssets(ToDelete, /*bShowConfirmation=*/true);

    // Count packages, not objects: the engine returns deleted UObjects, and asking the registry
    // what actually disappeared is the only figure that matches the Content Browser.
    for (FName Package : Confirmed)
    {
        TArray<FAssetData> Remaining;
        Registry.GetAssetsByPackageName(Package, Remaining, /*bIncludeOnlyOnDiskAssets=*/true);

        if (Remaining.Num() == 0)
        {
            Outcome.DeletedPackages.Add(Package);
        }
    }

    UE_LOG(LogAssetUsageAuditEditor, Display,
        TEXT("Delete finished: %d package(s) gone of %d confirmed (%d object(s) reported by the engine)."),
        Outcome.DeletedPackages.Num(), Confirmed.Num(), ObjectsDeleted);

    return Outcome;
}

#undef LOCTEXT_NAMESPACE
