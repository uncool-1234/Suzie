// Generic, game agnostic. A profile is a directory of data that parameterises the generator.
// MIT, same license as the rest of the plugin.
#pragma once

#include "CoreMinimal.h"

struct FSzProfile
{
	FString Id;
	FString Dir;

	// data files (absolute after Load)
	FString TypesFile;       // baked types (format 1), see Tools/ and the profile README
	FString EngineDiffFile;  // recipes for engine types whose game layout differs (shadow properties)
	FString SkipFile;        // classes whose cooked exports are skipped on load
	FString TagsFile;        // gameplay tag probe description (optional)
	FString UsmapTsv;        // optional independent schema dump for the verify command

	// policy (overrides.json)
	TArray<FString> AbstractIfDerivedFrom; // generated classes deriving from one of these are marked Abstract
	TArray<FString> DropStructSupers;      // struct super paths that are never applied to generated structs
	FString RowStructBase;                 // structs.row_struct.base (e.g. the engine's table row base)
	bool bRowStructKeepSuper = false;      // structs.row_struct.mode == keep_super_guarded: generated row structs keep the super (needs the engine DataTable guard)
	// aliases.json
	TMap<FString, FString> Aliases;        // game type path -> editor type path
	bool bAliasByLeaf = false;             // opt-in: unresolved path may be found by leaf name in any /Script package

	TArray<FString> SkipClasses;
	struct FPostSerialize { FString Class; FString Handler; };
	TArray<FPostSerialize> PostSerializeHandlers;  // load/skip.json post_serialize: class -> handler name provided by the extension
	TMap<FString, FString> NativeStructOps;        // load/native_ops.json: struct path -> ops name provided by the extension
	FString ExtensionModule;                       // profile.json extension_module: game plugin module loaded before generation
	FString ProbeTag;
	FString TagsIni;                       // project relative

	struct FConflict { FString Plugin; FString DisableSwitch; };
	TArray<FConflict> ConflictingPlugins;

	/** Resolves the profile from -SuzieProfile=<name|dir> or <Project>/ModkitLocal.ini [Local] SuzieProfile=. A bare name is looked up in <plugin>/Profiles. */
	static bool Load(FSzProfile& Out, bool bQuiet = false);
};
