// Generic, game agnostic: derives the generator's input from a game's jmap (reflection dump) and the live editor reflection.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FSzJmapTypes
{
	/** enums, structs, classes, delegates: every game type the editor lacks, in creation order (record keys: see SzJmapSource.cpp). */
	TSharedPtr<FJsonObject> Types;
	/** types: editor types whose game property list differs; game_props holds the game's full list. */
	TSharedPtr<FJsonObject> EngineDiff;
	/** Generated structs that carry STRUCT_SerializeNative in the game. */
	TArray<FString> NativeSerializedStructs;

	struct FTag { FString Tag, Comment; };
	TArray<FTag> Tags;
	TArray<TPair<FString, FString>> TagRedirects; // old name, new name
	bool bHasTagSettings = false;

	FString File;
	double ParseSeconds = 0.0, DeriveSeconds = 0.0;
};

namespace SzJmap
{
	/** -SuzieJmap=<file>, else <Project>/ModkitData/ModkitLocal.ini [Local] Jmap=. Empty when neither is set. */
	FString ResolveFile();

	/** Parses the jmap once per session and compares it with the reflection of the running editor. Null when no jmap is configured or it cannot be read. */
	const FSzJmapTypes* Get();
}
