// Fork addition (generic, game agnostic): console commands for the profile driven generator.
//   Suzie.GenerateNativeTypes [types file]   Suzie.VerifyNativeTypes [usmap tsv]   Suzie.DumpClass <class path>
//   Suzie.DumpAll <out file>                 Suzie.TagInfo [tag...]
#include "SzBakedGenerator.h"
#include "SzProfile.h"
#include "SuziePlugin.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Dom/JsonObject.h"

namespace
{
FString RunGenerateCommon(const FSzProfile& P, const FString& File)
{
	FSzProfile Copy = P;
	if (!File.IsEmpty()) Copy.TypesFile = File;
	FSzGenStats S;
	const bool bOk = FSzBakedGenerator::Generate(Copy, S);
	return FString::Printf(TEXT("GENERATE %s file=%s: enums=%d(+%d existing) structs=%d(+%d existing, %d failed) classes=%d(+%d existing, %d failed) delegates=%d functions=%d properties=%d unresolved=%d overrides=%d/%d/%d time=%.2fs"),
		bOk ? TEXT("OK") : TEXT("FAILED"), *Copy.TypesFile, S.Enums, S.EnumsExisting, S.Structs, S.StructsExisting, S.StructsFailed, S.Classes, S.ClassesExisting, S.ClassesFailed, S.Delegates, S.Functions, S.Properties, S.Unresolved, S.OverrideTypes, S.OverrideShadows, S.OverrideSkipped, S.Seconds);
}

struct FUsmapRow { FString Name, Type; int32 Dim = 1; };

FString PathOf(const UObject* O) { return O ? O->GetPathName() : FString(TEXT("None")); }

TSharedPtr<FJsonObject> LoadJson(const FString& File)
{
	FString Text;
	TSharedPtr<FJsonObject> Obj;
	if (FFileHelper::LoadFileToString(Text, *File))
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Obj);
	}
	return Obj;
}
}

namespace SuzieCommands
{
void RunVerify(const FSzProfile* Profile, const TArray<FString>& Args)
{
	FString Tsv = Args.Num() > 0 ? Args[0] : (Profile ? Profile->UsmapTsv : FString());
	FParse::Value(FCommandLine::Get(), TEXT("SuzieUsmapTsv="), Tsv);
	TMap<FString, TArray<FUsmapRow>> Usmap;
	TArray<FString> Lines;
	const bool bHaveTsv = FFileHelper::LoadFileToStringArray(Lines, *Tsv);
	if (bHaveTsv)
	{
		for (const FString& L : Lines)
		{
			TArray<FString> C;
			L.ParseIntoArray(C, TEXT("\t"), false);
			if (C.Num() >= 6 && C[0] == TEXT("P"))
			{
				FUsmapRow R;
				R.Name = C[3];
				R.Type = C[4];
				R.Dim = FCString::Atoi(*C[5]);
				TArray<FUsmapRow>& Rows = Usmap.FindOrAdd(C[1]);
				// the usmap lists a static array once per element: keep one row
				if (R.Dim > 1 && Rows.Num() > 0 && Rows.Last().Name == R.Name && Rows.Last().Dim == R.Dim && Rows.Last().Type == R.Type)
				{
					int32 Run = 1;
					for (int32 k = Rows.Num() - 2; k >= 0 && Rows[k].Name == R.Name; --k) ++Run;
					if (Run < R.Dim) continue;
				}
				Rows.Add(R);
			}
		}
	}
	int32 Ok = 0, Bad = 0, Total = 0, NoUsmap = 0;
	for (const TPair<FString, FSzGeneratedType>& KV : FSzBakedGenerator::GetGenerated())
	{
		UStruct* S = KV.Value.Struct.Get();
		if (!S) continue;
		Total++;
		TArray<FString> Errors;
		// live reflection, own properties only, in chain order
		TArray<const FProperty*> Live;
		for (TFieldIterator<FProperty> It(S, EFieldIterationFlags::IncludeDeprecated); It; ++It)
		{
			Live.Add(*It);
		}
		if (Live.Num() != KV.Value.PropNames.Num())
		{
			Errors.Add(FString::Printf(TEXT("count live=%d baked=%d"), Live.Num(), KV.Value.PropNames.Num()));
		}
		for (int32 i = 0; i < FMath::Min(Live.Num(), KV.Value.PropNames.Num()); ++i)
		{
			const FProperty* P = Live[i];
			if (P->GetName() != KV.Value.PropNames[i])
				Errors.Add(FString::Printf(TEXT("#%d name live=%s baked=%s"), i, *P->GetName(), *KV.Value.PropNames[i]));
			else if (P->GetClass()->GetName() != KV.Value.PropTypes[i])
				Errors.Add(FString::Printf(TEXT("#%d %s type live=%s baked=%s"), i, *P->GetName(), *P->GetClass()->GetName(), *KV.Value.PropTypes[i]));
			else if (P->ArrayDim != KV.Value.PropDims[i])
				Errors.Add(FString::Printf(TEXT("#%d %s dim live=%d baked=%d"), i, *P->GetName(), P->ArrayDim, KV.Value.PropDims[i]));
		}
		if (KV.Value.bIsStruct && KV.Value.BakedSize >= 0 && (S->GetPropertiesSize() != KV.Value.BakedSize || (KV.Value.BakedAlign > 0 && S->GetMinAlignment() != KV.Value.BakedAlign)))
		{
			// layout of a generated struct must equal the game's (zero-serialization depends on size/alignment/atomic)
			Errors.Add(FString::Printf(TEXT("layout size live=%d baked=%d align live=%d baked=%d flags=0x%x"), S->GetPropertiesSize(), KV.Value.BakedSize, S->GetMinAlignment(), KV.Value.BakedAlign, (uint32)Cast<UScriptStruct>(S)->StructFlags));
		}
		// property sizes (and for structs offsets) vs the game's
		{
			int32 SizeDiffs = 0, OffDiffs = 0; FString FirstSize, FirstOff;
			for (int32 i = 0; i < FMath::Min(Live.Num(), KV.Value.PropSizes.Num()); ++i)
			{
				const int32 G = KV.Value.PropSizes[i];
				const int32 L = Live[i]->GetElementSize() * Live[i]->ArrayDim;
				// the jmap size of a static array is the element size in some versions: accept both
				if (G > 0 && L != G && L != Live[i]->GetElementSize() * 1 && G != Live[i]->GetElementSize())
				{
					if (SizeDiffs++ == 0) FirstSize = FString::Printf(TEXT("%s(%s) ours=%d game=%d"), *Live[i]->GetName(), *Live[i]->GetClass()->GetName(), L, G);
				}
				if (KV.Value.bIsStruct && KV.Value.PropOffsets[i] >= 0 && Live[i]->GetOffset_ForInternal() != KV.Value.PropOffsets[i])
				{
					if (OffDiffs++ == 0) FirstOff = FString::Printf(TEXT("%s(%s) ours=%d game=%d"), *Live[i]->GetName(), *Live[i]->GetClass()->GetName(), Live[i]->GetOffset_ForInternal(), KV.Value.PropOffsets[i]);
				}
			}
			if (SizeDiffs) Errors.Add(FString::Printf(TEXT("%d property size diffs, first: %s"), SizeDiffs, *FirstSize));
			if (OffDiffs) Errors.Add(FString::Printf(TEXT("%d property offset diffs, first: %s"), OffDiffs, *FirstOff));
		}
		// usmap (independent source)
		if (bHaveTsv)
		{
			if (const TArray<FUsmapRow>* U = Usmap.Find(S->GetName()))
			{
				if (U->Num() != Live.Num())
				{
					Errors.Add(FString::Printf(TEXT("usmap count=%d live=%d"), U->Num(), Live.Num()));
				}
				for (int32 i = 0; i < FMath::Min(U->Num(), Live.Num()); ++i)
				{
					FString LiveType = Live[i]->GetClass()->GetName();
					if (LiveType == TEXT("MulticastInlineDelegateProperty")) LiveType = TEXT("MulticastDelegateProperty"); // usmap does not distinguish inline/sparse
					FString UsType = (*U)[i].Type;
					if (UsType == TEXT("EnumProperty") && LiveType == TEXT("ByteProperty")) LiveType = UsType; // byte with enum
					if (UsType == TEXT("MulticastInlineDelegateProperty") || UsType == TEXT("MulticastSparseDelegateProperty")) UsType = TEXT("MulticastDelegateProperty");
					if (LiveType == TEXT("MulticastSparseDelegateProperty")) LiveType = TEXT("MulticastDelegateProperty");
					if (LiveType == TEXT("ClassProperty")) LiveType = TEXT("ObjectProperty"); // usmap stores class/soft class as object/soft object
					if (LiveType == TEXT("SoftClassProperty")) LiveType = TEXT("SoftObjectProperty");
					if ((*U)[i].Name != Live[i]->GetName() || UsType != LiveType || (*U)[i].Dim != Live[i]->ArrayDim)
					{
						Errors.Add(FString::Printf(TEXT("usmap #%d %s/%s/%d vs live %s/%s/%d"), i, *(*U)[i].Name, *(*U)[i].Type, (*U)[i].Dim, *Live[i]->GetName(), *Live[i]->GetClass()->GetName(), Live[i]->ArrayDim));
						break;
					}
				}
			}
			else
			{
				NoUsmap++;
			}
		}
		if (Errors.Num() == 0)
		{
			Ok++;
		}
		else
		{
			Bad++;
			for (const FString& E : Errors)
			{
				UE_LOG(LogSuzie, Error, TEXT("VERIFY MISMATCH %s: %s"), *KV.Key, *E);
			}
		}
	}
	UE_LOG(LogSuzie, Log, TEXT("VERIFY RESULT: types=%d ok=%d mismatch=%d (not in usmap tsv: %d, tsv %s)"), Total, Ok, Bad, NoUsmap, bHaveTsv ? TEXT("loaded") : TEXT("missing"));
}

void RunDumpClass(const TArray<FString>& Args)
{
	if (Args.Num() < 1) return;
	UClass* C = FindObject<UClass>(nullptr, *Args[0]);
	if (!C) { UE_LOG(LogSuzie, Warning, TEXT("DumpClass: %s not found"), *Args[0]); return; }
	for (UClass* K = C; K; K = K->GetSuperClass())
	{
		UE_LOG(LogSuzie, Log, TEXT("DUMP class %s flags=0x%x size=%d cdo=%s"), *K->GetPathName(), (uint32)K->ClassFlags, K->GetPropertiesSize(), K->ClassDefaultObject ? *K->ClassDefaultObject->GetName() : TEXT("none"));
		for (TFieldIterator<FProperty> It(K, EFieldIterationFlags::IncludeDeprecated); It; ++It)
		{
			UE_LOG(LogSuzie, Log, TEXT("DUMP   %s : %s off=%d dim=%d owner=%s"), *It->GetName(), *It->GetClass()->GetName(), It->GetOffset_ForInternal(), It->ArrayDim, *It->GetOwnerVariant().GetFullName());
		}
	}
}

// ---------------------------------------------------------------- golden dump
// Canonical text dump of every type named by the profile's types file, read back from LIVE reflection (so it can be taken
// whichever generator created the types). Pointers and hashes are excluded; the output is sorted.

void DescribeProp(const FProperty* P, FString& Out)
{
	Out += FString::Printf(TEXT("%s:%s dim=%d fl=%llx off=%d es=%d"), *P->GetName(), *P->GetClass()->GetName(), P->ArrayDim, (uint64)P->PropertyFlags, P->GetOffset_ForInternal(), P->GetElementSize());
	if (const FClassProperty* CP = CastField<FClassProperty>(P))
	{
		Out += TEXT(" meta=") + PathOf(CP->MetaClass) + TEXT(" pc=") + PathOf(CP->PropertyClass);
	}
	else if (const FSoftClassProperty* SCP = CastField<FSoftClassProperty>(P))
	{
		Out += TEXT(" meta=") + PathOf(SCP->MetaClass) + TEXT(" pc=") + PathOf(SCP->PropertyClass);
	}
	else if (const FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(P))
	{
		Out += TEXT(" cls=") + PathOf(OP->PropertyClass);
	}
	else if (const FInterfaceProperty* IP = CastField<FInterfaceProperty>(P))
	{
		Out += TEXT(" ic=") + PathOf(IP->InterfaceClass);
	}
	else if (const FStructProperty* SP = CastField<FStructProperty>(P))
	{
		Out += TEXT(" st=") + PathOf(SP->Struct);
	}
	else if (const FEnumProperty* EP = CastField<FEnumProperty>(P))
	{
		Out += TEXT(" en=") + PathOf(EP->GetEnum());
		if (const FProperty* U = EP->GetUnderlyingProperty())
		{
			Out += FString::Printf(TEXT(" u=%s/%llx"), *U->GetClass()->GetName(), (uint64)U->PropertyFlags);
		}
	}
	else if (const FByteProperty* BP = CastField<FByteProperty>(P))
	{
		Out += TEXT(" en=") + PathOf(BP->Enum);
	}
	else if (const FBoolProperty* BoolP = CastField<FBoolProperty>(P))
	{
		Out += FString::Printf(TEXT(" nb=%d fm=%x"), BoolP->IsNativeBool() ? 1 : 0, (uint32)BoolP->GetFieldMask());
	}
	else if (const FDelegateProperty* DP = CastField<FDelegateProperty>(P))
	{
		Out += TEXT(" sg=") + PathOf(DP->SignatureFunction);
	}
	else if (const FMulticastDelegateProperty* MDP = CastField<FMulticastDelegateProperty>(P))
	{
		Out += TEXT(" sg=") + PathOf(MDP->SignatureFunction);
	}
	else if (const FArrayProperty* AP = CastField<FArrayProperty>(P))
	{
		Out += TEXT(" [");
		DescribeProp(AP->Inner, Out);
		Out += TEXT("]");
	}
	else if (const FSetProperty* SetP = CastField<FSetProperty>(P))
	{
		Out += TEXT(" [");
		DescribeProp(SetP->ElementProp, Out);
		Out += TEXT("]");
	}
	else if (const FMapProperty* MP = CastField<FMapProperty>(P))
	{
		Out += TEXT(" [");
		DescribeProp(MP->KeyProp, Out);
		Out += TEXT(" => ");
		DescribeProp(MP->ValueProp, Out);
		Out += TEXT("]");
	}
	else if (const FOptionalProperty* OptP = CastField<FOptionalProperty>(P))
	{
		Out += TEXT(" [");
		DescribeProp(OptP->GetValueProperty(), Out);
		Out += TEXT("]");
	}
}

void DescribeFunction(const UFunction* F, FString& Out)
{
	Out += FString::Printf(TEXT("  FN %s kind=%s fl=%x nparms=%d parmsz=%d retoff=%d script=%d super=%s"), *F->GetName(), *F->GetClass()->GetName(), (uint32)F->FunctionFlags, (int32)F->NumParms, (int32)F->ParmsSize, (int32)F->ReturnValueOffset, F->Script.Num(), *PathOf(F->GetSuperFunction()));
	if (const USparseDelegateFunction* SF = Cast<USparseDelegateFunction>(F))
	{
		Out += FString::Printf(TEXT(" sparse=%s/%s"), *SF->OwningClassName.ToString(), *SF->DelegateName.ToString());
	}
	Out += TEXT("\n");
	for (TFieldIterator<FProperty> It(F, EFieldIterationFlags::IncludeDeprecated); It; ++It)
	{
		Out += TEXT("    P ");
		DescribeProp(*It, Out);
		Out += TEXT("\n");
	}
}

void DumpStructLike(const UStruct* S, FString& Out)
{
	for (TFieldIterator<FProperty> It(S, EFieldIterationFlags::IncludeDeprecated); It; ++It)
	{
		Out += TEXT("  P ");
		DescribeProp(*It, Out);
		Out += TEXT("\n");
	}
}

void RunDumpAll(const FSzProfile* Profile, const TArray<FString>& Args)
{
	if (Args.Num() < 1 || !Profile)
	{
		UE_LOG(LogSuzie, Warning, TEXT("DumpAll <out file> (needs -SuzieProfile=)"));
		return;
	}
	const TSharedPtr<FJsonObject> Root = LoadJson(Profile->TypesFile);
	if (!Root.IsValid())
	{
		UE_LOG(LogSuzie, Error, TEXT("DumpAll: cannot read %s"), *Profile->TypesFile);
		return;
	}
	TMap<FString, FString> Blocks;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	int32 Missing = 0;
	auto PathsOf = [&](const TCHAR* Key, TArray<FString>& OutPaths)
	{
		if (Root->TryGetArrayField(Key, Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString P;
				if (V->AsObject()->TryGetStringField(TEXT("p"), P)) OutPaths.Add(P);
			}
		}
	};
	TArray<FString> EnumPaths, StructPaths, ClassPaths, DelegatePaths;
	PathsOf(TEXT("enums"), EnumPaths);
	PathsOf(TEXT("structs"), StructPaths);
	PathsOf(TEXT("classes"), ClassPaths);
	PathsOf(TEXT("delegates"), DelegatePaths);

	for (const FString& P : EnumPaths)
	{
		const UEnum* E = FindObject<UEnum>(nullptr, *P);
		if (!E) { Blocks.Add(TEXT("1ENUM ") + P, TEXT("MISSING\n")); Missing++; continue; }
		FString O = FString::Printf(TEXT("ENUM %s form=%d cpp=%s n=%d\n"), *P, (int32)E->GetCppForm(), *E->CppType, E->NumEnums());
		for (int32 i = 0; i < E->NumEnums(); ++i)
		{
			O += FString::Printf(TEXT("  %s=%lld\n"), *E->GetNameByIndex(i).ToString(), E->GetValueByIndex(i));
		}
		Blocks.Add(TEXT("1ENUM ") + P, O);
	}
	for (const FString& P : StructPaths)
	{
		const UScriptStruct* S = FindObject<UScriptStruct>(nullptr, *P);
		if (!S) { Blocks.Add(TEXT("2STRUCT ") + P, TEXT("MISSING\n")); Missing++; continue; }
		FString O = FString::Printf(TEXT("STRUCT %s super=%s fl=%x size=%d align=%d ops=%d\n"), *P, *PathOf(S->GetSuperStruct()), (uint32)S->StructFlags, S->GetPropertiesSize(), S->GetMinAlignment(), S->GetCppStructOps() ? 1 : 0);
		DumpStructLike(S, O);
		Blocks.Add(TEXT("2STRUCT ") + P, O);
	}
	for (const FString& P : DelegatePaths)
	{
		const UFunction* F = FindObject<UFunction>(nullptr, *P);
		FString O = FString::Printf(TEXT("DELEGATE %s\n"), *P);
		if (F) DescribeFunction(F, O); else { O += TEXT("MISSING\n"); Missing++; }
		Blocks.Add(TEXT("3DELEGATE ") + P, O);
	}
	for (const FString& P : ClassPaths)
	{
		UClass* C = FindObject<UClass>(nullptr, *P);
		if (!C) { Blocks.Add(TEXT("4CLASS ") + P, TEXT("MISSING\n")); Missing++; continue; }
		FString O = FString::Printf(TEXT("CLASS %s super=%s fl=%x within=%s config=%s size=%d align=%d ctor=%d\n"), *P, *PathOf(C->GetSuperClass()), (uint32)C->ClassFlags, *PathOf(C->ClassWithin), *C->ClassConfigName.ToString(), C->GetPropertiesSize(), C->GetMinAlignment(), C->ClassConstructor ? 1 : 0);
		for (const FImplementedInterface& I : C->Interfaces)
		{
			O += FString::Printf(TEXT("  IF %s off=%d k2=%d\n"), *PathOf(I.Class), I.PointerOffset, I.bImplementedByK2 ? 1 : 0);
		}
		DumpStructLike(C, O);
		TArray<FString> Fns;
		for (TFieldIterator<UFunction> It(C, EFieldIterationFlags::None); It; ++It)
		{
			FString F;
			DescribeFunction(*It, F);
			Fns.Add(F);
		}
		Fns.Sort();
		for (const FString& F : Fns) O += F;
		if (UObject* CDO = C->GetDefaultObject(false))
		{
			TArray<FString> Subs;
			ForEachObjectWithOuter(CDO, [&](UObject* Sub)
			{
				const uint32 Fl = (uint32)Sub->GetFlags() & (RF_Public | RF_Transient | RF_Standalone | RF_ClassDefaultObject | RF_DefaultSubObject | RF_ArchetypeObject | RF_MarkAsRootSet | RF_MarkAsNative);
				Subs.Add(FString::Printf(TEXT("  SUB %s class=%s rf=%x rooted=%d\n"), *Sub->GetPathName(CDO), *PathOf(Sub->GetClass()), Fl, Sub->IsRooted() ? 1 : 0));
			}, true);
			Subs.Sort();
			O += FString::Printf(TEXT("  CDO exists rf=%x\n"), (uint32)CDO->GetFlags() & (RF_Public | RF_ClassDefaultObject | RF_MarkAsRootSet));
			for (const FString& S : Subs) O += S;
		}
		else
		{
			O += TEXT("  CDO none\n");
		}
		Blocks.Add(TEXT("4CLASS ") + P, O);
	}
	TArray<FString> Keys;
	Blocks.GetKeys(Keys);
	Keys.Sort();
	FString All;
	{
		FGameplayTagContainer Tags;
		UGameplayTagsManager::Get().RequestAllGameplayTags(Tags, false);
		All += FString::Printf(TEXT("SUMMARY enums=%d structs=%d classes=%d delegates=%d missing=%d tags=%d\n"), EnumPaths.Num(), StructPaths.Num(), ClassPaths.Num(), DelegatePaths.Num(), Missing, Tags.Num());
	}
	for (const FString& K : Keys) All += Blocks[K];
	FFileHelper::SaveStringToFile(All, *Args[0], FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogSuzie, Log, TEXT("DUMPALL wrote %s: %d types, %d missing, %d chars"), *Args[0], Keys.Num(), Missing, All.Len());
}

void RunTagInfo(const FSzProfile* Profile, const TArray<FString>& Args)
{
	UGameplayTagsManager& M = UGameplayTagsManager::Get();
	FGameplayTagContainer All;
	M.RequestAllGameplayTags(All, false);
	const UGameplayTagsSettings* St = GetDefault<UGameplayTagsSettings>();
	UE_LOG(LogSuzie, Log, TEXT("TAGINFO manager tags=%d settings list=%d importIni=%d redirects=%d"), All.Num(), St->GameplayTagList.Num(), (int32)St->ImportTagsFromConfig, St->GameplayTagRedirects.Num());
	if (Args.Num() && Args[0] == TEXT("refresh"))
	{
		UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
		FGameplayTagContainer A2; UGameplayTagsManager::Get().RequestAllGameplayTags(A2, false);
		UE_LOG(LogSuzie, Log, TEXT("TAGINFO after refresh tags=%d"), A2.Num());
		return;
	}
	TArray<FString> Probe = Args;
	if (Probe.Num() == 0 && Profile && !Profile->ProbeTag.IsEmpty()) Probe.Add(Profile->ProbeTag);
	for (const FString& T : Probe)
	{
		UE_LOG(LogSuzie, Log, TEXT("TAGINFO %s valid=%d"), *T, (int32)M.RequestGameplayTag(FName(*T), false).IsValid());
	}
	TArray<const FGameplayTagSource*> Sources; M.FindTagSourcesWithType(EGameplayTagSourceType::TagList, Sources);
	for (const FGameplayTagSource* S : Sources) UE_LOG(LogSuzie, Log, TEXT("TAGINFO source %s tags=%d"), *S->SourceName.ToString(), S->SourceTagList ? S->SourceTagList->GameplayTagList.Num() : -1);
}

const FSzProfile* GetProfile()
{
	static FSzProfile P;
	static bool bTried = false, bOk = false;
	if (!bTried) { bTried = true; bOk = FSzProfile::Load(P, true); }
	return bOk ? &P : nullptr;
}

FAutoConsoleCommand GGenerateCmd(TEXT("Suzie.GenerateNativeTypes"), TEXT("Suzie.GenerateNativeTypes [baked types file]"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (const FSzProfile* P = GetProfile()) { UE_LOG(LogSuzie, Log, TEXT("%s"), *RunGenerateCommon(*P, Args.Num() ? Args[0] : FString())); }
		else { UE_LOG(LogSuzie, Error, TEXT("No profile (-SuzieProfile=)")); }
	}));
FAutoConsoleCommand GVerifyCmd(TEXT("Suzie.VerifyNativeTypes"), TEXT("Suzie.VerifyNativeTypes [usmap schema tsv]: compare generated property lists with baked data and an independent schema dump"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { RunVerify(GetProfile(), Args); }));
FAutoConsoleCommand GDumpClassCmd(TEXT("Suzie.DumpClass"), TEXT("Suzie.DumpClass <class path>"), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { RunDumpClass(Args); }));
FAutoConsoleCommand GDumpAllCmd(TEXT("Suzie.DumpAll"), TEXT("Suzie.DumpAll <out file>: canonical reflection dump of all profile types"), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { RunDumpAll(GetProfile(), Args); }));
FAutoConsoleCommand GTagInfoCmd(TEXT("Suzie.TagInfo"), TEXT("Suzie.TagInfo [tag...|refresh]"), FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { RunTagInfo(GetProfile(), Args); }));

void RunGenerateFromModule(const FSzProfile& P)
{
	UE_LOG(LogSuzie, Log, TEXT("%s"), *RunGenerateCommon(P, FString()));
}
}
