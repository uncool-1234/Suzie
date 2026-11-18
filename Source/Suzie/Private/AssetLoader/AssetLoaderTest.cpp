#include "AssetLoader.h"

#if defined(UE_MODKIT_PACKAGE_FILTER) && WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* MiPackage = TEXT("/Game/Test/MI_A");
	const TCHAR* MaterialPackage = TEXT("/Game/Test/M_A");
	const TCHAR* TexturePackage = TEXT("/Game/Test/T_B");
	const TCHAR* UnrelatedPackage = TEXT("/Game/Test/Unrelated");
	const TCHAR* SoftPackage = TEXT("/Game/Test/Soft_S");
	const TCHAR* CycleXPackage = TEXT("/Game/Test/Cycle_X");
	const TCHAR* CycleYPackage = TEXT("/Game/Test/Cycle_Y");

	void AddTestAsset(FAssetRegistryState& State, const TCHAR* LongPackageName, const TCHAR* ClassName)
	{
		const FString Package(LongPackageName);
		State.AddAssetData(new FAssetData(
			FName(*Package),
			FName(*FPackageName::GetLongPackagePath(Package)),
			FName(*FPackageName::GetShortName(Package)),
			FTopLevelAssetPath(TEXT("/Script/Engine"), ClassName)));
	}

	void AddHardDependency(FAssetRegistryState& State, const TCHAR* Source, const TCHAR* Target)
	{
		State.AddDependencies(FAssetIdentifier(FName(Source)),
			TArray<FAssetDependency>{ FAssetDependency::PackageDependency(FName(Target), UE::AssetRegistry::EDependencyProperty::Hard) });
	}

	void AddSoftDependency(FAssetRegistryState& State, const TCHAR* Source, const TCHAR* Target)
	{
		State.AddDependencies(FAssetIdentifier(FName(Source)),
			TArray<FAssetDependency>{ FAssetDependency::PackageDependency(FName(Target), UE::AssetRegistry::EDependencyProperty::None) });
	}

	// MI_A -hard-> M_A -hard-> T_B, M_A -soft-> Soft_S, MI_A -hard-> /Script/Engine (no asset data), Cycle_X <-hard-> Cycle_Y.
	FAssetRegistryState MakeSyntheticRegistry()
	{
		FAssetRegistryState State;
		AddTestAsset(State, MiPackage, TEXT("MaterialInstanceConstant"));
		AddTestAsset(State, MaterialPackage, TEXT("Material"));
		AddTestAsset(State, TexturePackage, TEXT("Texture2D"));
		AddTestAsset(State, UnrelatedPackage, TEXT("StaticMesh"));
		AddTestAsset(State, SoftPackage, TEXT("Texture2D"));
		AddTestAsset(State, CycleXPackage, TEXT("Material"));
		AddTestAsset(State, CycleYPackage, TEXT("Material"));
		AddHardDependency(State, MiPackage, MaterialPackage);
		AddHardDependency(State, MiPackage, TEXT("/Script/Engine"));
		AddHardDependency(State, MaterialPackage, TexturePackage);
		AddSoftDependency(State, MaterialPackage, SoftPackage);
		AddHardDependency(State, CycleXPackage, CycleYPackage);
		AddHardDependency(State, CycleYPackage, CycleXPackage);
		return State;
	}

	FString TempFile(const TCHAR* Name)
	{
		return FPaths::ProjectSavedDir() / TEXT("AssetLoaderTest") / Name;
	}

	TArray<FName> Sorted(TArray<FName> Names)
	{
		Names.Sort([](const FName& A, const FName& B) { return A.Compare(B) < 0; });
		return Names;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetLoaderModelTest, "Suzie.AssetLoader.Model", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetLoaderModelTest::RunTest(const FString& Parameters)
{
	FAssetLoaderModel Model;
	Model.SetRegistry(MakeSyntheticRegistry());
	TestEqual(TEXT("package list has one entry per package with asset data"), Model.GetPackages().Num(), 7);
	TestTrue(TEXT("the script package has no entry"), Model.FindPackage(TEXT("/Script/Engine")) == nullptr);
	TestEqual(TEXT("the primary asset name is the short package name"), Model.FindPackage(FName(MiPackage))->AssetName, FName(TEXT("MI_A")));
	TestEqual(TEXT("the package path excludes the asset name"), Model.FindPackage(FName(MiPackage))->PackagePath, FName(TEXT("/Game/Test")));
	TestEqual(TEXT("the asset class is the short class name"), Model.FindPackage(FName(MiPackage))->AssetClass, FName(TEXT("MaterialInstanceConstant")));

	// Mode switches keep the selection and the dependency flag.
	Model.SetSelected(FName(MiPackage), true);
	Model.SetLoadDependencies(false);
	Model.SetMode(EAssetLoadMode::AllExceptSelected);
	Model.SetMode(EAssetLoadMode::None);
	Model.SetMode(EAssetLoadMode::All);
	Model.SetMode(EAssetLoadMode::OnlySelected);
	TestTrue(TEXT("selection survives mode switches"), Model.IsSelected(FName(MiPackage)));
	TestFalse(TEXT("dependency flag survives mode switches"), Model.GetLoadDependencies());

	// ComputeFilter per mode.
	Model.SetMode(EAssetLoadMode::All);
	TestEqual(TEXT("All writes Off"), static_cast<int32>(Model.ComputeFilter().Mode), static_cast<int32>(EAssetFilterMode::Off));
	Model.SetMode(EAssetLoadMode::None);
	FAssetFilter NoneFilter = Model.ComputeFilter();
	TestEqual(TEXT("None writes AllowList"), static_cast<int32>(NoneFilter.Mode), static_cast<int32>(EAssetFilterMode::AllowList));
	TestEqual(TEXT("None writes no packages"), NoneFilter.Packages.Num(), 0);
	Model.SetMode(EAssetLoadMode::AllExceptSelected);
	FAssetFilter DenyFilter = Model.ComputeFilter();
	TestEqual(TEXT("AllExceptSelected writes DenyList"), static_cast<int32>(DenyFilter.Mode), static_cast<int32>(EAssetFilterMode::DenyList));
	TestTrue(TEXT("AllExceptSelected writes the selection only"), DenyFilter.Packages == TArray<FName>{ FName(MiPackage) });

	Model.SetMode(EAssetLoadMode::OnlySelected);
	Model.SetLoadDependencies(false);
	FAssetFilter OnlyNoDeps = Model.ComputeFilter();
	TestTrue(TEXT("OnlySelected without dependencies writes the selection"), OnlyNoDeps.Packages == TArray<FName>{ FName(MiPackage) });

	// Closure: hard dependencies only, script packages and soft targets dropped, cycles terminate.
	Model.SetLoadDependencies(true);
	FAssetFilter OnlyWithDeps = Model.ComputeFilter();
	TestTrue(TEXT("OnlySelected with dependencies follows hard dependencies only"), OnlyWithDeps.Packages == Sorted({ FName(MaterialPackage), FName(MiPackage), FName(TexturePackage) }));
	TestEqual(TEXT("OnlySelected writes AllowList"), static_cast<int32>(OnlyWithDeps.Mode), static_cast<int32>(EAssetFilterMode::AllowList));

	Model.SetSelected(FName(MiPackage), false);
	Model.SetSelected(FName(CycleXPackage), true);
	FAssetFilter CycleFilter = Model.ComputeFilter();
	TestTrue(TEXT("a cycle of hard dependencies closes"), CycleFilter.Packages == Sorted({ FName(CycleXPackage), FName(CycleYPackage) }));

	// Apply rules.
	Model.SetSelected(FName(CycleXPackage), false);
	TestFalse(TEXT("Apply refused for OnlySelected with an empty selection"), Model.CanApply());
	Model.SetMode(EAssetLoadMode::None);
	TestTrue(TEXT("Apply allowed for None"), Model.CanApply());
	Model.SetMode(EAssetLoadMode::AllExceptSelected);
	TestTrue(TEXT("Apply allowed for AllExceptSelected with an empty selection"), Model.CanApply());

	// InitializeFromFilter.
	FAssetFilter Existing;
	Existing.Mode = EAssetFilterMode::DenyList;
	Existing.Packages = { FName(UnrelatedPackage) };
	Model.InitializeFromFilter(Existing);
	TestEqual(TEXT("DenyList starts AllExceptSelected"), static_cast<int32>(Model.GetMode()), static_cast<int32>(EAssetLoadMode::AllExceptSelected));
	TestTrue(TEXT("DenyList packages are the selection"), Model.IsSelected(FName(UnrelatedPackage)));
	Model.InitializeFromFilter(FAssetFilter());
	TestEqual(TEXT("Off starts All"), static_cast<int32>(Model.GetMode()), static_cast<int32>(EAssetLoadMode::All));

	// Filter file round trip.
	const FString FilterPath = TempFile(TEXT("AssetFilter.txt"));
	FString Error;
	TestTrue(TEXT("filter file written"), FAssetLoaderModel::WriteFilterFile(FilterPath, DenyFilter, Error));
	FAssetFilter Read;
	TestTrue(TEXT("filter file read"), FAssetLoaderModel::ReadFilterFile(FilterPath, Read, Error));
	TestEqual(TEXT("filter file round trip mode"), static_cast<int32>(Read.Mode), static_cast<int32>(DenyFilter.Mode));
	TestTrue(TEXT("filter file round trip packages"), Read.Packages == DenyFilter.Packages);
	TestTrue(TEXT("filter file has no temporary file left"), !IFileManager::Get().FileExists(*(FilterPath + TEXT(".tmp"))));

	// JSON state round trip.
	const FString StatePath = TempFile(TEXT("AssetLoader.json"));
	Model.SetMode(EAssetLoadMode::OnlySelected);
	Model.SetLoadDependencies(true);
	Model.SetSelected(FName(MaterialPackage), true);
	Model.SetSelected(FName(TexturePackage), true);
	TestTrue(TEXT("state written"), Model.SaveState(StatePath, Error));
	FAssetLoaderModel Reloaded;
	Reloaded.SetRegistry(MakeSyntheticRegistry());
	TestTrue(TEXT("state read"), Reloaded.LoadState(StatePath));
	TestEqual(TEXT("state round trip mode"), static_cast<int32>(Reloaded.GetMode()), static_cast<int32>(EAssetLoadMode::OnlySelected));
	TestTrue(TEXT("state round trip dependency flag"), Reloaded.GetLoadDependencies());
	TestTrue(TEXT("state round trip selection"), Sorted(Reloaded.GetSelected().Array()) == Sorted(Model.GetSelected().Array()));
	TestFalse(TEXT("a missing state file is not loaded"), Reloaded.LoadState(TempFile(TEXT("Missing.json"))));

	IFileManager::Get().Delete(*FilterPath);
	IFileManager::Get().Delete(*StatePath);
	return true;
}

#endif
