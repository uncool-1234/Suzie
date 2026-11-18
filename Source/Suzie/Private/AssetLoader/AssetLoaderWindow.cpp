#include "AssetLoader.h"

#if defined(UE_MODKIT_PACKAGE_FILTER)

#include "SuziePlugin.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "Interfaces/IMainFrameModule.h"
#include "InputCoreTypes.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "UnrealEdMisc.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SuzieAssetLoader"

namespace
{
	const FName NameColumn(TEXT("Name"));
	const FName PathColumn(TEXT("Path"));
	const FName TypeColumn(TEXT("Type"));
	const FName RemoveColumn(TEXT("Remove"));

	bool MatchesSearch(const FAssetLoaderPackage& Package, const FString& Filter)
	{
		if (Filter.IsEmpty())
		{
			return true;
		}
		return Package.AssetName.ToString().Contains(Filter, ESearchCase::IgnoreCase)
			|| Package.PackagePath.ToString().Contains(Filter, ESearchCase::IgnoreCase);
	}
}

class SAssetLoaderWindow;

/** A row of either list. The cells are built by the window, which owns the model. */
class SAssetLoaderRow : public SMultiColumnTableRow<FName>
{
public:
	SLATE_BEGIN_ARGS(SAssetLoaderRow) {}
		SLATE_ARGUMENT(FName, Package)
		SLATE_ARGUMENT(bool, bLeftPanel)
		SLATE_ARGUMENT(SAssetLoaderWindow*, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTableView)
	{
		Package = InArgs._Package;
		bLeftPanel = InArgs._bLeftPanel;
		Owner = InArgs._Owner;
		SMultiColumnTableRow<FName>::Construct(SMultiColumnTableRow<FName>::FArguments().Padding(FMargin(0.f, 1.f)), InOwnerTableView);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override;

private:
	FName Package;
	bool bLeftPanel = true;
	SAssetLoaderWindow* Owner = nullptr;
};

class SAssetLoaderWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAssetLoaderWindow) {}
		SLATE_ARGUMENT(TSharedPtr<FAssetLoaderModel>, Model)
		SLATE_ARGUMENT(TWeakPtr<SWindow>, OwnWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Model = InArgs._Model.ToSharedRef();
		OwnWindow = InArgs._OwnWindow;
		RebuildLeftItems();
		RebuildRightItems();

		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 12.f, 0.f, 8.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Heading", "Choose assets to load"))
				.Font(FAppStyle::Get().GetFontStyle("HeadingSmall"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.f, 0.f, 8.f, 8.f)
			[
				SNew(SSegmentedControl<EAssetLoadMode>)
				.Value(this, &SAssetLoaderWindow::GetModeValue)
				.OnValueChanged(this, &SAssetLoaderWindow::OnModeChanged)
				+ SSegmentedControl<EAssetLoadMode>::Slot(EAssetLoadMode::All)
				.Text(LOCTEXT("ModeAll", "All"))
				.ToolTip(LOCTEXT("ModeAllTip", "Load every game asset. The stock modkit behaviour."))
				+ SSegmentedControl<EAssetLoadMode>::Slot(EAssetLoadMode::OnlySelected)
				.Text(LOCTEXT("ModeOnlySelected", "Only selected"))
				.ToolTip(LOCTEXT("ModeOnlySelectedTip", "Load only the selected assets, and their hard dependencies when Load dependencies is on."))
				+ SSegmentedControl<EAssetLoadMode>::Slot(EAssetLoadMode::AllExceptSelected)
				.Text(LOCTEXT("ModeAllExceptSelected", "All except selected"))
				.ToolTip(LOCTEXT("ModeAllExceptSelectedTip", "Load every game asset except the selected ones."))
				+ SSegmentedControl<EAssetLoadMode>::Slot(EAssetLoadMode::None)
				.Text(LOCTEXT("ModeNone", "None"))
				.ToolTip(LOCTEXT("ModeNoneTip", "Load no game assets."))
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			.Padding(8.f, 4.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.Padding(0.f, 0.f, 4.f, 0.f)
				[
					BuildLeftPanel()
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.Padding(4.f, 0.f, 0.f, 0.f)
				[
					BuildRightPanel()
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.f, 4.f)
			[
				SNew(SCheckBox)
				.IsChecked(this, &SAssetLoaderWindow::GetLoadDependenciesState)
				.OnCheckStateChanged(this, &SAssetLoaderWindow::OnLoadDependenciesChanged)
				.IsEnabled(this, &SAssetLoaderWindow::IsOnlySelectedMode)
				.ToolTipText(LOCTEXT("LoadDependenciesTip", "Also load every package the selected assets depend on (hard references), so selected materials keep their parents and textures."))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LoadDependencies", "Load dependencies"))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.f, 4.f, 8.f, 8.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				[
					SNew(SSpacer)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Cancel", "Cancel"))
					.OnClicked(this, &SAssetLoaderWindow::OnCancelClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
					.Text(LOCTEXT("Apply", "Apply"))
					.IsEnabled(this, &SAssetLoaderWindow::IsApplyEnabled)
					.ToolTipText(this, &SAssetLoaderWindow::GetApplyTooltip)
					.OnClicked(this, &SAssetLoaderWindow::OnApplyClicked)
				]
			]
		];
	}

	/** The cell content of one row. Called by SAssetLoaderRow. */
	TSharedRef<SWidget> MakeCell(FName PackageName, FName ColumnName, bool bLeftPanel)
	{
		const FAssetLoaderPackage* Package = Model->FindPackage(PackageName);
		const FText AssetText = FText::FromName(Package ? Package->AssetName : PackageName);
		const FText PathText = FText::FromName(Package ? Package->PackagePath : FName());
		const FText TypeText = FText::FromName(Package ? Package->AssetClass : FName());

		if (ColumnName == NameColumn)
		{
			if (!bLeftPanel)
			{
				return SNew(STextBlock)
					.Text(AssetText)
					.Margin(FMargin(4.f, 0.f));
			}
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.f, 0.f, 6.f, 0.f)
				[
					SNew(SCheckBox)
					.IsChecked(TAttribute<ECheckBoxState>::Create([this, PackageName]()
					{
						return Model->IsSelected(PackageName) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					}))
					.OnCheckStateChanged(FOnCheckStateChanged::CreateLambda([this, PackageName](ECheckBoxState NewState)
					{
						SetPackageSelection(PackageName, NewState == ECheckBoxState::Checked);
					}))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(AssetText)
				];
		}
		if (ColumnName == PathColumn)
		{
			return SNew(STextBlock)
				.Text(PathText)
				.Margin(FMargin(4.f, 0.f));
		}
		if (ColumnName == TypeColumn)
		{
			return SNew(STextBlock)
				.Text(TypeText)
				.Margin(FMargin(4.f, 0.f));
		}
		if (ColumnName == RemoveColumn)
		{
			return SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(2.f))
				.ToolTipText(FText::Format(LOCTEXT("RemoveTip", "Remove {0}"), AssetText))
				.IsEnabled(this, &SAssetLoaderWindow::IsRemoveEnabled)
				.OnClicked(FOnClicked::CreateLambda([this, PackageName]()
				{
					SetPackageSelection(PackageName, false);
					return FReply::Handled();
				}))
				[
					SNew(SImage)
					.Image(FAppStyle::Get().GetBrush("Icons.X"))
				];
		}
		return SNullWidget::NullWidget;
	}

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
	{
		if (InKeyEvent.GetKey() == EKeys::Escape)
		{
			CloseWindow();
			return FReply::Handled();
		}
		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}

private:
	TSharedRef<SWidget> BuildLeftPanel()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 4.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("GameAssets", "Game assets"))
					.Font(FAppStyle::Get().GetFontStyle("PropertyWindow.BoldFont"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SAssetLoaderWindow::GetLeftCountText)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 4.f)
			[
				SAssignNew(LeftSearch, SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Search name or path"))
				.OnTextChanged(this, &SAssetLoaderWindow::OnLeftSearchChanged)
				.IsEnabled(this, &SAssetLoaderWindow::IsLeftEnabled)
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
					[
						SAssignNew(LeftList, SListView<FName>)
						.ListItemsSource(&LeftItems)
						.OnGenerateRow(this, &SAssetLoaderWindow::GenerateLeftRow)
						.SelectionMode(ESelectionMode::Multi)
						.IsEnabled(this, &SAssetLoaderWindow::IsLeftEnabled)
						.OnContextMenuOpening(this, &SAssetLoaderWindow::OnLeftContextMenu)
						.OnKeyDownHandler(this, &SAssetLoaderWindow::OnLeftListKeyDown)
						.HeaderRow(MakeHeader(true))
					]
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SAssetLoaderWindow::GetLeftEmptyText)
					.Visibility(this, &SAssetLoaderWindow::GetLeftEmptyVisibility)
				]
			];
	}

	TSharedRef<SWidget> BuildRightPanel()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 4.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SAssetLoaderWindow::GetRightTitle)
					.Font(FAppStyle::Get().GetFontStyle("PropertyWindow.BoldFont"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SAssetLoaderWindow::GetRightCountText)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 4.f)
			[
				SAssignNew(RightSearch, SSearchBox)
				.HintText(LOCTEXT("SearchHintRight", "Search name or path"))
				.OnTextChanged(this, &SAssetLoaderWindow::OnRightSearchChanged)
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
					[
						SAssignNew(RightList, SListView<FName>)
						.ListItemsSource(&RightItems)
						.OnGenerateRow(this, &SAssetLoaderWindow::GenerateRightRow)
						.SelectionMode(ESelectionMode::Single)
						.HeaderRow(MakeHeader(false))
					]
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SAssetLoaderWindow::GetRightEmptyText)
					.Visibility(this, &SAssetLoaderWindow::GetRightEmptyVisibility)
				]
			];
	}

	TSharedRef<SHeaderRow> MakeHeader(bool bLeftPanel)
	{
		TSharedRef<SHeaderRow> Header = SNew(SHeaderRow);
		Header->AddColumn(SHeaderRow::Column(NameColumn).DefaultLabel(LOCTEXT("ColumnName", "Name")).FillWidth(0.4f));
		Header->AddColumn(SHeaderRow::Column(PathColumn).DefaultLabel(LOCTEXT("ColumnPath", "Path")).FillWidth(0.4f));
		Header->AddColumn(SHeaderRow::Column(TypeColumn).DefaultLabel(LOCTEXT("ColumnType", "Type")).FillWidth(0.2f));
		if (!bLeftPanel)
		{
			Header->AddColumn(SHeaderRow::Column(RemoveColumn).DefaultLabel(FText::GetEmpty()).FixedWidth(28.f));
		}
		return Header;
	}

	TSharedRef<ITableRow> GenerateLeftRow(FName PackageName, const TSharedRef<STableViewBase>& OwnerTable)
	{
		return SNew(SAssetLoaderRow, OwnerTable)
			.Package(PackageName)
			.bLeftPanel(true)
			.Owner(this);
	}

	TSharedRef<ITableRow> GenerateRightRow(FName PackageName, const TSharedRef<STableViewBase>& OwnerTable)
	{
		return SNew(SAssetLoaderRow, OwnerTable)
			.Package(PackageName)
			.bLeftPanel(false)
			.Owner(this);
	}

	// Mode and enablement

	EAssetLoadMode GetModeValue() const
	{
		return Model->GetMode();
	}

	void OnModeChanged(EAssetLoadMode NewMode)
	{
		Model->SetMode(NewMode);
		RebuildRightItems();
		if (LeftList.IsValid())
		{
			LeftList->RequestListRefresh();
		}
		RightList->RequestListRefresh();
	}

	bool IsOnlySelectedMode() const
	{
		return Model->GetMode() == EAssetLoadMode::OnlySelected;
	}

	bool IsLeftEnabled() const
	{
		const EAssetLoadMode Mode = Model->GetMode();
		return Mode == EAssetLoadMode::OnlySelected || Mode == EAssetLoadMode::AllExceptSelected;
	}

	bool IsRemoveEnabled() const
	{
		return IsLeftEnabled();
	}

	ECheckBoxState GetLoadDependenciesState() const
	{
		return Model->GetLoadDependencies() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
	}

	void OnLoadDependenciesChanged(ECheckBoxState NewState)
	{
		Model->SetLoadDependencies(NewState == ECheckBoxState::Checked);
	}

	// Lists: both are projections of the model, rebuilt when the mode, a search or the selection changes

	void RebuildLeftItems()
	{
		LeftItems.Reset();
		for (const FAssetLoaderPackage& Package : Model->GetPackages())
		{
			if (MatchesSearch(Package, LeftFilter))
			{
				LeftItems.Add(Package.PackageName);
			}
		}
	}

	void RebuildRightItems()
	{
		RightItems.Reset();
		TArray<FName> Candidates;
		const EAssetLoadMode Mode = Model->GetMode();
		if (Mode == EAssetLoadMode::All)
		{
			for (const FAssetLoaderPackage& Package : Model->GetPackages())
			{
				Candidates.Add(Package.PackageName);
			}
		}
		else if (Mode == EAssetLoadMode::OnlySelected || Mode == EAssetLoadMode::AllExceptSelected)
		{
			for (const FName PackageName : Model->GetSelected())
			{
				if (Model->FindPackage(PackageName))
				{
					Candidates.Add(PackageName);
				}
			}
			Candidates.Sort([](const FName& A, const FName& B) { return A.Compare(B) < 0; });
		}
		RightBaseCount = Candidates.Num();
		for (const FName PackageName : Candidates)
		{
			const FAssetLoaderPackage* Package = Model->FindPackage(PackageName);
			if (Package && MatchesSearch(*Package, RightFilter))
			{
				RightItems.Add(PackageName);
			}
		}
	}

	void OnLeftSearchChanged(const FText& NewText)
	{
		LeftFilter = NewText.ToString();
		RebuildLeftItems();
		LeftList->RequestListRefresh();
	}

	void OnRightSearchChanged(const FText& NewText)
	{
		RightFilter = NewText.ToString();
		RebuildRightItems();
		RightList->RequestListRefresh();
	}

	void SetPackageSelection(FName PackageName, bool bSelected)
	{
		Model->SetSelected(PackageName, bSelected);
		RebuildRightItems();
		if (RightList.IsValid())
		{
			RightList->RequestListRefresh();
		}
	}

	void SetLeftSelectionChecked(bool bChecked)
	{
		if (!LeftList.IsValid())
		{
			return;
		}
		for (const FName PackageName : LeftList->GetSelectedItems())
		{
			Model->SetSelected(PackageName, bChecked);
		}
		RebuildRightItems();
		RightList->RequestListRefresh();
	}

	FReply OnLeftListKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
	{
		if (InKeyEvent.GetKey() == EKeys::SpaceBar && IsLeftEnabled() && LeftList.IsValid())
		{
			for (const FName PackageName : LeftList->GetSelectedItems())
			{
				Model->SetSelected(PackageName, !Model->IsSelected(PackageName));
			}
			RebuildRightItems();
			RightList->RequestListRefresh();
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	TSharedPtr<SWidget> OnLeftContextMenu()
	{
		FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection*/ true, /*InCommandList*/ nullptr);
		MenuBuilder.AddMenuEntry(
			LOCTEXT("CheckSelected", "Check selected"),
			LOCTEXT("CheckSelectedTip", "Check the selected assets."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SAssetLoaderWindow::SetLeftSelectionChecked, true)));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("UncheckSelected", "Uncheck selected"),
			LOCTEXT("UncheckSelectedTip", "Uncheck the selected assets."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SAssetLoaderWindow::SetLeftSelectionChecked, false)));
		return MenuBuilder.MakeWidget();
	}

	// Counts and texts

	FText GetLeftCountText() const
	{
		const int32 Total = Model->GetPackages().Num();
		const int32 Shown = LeftItems.Num();
		if (Shown != Total)
		{
			return FText::Format(LOCTEXT("CountOf", "{0} of {1} items"), FText::AsNumber(Shown), FText::AsNumber(Total));
		}
		return FText::Format(LOCTEXT("Count", "{0} items"), FText::AsNumber(Total));
	}

	FText GetRightTitle() const
	{
		return Model->GetMode() == EAssetLoadMode::AllExceptSelected
			? LOCTEXT("WillNotLoad", "Will not load")
			: LOCTEXT("WillLoad", "Will load");
	}

	FText GetRightCountText() const
	{
		if (RightItems.Num() != RightBaseCount)
		{
			return FText::Format(LOCTEXT("CountOfRight", "{0} of {1} items"), FText::AsNumber(RightItems.Num()), FText::AsNumber(RightBaseCount));
		}
		return FText::Format(LOCTEXT("CountRight", "{0} items"), FText::AsNumber(RightBaseCount));
	}

	FText GetLeftEmptyText() const
	{
		if (!LeftFilter.IsEmpty() && Model->GetPackages().Num() > 0)
		{
			return LOCTEXT("NoMatches", "No matches");
		}
		return LOCTEXT("NoAssetsFound", "No assets found");
	}

	EVisibility GetLeftEmptyVisibility() const
	{
		return LeftItems.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
	}

	FText GetRightEmptyText() const
	{
		const EAssetLoadMode Mode = Model->GetMode();
		if (Mode == EAssetLoadMode::None)
		{
			return LOCTEXT("NoGameAssetsWillLoad", "No game assets will load");
		}
		if (RightBaseCount > 0)
		{
			return LOCTEXT("NoMatchesRight", "No matches");
		}
		if (Mode == EAssetLoadMode::All)
		{
			return LOCTEXT("NoAssetsFoundRight", "No assets found");
		}
		return LOCTEXT("NoAssetsSelected", "No assets selected");
	}

	EVisibility GetRightEmptyVisibility() const
	{
		return RightItems.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
	}

	// Footer

	bool IsApplyEnabled() const
	{
		return Model->CanApply();
	}

	FText GetApplyTooltip() const
	{
		return Model->CanApply() ? FText::GetEmpty() : LOCTEXT("ApplyNeedsSelection", "Select at least one asset, or choose None.");
	}

	FReply OnCancelClicked()
	{
		CloseWindow();
		return FReply::Handled();
	}

	FReply OnApplyClicked()
	{
		if (!Model->CanApply())
		{
			return FReply::Handled();
		}
		const FAssetFilter Filter = Model->ComputeFilter();
		FString Error;
		if (!FAssetLoaderModel::WriteFilterFile(SzAssetLoader::GetFilterFile(), Filter, Error) || !Model->SaveState(SzAssetLoader::GetStateFile(), Error))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
			return FReply::Handled();
		}
		const bool bRestartNeeded = !SzAssetLoader::IsSameAsSessionFilter(Filter);
		CloseWindow();
		if (bRestartNeeded)
		{
			// Runs after the window is gone: the editor refuses to close while a modal window is open.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No,
					LOCTEXT("RestartPrompt", "The asset selection takes effect when the editor restarts. Restart now?"));
				if (Answer == EAppReturnType::Yes)
				{
					// The engine's close path asks to save dirty packages and aborts the restart on Cancel.
					FUnrealEdMisc::Get().RestartEditor(/*bWarn*/ false);
				}
				return false;
			}), 0.1f);
		}
		return FReply::Handled();
	}

	void CloseWindow()
	{
		if (OwnWindow.IsValid())
		{
			OwnWindow.Pin()->RequestDestroyWindow();
		}
	}

	TSharedPtr<FAssetLoaderModel> Model;
	TWeakPtr<SWindow> OwnWindow;
	TSharedPtr<SSearchBox> LeftSearch;
	TSharedPtr<SSearchBox> RightSearch;
	TSharedPtr<SListView<FName>> LeftList;
	TSharedPtr<SListView<FName>> RightList;
	TArray<FName> LeftItems;
	TArray<FName> RightItems;
	int32 RightBaseCount = 0;
	FString LeftFilter;
	FString RightFilter;
};

TSharedRef<SWidget> SAssetLoaderRow::GenerateWidgetForColumn(const FName& ColumnName)
{
	return Owner->MakeCell(Package, ColumnName, bLeftPanel);
}

void SzAssetLoader::OpenWindow()
{
	static TWeakPtr<SWindow> GOpenWindow;
	if (GOpenWindow.IsValid())
	{
		GOpenWindow.Pin()->BringToFront();
		return;
	}
	if (!Modkit::IsActive())
	{
		UE_LOG(LogSuzie, Warning, TEXT("Asset Loader: the modkit profile is not active; nothing to choose."));
		return;
	}
	if (!FSlateApplication::Get().CanAddModalWindow())
	{
		UE_LOG(LogSuzie, Warning, TEXT("Asset Loader: cannot open a modal window now."));
		return;
	}

	TSharedPtr<FAssetLoaderModel> Model = MakeShared<FAssetLoaderModel>();
	{
		FScopedSlowTask SlowTask(2.f, LOCTEXT("ReadingRegistry", "Asset Loader: reading the premade asset registry"));
		SlowTask.MakeDialog();
		Model->LoadPremadeRegistry();
		SlowTask.EnterProgressFrame(1.f);
		if (!Model->LoadState(GetStateFile()))
		{
			// Without AssetLoader.json the window starts in the mode of the filter file. A missing file means every asset loads.
			FAssetFilter Existing;
			FString Error;
			if (!FAssetLoaderModel::ReadFilterFile(GetFilterFile(), Existing, Error))
			{
				Existing = FAssetFilter();
			}
			Model->InitializeFromFilter(Existing);
		}
		SlowTask.EnterProgressFrame(1.f);
	}

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "Asset Loader"))
		.ClientSize(FVector2D(900.0, 700.0))
		.SizingRule(ESizingRule::UserSized)
		.SupportsMinimize(false);
	Window->SetContent(
		SNew(SAssetLoaderWindow)
		.Model(Model)
		.OwnWindow(Window));
	GOpenWindow = Window;

	TSharedPtr<SWindow> ParentWindow;
	if (IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame")))
	{
		ParentWindow = MainFrame->GetParentWindow();
	}
	FSlateApplication::Get().AddModalWindow(Window, ParentWindow);
}

#undef LOCTEXT_NAMESPACE

#endif
