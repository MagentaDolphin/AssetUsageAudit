// MagentaDolphin 2026. Asset Usage Audit.

#include "SAssetImportDialog.h"

#include "AssetImportScanner.h"
#include "AssetImporter.h"
#include "AssetUsageAuditEditorModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "AssetImportDialog"

namespace ImportColumns
{
    static const FName Check("Check");
    static const FName Name("Name");
    static const FName Target("Target");
    static const FName Source("Source");
    static const FName Status("Status");
}

using FImportRowPtr = TSharedPtr<AssetImportScanner::FCandidate>;

namespace
{
    /** Colour the provenance so the untrustworthy rows stand out without being read. */
    FSlateColor SourceColour(AssetImportScanner::EPackageNameSource Source)
    {
        switch (Source)
        {
            case AssetImportScanner::EPackageNameSource::Manifest:
                return FSlateColor(FLinearColor(0.35f, 0.78f, 0.35f));
            case AssetImportScanner::EPackageNameSource::PackageHeader:
                return FSlateColor(FLinearColor(0.55f, 0.72f, 0.90f));
            case AssetImportScanner::EPackageNameSource::FolderStructure:
                return FSlateColor(FLinearColor(0.85f, 0.75f, 0.35f));
            case AssetImportScanner::EPackageNameSource::Unresolved:
                return FSlateColor(FLinearColor(0.90f, 0.45f, 0.40f));
        }

        return FSlateColor::UseForeground();
    }

    FText SourceTooltip(AssetImportScanner::EPackageNameSource Source)
    {
        switch (Source)
        {
            case AssetImportScanner::EPackageNameSource::Manifest:
                return LOCTEXT("SrcManifest", "From the manifest written alongside the export. Authoritative.");
            case AssetImportScanner::EPackageNameSource::PackageHeader:
                return LOCTEXT("SrcHeader", "From the package name stored inside the .uasset. Reliable when present, but the engine does not always write it.");
            case AssetImportScanner::EPackageNameSource::FolderStructure:
                return LOCTEXT("SrcFolder", "Guessed from where the file sits in the folder. Correct for an export that mirrored the content tree; a guess otherwise.");
            case AssetImportScanner::EPackageNameSource::Unresolved:
                return LOCTEXT("SrcNone", "The original path could not be recovered. Importing this file will not restore its references unless you name a destination folder below.");
        }

        return FText::GetEmpty();
    }
}

/** One scanned file. */
class SImportRowWidget : public SMultiColumnTableRow<FImportRowPtr>
{
public:
    SLATE_BEGIN_ARGS(SImportRowWidget) {}
        SLATE_ARGUMENT(FImportRowPtr, Item)
        SLATE_EVENT(FOnCheckStateChanged, OnCheckChanged)
        SLATE_ATTRIBUTE(ECheckBoxState, CheckState)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
    {
        Item = InArgs._Item;
        OnCheckChanged = InArgs._OnCheckChanged;
        CheckState = InArgs._CheckState;

        SMultiColumnTableRow<FImportRowPtr>::Construct(FSuperRowType::FArguments(), InOwnerTable);
    }

    virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
    {
        if (!Item.IsValid())
        {
            return SNullWidget::NullWidget;
        }

        const auto TextCell = [](const FString& Text, const FSlateColor& Colour = FSlateColor::UseForeground(), const FText& Tooltip = FText::GetEmpty())
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.0f, 0.0f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Text))
                    .ColorAndOpacity(Colour)
                    .ToolTipText(Tooltip.IsEmpty() ? FText::FromString(Text) : Tooltip)
                ];
        };

        if (ColumnName == ImportColumns::Check)
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

        if (ColumnName == ImportColumns::Name)
        {
            return TextCell(Item->RelativePath);
        }

        if (ColumnName == ImportColumns::Target)
        {
            return Item->TargetPackage.IsNone()
                ? TextCell(TEXT("-"), FSlateColor::UseSubduedForeground(),
                    LOCTEXT("NoTarget", "No package path could be recovered for this file."))
                : TextCell(Item->TargetPackage.ToString());
        }

        if (ColumnName == ImportColumns::Source)
        {
            return TextCell(
                AssetImportScanner::LexToString(Item->NameSource),
                SourceColour(Item->NameSource),
                SourceTooltip(Item->NameSource));
        }

        if (ColumnName == ImportColumns::Status)
        {
            TArray<FString> Notes;

            if (Item->bTargetExists)
            {
                Notes.Add(TEXT("already in project"));
            }

            if (Item->MissingDependencies > 0)
            {
                Notes.Add(FString::Printf(TEXT("%d missing dependencies"), Item->MissingDependencies));
            }

            if (Notes.IsEmpty())
            {
                return TextCell(TEXT("new"), FSlateColor(FLinearColor(0.35f, 0.78f, 0.35f)));
            }

            return TextCell(
                FString::Join(Notes, TEXT(", ")),
                FSlateColor(FLinearColor(0.85f, 0.75f, 0.35f)),
                LOCTEXT("StatusTip", "\"Already in project\" means importing would replace an existing asset - only possible with the overwrite option below. Missing dependencies import fine but open with something absent."));
        }

        return SNullWidget::NullWidget;
    }

private:
    FImportRowPtr Item;
    FOnCheckStateChanged OnCheckChanged;
    TAttribute<ECheckBoxState> CheckState;
};

/** The import window body. */
class SAssetImportDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAssetImportDialog) {}
        SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs)
    {
        ParentWindow = InArgs._ParentWindow;

        StatusText = LOCTEXT("PickFolder", "Choose a folder of exported assets, then press Scan.");

        HeaderRow = SNew(SHeaderRow)

            + SHeaderRow::Column(ImportColumns::Check)
            .DefaultLabel(FText::GetEmpty())
            .FixedWidth(28.0f)
            .HAlignHeader(HAlign_Center)
            [
                SNew(SCheckBox)
                .IsChecked(this, &SAssetImportDialog::GetHeaderCheckState)
                .OnCheckStateChanged(this, &SAssetImportDialog::OnHeaderCheckChanged)
                .ToolTipText(LOCTEXT("TickAll", "Tick or clear every importable row. Rows with no recoverable path stay untouched unless a destination folder is given."))
            ]

            + SHeaderRow::Column(ImportColumns::Name).DefaultLabel(LOCTEXT("ColFile", "File")).FillWidth(0.30f)
            + SHeaderRow::Column(ImportColumns::Target).DefaultLabel(LOCTEXT("ColTarget", "Will become")).FillWidth(0.35f)
            + SHeaderRow::Column(ImportColumns::Source).DefaultLabel(LOCTEXT("ColSource", "Path from")).FillWidth(0.15f)
            + SHeaderRow::Column(ImportColumns::Status).DefaultLabel(LOCTEXT("ColStatus", "Status")).FillWidth(0.20f);

        ChildSlot
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .Padding(10.0f)
            [
                SNew(SVerticalBox)

                // --- Source folder -------------------------------------------------------------
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 6.0f)
                [
                    SNew(SHorizontalBox)

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .VAlign(VAlign_Center)
                    [
                        SNew(STextBlock).Text(LOCTEXT("Folder", "Folder"))
                    ]

                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    .Padding(8.0f, 0.0f)
                    [
                        SNew(SEditableTextBox)
                        .Text_Lambda([this]() { return FText::FromString(SourceDirectory); })
                        .OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
                        {
                            SourceDirectory = NewText.ToString();
                        })
                        .HintText(LOCTEXT("FolderHint", "Folder containing exported .uasset files"))
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Browse", "Browse..."))
                        .OnClicked(this, &SAssetImportDialog::OnBrowse)
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Scan", "Scan"))
                        .IsEnabled_Lambda([this]() { return !SourceDirectory.IsEmpty(); })
                        .OnClicked(this, &SAssetImportDialog::OnScan)
                    ]
                ]

                + SVerticalBox::Slot().AutoHeight()[ SNew(SSeparator) ]

                // --- List ----------------------------------------------------------------------
                + SVerticalBox::Slot()
                .FillHeight(1.0f)
                .Padding(0.0f, 6.0f)
                [
                    SAssignNew(ListView, SListView<FImportRowPtr>)
                    .ListItemsSource(&Rows)
                    .OnGenerateRow(this, &SAssetImportDialog::OnGenerateRow)
                    .SelectionMode(ESelectionMode::None)
                    .HeaderRow(HeaderRow)
                ]

                // --- Options -------------------------------------------------------------------
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 6.0f)
                [
                    SNew(SHorizontalBox)

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .VAlign(VAlign_Center)
                    [
                        SNew(SCheckBox)
                        .IsChecked_Lambda([this]()
                        {
                            return bOverwriteExisting ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                        })
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
                        {
                            bOverwriteExisting = (NewState == ECheckBoxState::Checked);
                        })
                        .ToolTipText(LOCTEXT("OverwriteTip", "Replace assets that already exist at the target path. This cannot be undone and there is no copy of what was there before. Off unless you mean it."))
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("Overwrite", "Overwrite existing assets"))
                            .ColorAndOpacity_Lambda([this]()
                            {
                                return bOverwriteExisting
                                    ? FSlateColor(FLinearColor(0.90f, 0.45f, 0.40f))
                                    : FSlateColor::UseForeground();
                            })
                        ]
                    ]

                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    .VAlign(VAlign_Center)
                    .Padding(16.0f, 0.0f, 0.0f, 0.0f)
                    [
                        SNew(SEditableTextBox)
                        .Text_Lambda([this]() { return FText::FromString(UnresolvedDestination); })
                        .OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
                        {
                            UnresolvedDestination = NewText.ToString();
                        })
                        .HintText(LOCTEXT("UnresolvedHint", "Folder for files with no recoverable path, e.g. /Game/Imported"))
                        .ToolTipText(LOCTEXT("UnresolvedTip", "Files whose original package path could not be recovered are skipped unless you name a folder here. They will import, but their references will not resolve."))
                    ]
                ]

                // --- Footer --------------------------------------------------------------------
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SHorizontalBox)

                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    .VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .Text_Lambda([this]() { return StatusText; })
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(8.0f, 0.0f, 0.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text_Lambda([this]()
                        {
                            return FText::Format(LOCTEXT("ImportN", "Import {0}"), FText::AsNumber(CheckedFiles.Num()));
                        })
                        .IsEnabled_Lambda([this]() { return CheckedFiles.Num() > 0; })
                        .OnClicked(this, &SAssetImportDialog::OnImport)
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Close", "Close"))
                        .OnClicked(this, &SAssetImportDialog::OnClose)
                    ]
                ]
            ]
        ];
    }

private:
    TSharedRef<ITableRow> OnGenerateRow(FImportRowPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
    {
        return SNew(SImportRowWidget, OwnerTable)
            .Item(Item)
            .CheckState(TAttribute<ECheckBoxState>::CreateLambda([this, Item]()
            {
                return Item.IsValid() && CheckedFiles.Contains(Item->SourceFile)
                    ? ECheckBoxState::Checked
                    : ECheckBoxState::Unchecked;
            }))
            .OnCheckChanged(FOnCheckStateChanged::CreateLambda([this, Item](ECheckBoxState NewState)
            {
                if (!Item.IsValid())
                {
                    return;
                }

                if (NewState == ECheckBoxState::Checked)
                {
                    CheckedFiles.Add(Item->SourceFile);
                }
                else
                {
                    CheckedFiles.Remove(Item->SourceFile);
                }
            }));
    }

    ECheckBoxState GetHeaderCheckState() const
    {
        if (CheckedFiles.IsEmpty())
        {
            return ECheckBoxState::Unchecked;
        }

        return CheckedFiles.Num() == Rows.Num() ? ECheckBoxState::Checked : ECheckBoxState::Undetermined;
    }

    void OnHeaderCheckChanged(ECheckBoxState NewState)
    {
        CheckedFiles.Reset();

        if (NewState == ECheckBoxState::Checked)
        {
            for (const FImportRowPtr& Row : Rows)
            {
                if (Row.IsValid())
                {
                    CheckedFiles.Add(Row->SourceFile);
                }
            }
        }
    }

    FReply OnBrowse()
    {
        IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();

        if (!DesktopPlatform)
        {
            StatusText = LOCTEXT("NoPicker", "Could not open a folder picker (DesktopPlatform is unavailable).");
            UE_LOG(LogAssetUsageAuditEditor, Error, TEXT("Import dialog: DesktopPlatform unavailable."));
            return FReply::Handled();
        }

        const void* ParentHandle = ParentWindow.IsValid() && ParentWindow->GetNativeWindow().IsValid()
            ? ParentWindow->GetNativeWindow()->GetOSWindowHandle()
            : nullptr;

        FString Chosen;
        if (DesktopPlatform->OpenDirectoryDialog(ParentHandle,
                LOCTEXT("PickImportDir", "Choose a folder of exported assets").ToString(),
                SourceDirectory, Chosen))
        {
            SourceDirectory = Chosen;
        }

        return FReply::Handled();
    }

    FReply OnScan()
    {
        Rows.Reset();
        CheckedFiles.Reset();

        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        if (Registry.IsLoadingAssets())
        {
            // Without this the "already in project" column is answered against a partial registry,
            // which would invite someone to overwrite an asset the scan simply had not seen yet.
            StatusText = LOCTEXT("WaitingRegistry", "Waiting for the Asset Registry to finish scanning...");
            Registry.WaitForCompletion();
        }

        AssetImportScanner::FOptions Options;
        Options.SourceDirectory = SourceDirectory;

        AssetImportScanner::FStats Stats;
        const TArray<AssetImportScanner::FCandidate> Candidates =
            AssetImportScanner::Scan(Registry, Options, Stats);

        Rows.Reserve(Candidates.Num());
        for (const AssetImportScanner::FCandidate& Candidate : Candidates)
        {
            Rows.Add(MakeShared<AssetImportScanner::FCandidate>(Candidate));
        }

        if (ListView.IsValid())
        {
            ListView->RequestListRefresh();
        }

        StatusText = Rows.IsEmpty()
            ? LOCTEXT("NothingFound", "No .uasset or .umap files were found in that folder.")
            : FText::Format(
                LOCTEXT("ScanDone", "{0}{1}"),
                FText::FromString(Stats.Summarise()),
                Stats.bManifestFound
                    ? LOCTEXT("WithManifest", ". A manifest was found, so the original paths are exact.")
                    : LOCTEXT("NoManifest", ". No manifest here - paths come from the files themselves, so check the \"Path from\" column."));

        return FReply::Handled();
    }

    FReply OnImport()
    {
        TArray<AssetImportScanner::FCandidate> Selected;
        Selected.Reserve(CheckedFiles.Num());

        int32 WouldOverwrite = 0;

        for (const FImportRowPtr& Row : Rows)
        {
            if (!Row.IsValid() || !CheckedFiles.Contains(Row->SourceFile))
            {
                continue;
            }

            Selected.Add(*Row);

            if (Row->bTargetExists)
            {
                ++WouldOverwrite;
            }
        }

        if (Selected.IsEmpty())
        {
            return FReply::Handled();
        }

        // The one destructive path in this whole tool. Named assets, not just a count, because
        // "12 assets will be replaced" is not something anyone can check.
        if (WouldOverwrite > 0 && bOverwriteExisting)
        {
            const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo, FText::Format(
                LOCTEXT("ConfirmOverwrite",
                    "Replace {0} asset(s) that already exist in this project?\n\n"
                    "The existing files are overwritten in place. There is no undo, and no copy is kept.\n\n"
                    "Anything currently open in the editor is skipped rather than replaced."),
                FText::AsNumber(WouldOverwrite)));

            if (Answer != EAppReturnType::Yes)
            {
                UE_LOG(LogAssetUsageAuditEditor, Log, TEXT("Import cancelled at the overwrite confirmation."));
                return FReply::Handled();
            }
        }

        AssetImporter::FOptions Options;
        Options.bOverwriteExisting = bOverwriteExisting;
        Options.UnresolvedDestinationPath = UnresolvedDestination;

        FScopedSlowTask SlowTask(static_cast<float>(Selected.Num()), LOCTEXT("Importing", "Importing assets..."));
        SlowTask.MakeDialog(true);

        Options.OnProgress = [&SlowTask](int32, int32)
        {
            SlowTask.EnterProgressFrame(1.0f);
            return !SlowTask.ShouldCancel();
        };

        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        const AssetImporter::FResult Result = AssetImporter::Import(Registry, Selected, Options);

        StatusText = FText::FromString(Result.Summarise());

        if (!Result.Errors.IsEmpty())
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                LOCTEXT("ImportFailures", "{0} file(s) could not be imported. See the Output Log for details."),
                FText::AsNumber(Result.Errors.Num())));
        }

        // Re-scan so the list reflects what the project now holds: rows just imported become
        // "already in project", which is the truth and stops a second click doing it again.
        return OnScan();
    }

    FReply OnClose()
    {
        if (ParentWindow.IsValid())
        {
            ParentWindow->RequestDestroyWindow();
        }

        return FReply::Handled();
    }

    FString SourceDirectory;
    FString UnresolvedDestination;
    bool bOverwriteExisting = false;

    TArray<FImportRowPtr> Rows;

    /** Ticked rows, keyed by source file so ticks survive a re-scan. */
    TSet<FString> CheckedFiles;

    FText StatusText;

    TSharedPtr<SListView<FImportRowPtr>> ListView;
    TSharedPtr<SHeaderRow> HeaderRow;
    TSharedPtr<SWindow> ParentWindow;
};

void ShowAssetImportDialog()
{
    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(LOCTEXT("WindowTitle", "Import assets from a folder"))
        .ClientSize(FVector2D(900.0f, 600.0f))
        .SupportsMinimize(false)
        .SupportsMaximize(true);

    Window->SetContent(SNew(SAssetImportDialog).ParentWindow(Window));

    FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());
}

#undef LOCTEXT_NAMESPACE
