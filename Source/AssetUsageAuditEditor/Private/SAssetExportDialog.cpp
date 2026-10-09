// MagentaDolphin 2026. Asset Usage Audit.

#include "SAssetExportDialog.h"

#include "AssetUsageAuditEditorModule.h"
#include "AssetUsagePaths.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "IDesktopPlatform.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "AssetExportDialog"

namespace
{
    FText LayoutLabel(EExportLayout Layout)
    {
        switch (Layout)
        {
            case EExportLayout::Flat:           return LOCTEXT("LayoutFlat", "Flat - everything in one folder");
            case EExportLayout::MirrorTree:     return LOCTEXT("LayoutMirror", "Mirror the content tree");
            case EExportLayout::FolderPerAsset: return LOCTEXT("LayoutPerAsset", "One folder per asset");
            case EExportLayout::Migrate:        return LOCTEXT("LayoutMigrate", "Migrate into another Unreal project");
        }

        return FText::GetEmpty();
    }

    FText LayoutTooltip(EExportLayout Layout)
    {
        switch (Layout)
        {
            case EExportLayout::Flat:
                return LOCTEXT("LayoutFlatTip", "Every file side by side in the destination. Two assets with the same name collide, which is what the naming policy below is for.");

            case EExportLayout::MirrorTree:
                return LOCTEXT("LayoutMirrorTip", "Recreate the /Game folder structure under the destination. Collisions become almost impossible, but you get a deep tree.");

            case EExportLayout::FolderPerAsset:
                return LOCTEXT("LayoutPerAssetTip", "Each ticked asset gets its own folder with the assets it references beside it. Anything shared is copied into every folder that needs it, so the file count is higher than the number of assets.");

            case EExportLayout::Migrate:
                return LOCTEXT("LayoutMigrateTip", "Hand the transfer to Unreal's own Migrate. Choose the destination project's Content folder. This is the only option whose result opens in Unreal with references intact - the others produce files for people, not for the engine.");
        }

        return FText::GetEmpty();
    }
}

FText ValidateMigrateDestination(const FString& Directory)
{
    // The rule itself lives in Core with the other path predicates, where it can be tested without
    // an editor module. This is only the wrapper that turns it into displayable text.
    const FString Reason = AssetUsagePaths::ValidateMigrateDestination(Directory);

    return Reason.IsEmpty() ? FText::GetEmpty() : FText::FromString(Reason);
}

/**
 * The dialog body.
 *
 * Holds its own copy of the request and writes it back only when Export is pressed, so cancelling
 * genuinely changes nothing - including the persisted defaults.
 */
class SAssetExportDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAssetExportDialog) {}
        SLATE_ARGUMENT(FAssetExportRequest, InitialRequest)
        SLATE_ARGUMENT(int32, TickedCount)
        SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs)
    {
        Request = InArgs._InitialRequest;
        TickedCount = InArgs._TickedCount;
        ParentWindow = InArgs._ParentWindow;

        for (EExportLayout Layout : { EExportLayout::Flat, EExportLayout::MirrorTree, EExportLayout::FolderPerAsset, EExportLayout::Migrate })
        {
            LayoutOptions.Add(MakeShared<EExportLayout>(Layout));
        }

        ChildSlot
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .Padding(12.0f)
            [
                SNew(SVerticalBox)

                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::Format(
                        LOCTEXT("TickedHeader", "{0} asset(s) ticked."),
                        FText::AsNumber(TickedCount)))
                ]

                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(SSegmentedControl<EFileExportMode>)
                    .Value_Lambda([this]() { return Request.Mode; })
                    .OnValueChanged_Lambda([this](EFileExportMode NewMode)
                    {
                        Request.Mode = NewMode;

                        // Migrate transfers packages; there is nothing to convert. Rather than
                        // ignore one of the two settings at export time, drop back to Flat here so
                        // the window always shows what will actually happen.
                        if (NewMode == EFileExportMode::ConvertToExchangeFormat && Request.Layout == EExportLayout::Migrate)
                        {
                            Request.Layout = EExportLayout::Flat;
                        }
                    })

                    + SSegmentedControl<EFileExportMode>::Slot(EFileExportMode::CopyPackages)
                        .Text(LOCTEXT("ModeCopy", "Copy .uasset"))
                        .ToolTip(LOCTEXT("ModeCopyTip", "Byte-exact copies of the package files. Fast; loads nothing. Levels bring their One File Per Actor packages along."))

                    + SSegmentedControl<EFileExportMode>::Slot(EFileExportMode::ConvertToExchangeFormat)
                        .Text(LOCTEXT("ModeConvert", "Convert (FBX/PNG/WAV)"))
                        .ToolTip(LOCTEXT("ModeConvertTip", "Through the engine's exporters. Loads every asset, so expect minutes rather than seconds. Classes with no configured format are skipped and counted."))
                ]

                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(SSeparator)
                ]

                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    MakeOptionsGrid()
                ]

                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 12.0f, 0.0f, 0.0f)
                [
                    SNew(SHorizontalBox)

                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                        .Text_Lambda([this]() { return GetFootnoteText(); })
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(8.0f, 0.0f, 0.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Export", "Export"))
                        // Disabled rather than allowed-and-then-failed. Migrate reports a bad
                        // destination only to the Output Log, so pressing Export would look like a
                        // button that does nothing at all.
                        .IsEnabled_Lambda([this]() { return GetBlockingReason().IsEmpty(); })
                        .ToolTipText_Lambda([this]()
                        {
                            const FText Reason = GetBlockingReason();
                            return Reason.IsEmpty()
                                ? LOCTEXT("ExportTip", "Start the export. The exact file count is confirmed once more before anything is written.")
                                : Reason;
                        })
                        .OnClicked(this, &SAssetExportDialog::OnAccept)
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Cancel", "Cancel"))
                        .OnClicked(this, &SAssetExportDialog::OnCancel)
                    ]
                ]
            ]
        ];
    }

    bool WasAccepted() const
    {
        return bAccepted;
    }

    const FAssetExportRequest& GetRequest() const
    {
        return Request;
    }

private:
    TSharedRef<SWidget> MakeOptionsGrid()
    {
        TSharedRef<SGridPanel> Grid = SNew(SGridPanel).FillColumn(1, 1.0f);

        int32 Row = 0;

        // --- Destination ---------------------------------------------------------------------
        Grid->AddSlot(0, Row).Padding(0.0f, 4.0f)
        [
            SNew(STextBlock).Text(LOCTEXT("Destination", "Destination"))
        ];

        Grid->AddSlot(1, Row).Padding(8.0f, 4.0f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SNew(SEditableTextBox)
                .Text_Lambda([this]() { return FText::FromString(Request.TargetDirectory); })
                .OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
                {
                    Request.TargetDirectory = NewText.ToString();
                })
                .HintText(LOCTEXT("DestHint", "Choose a folder..."))
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("Browse", "Browse..."))
                .OnClicked(this, &SAssetExportDialog::OnBrowse)
            ]
        ];

        ++Row;

        // --- Layout --------------------------------------------------------------------------
        Grid->AddSlot(0, Row).Padding(0.0f, 4.0f)
        [
            SNew(STextBlock).Text(LOCTEXT("Layout", "Folder layout"))
        ];

        Grid->AddSlot(1, Row).Padding(8.0f, 4.0f)
        [
            SNew(SComboBox<TSharedPtr<EExportLayout>>)
            .OptionsSource(&LayoutOptions)
            .OnGenerateWidget_Lambda([](TSharedPtr<EExportLayout> Item)
            {
                return SNew(STextBlock)
                    .Text(LayoutLabel(*Item))
                    .ToolTipText(LayoutTooltip(*Item));
            })
            .OnSelectionChanged_Lambda([this](TSharedPtr<EExportLayout> Item, ESelectInfo::Type)
            {
                if (!Item.IsValid())
                {
                    return;
                }

                Request.Layout = *Item;

                // The mirror of the rule in the mode switch above: picking Migrate while Convert is
                // selected would leave two settings that contradict each other.
                if (Request.Layout == EExportLayout::Migrate)
                {
                    Request.Mode = EFileExportMode::CopyPackages;

                    // The destination carried over from the other layouts is this project's
                    // Saved/AssetUsageAudit, which can never be a valid Migrate target. Leaving it
                    // in place would present a filled-in path that is guaranteed to fail; clearing
                    // it makes the next step obvious.
                    if (!ValidateMigrateDestination(Request.TargetDirectory).IsEmpty())
                    {
                        Request.TargetDirectory.Reset();
                    }
                }
            })
            [
                SNew(STextBlock)
                .Text_Lambda([this]() { return LayoutLabel(Request.Layout); })
                .ToolTipText_Lambda([this]() { return LayoutTooltip(Request.Layout); })
            ]
        ];

        ++Row;

        // --- Group by type -------------------------------------------------------------------
        Grid->AddSlot(1, Row).Padding(8.0f, 4.0f)
        [
            SNew(SCheckBox)
            // Disabled rather than hidden: a checkbox that vanishes reads as a bug, whereas a
            // greyed one with this tooltip explains which layout to pick to use it.
            .IsEnabled_Lambda([this]() { return Request.Layout == EExportLayout::FolderPerAsset; })
            .IsChecked_Lambda([this]()
            {
                return Request.bGroupDependenciesByType ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
            {
                Request.bGroupDependenciesByType = (NewState == ECheckBoxState::Checked);
            })
            .ToolTipText(LOCTEXT("GroupByTypeTip", "Inside each asset's folder, put its dependencies into subfolders by type - Texture, Material, StaticMesh. The asset itself stays at the top of its folder. Only available with the one-folder-per-asset layout."))
            [
                SNew(STextBlock).Text(LOCTEXT("GroupByType", "Sort dependencies into type subfolders"))
            ]
        ];

        ++Row;

        // --- Dependencies --------------------------------------------------------------------
        Grid->AddSlot(1, Row).Padding(8.0f, 4.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this]()
            {
                return Request.bIncludeDependencies ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
            {
                Request.bIncludeDependencies = (NewState == ECheckBoxState::Checked);
            })
            .ToolTipText(LOCTEXT("DepsTip", "Also export the materials, textures and other assets the ticked ones reference. Without this a mesh arrives without its materials and opens grey."))
            [
                SNew(STextBlock).Text(LOCTEXT("Deps", "Include referenced assets"))
            ]
        ];

        ++Row;

        // --- Manifest ------------------------------------------------------------------------
        Grid->AddSlot(1, Row).Padding(8.0f, 4.0f)
        [
            SNew(SCheckBox)
            .IsEnabled_Lambda([this]() { return Request.Layout != EExportLayout::Migrate; })
            .IsChecked_Lambda([this]()
            {
                return Request.bWriteManifest ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
            {
                Request.bWriteManifest = (NewState == ECheckBoxState::Checked);
            })
            .ToolTipText(LOCTEXT("ManifestTip", "Write AssetUsageAudit.manifest.json beside the files: which package each file came from, and what it referenced. These layouts lose that information, and the manifest is what an importer would need to put it back. Not applicable to Migrate, which keeps the references itself."))
            [
                SNew(STextBlock).Text(LOCTEXT("Manifest", "Write dependency manifest"))
            ]
        ];

        ++Row;

        // --- Collision -----------------------------------------------------------------------
        Grid->AddSlot(0, Row).Padding(0.0f, 4.0f)
        [
            SNew(STextBlock).Text(LOCTEXT("Collision", "If a file exists"))
        ];

        Grid->AddSlot(1, Row).Padding(8.0f, 4.0f)
        [
            SNew(SSegmentedControl<EExportCollisionPolicy>)
            .Value_Lambda([this]() { return Request.CollisionPolicy; })
            .OnValueChanged_Lambda([this](EExportCollisionPolicy NewPolicy) { Request.CollisionPolicy = NewPolicy; })

            + SSegmentedControl<EExportCollisionPolicy>::Slot(EExportCollisionPolicy::Index)
                .Text(LOCTEXT("PolicyIndex", "Keep both"))
                .ToolTip(LOCTEXT("PolicyIndexTip", "Write Foo_1, Foo_2 and so on. An existing index is incremented rather than having a second one appended: Foo_7 becomes Foo_8."))

            + SSegmentedControl<EExportCollisionPolicy>::Slot(EExportCollisionPolicy::Overwrite)
                .Text(LOCTEXT("PolicyOverwrite", "Overwrite"))
                .ToolTip(LOCTEXT("PolicyOverwriteTip", "Replace the existing file. Cannot be undone."))
        ];

        return Grid;
    }

    /** Why Export is unavailable, or empty when it is available. */
    FText GetBlockingReason() const
    {
        if (Request.TargetDirectory.IsEmpty())
        {
            return LOCTEXT("NeedDir", "Choose a destination folder first.");
        }

        if (Request.Layout == EExportLayout::Migrate)
        {
            return ValidateMigrateDestination(Request.TargetDirectory);
        }

        return FText::GetEmpty();
    }

    FText GetFootnoteText() const
    {
        // A blocking reason outranks the explanatory footnote: the user needs to know why they
        // cannot proceed before they need to know how the layout behaves.
        const FText Blocking = GetBlockingReason();
        if (!Blocking.IsEmpty())
        {
            return Blocking;
        }

        if (Request.Layout == EExportLayout::Migrate)
        {
            return LOCTEXT("FootnoteMigrate",
                "Choose the destination project's Content folder. Unreal performs the transfer and reports the result itself.");
        }

        // The single most useful thing this window can say. References inside a .uasset are full
        // package paths, so a file only resolves when it sits at the same path in the destination -
        // and neither of these two layouts puts it there.
        if (Request.Layout == EExportLayout::FolderPerAsset)
        {
            return LOCTEXT("FootnotePerAsset",
                "Shared assets are copied into every folder that needs them, so more files are written than there are assets. "
                "These files are for people, not for Unreal: copied into a project they open with broken references. Use Migrate for that.");
        }

        if (Request.Layout == EExportLayout::Flat)
        {
            return LOCTEXT("FootnoteFlat",
                "These files are for people, not for Unreal: copied into a project they open with broken references. Use Migrate for that.");
        }

        if (Request.Mode == EFileExportMode::ConvertToExchangeFormat)
        {
            return LOCTEXT("FootnoteConvert",
                "Conversion loads every asset. Blueprints and materials have no exchange format and are skipped.");
        }

        return FText::GetEmpty();
    }

    FReply OnBrowse()
    {
        IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();

        if (!DesktopPlatform)
        {
            UE_LOG(LogAssetUsageAuditEditor, Error, TEXT("Export dialog: DesktopPlatform unavailable; cannot open a folder picker."));
            return FReply::Handled();
        }

        const void* ParentHandle = ParentWindow.IsValid() && ParentWindow->GetNativeWindow().IsValid()
            ? ParentWindow->GetNativeWindow()->GetOSWindowHandle()
            : nullptr;

        FString ChosenDir;
        if (DesktopPlatform->OpenDirectoryDialog(
                ParentHandle,
                LOCTEXT("PickDir", "Choose a destination folder").ToString(),
                Request.TargetDirectory,
                ChosenDir))
        {
            Request.TargetDirectory = ChosenDir;
        }

        return FReply::Handled();
    }

    FReply OnAccept()
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

    FAssetExportRequest Request;
    int32 TickedCount = 0;
    bool bAccepted = false;

    TSharedPtr<SWindow> ParentWindow;
    TArray<TSharedPtr<EExportLayout>> LayoutOptions;
};

bool ShowAssetExportDialog(FAssetExportRequest& InOutRequest, int32 TickedCount)
{
    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(LOCTEXT("WindowTitle", "Export ticked assets"))
        .SizingRule(ESizingRule::Autosized)
        .SupportsMinimize(false)
        .SupportsMaximize(false);

    TSharedRef<SAssetExportDialog> Dialog = SNew(SAssetExportDialog)
        .InitialRequest(InOutRequest)
        .TickedCount(TickedCount)
        .ParentWindow(Window);

    Window->SetContent(Dialog);

    FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());

    if (!Dialog->WasAccepted())
    {
        UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Export cancelled in the options dialog."));
        return false;
    }

    InOutRequest = Dialog->GetRequest();
    return true;
}

#undef LOCTEXT_NAMESPACE
