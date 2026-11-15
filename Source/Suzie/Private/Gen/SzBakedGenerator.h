// Generic, game agnostic: generates a game's native reflection types from baked JSON (format 1), driven by an FSzProfile.
// All game specifics live in the profile data.
#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "SzProfile.h"

struct FSzGenStats
{
	int32 Enums = 0, EnumsExisting = 0;
	int32 Structs = 0, StructsExisting = 0, StructsFailed = 0;
	int32 Classes = 0, ClassesExisting = 0, ClassesFailed = 0;
	int32 Delegates = 0, Functions = 0, Properties = 0;
	int32 Unresolved = 0;
	int32 OverrideTypes = 0, OverrideShadows = 0, OverrideSkipped = 0;
	int32 NativeStructOps = 0, PostSerializeHandlers = 0;
	double Seconds = 0.0;
};

/** What we remember per generated type, for the verify command. */
struct FSzGeneratedType
{
	TWeakObjectPtr<UStruct> Struct;
	TArray<FString> PropNames;
	TArray<FString> PropTypes;
	TArray<int32> PropDims;
	TArray<int32> PropSizes;   // game sizes (total incl. array dim)
	TArray<int32> PropOffsets; // game offsets
	int32 BakedSize = -1;   // structs only
	int32 BakedAlign = -1;  // structs only
	bool bIsStruct = false;
};

class FSzBakedGenerator
{
public:
	static bool Generate(const FSzProfile& Profile, FSzGenStats& OutStats);
	static TMap<FString, FSzGeneratedType>& GetGenerated();
};

namespace SuzieCommands { void RunGenerateFromModule(const FSzProfile& Profile); }
