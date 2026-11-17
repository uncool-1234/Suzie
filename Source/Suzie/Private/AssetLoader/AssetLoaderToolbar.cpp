#include "AssetLoader.h"

#if defined(UE_MODKIT_PACKAGE_FILTER)

#include "SuziePlugin.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "SuzieAssetLoader"

namespace
{
	// The filter this session started with. Written once by Startup, before the toolbar is registered.
	FAssetFilter GSessionFilter;
	int32 GSessionPackageCount = 0;
	FDelegateHandle GStartupHandle;
	const FName GToolMenuOwner(TEXT("SuzieAssetLoader"));
	const FName GLevelToolbarUserMenu(TEXT("LevelEditor.LevelEditorToolBar.User"));

	void RegisterToolbarButton()
	{
		FToolMenuOwnerScoped OwnerScoped(GToolMenuOwner);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(GLevelToolbarUserMenu);
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("AssetLoader"));
		const FText ToolTip = FText::Format(
			LOCTEXT("ButtonTooltip", "Choose which game assets the editor loads. Current: {0} ({1} packages listed)."),
			SzAssetLoader::DescribeFilterMode(GSessionFilter),
			FText::AsNumber(GSessionPackageCount));
		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			FName(TEXT("AssetLoader.Open")),
			FUIAction(FExecuteAction::CreateStatic(&SzAssetLoader::OpenWindow)),
			LOCTEXT("ButtonLabel", "Asset Loader"),
			ToolTip,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Package")));
	}
}

void SzAssetLoader::Startup()
{
	if (!Modkit::IsActive())
	{
		return;
	}
	GSessionPackageCount = Modkit::GetPackageFilterCount();
	GSessionFilter = FAssetFilter();
	if (Modkit::GetPackageFilterMode() != Modkit::EPackageFilterMode::Off)
	{
		FString Error;
		if (!FAssetLoaderModel::ReadFilterFile(Modkit::GetPackageFilterFile(), GSessionFilter, Error))
		{
			UE_LOG(LogSuzie, Warning, TEXT("Asset Loader: %s"), *Error);
		}
	}
	GStartupHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterToolbarButton));
}

void SzAssetLoader::Shutdown()
{
	if (GStartupHandle.IsValid())
	{
		UToolMenus::UnRegisterStartupCallback(GStartupHandle);
		GStartupHandle.Reset();
	}
	if (UToolMenus* ToolMenus = UToolMenus::TryGet())
	{
		ToolMenus->UnregisterOwnerByName(GToolMenuOwner);
	}
}

FString SzAssetLoader::GetFilterFile()
{
	return FPaths::ProjectDir() / TEXT("ModkitData/AssetFilter.txt");
}

FString SzAssetLoader::GetStateFile()
{
	return FPaths::ProjectDir() / TEXT("ModkitData/AssetLoader.json");
}

const FAssetFilter& SzAssetLoader::GetSessionFilter()
{
	return GSessionFilter;
}

bool SzAssetLoader::IsSameAsSessionFilter(const FAssetFilter& Filter)
{
	return Filter.Mode == GSessionFilter.Mode && Filter.Packages == GSessionFilter.Packages;
}

FText SzAssetLoader::DescribeFilterMode(const FAssetFilter& Filter)
{
	switch (Filter.Mode)
	{
	case EAssetFilterMode::AllowList:
		return Filter.Packages.Num() > 0 ? LOCTEXT("ModeOnlySelectedName", "Only selected") : LOCTEXT("ModeNoneName", "None");
	case EAssetFilterMode::DenyList:
		return LOCTEXT("ModeAllExceptSelectedName", "All except selected");
	default:
		return LOCTEXT("ModeAllName", "All");
	}
}

#undef LOCTEXT_NAMESPACE

#endif
