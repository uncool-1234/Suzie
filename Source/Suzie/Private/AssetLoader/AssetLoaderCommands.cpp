#include "AssetLoader.h"

#if defined(UE_MODKIT_PACKAGE_FILTER)

#include "SuziePlugin.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IO/PackageId.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
	FString DescribeEngineFilterMode(Modkit::EPackageFilterMode Mode)
	{
		switch (Mode)
		{
		case Modkit::EPackageFilterMode::AllowList: return TEXT("AllowList");
		case Modkit::EPackageFilterMode::DenyList: return TEXT("DenyList");
		default: return TEXT("Off");
		}
	}

	void RunOpenCommand()
	{
		SzAssetLoader::OpenWindow();
	}

	// Suzie.AssetFilter [<package> ...]: the engine's filter state, then per package what the filter and the asset registry say.
	void RunAssetFilterCommand(const TArray<FString>& Args)
	{
		UE_LOG(LogSuzie, Display, TEXT("Asset filter: mode %s, file '%s', %d packages listed."),
			*DescribeEngineFilterMode(Modkit::GetPackageFilterMode()), *Modkit::GetPackageFilterFile(), Modkit::GetPackageFilterCount());

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		for (const FString& Package : Args)
		{
			const FName PackageName(*Package);
			const bool bHidden = Modkit::IsPackageHiddenByFilter(FPackageId::FromName(PackageName));
			TArray<FAssetData> Assets;
			AssetRegistry.GetAssetsByPackageName(PackageName, Assets);

			FString File;
			const bool bExists = FPackageName::DoesPackageExist(Package, &File);
			FString Source = TEXT("absent");
			if (bExists)
			{
				// DoesPackageExist can return the loose path a package would have even when no such file exists (IoStore packages).
				if (File.IsEmpty() || !IFileManager::Get().FileExists(*File))
				{
					Source = TEXT("IoStore");
				}
				else if (FPaths::IsUnderDirectory(File, FPaths::ProjectContentDir()))
				{
					Source = FString::Printf(TEXT("uncooked %s"), *File);
				}
				else
				{
					Source = FString::Printf(TEXT("file %s"), *File);
				}
			}

			UE_LOG(LogSuzie, Display, TEXT("%s: hidden by filter %s, live registry assets %d, DoesPackageExist %s, found %s"),
				*Package, bHidden ? TEXT("yes") : TEXT("no"), Assets.Num(), bExists ? TEXT("true") : TEXT("false"), *Source);
		}
	}

	// Suzie.AssetLoader.Closure <long package name>: hard package dependency closure of one root in the premade registry (unfiltered).
	void RunClosureCommand(const TArray<FString>& Args)
	{
		if (Args.Num() != 1)
		{
			UE_LOG(LogSuzie, Warning, TEXT("usage: Suzie.AssetLoader.Closure <long package name>"));
			return;
		}
		FAssetLoaderModel Model;
		if (Model.LoadPremadeRegistry().IsEmpty())
		{
			UE_LOG(LogSuzie, Warning, TEXT("Suzie.AssetLoader.Closure: no premade asset registry found."));
			return;
		}
		TArray<FName> Members = Model.ComputeHardDependencyClosure(TSet<FName>{ FName(*Args[0]) }).Array();
		Members.Sort([](const FName& A, const FName& B) { return A.Compare(B) < 0; });
		UE_LOG(LogSuzie, Display, TEXT("closure root=%s size=%d"), *Args[0], Members.Num());
		for (int32 Index = 0; Index < Members.Num() && Index < 20; ++Index)
		{
			UE_LOG(LogSuzie, Display, TEXT("closure member %s"), *Members[Index].ToString());
		}
	}

	FAutoConsoleCommand GAssetLoaderClosureCmd(
		TEXT("Suzie.AssetLoader.Closure"),
		TEXT("Suzie.AssetLoader.Closure <long package name>: hard package dependency closure in the premade registry"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunClosureCommand));

	FAutoConsoleCommand GAssetLoaderOpenCmd(
		TEXT("Suzie.AssetLoader.Open"),
		TEXT("Suzie.AssetLoader.Open: opens the Asset Loader window (same as its toolbar button)"),
		FConsoleCommandDelegate::CreateStatic(&RunOpenCommand));

	FAutoConsoleCommand GAssetFilterCmd(
		TEXT("Suzie.AssetFilter"),
		TEXT("Suzie.AssetFilter [<package> ...]: engine filter state; per package: hidden by filter, live registry, DoesPackageExist, uncooked or IoStore"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunAssetFilterCommand));
}

#endif
