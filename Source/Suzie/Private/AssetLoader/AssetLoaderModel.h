#pragma once

#if __has_include("Misc/ModkitCore.h")
#include "Misc/ModkitCore.h"
#endif

#if defined(UE_MODKIT_PACKAGE_FILTER)

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryState.h"

/** What the Asset Loader window selects. The names are the values of "mode" in AssetLoader.json. */
enum class EAssetLoadMode : uint8
{
	All,
	OnlySelected,
	AllExceptSelected,
	None,
};

/** The mode of AssetFilter.txt, as the engine reads it. */
enum class EAssetFilterMode : uint8
{
	Off,
	AllowList,
	DenyList,
};

/** Contents of AssetFilter.txt. Packages are long package names, sorted and unique. */
struct FAssetFilter
{
	EAssetFilterMode Mode = EAssetFilterMode::Off;
	TArray<FName> Packages;
};

/** One row of the game asset list. */
struct FAssetLoaderPackage
{
	FName PackageName;  // long package name, /Game/...
	FName PackagePath;  // long package path (the package name without the asset name)
	FName AssetName;    // primary asset name, or the short package name when there is no such asset
	FName AssetClass;   // short class name, e.g. MaterialInstanceConstant
};

/**
 * State and rules of the Asset Loader window. Plain C++, no Slate.
 * The registry held here is the premade registry without any filter, so hidden packages are listed too.
 */
class FAssetLoaderModel
{
public:
	/**
	 * Loads the premade asset registry from the first candidate that exists: <Project>/EditorClientAssetRegistry.bin,
	 * <Project>/AssetRegistry.bin, then the profile's [AssetRegistry] PakPath entries. Returns the path loaded, or empty.
	 */
	FString LoadPremadeRegistry();

	/** Replaces the registry with State and rebuilds the package list. */
	void SetRegistry(FAssetRegistryState&& State);

	bool HasRegistry() const;
	const TArray<FAssetLoaderPackage>& GetPackages() const;
	const FAssetLoaderPackage* FindPackage(FName PackageName) const;

	EAssetLoadMode GetMode() const;
	/** Switching modes never changes the selection or the dependency flag. */
	void SetMode(EAssetLoadMode NewMode);

	const TSet<FName>& GetSelected() const;
	bool IsSelected(FName PackageName) const;
	void SetSelected(FName PackageName, bool bSelected);

	bool GetLoadDependencies() const;
	void SetLoadDependencies(bool bLoad);

	/** The filter that Apply writes; see the per mode rules in the Suzie Asset Loader documentation. */
	FAssetFilter ComputeFilter() const;

	/** Apply is refused only when OnlySelected has no selection. */
	bool CanApply() const;

	/**
	 * Packages reachable from Roots, roots included. A package's dependencies are its hard package dependencies in the
	 * registry plus the imported packages of its package store entry (cooked registries carry no dependencies; the
	 * package store entry lists every package the loader needs first). Imports are read past the package filter.
	 */
	TSet<FName> ComputeHardDependencyClosure(const TSet<FName>& Roots) const;

	/** Sets the mode, selection and dependency flag from an existing filter (used when AssetLoader.json does not exist). */
	void InitializeFromFilter(const FAssetFilter& Filter);

	/** Reads AssetLoader.json. Returns false, leaving the model unchanged, when the file is missing or not valid JSON. */
	bool LoadState(const FString& File);
	bool SaveState(const FString& File, FString& OutError) const;

	/** Reads an AssetFilter.txt. A missing or unreadable file is an error; an unknown mode is treated as Off with a warning. */
	static bool ReadFilterFile(const FString& File, FAssetFilter& OutFilter, FString& OutError);
	/** Writes an AssetFilter.txt through a temporary file, so the target is never half written. */
	static bool WriteFilterFile(const FString& File, const FAssetFilter& Filter, FString& OutError);

	static FString ModeName(EAssetLoadMode InMode);
	static bool ParseModeName(const FString& Name, EAssetLoadMode& OutMode);

private:
	FAssetRegistryState Registry;
	bool bHasRegistry = false;
	TArray<FAssetLoaderPackage> Packages; // sorted by PackageName
	TMap<FName, int32> PackageIndex;      // PackageName -> index in Packages
	TMap<uint64, FName> PackageIdIndex;   // FPackageId value -> PackageName, for every package in Packages

	EAssetLoadMode Mode = EAssetLoadMode::OnlySelected;
	TSet<FName> Selected;
	bool bLoadDependencies = true;
};

#endif
