// Generic, game agnostic. Machine local paths of a modkit project: resolution, the setup window and ModkitData\ModkitLocal.ini.
#pragma once

#include "CoreMinimal.h"

namespace SzProjectSetup
{
	// One path the project needs: the game folder (Key GameRoot) or a file named by the profile's project.json data_files.
	struct FEntry
	{
		FString Key;                   // ini key: GameRoot, or the data file key (KeyFile, Jmap, Usmap)
		FString Label;                 // row label in the window
		FString Description;           // tooltip only
		FString Extension;             // data files: extension with dot, e.g. ".txt"
		FString Probe;                 // game folder: file relative to the folder that must exist
		FString Value;                 // as typed or resolved, forward slashes; cleaned by Clean() when used
		TArray<FString> Candidates;    // several ModkitData files match the extension and none is set
		bool bFolder = false;
		bool bRequired = true;
		bool bValid = false;
		bool bEngineRejected = false;  // KeyFile: the engine read this file at startup and did not accept its key
	};

	struct FState
	{
		FString ProfileDir;                // chosen profile folder; empty when none is chosen yet
		TArray<FString> ProfileChoices;    // every profile folder found under the Suzie plugin's Profiles
		TArray<FEntry> Entries;            // [0] is the game folder, then the profile's data files
		bool bComplete = false;            // profile chosen and every entry valid
		bool bProfileInIni = false;        // ModkitLocal.ini names a valid profile folder (the engine reads it before plugins load)
		FString Summary;                   // what is missing, for the log
	};

	// Reads ModkitLocal.ini and the project folders, validates every entry. Does not show UI.
	// ProfileOverride, when set, is used as the profile folder instead of the one in the ini.
	FState Resolve(const FString& ProfileOverride = FString());

	// Recomputes bValid of every entry and bComplete of the state.
	void Validate(FState& State);

	// Writes [Local] Profile, GameRoot and the data file keys of DataFiles (an empty value removes the key) to ModkitLocal.ini.
	// Other lines of an existing file are kept.
	bool WriteLocalIni(const FString& ProfileDir, const FString& GameRoot, const TMap<FString, FString>& DataFiles);

	// Shows the modal setup window and waits until it closes. Returns true when Save wrote the ini and the editor was asked to restart.
	bool RunWindow(FState& State);
}
