#include "SzBakedGenerator.h"
#include "SzJmapSource.h"
#include "SzPropertyValues.h"
#include "SuziePlugin.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/AsyncLoadingEvents.h"
#include "Features/IModularFeatures.h"
#include "Misc/CommandLine.h"
#include "Modules/ModuleManager.h"
#include "SuzieExtension.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/PropertyOptional.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "UObject/UnrealTypePrivate.h"
#if __has_include("UObject/UnversionedSchemaOverride.h")
#include "UObject/UnversionedSchemaOverride.h"
#define SUZIE_HAS_SCHEMA_OVERRIDE 1
#if defined(UE_MODKIT_SCHEMA_HOOK_LEVEL) && UE_MODKIT_SCHEMA_HOOK_LEVEL >= 2
#define SUZIE_HAS_HOOK_LEVEL2 1 // post-serialize handlers, generated struct registry (row structs keep their super with the engine DataTable guard)
#else
#define SUZIE_HAS_HOOK_LEVEL2 0
#endif
#if defined(UE_MODKIT_SCHEMA_HOOK_LEVEL) && UE_MODKIT_SCHEMA_HOOK_LEVEL >= 3
#define SUZIE_HAS_HOOK_LEVEL3 1 // discarded shadow properties are read like live ones (integer-like values as integers)
#else
#define SUZIE_HAS_HOOK_LEVEL3 0
#endif
#else
#define SUZIE_HAS_SCHEMA_OVERRIDE 0
#define SUZIE_HAS_HOOK_LEVEL2 0
#define SUZIE_HAS_HOOK_LEVEL3 0
#endif
#include "Serialization/ArchiveUObject.h"
#include "UObject/UObjectIterator.h"

TMap<FString, FSzGeneratedType>& FSzBakedGenerator::GetGenerated()
{
	static TMap<FString, FSzGeneratedType> G;
	return G;
}

namespace
{
using FJObj = TSharedPtr<FJsonObject>;

uint64 HexField(const FJObj& O, const TCHAR* Key)
{
	FString S;
	if (O->TryGetStringField(Key, S))
	{
		return FCString::Strtoui64(*S, nullptr, 16);
	}
	double D = 0;
	if (O->TryGetNumberField(Key, D))
	{
		return (uint64)D;
	}
	return 0;
}

FString StrField(const FJObj& O, const TCHAR* Key)
{
	FString S;
	O->TryGetStringField(Key, S);
	return S;
}

FString PackageOfPath(const FString& Path)
{
	int32 Dot;
	if (Path.FindChar(TEXT('.'), Dot))
	{
		return Path.Left(Dot);
	}
	return Path;
}

/** "/Script/Mod.Class:Func" -> "Func"; "/Script/Mod.Name" -> "Name" */
FString LeafOfPath(const FString& Path)
{
	int32 Idx;
	if (Path.FindLastChar(TEXT(':'), Idx))
	{
		return Path.Mid(Idx + 1);
	}
	if (Path.FindLastChar(TEXT('.'), Idx))
	{
		return Path.Mid(Idx + 1);
	}
	return Path;
}

// Generated classes carry CLASS_Native (the engine treats non-native classes as Blueprint classes: e.g.
// UBlueprintGeneratedClass::AddReferencedObjectsInUbergraphFrame walks supers until the first native class and static_casts
// every class before it). A native class's constructor is responsible for initializing its properties, so the shared
// constructor below runs the first non-generated ancestor's constructor and then default-initializes the properties
// introduced by the generated classes of the chain (top-most first).
TSet<UClass*> GGeneratedClasses;
bool GFillDone = false;
TMap<UClass*, TArray<TPair<FName, UClass*>>> GSubobjectDefs; // default subobjects per generated class (jmap CDO children)
// Properties whose jmap values are set on a generated class's default object, parent entries first. A new instance takes these
// values from its class's default object, and a default object takes them from its generated parent's (SzGenericConstruct).
TMap<UClass*, TArray<FProperty*>> GCdoValueProps;
// The subset of a class's value properties whose value on its default object references objects inside that default object.
TMap<UClass*, TArray<FProperty*>> GCdoRefProps;

// The default subobject of a default object that a "so" record names. A nested record ("Outer:Name" with outer "Outer") is looked up under its outer.
UObject* FindDefaultSubobject(UObject* CDO, const FJObj& So)
{
	FString Name = StrField(So, TEXT("n"));
	UObject* Outer = CDO;
	if (So->HasField(TEXT("outer")))
	{
		const FString OuterName = StrField(So, TEXT("outer"));
		int32 Colon;
		if (!Name.FindLastChar(TEXT(':'), Colon) || OuterName.Contains(TEXT(":")))
		{
			return nullptr;
		}
		Outer = StaticFindObjectFast(UObject::StaticClass(), CDO, FName(*OuterName));
		Name = Name.Mid(Colon + 1);
	}
	else if (Name.Contains(TEXT(":")))
	{
		return nullptr;
	}
	return Outer ? StaticFindObjectFast(UObject::StaticClass(), Outer, FName(*Name)) : nullptr;
}

int32 GRefsRemapped = 0; // object references of copied values that were pointed at the same-named object of the new object
int32 GRefsNulled = 0;   // object references of copied values into the source that the new object has no object for

// The object below Obj that has the name path of Ref below Source, or null.
UObject* FindCounterpart(UObject* Obj, const UObject* Source, const UObject* Ref)
{
	if (Ref == Source)
	{
		return Obj;
	}
	TArray<FName, TInlineAllocator<8>> Names;
	for (const UObject* O = Ref; O && O != Source; O = O->GetOuter())
	{
		Names.Add(O->GetFName());
	}
	UObject* Cur = Obj;
	for (int32 i = Names.Num() - 1; i >= 0 && Cur; --i)
	{
		Cur = StaticFindObjectFast(UObject::StaticClass(), Cur, Names[i]);
	}
	return Cur;
}

// Object references of the copied properties that point into Source are pointed at the counterpart in Obj, or nulled when there is none.
void RemapCopiedReferences(UObject* Obj, UObject* Source, const TArray<FProperty*>& Props)
{
	TSet<const FProperty*> Copied;
	for (const FProperty* Prop : Props)
	{
		Copied.Add(Prop);
	}
	TArray<const FProperty*> Chain;
	for (TPropertyValueIterator<FObjectProperty> It(Obj->GetClass(), Obj); It; ++It)
	{
		Chain.Reset();
		It.GetPropertyChain(Chain);
		if (Chain.IsEmpty() || !Copied.Contains(Chain.Last()))
		{
			continue;
		}
		UObject* Ref = It.Key()->GetObjectPropertyValue(It.Value());
		if (!Ref || !Ref->IsIn(Source))
		{
			continue;
		}
		UObject* Counterpart = FindCounterpart(Obj, Source, Ref);
		It.Key()->SetObjectPropertyValue(const_cast<void*>(It.Value()), Counterpart);
		if (Counterpart)
		{
			GRefsRemapped++;
		}
		else
		{
			GRefsNulled++;
		}
	}
}

void SzGenericConstruct(const FObjectInitializer& OI)
{
	UClass* Cls = OI.GetClass();
	UClass* K = Cls;
	while (K && !GGeneratedClasses.Contains(K))
	{
		K = K->GetSuperClass();
	}
	TArray<UClass*, TInlineAllocator<8>> Chain;
	UClass* G = K;
	while (G && GGeneratedClasses.Contains(G))
	{
		Chain.Add(G);
		G = G->GetSuperClass();
	}
	check(G && G->ClassConstructor);
	// Final class of every default subobject of the chain (a derived class's jmap CDO lists the overridden class, e.g.
	// NavLinkProxy.SmartLinkComp -> a game specific component class). Overrides must be announced BEFORE the engine constructor runs.
	TMap<FName, UClass*> FinalSubs;
	for (int32 i = Chain.Num() - 1; i >= 0; --i)
	{
		if (const TArray<TPair<FName, UClass*>>* Subs = GSubobjectDefs.Find(Chain[i]))
		{
			for (const TPair<FName, UClass*>& Sub : *Subs)
			{
				UClass** Cur = FinalSubs.Find(Sub.Key);
				if (!Cur || (Sub.Value && Sub.Value->IsChildOf(*Cur))) FinalSubs.Add(Sub.Key, Sub.Value);
			}
		}
	}
	if (FinalSubs.Num())
	{
		UObject* AncestorCDO = G->GetDefaultObject();
		for (const TPair<FName, UClass*>& F : FinalSubs)
		{
			UObject* Existing = AncestorCDO ? StaticFindObjectFast(UObject::StaticClass(), AncestorCDO, F.Key) : nullptr;
			if (Existing && F.Value && F.Value != Existing->GetClass() && F.Value->IsChildOf(Existing->GetClass()) && !F.Value->HasAnyClassFlags(CLASS_Abstract))
			{
				OI.SetDefaultSubobjectClass(F.Key, F.Value);
			}
		}
	}
	G->ClassConstructor(OI);
	UObject* Obj = OI.GetObj();
	if (!GFillDone)
	{
		UE_LOG(LogSuzie, Warning, TEXT("Object %s constructed before the fill phase finished"), *Obj->GetName());
	}
	// default subobjects (top-most generated class first; names already created by an ancestor constructor are kept)
	for (int32 i = Chain.Num() - 1; i >= 0; --i)
	{
		if (const TArray<TPair<FName, UClass*>>* Subs = GSubobjectDefs.Find(Chain[i]))
		{
			for (const TPair<FName, UClass*>& Sub0 : *Subs)
			{
				TPair<FName, UClass*> Sub(Sub0.Key, FinalSubs.FindRef(Sub0.Key) ? FinalSubs.FindRef(Sub0.Key) : Sub0.Value);
				if (!Sub.Value || Sub.Value->HasAnyClassFlags(CLASS_Abstract) || StaticFindObjectFast(UObject::StaticClass(), Obj, Sub.Key))
				{
					continue;
				}
				UObject* Made = OI.CreateDefaultSubobject(Obj, Sub.Key, Sub.Value, Sub.Value, /*bIsRequired*/ true, /*bIsTransient*/ false);
				bool bUnderCDO = false;
				for (const UObject* O = Obj; O; O = O->GetOuter()) { if (O->HasAnyFlags(RF_ClassDefaultObject)) { bUnderCDO = true; break; } }
				if (Made && bUnderCDO)
				{
					Made->AddToRoot(); // nothing references a generated CDO's subobjects (no UPROPERTY pointer), and the loader keeps raw pointers to them
				}
				static int32 Logged = 0;
				if (!Made && Logged++ < 30)
				{
					UE_LOG(LogSuzie, Warning, TEXT("Default subobject %s (%s) of %s not created"), *Sub.Key.ToString(), *Sub.Value->GetName(), *Obj->GetName());
				}
			}
		}
	}
	for (int32 i = Chain.Num() - 1; i >= 0; --i)
	{
		for (TFieldIterator<FProperty> It(Chain[i], EFieldIterationFlags::IncludeDeprecated); It; ++It)
		{
			It->InitializeValue_InContainer(Obj);
		}
	}
	// The source is the class default object of the most derived generated class, or for that default object the one of its generated parent.
	const bool bIsKDefault = K && Obj->HasAnyFlags(RF_ClassDefaultObject) && Obj->GetClass() == K;
	UObject* Source = nullptr;
	if (K)
	{
		Source = bIsKDefault ? (Chain.Num() > 1 ? Chain[1]->GetDefaultObject(false) : nullptr) : K->GetDefaultObject(false);
	}
	if (Source && Source != Obj)
	{
		if (const TArray<FProperty*>* Props = GCdoValueProps.Find(Source->GetClass()))
		{
			for (FProperty* Prop : *Props)
			{
				Prop->CopyCompleteValue_InContainer(Obj, Source);
			}
			const TArray<FProperty*>* RefProps = GCdoRefProps.Find(Source->GetClass());
			if (RefProps && RefProps->Num())
			{
				RemapCopiedReferences(Obj, Source, *RefProps);
			}
		}
	}
}

// ---- unversioned schema overrides (engine hook CoreUObject/UnversionedSchemaOverride.h; compiled out when the engine has none)
struct FRecipeEntry
{
	FProperty* Property = nullptr; // live property or shadow
	bool bDiscard = false;
	int32 Dim = 1;
};
TMap<const UStruct*, TArray<FRecipeEntry>> GRecipes; // immutable after FGen::BuildOverrides

#if SUZIE_HAS_SCHEMA_OVERRIDE
bool SzSchemaOverrideProvider(const UStruct* Struct, TArray<UE::UnversionedSchema::FOverrideEntry>& Entries)
{
	if (GRecipes.Num() == 0)
	{
		return false;
	}
	bool bAny = false;
	TArray<UE::UnversionedSchema::FOverrideEntry> Out;
	Out.Reserve(Entries.Num() + 8);
	for (int32 i = 0; i < Entries.Num();)
	{
		const UStruct* Owner = Entries[i].Property->GetOwnerStruct();
		int32 j = i;
		while (j < Entries.Num() && Entries[j].Property->GetOwnerStruct() == Owner)
		{
			++j;
		}
		if (const TArray<FRecipeEntry>* R = Owner ? GRecipes.Find(Owner) : nullptr)
		{
			for (const FRecipeEntry& E : *R)
			{
				for (int32 k = 0; k < E.Dim; ++k)
				{
					Out.Add({E.Property, k, E.bDiscard});
				}
			}
			bAny = true;
		}
		else
		{
			for (int32 k = i; k < j; ++k)
			{
				Out.Add(Entries[k]);
			}
		}
		i = j;
	}
	if (bAny)
	{
		Entries = MoveTemp(Out);
	}
	return bAny;
}
#endif

class FGen
{
public:
	FSzGenStats Stats;
	const FSzProfile* Profile = nullptr;
	FJObj Root;
	uint64 DerivedMask = 0;

	TMap<FString, UObject*> Made;           // path -> object created (or found) by us
	TSet<FString> ExistingBefore;           // paths that already existed in the editor
	TSet<FString> Failed;
	TSet<FString> Unresolved;
	TMap<FString, FJObj> FuncDefs;          // delegate function defs, keyed like Made
	TMap<FString, UPackage*> Packages;

	UPackage* GetPackage(const FString& PackagePath)
	{
		if (UPackage** P = Packages.Find(PackagePath))
		{
			return *P;
		}
		UPackage* Pkg = CreatePackage(*PackagePath);
		Pkg->SetPackageFlags(PKG_CompiledIn);
		Pkg->AddToRoot();
		Packages.Add(PackagePath, Pkg);
		return Pkg;
	}

	void TryLoadModuleFor(const FString& Path)
	{
		FString Pkg = PackageOfPath(Path);
		if (!Pkg.StartsWith(TEXT("/Script/")))
		{
			return;
		}
		const FName ModName(*Pkg.RightChop(8));
		if (FModuleManager::Get().IsModuleLoaded(ModName))
		{
			return;
		}
		if (FModuleManager::Get().ModuleExists(*ModName.ToString()))
		{
			UE_LOG(LogSuzie, Log, TEXT("Loading module %s for %s"), *ModName.ToString(), *Path);
			FModuleManager::Get().LoadModule(ModName);
		}
	}

	void RegisterAlias(const FString& AliasPath, UObject* Target)
	{
		const FString Pkg = PackageOfPath(AliasPath);
		ENotifyRegistrationType Type = Target->IsA<UEnum>() ? ENotifyRegistrationType::NRT_Enum : Target->IsA<UClass>() ? ENotifyRegistrationType::NRT_Class : ENotifyRegistrationType::NRT_Struct;
		NotifyRegistrationEvent(FName(*Pkg), FName(*LeafOfPath(AliasPath)), Type, ENotifyRegistrationPhase::NRP_Finished, nullptr, false, Target);
		// the notification also (re)binds the alias package's own import hash to the target's package: restore it with a real member
		if (UPackage* P = FindPackage(nullptr, *Pkg))
		{
			TArray<UObject*> Members;
			GetObjectsWithOuter(P, Members, false, RF_NoFlags, EInternalObjectFlags::None);
			for (UObject* M : Members)
			{
				if (M->HasAnyFlags(RF_Public) && (M->IsA<UClass>() || M->IsA<UScriptStruct>() || M->IsA<UEnum>()))
				{
					NotifyRegistrationEvent(FName(*Pkg), M->GetFName(), M->IsA<UEnum>() ? ENotifyRegistrationType::NRT_Enum : M->IsA<UClass>() ? ENotifyRegistrationType::NRT_Class : ENotifyRegistrationType::NRT_Struct, ENotifyRegistrationPhase::NRP_Finished, nullptr, false, M);
					break;
				}
			}
		}
	}

	template <typename T>
	T* Resolve(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (UObject** M = Made.Find(Path))
		{
			return Cast<T>(*M);
		}
		T* O = FindObject<T>(nullptr, *Path);
		if (!O)
		{
			if (!Unresolved.Contains(Path))
			{
				TryLoadModuleFor(Path);
				O = FindObject<T>(nullptr, *Path);
			}
		}
		if (!O && !Unresolved.Contains(Path))
		{
			// the type may live under another path in the editor (profile aliases, e.g. a type that moved between packages)
			const FString* AliasPath = Profile->Aliases.Find(Path);
			T* A = AliasPath ? FindObject<T>(nullptr, **AliasPath) : nullptr;
			if (!A && Profile->bAliasByLeaf && !Path.Contains(TEXT(":")))
			{
				A = FindFirstObject<T>(*LeafOfPath(Path), EFindFirstObjectOptions::NativeFirst);
				if (A && !(A->GetOutermost()->GetName().StartsWith(TEXT("/Script/")) && A->GetOutermost()->GetName() != PackageOfPath(Path)))
				{
					A = nullptr;
				}
			}
			if (A)
			{
				UE_LOG(LogSuzie, Log, TEXT("Alias %s -> %s"), *Path, *A->GetPathName());
				RegisterAlias(Path, A);
				Made.Add(Path, A);
				return A;
			}
		}
		if (!O)
		{
			if (!Unresolved.Contains(Path))
			{
				Unresolved.Add(Path);
				UE_LOG(LogSuzie, Warning, TEXT("Unresolved reference %s"), *Path);
				Stats.Unresolved++;
			}
		}
		return O;
	}

	static void Notify(UPackage* Pkg, UObject* Obj, ENotifyRegistrationType Type)
	{
		NotifyRegistrationEvent(Pkg->GetFName(), Obj->GetFName(), Type, ENotifyRegistrationPhase::NRP_Finished, nullptr, false, Obj);
	}

	// ------------------------------------------------------------------ properties

	EPropertyFlags ConvertPropFlags(uint64 F) const
	{
		F &= ~(DerivedMask & ~(uint64)CPF_HasGetValueTypeHash); // the flag must be present on map keys/set elements at AddCppProperty time
		F &= ~(uint64)(CPF_Net | CPF_RepNotify | CPF_RepSkip);
		return (EPropertyFlags)F;
	}

	FProperty* BuildProp(FFieldVariant Owner, const FJObj& J, const FString& Ctx, bool& bOk)
	{
		const FString Type = StrField(J, TEXT("t"));
		const FString Name = StrField(J, TEXT("n"));
		FField* Field = FField::TryConstruct(FName(*Type), Owner, FName(*Name), RF_Public | RF_Transient);
		FProperty* P = CastField<FProperty>(Field);
		if (!P)
		{
			UE_LOG(LogSuzie, Error, TEXT("%s: cannot construct property type %s (%s)"), *Ctx, *Type, *Name);
			bOk = false;
			return nullptr;
		}
		Stats.Properties++;
		P->PropertyFlags = ConvertPropFlags(HexField(J, TEXT("f")));
		double Dim = 0;
		if (J->TryGetNumberField(TEXT("d"), Dim) && Dim > 1)
		{
			P->ArrayDim = (int32)Dim;
		}

		auto ResolveClassField = [&](const TCHAR* Key) -> UClass*
		{
			const FString Path = StrField(J, Key);
			UClass* C = Resolve<UClass>(Path);
			if (!C)
			{
				UE_LOG(LogSuzie, Warning, TEXT("%s.%s: class %s missing, using UObject"), *Ctx, *Name, *Path);
				C = UObject::StaticClass();
			}
			return C;
		};

		if (FClassProperty* CP = CastField<FClassProperty>(P))
		{
			CP->SetMetaClass(ResolveClassField(TEXT("meta")));
			CP->SetPropertyClass(UClass::StaticClass());
		}
		else if (FSoftClassProperty* SCP = CastField<FSoftClassProperty>(P))
		{
			SCP->SetMetaClass(ResolveClassField(TEXT("meta")));
			SCP->SetPropertyClass(UClass::StaticClass());
		}
		else if (FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(P))
		{
			OP->SetPropertyClass(ResolveClassField(TEXT("cls")));
		}
		else if (FInterfaceProperty* IP = CastField<FInterfaceProperty>(P))
		{
			IP->SetInterfaceClass(ResolveClassField(TEXT("ic")));
		}
		else if (FStructProperty* SP = CastField<FStructProperty>(P))
		{
			UScriptStruct* S = Resolve<UScriptStruct>(StrField(J, TEXT("st")));
			if (!S)
			{
				UE_LOG(LogSuzie, Error, TEXT("%s.%s: struct %s missing"), *Ctx, *Name, *StrField(J, TEXT("st")));
				bOk = false;
				return nullptr;
			}
			SP->Struct = S;
		}
		else if (FEnumProperty* EP = CastField<FEnumProperty>(P))
		{
			UEnum* E = Resolve<UEnum>(StrField(J, TEXT("en")));
			if (!E)
			{
				UE_LOG(LogSuzie, Error, TEXT("%s.%s: enum %s missing"), *Ctx, *Name, *StrField(J, TEXT("en")));
				bOk = false;
				return nullptr;
			}
			EP->SetEnum(E);
			FField* UF = FField::TryConstruct(FName(*StrField(J, TEXT("u"))), EP, FName(TEXT("UnderlyingType")), RF_Public | RF_Transient);
			FProperty* UP = CastField<FProperty>(UF);
			if (!UP)
			{
				UE_LOG(LogSuzie, Error, TEXT("%s.%s: underlying type %s unknown"), *Ctx, *Name, *StrField(J, TEXT("u")));
				bOk = false;
				return nullptr;
			}
			UP->PropertyFlags = CPF_None;
			EP->AddCppProperty(UP);
		}
		else if (FByteProperty* BP = CastField<FByteProperty>(P))
		{
			FString EnumPath;
			if (J->TryGetStringField(TEXT("en"), EnumPath) && !EnumPath.IsEmpty())
			{
				UEnum* E = Resolve<UEnum>(EnumPath);
				if (!E)
				{
					UE_LOG(LogSuzie, Warning, TEXT("%s.%s: byte enum %s missing"), *Ctx, *Name, *EnumPath);
				}
				BP->Enum = E;
			}
		}
		else if (FBoolProperty* BoolP = CastField<FBoolProperty>(P))
		{
			const TSharedPtr<FJsonObject>* B = nullptr;
			bool bNative = true;
			uint32 Size = 1, ByteMask = 1, ByteOffset = 0;
			if (J->TryGetObjectField(TEXT("b"), B))
			{
				double V = 0;
				bNative = (*B)->TryGetNumberField(TEXT("nat"), V) && V != 0;
				if ((*B)->TryGetNumberField(TEXT("fs"), V)) Size = (uint32)V;
				if ((*B)->TryGetNumberField(TEXT("bm"), V)) ByteMask = (uint32)V;
				if ((*B)->TryGetNumberField(TEXT("bo"), V)) ByteOffset = (uint32)V;
			}
			if (bNative)
			{
				BoolP->SetBoolSize(sizeof(bool), true);
			}
			else
			{
				BoolP->SetBoolSize(Size, false, ByteMask << (8 * ByteOffset));
			}
		}
		else if (FArrayProperty* AP = CastField<FArrayProperty>(P))
		{
			const TSharedPtr<FJsonObject>* In;
			if (!J->TryGetObjectField(TEXT("in"), In))
			{
				bOk = false;
				return nullptr;
			}
			FProperty* Inner = BuildProp(AP, *In, Ctx, bOk);
			if (!Inner) { return nullptr; }
			AP->AddCppProperty(Inner);
		}
		else if (FSetProperty* SetP = CastField<FSetProperty>(P))
		{
			const TSharedPtr<FJsonObject>* In;
			if (!J->TryGetObjectField(TEXT("key"), In) && !J->TryGetObjectField(TEXT("in"), In))
			{
				bOk = false;
				return nullptr;
			}
			FProperty* Inner = BuildProp(SetP, *In, Ctx, bOk);
			if (!Inner) { return nullptr; }
			SetP->AddCppProperty(Inner);
		}
		else if (FMapProperty* MP = CastField<FMapProperty>(P))
		{
			const TSharedPtr<FJsonObject>* K;
			const TSharedPtr<FJsonObject>* V;
			if (!J->TryGetObjectField(TEXT("key"), K) || !J->TryGetObjectField(TEXT("val"), V))
			{
				bOk = false;
				return nullptr;
			}
			FProperty* KP = BuildProp(MP, *K, Ctx, bOk);
			if (!KP) { return nullptr; }
			MP->AddCppProperty(KP);
			FProperty* VP = BuildProp(MP, *V, Ctx, bOk);
			if (!VP) { return nullptr; }
			MP->AddCppProperty(VP);
		}
		else if (FOptionalProperty* OptP = CastField<FOptionalProperty>(P))
		{
			const TSharedPtr<FJsonObject>* In;
			if (!J->TryGetObjectField(TEXT("in"), In))
			{
				bOk = false;
				return nullptr;
			}
			FProperty* Inner = BuildProp(OptP, *In, Ctx, bOk);
			if (!Inner) { return nullptr; }
			OptP->AddCppProperty(Inner);
		}
		else if (FDelegateProperty* DP = CastField<FDelegateProperty>(P))
		{
			DP->SignatureFunction = Resolve<UFunction>(StrField(J, TEXT("sg")));
		}
		else if (FMulticastDelegateProperty* MDP = CastField<FMulticastDelegateProperty>(P))
		{
			MDP->SignatureFunction = Resolve<UFunction>(StrField(J, TEXT("sg")));
		}
		return P;
	}

	/** Builds the property list of Owner from the "pr" array; properties are added in reverse (AddCppProperty pushes to the head). */
	bool BuildProps(UStruct* Owner, const FJObj& Def, const FString& Ctx, FSzGeneratedType* Rec)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!Def->TryGetArrayField(TEXT("pr"), Arr))
		{
			return true;
		}
		TArray<FProperty*> Props;
		bool bOk = true;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FJObj PJ = V->AsObject();
			FProperty* P = BuildProp(Owner, PJ, Ctx, bOk);
			if (!P || !bOk)
			{
				return false;
			}
			Props.Add(P);
			if (Rec)
			{
				Rec->PropNames.Add(StrField(PJ, TEXT("n")));
				Rec->PropTypes.Add(StrField(PJ, TEXT("t")));
				Rec->PropDims.Add(P->ArrayDim);
				double SV = -1, OV = -1;
				PJ->TryGetNumberField(TEXT("s"), SV);
				PJ->TryGetNumberField(TEXT("o"), OV);
				Rec->PropSizes.Add((int32)SV);
				Rec->PropOffsets.Add((int32)OV);
			}
		}
		for (int32 i = Props.Num() - 1; i >= 0; --i)
		{
			Owner->AddCppProperty(Props[i]);
		}
		return true;
	}

	// ------------------------------------------------------------------ enums

	void MakeEnum(const FJObj& Def)
	{
		const FString Path = StrField(Def, TEXT("p"));
		if (UEnum* Ex = FindObject<UEnum>(nullptr, *Path))
		{
			Made.Add(Path, Ex);
			ExistingBefore.Add(Path);
			Stats.EnumsExisting++;
			return;
		}
		UPackage* Pkg = GetPackage(PackageOfPath(Path));
		UEnum* E = NewObject<UEnum>(Pkg, FName(*LeafOfPath(Path)), RF_Public | RF_Standalone);
		TArray<TPair<FName, int64>> Names;
		for (const TSharedPtr<FJsonValue>& V : Def->GetArrayField(TEXT("n")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Pair = V->AsArray();
			Names.Emplace(FName(*Pair[0]->AsString()), (int64)Pair[1]->AsNumber());
		}
		const FString Cpp = StrField(Def, TEXT("cpp"));
		UEnum::ECppForm Form = UEnum::ECppForm::Regular;
		if (Cpp == TEXT("EnumClass")) Form = UEnum::ECppForm::EnumClass;
		else if (Cpp == TEXT("Namespaced")) Form = UEnum::ECppForm::Namespaced;
		E->SetEnums(Names, Form, EEnumFlags::None, false);
		E->CppType = StrField(Def, TEXT("ct"));
		E->AddToRoot();
		Made.Add(Path, E);
		Notify(Pkg, E, ENotifyRegistrationType::NRT_Enum);
		Stats.Enums++;
	}

	// ------------------------------------------------------------------ structs

	TMap<FString, FJObj> StructDefs;
	TArray<FString> StructOrder;
	bool bWarnedNoRowGuard = false;

	void MakeStructShell(const FJObj& Def)
	{
		const FString Path = StrField(Def, TEXT("p"));
		if (UScriptStruct* Ex = FindObject<UScriptStruct>(nullptr, *Path))
		{
			Made.Add(Path, Ex);
			ExistingBefore.Add(Path);
			Stats.StructsExisting++;
			return;
		}
		UPackage* Pkg = GetPackage(PackageOfPath(Path));
		UScriptStruct* S = NewObject<UScriptStruct>(Pkg, FName(*LeafOfPath(Path)), RF_Public | RF_Standalone);
		FString Sup;
		bool bDropSuper = false;
		if (Def->TryGetStringField(TEXT("sup"), Sup))
		{
			bDropSuper = Profile->DropStructSupers.Contains(Sup);
			if (!bDropSuper && !Profile->RowStructBase.IsEmpty() && Sup == Profile->RowStructBase && !(Profile->bRowStructKeepSuper && SUZIE_HAS_HOOK_LEVEL2))
			{
				// row_struct policy: keep_super_guarded needs the engine DataTable guard; without it (or in drop_super mode) rows lose the super
				bDropSuper = true;
				if (Profile->bRowStructKeepSuper && !bWarnedNoRowGuard)
				{
					bWarnedNoRowGuard = true;
					UE_LOG(LogSuzie, Warning, TEXT("Profile asks for keep_super_guarded row structs but the engine has no generated-struct hook (UE_MODKIT_SCHEMA_HOOK_LEVEL >= 2): dropping the row super"));
				}
			}
		}
		if (!Sup.IsEmpty() && !bDropSuper)
		{
			// (profile policy structs.drop_super: a base with a vtable and no properties has no C++ constructor to set the vptr for a
			// generated struct, so engine code calling a virtual on it would crash; generated structs then simply do not derive from it.)
			UScriptStruct* Super = Resolve<UScriptStruct>(Sup);
			if (Super)
			{
				S->SetSuperStruct(Super);
			}
			else
			{
				UE_LOG(LogSuzie, Error, TEXT("Struct %s: super %s missing"), *Path, *Sup);
			}
		}
		const uint32 Keep = STRUCT_HasInstancedReference | STRUCT_NoExport | STRUCT_Atomic | STRUCT_Immutable | STRUCT_RequiredAPI;
		S->StructFlags = (EStructFlags)((uint32)HexField(Def, TEXT("fl")) & Keep);
		S->AddToRoot();
		Made.Add(Path, S);
#if SUZIE_HAS_HOOK_LEVEL2
		UE::NativeSerialize::AddGeneratedStruct(S);
#endif
	}

	void FillStruct(const FJObj& Def)
	{
		const FString Path = StrField(Def, TEXT("p"));
		if (ExistingBefore.Contains(Path))
		{
			return;
		}
		UScriptStruct* S = Cast<UScriptStruct>(Made.FindRef(Path));
		if (!S) { return; }
		FSzGeneratedType Rec;
		Rec.Struct = S;
		Rec.bIsStruct = true;
		{
			double V = 0;
			if (Def->TryGetNumberField(TEXT("sz"), V)) Rec.BakedSize = (int32)V;
			if (Def->TryGetNumberField(TEXT("al"), V)) Rec.BakedAlign = (int32)V;
		}
		if (!BuildProps(S, Def, Path, &Rec))
		{
			Failed.Add(Path);
			Stats.StructsFailed++;
			UE_LOG(LogSuzie, Error, TEXT("Struct %s failed"), *Path);
			return;
		}
		S->Bind();
		S->StaticLink(true);
		if (S->GetPropertiesSize() == 0)
		{
			S->SetPropertiesSize(1); // like a C++ empty struct; size 0 breaks the unversioned zero-mask fast path
		}
		// The game's struct may have non-reflected members (vtable, shared pointers, padding). Its size and alignment decide
		// the unversioned "serialize as zero" fast path of atomic structs (UnversionedPropertySerialization CanSerializeAsZero),
		// so pad ours up to the game's (never down: our properties must fit).
		if (Rec.BakedSize > S->GetPropertiesSize())
		{
			S->SetPropertiesSize(Rec.BakedSize);
		}
		if (Rec.BakedAlign > S->GetMinAlignment())
		{
			S->MinAlignment = Rec.BakedAlign;
		}
		if (Rec.BakedSize >= 0 && S->GetPropertiesSize() != Rec.BakedSize)
		{
			UE_LOG(LogSuzie, Verbose, TEXT("Struct %s: our size %d > game size %d"), *Path, S->GetPropertiesSize(), Rec.BakedSize);
		}
		FSzBakedGenerator::GetGenerated().Add(Path, MoveTemp(Rec));
		Notify(GetPackage(PackageOfPath(Path)), S, ENotifyRegistrationType::NRT_Struct);
		Stats.Structs++;
	}

	// ------------------------------------------------------------------ functions

	UFunction* MakeFunctionShell(UObject* Outer, const FJObj& F)
	{
		const FString Kind = StrField(F, TEXT("k"));
		const FName FName_(*StrField(F, TEXT("n")));
		UFunction* Fn = nullptr;
		if (Kind == TEXT("D"))
		{
			Fn = NewObject<UDelegateFunction>(Outer, FName_, RF_Public);
		}
		else if (Kind == TEXT("S"))
		{
			USparseDelegateFunction* SF = NewObject<USparseDelegateFunction>(Outer, FName_, RF_Public);
			FString Leaf = FName_.ToString();
			Leaf.RemoveFromEnd(TEXT("__DelegateSignature"));
			SF->DelegateName = FName(*Leaf);
			SF->OwningClassName = Outer->GetFName();
			Fn = SF;
		}
		else
		{
			Fn = NewObject<UFunction>(Outer, FName_, RF_Public);
		}
		Fn->AddToRoot();
		return Fn;
	}

	/** Fills a function from its record. Without NativeImpl the function is a script stub; with it, the thunk is the function's body. */
	bool FillFunction(UFunction* Fn, const FJObj& F, const FString& Ctx, FNativeFuncPtr NativeImpl = nullptr)
	{
		const FString Kind = StrField(F, TEXT("k"));
		uint32 Flags = (uint32)(uint64)F->GetNumberField(TEXT("f"));
		if (NativeImpl)
		{
			Flags |= FUNC_Native;
		}
		else
		{
			Flags &= ~(uint32)FUNC_Native; // stub: script body below, executed by the VM
		}
		Fn->FunctionFlags = (EFunctionFlags)Flags;
		FString Sup;
		if (F->TryGetStringField(TEXT("sup"), Sup) && !Sup.IsEmpty())
		{
			if (UFunction* SupFn = Resolve<UFunction>(Sup))
			{
				Fn->SetSuperStruct(SupFn);
			}
		}
		const bool bOk = BuildProps(Fn, F, Ctx, nullptr);
		if (!bOk)
		{
			UE_LOG(LogSuzie, Error, TEXT("Function %s failed"), *Ctx);
		}
		if (Kind == TEXT("F") && !NativeImpl)
		{
			Fn->Script.Add(EX_Return);
			Fn->Script.Add(EX_Nothing);
			Fn->Script.Add(EX_EndOfScript);
		}
		// Bind() would look the native function up in the owner's table: a registered thunk is set directly
		if (NativeImpl)
		{
			Fn->SetNativeFunc(NativeImpl);
		}
		else
		{
			Fn->Bind();
		}
		Fn->StaticLink(true);
		Stats.Functions++;
		return bOk;
	}

	// ------------------------------------------------------------------ functions of engine classes

	int32 EngineFuncNative = 0, EngineFuncStub = 0, EngineFuncReplaced = 0, EngineFuncSkipped = 0, EngineFuncDiffers = 0;

	/** Same parameter count, names (case-insensitive) and property classes, in declaration order. */
	static bool SignatureMatches(const UFunction* Fn, const TArray<TSharedPtr<FJsonValue>>& JParams)
	{
		TArray<const FProperty*> Params;
		for (TFieldIterator<FProperty> It(Fn); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			Params.Add(*It);
		}
		TArray<FJObj> JP;
		for (const TSharedPtr<FJsonValue>& V : JParams)
		{
			FJObj P = V->AsObject();
			if (HexField(P, TEXT("f")) & (uint64)CPF_Parm)
			{
				JP.Add(P);
			}
		}
		if (JP.Num() != Params.Num())
		{
			return false;
		}
		for (int32 i = 0; i < JP.Num(); ++i)
		{
			if (!Params[i]->GetFName().ToString().Equals(StrField(JP[i], TEXT("n")), ESearchCase::IgnoreCase)
				|| Params[i]->GetClass()->GetFName() != FName(*StrField(JP[i], TEXT("t"))))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * The game's jmap functions of a class that the editor already has: a function the editor lacks is added (a registered thunk
	 * as its body, else a script stub; static functions of a function library are not added, a stub would be called through
	 * EX_CallMath), a function with another signature gets the registered thunk. Delegate signatures are skipped.
	 */
	void EngineClassFunctions(const FString& Path, const TArray<TSharedPtr<FJsonValue>>& Fns)
	{
		UClass* C = FindObject<UClass>(nullptr, *Path);
		if (!C)
		{
			return;
		}
		const bool bInLibrary = HasPathInChain(C, TEXT("/Script/Engine.BlueprintFunctionLibrary"));
		int32 AddedToClass = 0;
		for (const TSharedPtr<FJsonValue>& V : Fns)
		{
			FJObj F = V->AsObject();
			const uint32 Flags = (uint32)(uint64)F->GetNumberField(TEXT("f"));
			if (StrField(F, TEXT("k")) != TEXT("F") || (Flags & FUNC_Delegate))
			{
				continue;
			}
			const FString Name = StrField(F, TEXT("n"));
			const FString FuncPath = Path + TEXT(":") + Name;
			const FName FnName(*Name);
			const FNativeFuncPtr* Impl = ExtReg.NativeFunctions.Find(FuncPath);
			static const TArray<TSharedPtr<FJsonValue>> NoParams;
			const TArray<TSharedPtr<FJsonValue>>* Pr = nullptr;
			F->TryGetArrayField(TEXT("pr"), Pr);
			const TArray<TSharedPtr<FJsonValue>>& JParams = Pr ? *Pr : NoParams;

			if (UFunction* Own = C->FindFunctionByName(FnName, EIncludeSuperFlag::ExcludeSuper))
			{
				if (SignatureMatches(Own, JParams))
				{
					continue;
				}
				if (Impl)
				{
					Own->SetNativeFunc(*Impl);
					EngineFuncReplaced++;
					UE_LOG(LogSuzie, Verbose, TEXT("Native replacement for %s"), *FuncPath);
				}
				else
				{
					EngineFuncDiffers++;
					UE_LOG(LogSuzie, Warning, TEXT("Signature differs from the game: %s"), *FuncPath);
				}
				continue;
			}
			if (UFunction* Inherited = C->FindFunctionByName(FnName))
			{
				// an inherited function of that name is the one that runs: it is not shadowed by a stub
				if (!SignatureMatches(Inherited, JParams))
				{
					EngineFuncDiffers++;
					UE_LOG(LogSuzie, Warning, TEXT("Signature differs from the game (inherited): %s"), *FuncPath);
				}
				continue;
			}
			if (!Impl && bInLibrary && (Flags & FUNC_Static))
			{
				EngineFuncSkipped++;
				continue;
			}
			UFunction* Fn = MakeFunctionShell(C, F);
			if (!FillFunction(Fn, F, FuncPath, Impl ? *Impl : nullptr))
			{
				EngineFuncSkipped++;
				UE_LOG(LogSuzie, Error, TEXT("Engine class function %s not added: parameter types"), *FuncPath);
				continue;
			}
			Fn->Next = C->Children;
			C->Children = Fn;
			C->AddFunctionToFunctionMap(Fn, Fn->GetFName());
			AddedToClass++;
			if (Impl)
			{
				EngineFuncNative++;
			}
			else
			{
				EngineFuncStub++;
			}
		}
		if (AddedToClass > 0)
		{
			// the class and its RF_Public members (the new functions) go to the script import table; re-adding an entry replaces it
			Notify(C->GetOutermost(), C, ENotifyRegistrationType::NRT_Class);
		}
	}

	// ------------------------------------------------------------------ classes

	TMap<FString, FJObj> ClassDefs;
	TArray<FString> ClassOrder;
	FSzValueStats ValueStats;

	/** Applies the jmap values of a generated class's default object and of its default subobjects, and records the class's value properties. */
	void ApplyCdoValues(UClass* C, UObject* CDO, const FJObj& Def)
	{
		TArray<FProperty*> Applied;
		const FJObj* Values = nullptr;
		if (Def->TryGetObjectField(TEXT("cdo"), Values) && Values && Values->IsValid())
		{
			ValueStats.Classes++;
			SzApplyPropertyValues(C, CDO, *Values, ValueStats, &Applied);
		}
		const TArray<TSharedPtr<FJsonValue>>* Sos = nullptr;
		if (Def->TryGetArrayField(TEXT("so"), Sos))
		{
			for (const TSharedPtr<FJsonValue>& V : *Sos)
			{
				const FJObj So = V->AsObject();
				const FJObj* Pv = nullptr;
				if (!So.IsValid() || !So->TryGetObjectField(TEXT("pv"), Pv) || !Pv || !Pv->IsValid())
				{
					continue;
				}
				if (UObject* Sub = FindDefaultSubobject(CDO, So))
				{
					SzApplyPropertyValues(Sub->GetClass(), Sub, *Pv, ValueStats, nullptr);
					ValueStats.Subobjects++;
				}
			}
		}
		// the parent's properties come first; the class's own values are added without duplicates
		TArray<FProperty*> Union;
		if (const TArray<FProperty*>* ParentProps = GCdoValueProps.Find(C->GetSuperClass()))
		{
			Union = *ParentProps;
		}
		for (FProperty* Prop : Applied)
		{
			Union.AddUnique(Prop);
		}
		TSet<const FProperty*> Listed;
		for (const FProperty* Prop : Union)
		{
			Listed.Add(Prop);
		}
		TArray<FProperty*> RefProps;
		TArray<const FProperty*> Chain;
		for (TPropertyValueIterator<FObjectProperty> It(C, CDO); It; ++It)
		{
			Chain.Reset();
			It.GetPropertyChain(Chain);
			const UObject* Ref = It.Key()->GetObjectPropertyValue(It.Value());
			if (!Chain.IsEmpty() && Listed.Contains(Chain.Last()) && Ref && Ref->IsIn(CDO))
			{
				RefProps.AddUnique(const_cast<FProperty*>(Chain.Last()));
			}
		}
		GCdoRefProps.Add(C, MoveTemp(RefProps));
		GCdoValueProps.Add(C, MoveTemp(Union));
	}

	static bool HasPathInChain(const UClass* C, const TCHAR* Path)
	{
		for (; C; C = C->GetSuperClass())
		{
			if (C->GetPathName() == Path) return true;
		}
		return false;
	}

	void MakeClassShell(const FJObj& Def)
	{
		const FString Path = StrField(Def, TEXT("p"));
		if (UClass* Ex = FindObject<UClass>(nullptr, *Path))
		{
			Made.Add(Path, Ex);
			ExistingBefore.Add(Path);
			Stats.ClassesExisting++;
			return;
		}
		const FString Sup = StrField(Def, TEXT("sup"));
		UClass* Super = Resolve<UClass>(Sup);
		if (!Super || Failed.Contains(Sup))
		{
			UE_LOG(LogSuzie, Error, TEXT("Class %s: super %s missing or failed"), *Path, *Sup);
			Failed.Add(Path);
			Stats.ClassesFailed++;
			return;
		}
		UPackage* Pkg = GetPackage(PackageOfPath(Path));
		UClass* C = NewObject<UClass>(Pkg, FName(*LeafOfPath(Path)), RF_Public | RF_Standalone);
		C->SetSuperStruct(Super);
		C->ClassWithin = UObject::StaticClass();
		FString Within;
		if (Def->TryGetStringField(TEXT("within"), Within))
		{
			if (UClass* W = Resolve<UClass>(Within)) C->ClassWithin = W;
		}
		uint32 Strip = 0x304000A0u | CLASS_Config | CLASS_DefaultConfig | CLASS_GlobalUserConfig | CLASS_ProjectUserConfig | CLASS_Native | CLASS_Intrinsic | CLASS_Constructed | CLASS_TokenStreamAssembled;
		uint32 Fl = (uint32)HexField(Def, TEXT("fl")) & ~Strip;
		C->ClassFlags = (EClassFlags)Fl;
		C->ClassConfigName = Super->ClassConfigName;
		C->ClassFlags |= CLASS_Native;
		C->ClassConstructor = &SzGenericConstruct;
		GGeneratedClasses.Add(C);
		#if SUZIE_HAS_SCHEMA_OVERRIDE
		UE::NativeSerialize::AddGeneratedClass(C);
#endif
		for (const FString& Base : Profile->AbstractIfDerivedFrom)
		{
			if (HasPathInChain(Super, *Base))
			{
				C->ClassFlags |= CLASS_Abstract;
				break;
			}
		}
		C->AddToRoot();
		Made.Add(Path, C);

		// delegate signature shells owned by this class (needed by properties before the class is filled)
		const TArray<TSharedPtr<FJsonValue>>* Fns;
		if (Def->TryGetArrayField(TEXT("fn"), Fns))
		{
			for (const TSharedPtr<FJsonValue>& V : *Fns)
			{
				FJObj F = V->AsObject();
				const FString Kind = StrField(F, TEXT("k"));
				if (Kind == TEXT("D") || Kind == TEXT("S"))
				{
					UFunction* Fn = MakeFunctionShell(C, F);
					const FString Key = Path + TEXT(":") + StrField(F, TEXT("n"));
					Made.Add(Key, Fn);
					FuncDefs.Add(Key, F);
					Fn->Next = C->Children;
					C->Children = Fn;
				}
			}
		}
	}

	void FillClass(const FJObj& Def)
	{
		const FString Path = StrField(Def, TEXT("p"));
		if (ExistingBefore.Contains(Path) || Failed.Contains(Path))
		{
			return;
		}
		UClass* C = Cast<UClass>(Made.FindRef(Path));
		if (!C) { return; }
		UClass* Super = C->GetSuperClass();
		if (Super && Failed.Contains(Super->GetPathName()))
		{
			Failed.Add(Path);
			Stats.ClassesFailed++;
			return;
		}
		FSzGeneratedType Rec;
		Rec.Struct = C;
		if (!BuildProps(C, Def, Path, &Rec))
		{
			Failed.Add(Path);
			Stats.ClassesFailed++;
			UE_LOG(LogSuzie, Error, TEXT("Class %s failed"), *Path);
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Fns;
		if (Def->TryGetArrayField(TEXT("fn"), Fns))
		{
			for (const TSharedPtr<FJsonValue>& V : *Fns)
			{
				FJObj F = V->AsObject();
				const FString Kind = StrField(F, TEXT("k"));
				if (Kind == TEXT("F"))
				{
					UFunction* Fn = MakeFunctionShell(C, F);
					FillFunction(Fn, F, Path + TEXT(":") + StrField(F, TEXT("n")));
					Fn->Next = C->Children;
					C->Children = Fn;
				}
			}
			// function map (includes the delegate signatures created at shell time)
			for (UField* Ch = C->Children; Ch; Ch = Ch->Next)
			{
				if (UFunction* Fn = Cast<UFunction>(Ch))
				{
					C->AddFunctionToFunctionMap(Fn, Fn->GetFName());
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Sos;
		if (Def->TryGetArrayField(TEXT("so"), Sos))
		{
			TArray<TPair<FName, UClass*>>& Subs = GSubobjectDefs.FindOrAdd(C);
			for (const TSharedPtr<FJsonValue>& V : *Sos)
			{
				FJObj So = V->AsObject();
				if (So->HasField(TEXT("outer")) && !StrField(So, TEXT("n")).Contains(TEXT(":")))
				{
					continue;
				}
				if (StrField(So, TEXT("n")).Contains(TEXT(":")))
				{
					// nested default subobject "Outer:Name": created by the generated constructor of the outer subobject's class
					FString OuterName = StrField(So, TEXT("outer")), Leaf = StrField(So, TEXT("n"));
					Leaf = Leaf.Mid(Leaf.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
					if (OuterName.Contains(TEXT(":")) || StrField(So, TEXT("fl")).Contains(TEXT("Transient")))
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& V2 : *Sos)
					{
						FJObj O2 = V2->AsObject();
						if (StrField(O2, TEXT("n")) == OuterName && !O2->HasField(TEXT("outer")))
						{
							UClass* OC = Resolve<UClass>(StrField(O2, TEXT("c")));
							UClass* NC = Resolve<UClass>(StrField(So, TEXT("c")));
							if (OC && NC && Made.FindRef(StrField(O2, TEXT("c"))) == OC && !ExistingBefore.Contains(StrField(O2, TEXT("c"))))
							{
								TArray<TPair<FName, UClass*>>& NS = GSubobjectDefs.FindOrAdd(OC);
								const FName LN(*Leaf);
								if (!NS.ContainsByPredicate([&](const TPair<FName, UClass*>& E) { return E.Key == LN; })) NS.Emplace(LN, NC);
							}
							break;
						}
					}
					continue;
				}
				if (UClass* SC = Resolve<UClass>(StrField(So, TEXT("c"))))
				{
					Subs.Emplace(FName(*StrField(So, TEXT("n"))), SC);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Ifs;
		if (Def->TryGetArrayField(TEXT("if"), Ifs))
		{
			for (const TSharedPtr<FJsonValue>& V : *Ifs)
			{
				FJObj I = V->AsObject();
				UClass* IC = Resolve<UClass>(StrField(I, TEXT("c")));
				// Only generated interfaces: for an engine interface (INotifyFieldValueChanged, IUserObjectListEntry...) native code
				// would Cast to the C++ interface through the entry, but the C++ object is the engine ancestor and does not
				// implement it (PointerOffset 0 = "implemented by K2" gives null, and e.g. UBlueprintGeneratedClass::InitializeFieldNotifies
				// dereferences it).
				if (IC && GGeneratedClasses.Contains(IC))
				{
					C->Interfaces.Add(FImplementedInterface(IC, 0, I->GetNumberField(TEXT("k2")) != 0));
				}
			}
		}

		C->Bind();
		C->StaticLink(true);
		C->AssembleReferenceTokenStream();
		FSzBakedGenerator::GetGenerated().Add(Path, MoveTemp(Rec));
		Notify(GetPackage(PackageOfPath(Path)), C, ENotifyRegistrationType::NRT_Class);
		Stats.Classes++;
	}

	// ------------------------------------------------------------------ engine patches

	static void RelinkWithDerived(UStruct* S)
	{
		S->StaticLink(false); // keeps offsets; also drops the cached unversioned schema
		if (UClass* C = Cast<UClass>(S))
		{
			TArray<UClass*> Derived;
			GetDerivedClasses(C, Derived, true);
			// super first
			Derived.Sort([](const UClass& A, const UClass& B)
			{
				int32 DA = 0, DB = 0;
				for (const UClass* K = &A; K; K = K->GetSuperClass()) ++DA;
				for (const UClass* K = &B; K; K = K->GetSuperClass()) ++DB;
				return DA < DB;
			});
			for (UClass* D : Derived)
			{
				D->StaticLink(false);
			}
		}
	}

	/** For every engine type whose game layout differs (types[] of the records derived from the jmap, with the game's full property list in
	 *  game_props) build a recipe: the game's properties in the game's order; a live property is used when name and type match,
	 *  otherwise a standalone shadow property (discarded on load) stands in. Consumed by SzSchemaOverrideProvider. */
	void BuildOverrides(const FJObj& D)
	{
		if (!D.IsValid())
		{
			return;
		}
		int32 Types = 0, Shadows = 0, Skipped = 0;
		UObject* ShadowOwner = GetTransientPackage();
		for (const TSharedPtr<FJsonValue>& V : D->GetArrayField(TEXT("types")))
		{
			FJObj T = V->AsObject();
			const FString Path = StrField(T, TEXT("p"));
			const TArray<TSharedPtr<FJsonValue>>* GP;
			if (!T->TryGetArrayField(TEXT("game_props"), GP))
			{
				continue;
			}
			UStruct* S = Resolve<UStruct>(Path);
			if (!S)
			{
				UE_LOG(LogSuzie, Warning, TEXT("Engine diff: %s not available"), *Path);
				Skipped++;
				continue;
			}
			TMap<FString, FProperty*> Live;
			for (FField* F = S->ChildProperties; F; F = F->Next)
			{
				if (FProperty* P = CastField<FProperty>(F))
				{
					if (P->GetOwner<UObject>() == S) Live.Add(P->GetName(), P);
				}
			}
			TArray<FRecipeEntry> Recipe;
			bool bOk = true;
			int32 NewShadows = 0;
			for (const TSharedPtr<FJsonValue>& PV : *GP)
			{
				FJObj PJ = PV->AsObject();
				const FString Name = StrField(PJ, TEXT("n"));
				const FString Type = StrField(PJ, TEXT("t"));
				double Dim = 1;
				PJ->TryGetNumberField(TEXT("d"), Dim);
				FRecipeEntry E;
				E.Dim = FMath::Max(1, (int32)Dim);
				FProperty** L = Live.Find(Name);
				if (L && (*L)->GetClass()->GetName() == Type && (*L)->ArrayDim == E.Dim)
				{
					bool bSameStruct = true;
					if (FStructProperty* LS = CastField<FStructProperty>(*L))
					{
						bSameStruct = LS->Struct && LS->Struct->GetPathName() == StrField(PJ, TEXT("st"));
					}
					if (bSameStruct)
					{
						E.Property = *L;
						Recipe.Add(E);
						continue;
					}
				}
				bool bBuilt = true;
				// A discarded shadow is read with SerializeItem, which reads a byte/enum property that has an enum BY NAME; the unversioned
				// format stores the raw integer (what a live property reads). Engines whose discard path reads integer-like values like live
				// properties (UE_MODKIT_SCHEMA_HOOK_LEVEL >= 3) do not need this; for older ones a top-level enum shadow becomes the plain
				// integer property of the same size, which consumes exactly the same bytes.
				FJObj ShadowDef = PJ;
#if !SUZIE_HAS_HOOK_LEVEL3
				if (Type == TEXT("EnumProperty") || (Type == TEXT("ByteProperty") && PJ->HasField(TEXT("en"))))
				{
					ShadowDef = MakeShared<FJsonObject>();
					for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : PJ->Values)
					{
						ShadowDef->Values.Add(KV.Key, KV.Value);
					}
					ShadowDef->RemoveField(TEXT("en"));
					if (Type == TEXT("EnumProperty"))
					{
						ShadowDef->SetStringField(TEXT("t"), StrField(PJ, TEXT("u")));
						ShadowDef->RemoveField(TEXT("u"));
					}
				}
#endif
				FProperty* P = BuildProp(ShadowOwner, ShadowDef, Path, bBuilt);
				if (!P || !bBuilt)
				{
					UE_LOG(LogSuzie, Warning, TEXT("Engine diff %s: cannot build shadow for %s, type not overridden"), *Path, *Name);
					bOk = false;
					break;
				}
				P->ArrayDim = 1; // dim handled by recipe expansion; size per element
				P->ArrayDim = E.Dim;
				FArchive Ar;
				P->LinkWithoutChangingOffset(Ar);
				E.Property = P;
				E.bDiscard = true;
				NewShadows++;
				Recipe.Add(E);
			}
			if (!bOk)
			{
				Skipped++;
				continue;
			}
			Shadows += NewShadows;
			Types++;
			GRecipes.Add(S, MoveTemp(Recipe));
		}
		Stats.OverrideTypes = Types; Stats.OverrideShadows = Shadows; Stats.OverrideSkipped = Skipped;
		UE_LOG(LogSuzie, Log, TEXT("Schema overrides: %d types, %d shadow properties, %d skipped"), Types, Shadows, Skipped);
#if SUZIE_HAS_SCHEMA_OVERRIDE
		UE::UnversionedSchema::SetOverrideProvider(&SzSchemaOverrideProvider);
		UE::UnversionedSchema::FlushAllUnversionedSchemas();
#endif
	}

	// ------------------------------------------------------------------ driver

	FSzExtensionRegistry ExtReg;

	/** Loads the profile's game extension module (if any), lets it register its named ops/handlers and defers the struct ops the profile binds. */
	void LoadExtension()
	{
		if (!Profile->ExtensionModule.IsEmpty())
		{
			if (!FModuleManager::Get().LoadModule(*Profile->ExtensionModule))
			{
				UE_LOG(LogSuzie, Error, TEXT("Profile '%s': extension module %s could not be loaded; native struct ops and post-serialize handlers are unavailable"), *Profile->Id, *Profile->ExtensionModule);
			}
		}
		const FName Id(*Profile->Id);
		for (ISuzieGameExtension* Ext : IModularFeatures::Get().GetModularFeatureImplementations<ISuzieGameExtension>(ISuzieGameExtension::GetModularFeatureName()))
		{
			if (Ext->GetProfileId() == Id)
			{
				Ext->Register(ExtReg);
			}
		}
		for (const TPair<FString, FString>& Op : Profile->NativeStructOps)
		{
			const TFunction<UScriptStruct::ICppStructOps*()>* Make = ExtReg.StructOps.Find(FName(*Op.Value));
			if (!Make)
			{
				UE_LOG(LogSuzie, Error, TEXT("native_ops %s -> %s: not registered by the extension, the struct is generated as a plain script struct"), *Op.Key, *Op.Value);
				continue;
			}
			UScriptStruct::DeferCppStructOps(FTopLevelAssetPath(Op.Key), (*Make)());
			Stats.NativeStructOps++;
			UE_LOG(LogSuzie, Log, TEXT("Native struct ops %s for %s"), *Op.Value, *Op.Key);
		}
	}

	bool Run(const FSzJmapTypes& Source)
	{
		const double T0 = FPlatformTime::Seconds();
		Root = Source.Types;
		// flags that property creation derives on its own
		DerivedMask = CPF_ZeroConstructor | CPF_IsPlainOldData | CPF_NoDestructor | CPF_HasGetValueTypeHash | CPF_UObjectWrapper | CPF_TObjectPtr;

		const TArray<TSharedPtr<FJsonValue>>& Enums = Root->GetArrayField(TEXT("enums"));
		const TArray<TSharedPtr<FJsonValue>>& Structs = Root->GetArrayField(TEXT("structs"));
		const TArray<TSharedPtr<FJsonValue>>& Classes = Root->GetArrayField(TEXT("classes"));
		const TArray<TSharedPtr<FJsonValue>>* Delegates = nullptr;
		Root->TryGetArrayField(TEXT("delegates"), Delegates);

		LoadExtension();
		for (const auto& V : Enums) MakeEnum(V->AsObject());

		// class shells, super first (file order)
		for (const auto& V : Classes)
		{
			FJObj D = V->AsObject();
			ClassDefs.Add(StrField(D, TEXT("p")), D);
			ClassOrder.Add(StrField(D, TEXT("p")));
			MakeClassShell(D);
		}
		// struct shells
		for (const auto& V : Structs)
		{
			FJObj D = V->AsObject();
			StructDefs.Add(StrField(D, TEXT("p")), D);
			MakeStructShell(D);
		}
		// package level delegate signature shells
		TArray<FJObj> NewDelegates;
		if (Delegates)
		{
			for (const auto& V : *Delegates)
			{
				FJObj D = V->AsObject();
				const FString Path = StrField(D, TEXT("p"));
				double Pe = 0;
				D->TryGetNumberField(TEXT("pe"), Pe);
				if (Pe != 0 || FindObject<UFunction>(nullptr, *Path))
				{
					if (UFunction* Ex = FindObject<UFunction>(nullptr, *Path))
					{
						Made.Add(Path, Ex);
					}
					continue;
				}
				UPackage* Pkg = GetPackage(PackageOfPath(Path));
				UFunction* Fn = MakeFunctionShell(Pkg, D);
				Made.Add(Path, Fn);
				FuncDefs.Add(Path, D);
				NewDelegates.Add(D);
			}
		}

		// structs (members first)
		for (const auto& V : Structs) FillStruct(V->AsObject());

		// delegate signatures (package level and class scoped)
		for (const TPair<FString, FJObj>& KV : FuncDefs)
		{
			UFunction* Fn = Cast<UFunction>(Made.FindRef(KV.Key));
			if (Fn)
			{
				FillFunction(Fn, KV.Value, KV.Key);
				Stats.Delegates++;
			}
		}
		for (const FJObj& D : NewDelegates)
		{
			const FString Path = StrField(D, TEXT("p"));
			if (UFunction* Fn = Cast<UFunction>(Made.FindRef(Path)))
			{
				Notify(GetPackage(PackageOfPath(Path)), Fn, ENotifyRegistrationType::NRT_NoExportObject);
			}
		}

		// classes (super first)
		for (const auto& V : Classes) FillClass(V->AsObject());
		// functions the game has on engine classes that the editor lacks or has with another signature
		{
			TMap<FString, TArray<TSharedPtr<FJsonValue>>> EngineFns;
			const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
			if (Root->TryGetArrayField(TEXT("engine_class_functions"), Listed))
			{
				for (const TSharedPtr<FJsonValue>& V : *Listed)
				{
					FJObj F = V->AsObject();
					EngineFns.FindOrAdd(StrField(F, TEXT("owner"))).Add(V);
				}
			}
			for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& KV : EngineFns)
			{
				EngineClassFunctions(KV.Key, KV.Value);
			}
		}
		UE_LOG(LogSuzie, Display, TEXT("engine class functions: added %d (native %d, stub %d), replaced %d, skipped %d, signature differs %d"),
			EngineFuncNative + EngineFuncStub, EngineFuncNative, EngineFuncStub, EngineFuncReplaced, EngineFuncSkipped, EngineFuncDiffers);

#if SUZIE_HAS_HOOK_LEVEL2
		for (const FSzProfile::FPostSerialize& H : Profile->PostSerializeHandlers)
		{
			const FSzPostSerializeFn* Fn = ExtReg.PostSerialize.Find(FName(*H.Handler));
			UClass* C = Resolve<UClass>(H.Class);
			if (!Fn || !C)
			{
				UE_LOG(LogSuzie, Error, TEXT("post_serialize %s -> %s: %s"), *H.Class, *H.Handler, !Fn ? TEXT("handler not registered by the extension") : TEXT("class not found"));
				continue;
			}
			UE::NativeSerialize::AddPostSerialize(C, *Fn);
			Stats.PostSerializeHandlers++;
			UE_LOG(LogSuzie, Log, TEXT("Post-serialize handler %s on %s"), *H.Handler, *H.Class);
		}
#endif
#if SUZIE_HAS_SCHEMA_OVERRIDE
		for (const FString& SkipPath : Profile->SkipClasses)
		{
			if (UClass* C = Resolve<UClass>(SkipPath))
			{
				UE::NativeSerialize::AddSkipClass(C);
				UE_LOG(LogSuzie, Log, TEXT("Native serialize skip: %s"), *SkipPath);
			}
		}
#endif
		if (GRecipes.Num() == 0)
		{
			BuildOverrides(Source.EngineDiff);
		}

		GFillDone = true;
		// CDOs (class order is super first: a class's shell needs its super)
		const bool bCdoValues = !FParse::Param(FCommandLine::Get(), TEXT("SuzieNoCdoValues"));
		GCdoValueProps.Reset();
		GCdoRefProps.Reset();
		int32 CdoCount = 0;
		for (const FString& Path : ClassOrder)
		{
			if (ExistingBefore.Contains(Path) || Failed.Contains(Path)) continue;
			if (UClass* C = Cast<UClass>(Made.FindRef(Path)))
			{
				if (C->IsChildOf(UObject::StaticClass()) && C->GetSuperClass() && !Failed.Contains(C->GetSuperClass()->GetPathName()))
				{
					UObject* CDO = C->GetDefaultObject();
					CdoCount++;
					if (bCdoValues)
					{
						ApplyCdoValues(C, CDO, ClassDefs.FindRef(Path));
					}
				}
			}
		}
		if (bCdoValues)
		{
			UE_LOG(LogSuzie, Log, TEXT("Suzie: CDO values: classes=%d applied=%d skipped=%d typeMismatch=%d unresolvedObjects=%d subobjects=%d cdoInternal=%d instancedApplied=%d instancedForeign=%d"),
				ValueStats.Classes, ValueStats.Applied, ValueStats.Skipped, ValueStats.TypeMismatch, ValueStats.UnresolvedObjects, ValueStats.Subobjects, ValueStats.CdoInternal,
				ValueStats.InstancedApplied, ValueStats.InstancedForeign);
			UE_LOG(LogSuzie, Log, TEXT("Suzie: CDO references: remapped=%d nulled=%d"), GRefsRemapped, GRefsNulled);
		}
		else
		{
			UE_LOG(LogSuzie, Log, TEXT("Suzie: CDO values disabled by -SuzieNoCdoValues"));
		}
		Stats.Seconds = FPlatformTime::Seconds() - T0;
		UE_LOG(LogSuzie, Log, TEXT("CDOs created: %d"), CdoCount);
		return true;
	}
};
} // namespace

void FSzBakedGenerator::GetReferenceRemapCounts(int32& OutRemapped, int32& OutNulled)
{
	OutRemapped = GRefsRemapped;
	OutNulled = GRefsNulled;
}

bool FSzBakedGenerator::Generate(const FSzProfile& Profile, const FSzJmapTypes& Source, FSzGenStats& OutStats)
{
	FGen Gen;
	Gen.Profile = &Profile;
	const bool bOk = Gen.Run(Source);
	OutStats = Gen.Stats;
	return bOk;
}
