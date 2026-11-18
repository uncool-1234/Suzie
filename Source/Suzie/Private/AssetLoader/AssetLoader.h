#pragma once

#if __has_include("Misc/ModkitCore.h")
#include "Misc/ModkitCore.h"
#endif

#if defined(UE_MODKIT_PACKAGE_FILTER)

#include "CoreMinimal.h"
#include "AssetLoaderModel.h"

namespace SzAssetLoader
{
	/** Registers the toolbar button when the modkit profile is active. Called from the module startup. */
	void Startup();

	/** Unregisters the toolbar button. */
	void Shutdown();

	/** Opens the Asset Loader window as a modal window. */
	void OpenWindow();

	/** <Project>/ModkitData/AssetFilter.txt, the file Apply writes. */
	FString GetFilterFile();

	/** <Project>/ModkitData/AssetLoader.json, the window state. */
	FString GetStateFile();

	/** The filter this editor session started with, as read from the engine's filter file at startup. */
	const FAssetFilter& GetSessionFilter();

	/** True when Filter selects the same packages in the same mode as the session filter. */
	bool IsSameAsSessionFilter(const FAssetFilter& Filter);

	/** Human readable name of a filter, as the window's mode selector names it. */
	FText DescribeFilterMode(const FAssetFilter& Filter);
}

#endif
