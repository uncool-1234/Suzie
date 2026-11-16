// Game modkit: console command that dumps the editor's reflected struct/class property lists so they can be diffed against the game's usmap.
#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

#if WITH_EDITOR
static void GameDumpSchemas(const TArray<FString>& Args)
{
	const FString OutFile = Args.Num() > 0 ? Args[0] : FString(TEXT("GameEditorSchemas.tsv"));
	FString Out;
	for (TObjectIterator<UStruct> It; It; ++It)
	{
		UStruct* Struct = *It;
		const UPackage* Package = Struct->GetOutermost();
		if (!Package || !Package->GetName().StartsWith(TEXT("/Script/")))
		{
			continue;
		}
		const UStruct* Super = Struct->GetSuperStruct();
		Out += FString::Printf(TEXT("S\t%s\t%s\t%s\t%d\n"), *Struct->GetName(), Super ? *Super->GetName() : TEXT(""), *Package->GetName(), Struct->GetStructureSize());
		int32 Index = 0;
		for (TFieldIterator<FProperty> PropIt(Struct, EFieldIterationFlags::None); PropIt; ++PropIt)
		{
			const FProperty* P = *PropIt;
			Out += FString::Printf(TEXT("P\t%s\t%d\t%s\t%s\t%d\t%d\t%d\n"), *Struct->GetName(), Index, *P->GetName(), *P->GetClass()->GetName(),
				P->IsEditorOnlyProperty() ? 1 : 0, P->HasAnyPropertyFlags(CPF_Transient) ? 1 : 0, P->ArrayDim);
			Index += P->ArrayDim;
		}
	}
	FFileHelper::SaveStringToFile(Out, *OutFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("Game.DumpSchemas wrote %s"), *OutFile);
}

static FAutoConsoleCommand GGameDumpSchemasCmd(TEXT("Game.DumpSchemas"), TEXT("Dump reflected property lists of all /Script structs to a tsv"), FConsoleCommandWithArgsDelegate::CreateStatic(&GameDumpSchemas));
#endif
