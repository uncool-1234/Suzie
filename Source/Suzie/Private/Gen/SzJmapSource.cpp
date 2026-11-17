// Generic, game agnostic.
//
// Record keys of the derived types (consumed by SzBakedGenerator.cpp):
//   class    p path, sup super, fl class flags (hex), sz properties size, within ClassWithin when not Object,
//            if [{c interface class, o pointer offset, k2}], pr properties, fn functions, so default subobjects [{n, c, fl, outer?}]
//   struct   p, sup, fl struct flags (hex), sz, al alignment, pr
//   enum     p, n [[name, value]], cpp cpp form, ct cpp type
//   function n name, k F|D|S (function, delegate, sparse delegate), f function flags, sup, sz params size, pr params;
//            package level delegates also carry p and pe (1: the package exists in the editor)
//   property n, t property class name, f property flags (hex), o offset, s size, d array dim when not 1, cls, meta, st, en,
//            u underlying type of an EnumProperty, ic, sg signature function, b {fs, bo, bm, fm, nat}, in / key / val inner properties
#include "SzJmapSource.h"
#include "SuziePlugin.h"
#include "SzProfile.h"

#include "Dom/JsonValue.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace
{
#include "SzFlagNames.inl"

using FJObj = TSharedPtr<FJsonObject>;
using FJArr = TArray<TSharedPtr<FJsonValue>>;

const FJArr GEmptyArray;

bool LessCs(const FString& A, const FString& B)
{
	return FCString::Strcmp(*A, *B) < 0;
}

FString Str(const FJObj& O, const TCHAR* Key)
{
	FString S;
	O->TryGetStringField(Key, S);
	return S;
}

const FJArr& Arr(const FJObj& O, const TCHAR* Key)
{
	const FJArr* A = nullptr;
	return O->TryGetArrayField(Key, A) && A ? *A : GEmptyArray;
}

FJObj Sub(const FJObj& O, const TCHAR* Key)
{
	const FJObj* S = nullptr;
	return O->TryGetObjectField(Key, S) && S ? *S : FJObj();
}

double Num(const FJObj& O, const TCHAR* Key)
{
	double D = 0;
	O->TryGetNumberField(Key, D);
	return D;
}

/** "/Script/Mod.Name" -> "/Script/Mod" */
FString PackageOf(const FString& Path)
{
	int32 Dot;
	return Path.FindLastChar(TEXT('.'), Dot) ? Path.Left(Dot) : Path;
}

FString ShortOf(const FString& Path)
{
	int32 Dot;
	return Path.FindLastChar(TEXT('.'), Dot) ? Path.Mid(Dot + 1) : Path;
}

FString Hex(uint64 V)
{
	return FString::Printf(TEXT("0x%llX"), V);
}

struct FFlagTable
{
	TMap<FString, uint64> Map;
	template <int32 N>
	explicit FFlagTable(const TPair<const TCHAR*, uint64> (&Names)[N])
	{
		for (const TPair<const TCHAR*, uint64>& P : Names) Map.Add(P.Key, P.Value);
	}
	uint64 Parse(const FString& Text, TMap<FString, int32>& Unknown) const
	{
		uint64 V = 0;
		TArray<FString> Tokens;
		Text.ParseIntoArray(Tokens, TEXT("|"));
		for (FString& T : Tokens)
		{
			T.TrimStartAndEndInline();
			if (T.IsEmpty()) continue;
			if (const uint64* F = Map.Find(T)) V |= *F;
			else if (T.StartsWith(TEXT("0x"))) V |= FCString::Strtoui64(*T, nullptr, 16);
			else Unknown.FindOrAdd(T)++;
		}
		return V;
	}
};

/** Name, type and dimension of a property in the form both sides are compared in. */
struct FPropKey
{
	FString Name, Type;
	int32 Dim = 1;
	bool operator==(const FPropKey& O) const { return Name.Equals(O.Name, ESearchCase::CaseSensitive) && Type.Equals(O.Type, ESearchCase::CaseSensitive) && Dim == O.Dim; }
};

/** Property names compare case-insensitively (FName) and a deprecated member keeps its place in the schema. */
FString CompareName(const FString& Name)
{
	return (Name.EndsWith(TEXT("_DEPRECATED"), ESearchCase::CaseSensitive) ? Name.LeftChop(11) : Name).ToLower();
}

/** Property class names that serialize alike in the unversioned format count as the same type. */
FString CompareType(const FString& PropertyClass)
{
	const FString T = PropertyClass.Replace(TEXT("Property"), TEXT(""), ESearchCase::CaseSensitive);
	if (T == TEXT("Class")) return TEXT("Object");
	if (T == TEXT("SoftClass") || T == TEXT("AssetObject")) return TEXT("SoftObject");
	if (T == TEXT("Enum")) return TEXT("Byte");
	if (T == TEXT("MulticastInlineDelegate") || T == TEXT("MulticastSparseDelegate")) return TEXT("MulticastDelegate");
	if (T == TEXT("FUtf8Str")) return TEXT("Utf8Str");
	return T;
}

class FDeriver
{
public:
	const TMap<FString, TSharedPtr<FJsonValue>>& O;
	FSzJmapTypes& Out;

	const FFlagTable PropertyFlags{GSzPropertyFlagNames};
	const FFlagTable ClassFlags{GSzClassFlagNames};
	const FFlagTable FunctionFlags{GSzFunctionFlagNames};
	const FFlagTable StructFlags{GSzStructFlagNames};
	TMap<FString, int32> UnknownFlags;

	// live editor reflection
	TSet<FString> EditorTypes;              // "<package>.<name>" of every UStruct in a /Script package
	TSet<FString> EditorPackages;
	TMap<FString, int32> EditorShortCount;
	TMap<FString, const UStruct*> EditorByShort;

	TSet<FString> MissingClasses, MissingStructs, MissingEnums, SuspectEnums;
	TMap<FString, TArray<FString>> Subobjects; // object -> objects directly inside it
	TSet<FString> Emitted;

	FDeriver(const TMap<FString, TSharedPtr<FJsonValue>>& InObjects, FSzJmapTypes& InOut) : O(InObjects), Out(InOut) {}

	FJObj Obj(const FString& Name) const
	{
		const TSharedPtr<FJsonValue>* V = O.Find(Name);
		return V && V->IsValid() && (*V)->Type == EJson::Object ? (*V)->AsObject() : FJObj();
	}

	bool IsType(const FString& Name, const TCHAR* Type) const
	{
		const FJObj V = Obj(Name);
		return V.IsValid() && Str(V, TEXT("type")) == Type;
	}

	void ReadEditor()
	{
		for (TObjectIterator<UStruct> It; It; ++It)
		{
			const FString Package = It->GetOutermost()->GetName();
			if (!Package.StartsWith(TEXT("/Script/"))) continue;
			const FString Name = It->GetName();
			EditorTypes.Add(Package + TEXT(".") + Name);
			EditorPackages.Add(Package);
			EditorShortCount.FindOrAdd(Name)++;
			EditorByShort.Add(Name, *It);
		}
	}

	void Classify()
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : O)
		{
			const FJObj V = KV.Value->AsObject();
			const FString Type = Str(V, TEXT("type"));
			if (Type == TEXT("Class"))
			{
				if (!EditorTypes.Contains(KV.Key)) MissingClasses.Add(KV.Key);
			}
			else if (Type == TEXT("ScriptStruct"))
			{
				if (!EditorTypes.Contains(KV.Key)) MissingStructs.Add(KV.Key);
			}
			else if (Type == TEXT("Enum"))
			{
				// An enum of a package the editor has is taken as present; when the editor has no such enum it is generated only if an
				// engine-type recipe needs it (see Derive).
				if (!EditorPackages.Contains(PackageOf(KV.Key))) MissingEnums.Add(KV.Key);
				else if (!FindObject<UEnum>(nullptr, *KV.Key)) SuspectEnums.Add(KV.Key);
			}
			else if (Type == TEXT("Object"))
			{
				const FString Outer = Str(V, TEXT("outer"));
				if (IsType(Outer, TEXT("Object"))) Subobjects.FindOrAdd(Outer).Add(KV.Key);
			}
		}
	}

	bool IsMissing(const FString& Name) const
	{
		return MissingClasses.Contains(Name) || MissingStructs.Contains(Name) || MissingEnums.Contains(Name);
	}

	// ---- references

	void Refs(const FJObj& P, TSet<FString>* Hard, TSet<FString>* Signatures) const
	{
		FString S;
		if (Hard)
		{
			if (P->TryGetStringField(TEXT("struct"), S) && !S.IsEmpty()) Hard->Add(S);
			if (P->TryGetStringField(TEXT("enum"), S) && !S.IsEmpty()) Hard->Add(S);
		}
		if (Signatures && P->TryGetStringField(TEXT("signature_function"), S) && !S.IsEmpty()) Signatures->Add(S);
		for (const TCHAR* K : {TEXT("inner"), TEXT("key_prop"), TEXT("value_prop"), TEXT("container")})
		{
			if (const FJObj In = Sub(P, K)) Refs(In, Hard, Signatures);
		}
	}

	void PropRefs(const FJObj& V, TSet<FString>* Hard, TSet<FString>* Signatures) const
	{
		for (const TSharedPtr<FJsonValue>& P : Arr(V, TEXT("properties"))) Refs(P->AsObject(), Hard, Signatures);
	}

	// ---- encoding

	FJObj EncProp(const FJObj& P, bool bWithLayout = true)
	{
		FJObj R = MakeShared<FJsonObject>();
		const FString Type = Str(P, TEXT("type"));
		R->SetStringField(TEXT("n"), Str(P, TEXT("name")));
		R->SetStringField(TEXT("t"), Type);
		R->SetStringField(TEXT("f"), Hex(PropertyFlags.Parse(Str(P, TEXT("flags")), UnknownFlags)));
		if (bWithLayout)
		{
			R->SetNumberField(TEXT("o"), Num(P, TEXT("offset")));
			R->SetNumberField(TEXT("s"), Num(P, TEXT("size")));
		}
		const int32 Dim = (int32)Num(P, TEXT("array_dim"));
		if (Dim != 1) R->SetNumberField(TEXT("d"), Dim);
		FString S;
		if (P->TryGetStringField(TEXT("property_class"), S) && !S.IsEmpty() && Type != TEXT("ClassProperty") && Type != TEXT("SoftClassProperty")) R->SetStringField(TEXT("cls"), S);
		if (P->TryGetStringField(TEXT("meta_class"), S) && !S.IsEmpty()) R->SetStringField(TEXT("meta"), S);
		if (P->TryGetStringField(TEXT("struct"), S) && !S.IsEmpty()) R->SetStringField(TEXT("st"), S);
		if (P->TryGetStringField(TEXT("enum"), S) && !S.IsEmpty()) R->SetStringField(TEXT("en"), S);
		if (Type == TEXT("EnumProperty"))
		{
			if (const FJObj C = Sub(P, TEXT("container"))) R->SetStringField(TEXT("u"), Str(C, TEXT("type")));
		}
		if (P->TryGetStringField(TEXT("interface_class"), S) && !S.IsEmpty()) R->SetStringField(TEXT("ic"), S);
		if (P->TryGetStringField(TEXT("signature_function"), S) && !S.IsEmpty()) R->SetStringField(TEXT("sg"), S);
		if (Type == TEXT("BoolProperty"))
		{
			FJObj B = MakeShared<FJsonObject>();
			const int32 ByteMask = (int32)Num(P, TEXT("byte_mask")), FieldMask = (int32)Num(P, TEXT("field_mask"));
			B->SetNumberField(TEXT("fs"), Num(P, TEXT("field_size")));
			B->SetNumberField(TEXT("bo"), Num(P, TEXT("byte_offset")));
			B->SetNumberField(TEXT("bm"), ByteMask);
			B->SetNumberField(TEXT("fm"), FieldMask);
			B->SetNumberField(TEXT("nat"), FieldMask == 255 && ByteMask == 1 ? 1 : 0);
			R->SetObjectField(TEXT("b"), B);
		}
		if (const FJObj In = Sub(P, TEXT("inner"))) R->SetObjectField(TEXT("in"), EncProp(In, false));
		if (const FJObj In = Sub(P, TEXT("key_prop"))) R->SetObjectField(TEXT("key"), EncProp(In, false));
		if (const FJObj In = Sub(P, TEXT("value_prop"))) R->SetObjectField(TEXT("val"), EncProp(In, false));
		return R;
	}

	FJArr EncProps(const FJObj& V)
	{
		FJArr R;
		for (const TSharedPtr<FJsonValue>& P : Arr(V, TEXT("properties"))) R.Add(MakeShared<FJsonValueObject>(EncProp(P->AsObject())));
		return R;
	}

	FJObj EncFunc(const FString& Name, const FJObj& V)
	{
		FJObj R = MakeShared<FJsonObject>();
		int32 Idx;
		R->SetStringField(TEXT("n"), Name.FindLastChar(TEXT(':'), Idx) ? Name.Mid(Idx + 1) : ShortOf(Name));
		const FString Class = Str(V, TEXT("class"));
		R->SetStringField(TEXT("k"), Class == TEXT("/Script/CoreUObject.Function") ? TEXT("F") : Class == TEXT("/Script/CoreUObject.DelegateFunction") ? TEXT("D") : Class == TEXT("/Script/CoreUObject.SparseDelegateFunction") ? TEXT("S") : TEXT("?"));
		R->SetNumberField(TEXT("f"), (double)FunctionFlags.Parse(Str(V, TEXT("function_flags")), UnknownFlags));
		const FString Super = Str(V, TEXT("super_struct"));
		if (!Super.IsEmpty()) R->SetStringField(TEXT("sup"), Super);
		R->SetNumberField(TEXT("sz"), Num(V, TEXT("properties_size")));
		R->SetArrayField(TEXT("pr"), EncProps(V));
		return R;
	}

	FJObj EncSubobject(const FString& Name, const FString& Prefix) const
	{
		const FJObj V = Obj(Name);
		FJObj R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("n"), Name.StartsWith(Prefix, ESearchCase::CaseSensitive) ? Name.Mid(Prefix.Len()) : Name);
		R->SetStringField(TEXT("c"), Str(V, TEXT("class")));
		R->SetStringField(TEXT("fl"), Str(V, TEXT("object_flags")).Replace(TEXT("RF_"), TEXT(""), ESearchCase::CaseSensitive).Replace(TEXT(" | "), TEXT("|")));
		const FString Outer = Str(V, TEXT("outer"));
		if (Outer != Prefix.LeftChop(1) && Outer.StartsWith(Prefix, ESearchCase::CaseSensitive)) R->SetStringField(TEXT("outer"), Outer.Mid(Prefix.Len()));
		return R;
	}

	FJObj EncClass(const FString& Name, const FJObj& V)
	{
		FJObj R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("p"), Name);
		R->SetStringField(TEXT("sup"), Str(V, TEXT("super_struct")));
		R->SetStringField(TEXT("fl"), Hex(ClassFlags.Parse(Str(V, TEXT("class_flags")), UnknownFlags)));
		R->SetNumberField(TEXT("sz"), Num(V, TEXT("properties_size")));
		if (const FJObj Values = Sub(V, TEXT("property_values")))
		{
			const FString Within = Str(Values, TEXT("ClassWithin"));
			if (!Within.IsEmpty() && Within != TEXT("/Script/CoreUObject.Object")) R->SetStringField(TEXT("within"), Within);
		}
		FJArr Interfaces;
		for (const TSharedPtr<FJsonValue>& IV : Arr(V, TEXT("interfaces")))
		{
			const FJObj I = IV->AsObject();
			FJObj E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("c"), Str(I, TEXT("class")));
			E->SetNumberField(TEXT("o"), Num(I, TEXT("pointer_offset")));
			bool bK2 = false;
			I->TryGetBoolField(TEXT("implemented_by_k2"), bK2);
			E->SetNumberField(TEXT("k2"), bK2 ? 1 : 0);
			Interfaces.Add(MakeShared<FJsonValueObject>(E));
		}
		if (Interfaces.Num()) R->SetArrayField(TEXT("if"), Interfaces);
		R->SetArrayField(TEXT("pr"), EncProps(V));
		FJArr Functions;
		for (const TSharedPtr<FJsonValue>& C : Arr(V, TEXT("children")))
		{
			const FString Child = C->AsString();
			const FJObj F = Obj(Child);
			if (F.IsValid() && Str(F, TEXT("type")) == TEXT("Function")) Functions.Add(MakeShared<FJsonValueObject>(EncFunc(Child, F)));
		}
		if (Functions.Num()) R->SetArrayField(TEXT("fn"), Functions);
		// default subobjects: the direct children of the CDO and their direct children
		const FString Cdo = Str(V, TEXT("class_default_object"));
		const FString Prefix = Cdo + TEXT(":");
		FJArr Subs;
		if (const TArray<FString>* Direct = Subobjects.Find(Cdo))
		{
			for (const FString& S : *Direct)
			{
				Subs.Add(MakeShared<FJsonValueObject>(EncSubobject(S, Prefix)));
				if (const TArray<FString>* Nested = Subobjects.Find(S))
				{
					for (const FString& S2 : *Nested) Subs.Add(MakeShared<FJsonValueObject>(EncSubobject(S2, Prefix)));
				}
			}
		}
		if (Subs.Num()) R->SetArrayField(TEXT("so"), Subs);
		return R;
	}

	FJObj EncStruct(const FString& Name, const FJObj& V)
	{
		const uint64 Flags = StructFlags.Parse(Str(V, TEXT("struct_flags")), UnknownFlags);
		FJObj R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("p"), Name);
		const FString Super = Str(V, TEXT("super_struct"));
		if (!Super.IsEmpty()) R->SetStringField(TEXT("sup"), Super);
		R->SetStringField(TEXT("fl"), Hex(Flags));
		R->SetNumberField(TEXT("sz"), Num(V, TEXT("properties_size")));
		R->SetNumberField(TEXT("al"), (double)((int64)Num(V, TEXT("min_alignment")) & 0xFFFF));
		R->SetArrayField(TEXT("pr"), EncProps(V));
		if (Flags & STRUCT_SerializeNative) Out.NativeSerializedStructs.Add(Name);
		return R;
	}

	FJObj EncEnum(const FString& Name, const FJObj& V) const
	{
		FJObj R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("p"), Name);
		R->SetArrayField(TEXT("n"), Arr(V, TEXT("names")));
		R->SetStringField(TEXT("cpp"), Str(V, TEXT("cpp_form")));
		R->SetStringField(TEXT("ct"), Str(V, TEXT("cpp_type")));
		return R;
	}

	// ---- editor types whose game property list differs

	FJArr DiffEditorTypes()
	{
		TArray<FString> Paths;
		TMap<FString, FJObj> Records;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : O)
		{
			const FJObj V = KV.Value->AsObject();
			const FString Type = Str(V, TEXT("type"));
			if (Type != TEXT("Class") && Type != TEXT("ScriptStruct")) continue;
			const FString Short = ShortOf(KV.Key);
			// a short name that several editor types share is ambiguous in cooked data and is left alone
			if (!EditorTypes.Contains(KV.Key) || EditorShortCount.FindRef(Short) > 1) continue;
			const UStruct* Live = EditorByShort.FindRef(Short);
			if (!Live || Live->GetOutermost()->GetName() != PackageOf(KV.Key)) continue;

			TArray<FPropKey> Game, Editor;
			for (const TSharedPtr<FJsonValue>& PV : Arr(V, TEXT("properties")))
			{
				const FJObj P = PV->AsObject();
				Game.Add({CompareName(Str(P, TEXT("name"))), CompareType(Str(P, TEXT("type"))), (int32)Num(P, TEXT("array_dim"))});
			}
			for (TFieldIterator<FProperty> It(Live, EFieldIterationFlags::IncludeDeprecated); It; ++It)
			{
				if (!It->IsEditorOnlyProperty()) Editor.Add({CompareName(It->GetName()), CompareType(It->GetClass()->GetName()), It->ArrayDim});
			}
			if (Game == Editor) continue;

			TSet<FString> GameNames, EditorNames;
			TMap<FString, const FPropKey*> GameByName;
			for (const FPropKey& G : Game) { GameNames.Add(G.Name); GameByName.Add(G.Name, &G); }
			for (const FPropKey& E : Editor) EditorNames.Add(E.Name);
			bool bDiffers = false;
			for (const FPropKey& G : Game) bDiffers |= !EditorNames.Contains(G.Name);
			TArray<FString> CommonGame, CommonEditor;
			for (const FPropKey& G : Game) if (EditorNames.Contains(G.Name)) CommonGame.Add(G.Name);
			for (const FPropKey& E : Editor)
			{
				const FPropKey* const* G = GameByName.Find(E.Name);
				if (!G) { bDiffers = true; continue; }
				CommonEditor.Add(E.Name);
				bDiffers |= !(*G)->Type.Equals(E.Type, ESearchCase::CaseSensitive) || (*G)->Dim != E.Dim;
			}
			bDiffers |= CommonGame != CommonEditor;
			if (!bDiffers) continue;

			FJObj R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("p"), KV.Key);
			R->SetArrayField(TEXT("game_props"), EncProps(V));
			Paths.Add(KV.Key);
			Records.Add(KV.Key, R);
		}
		Paths.Sort(&LessCs);
		FJArr Result;
		for (const FString& P : Paths) Result.Add(MakeShared<FJsonValueObject>(Records[P]));
		return Result;
	}

	void CollectSuspectEnums(const FJObj& Prop, TSet<FString>& Found) const
	{
		for (const TCHAR* K : {TEXT("en"), TEXT("st"), TEXT("cls"), TEXT("meta"), TEXT("ic")})
		{
			const FString S = Str(Prop, K);
			if (!S.IsEmpty() && SuspectEnums.Contains(S)) Found.Add(S);
		}
		for (const TCHAR* K : {TEXT("in"), TEXT("key"), TEXT("val")})
		{
			if (const FJObj In = Sub(Prop, K)) CollectSuspectEnums(In, Found);
		}
	}

	// ---- ordering: dependencies before dependents, otherwise by path

	template <typename DepsFn>
	TArray<FString> Order(const TSet<FString>& Paths, DepsFn&& Deps) const
	{
		TArray<FString> Sorted = Paths.Array();
		Sorted.Sort(&LessCs);
		TArray<FString> Result;
		TMap<FString, uint8> State;
		TFunction<void(const FString&)> Visit = [&](const FString& N)
		{
			uint8& S = State.FindOrAdd(N);
			if (S != 0) return;
			S = 1;
			TArray<FString> D = Deps(N);
			D.Sort(&LessCs);
			for (const FString& X : D)
			{
				if (Paths.Contains(X)) Visit(X);
			}
			State.Add(N, 2);
			Result.Add(N);
		};
		for (const FString& N : Sorted) Visit(N);
		return Result;
	}

	void Derive()
	{
		ReadEditor();
		Classify();

		Out.EngineDiff = MakeShared<FJsonObject>();
		const FJArr Diff = DiffEditorTypes();
		TSet<FString> NeededEnums;
		for (const TSharedPtr<FJsonValue>& T : Diff)
		{
			for (const TSharedPtr<FJsonValue>& P : Arr(T->AsObject(), TEXT("game_props"))) CollectSuspectEnums(P->AsObject(), NeededEnums);
		}
		MissingEnums.Append(NeededEnums);
		Out.EngineDiff->SetArrayField(TEXT("types"), Diff);

		TArray<FString> Enums = MissingEnums.Array();
		Enums.Sort(&LessCs);
		const TArray<FString> Structs = Order(MissingStructs, [this](const FString& N)
		{
			const FJObj V = Obj(N);
			TSet<FString> Hard;
			PropRefs(V, &Hard, nullptr);
			TArray<FString> D;
			const FString Super = Str(V, TEXT("super_struct"));
			if (!Super.IsEmpty()) D.Add(Super);
			for (const FString& H : Hard)
			{
				if (IsType(H, TEXT("ScriptStruct"))) D.AddUnique(H);
			}
			return D;
		});
		const TArray<FString> Classes = Order(MissingClasses, [this](const FString& N)
		{
			TArray<FString> D;
			const FString Super = Str(Obj(N), TEXT("super_struct"));
			if (!Super.IsEmpty()) D.Add(Super);
			return D;
		});

		FJArr E, S, C, D;
		for (const FString& N : Enums) E.Add(MakeShared<FJsonValueObject>(EncEnum(N, Obj(N))));
		for (const FString& N : Structs) S.Add(MakeShared<FJsonValueObject>(EncStruct(N, Obj(N))));
		for (const FString& N : Classes) C.Add(MakeShared<FJsonValueObject>(EncClass(N, Obj(N))));

		// package level delegate signatures that the generated types refer to, and the signatures those refer to
		TSet<FString> Wanted;
		for (const FString& N : Classes)
		{
			const FJObj V = Obj(N);
			PropRefs(V, nullptr, &Wanted);
			for (const TSharedPtr<FJsonValue>& Child : Arr(V, TEXT("children")))
			{
				const FJObj F = Obj(Child->AsString());
				if (F.IsValid() && Str(F, TEXT("type")) == TEXT("Function")) PropRefs(F, nullptr, &Wanted);
			}
		}
		for (const FString& N : Structs) PropRefs(Obj(N), nullptr, &Wanted);
		TArray<FString> Pending = Wanted.Array();
		TSet<FString> Done;
		while (Pending.Num())
		{
			const FString X = Pending.Pop();
			const FJObj V = Obj(X);
			if (Done.Contains(X) || !V.IsValid()) continue;
			Done.Add(X);
			TSet<FString> More;
			PropRefs(V, nullptr, &More);
			Pending.Append(More.Array());
		}
		TArray<FString> Signatures = Done.Array();
		Signatures.Sort(&LessCs);
		for (const FString& X : Signatures)
		{
			if (X.Contains(TEXT(":"))) continue; // class scoped: generated with its class
			FJObj R = EncFunc(X, Obj(X));
			R->SetStringField(TEXT("p"), X);
			R->SetNumberField(TEXT("pe"), EditorPackages.Contains(PackageOf(X)) ? 1 : 0);
			D.Add(MakeShared<FJsonValueObject>(R));
		}

		Out.Types = MakeShared<FJsonObject>();
		Out.Types->SetArrayField(TEXT("enums"), E);
		Out.Types->SetArrayField(TEXT("structs"), S);
		Out.Types->SetArrayField(TEXT("classes"), C);
		Out.Types->SetArrayField(TEXT("delegates"), D);

		ReadTags();
		for (const TPair<FString, int32>& U : UnknownFlags)
		{
			UE_LOG(LogSuzie, Warning, TEXT("jmap: unknown flag name %s (%d uses), ignored"), *U.Key, U.Value);
		}
	}

	void ReadTags()
	{
		static const TCHAR* SettingsCdo = TEXT("/Script/GameplayTags.Default__GameplayTagsSettings");
		const FJObj Cdo = Obj(SettingsCdo);
		const FJObj Values = Cdo.IsValid() ? Sub(Cdo, TEXT("property_values")) : FJObj();
		if (!Values.IsValid()) return;
		Out.bHasTagSettings = true;
		for (const TSharedPtr<FJsonValue>& T : Arr(Values, TEXT("GameplayTagList")))
		{
			const FJObj E = T->AsObject();
			if (E.IsValid()) Out.Tags.Add({Str(E, TEXT("Tag")), Str(E, TEXT("DevComment"))});
		}
		for (const TSharedPtr<FJsonValue>& T : Arr(Values, TEXT("GameplayTagRedirects")))
		{
			const FJObj E = T->AsObject();
			if (E.IsValid()) Out.TagRedirects.Emplace(Str(E, TEXT("OldTagName")), Str(E, TEXT("NewTagName")));
		}
	}
};

bool Build(const FString& File, FSzJmapTypes& Out)
{
	const double T0 = FPlatformTime::Seconds();
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		UE_LOG(LogSuzie, Error, TEXT("Cannot read jmap %s"), *File);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MoveTemp(Text));
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogSuzie, Error, TEXT("jmap %s: JSON parse failed"), *File);
			return false;
		}
	}
	const FJObj Objects = Sub(Root, TEXT("objects"));
	if (!Objects.IsValid())
	{
		UE_LOG(LogSuzie, Error, TEXT("jmap %s: no \"objects\""), *File);
		return false;
	}
	const double T1 = FPlatformTime::Seconds();
	Out.File = File;
	FDeriver Deriver(Objects->Values, Out);
	Deriver.Derive();
	Out.ParseSeconds = T1 - T0;
	Out.DeriveSeconds = FPlatformTime::Seconds() - T1;
	UE_LOG(LogSuzie, Display, TEXT("jmap %s: %d objects, parse %.1fs, derive %.1fs"), *File, Objects->Values.Num(), Out.ParseSeconds, Out.DeriveSeconds);
	return true;
}

void SaveJson(const FJObj& Obj, const FString& File)
{
	FString Text;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}

FString SzJmap::ResolveFile()
{
	FString File;
	if (!FParse::Value(FCommandLine::Get(), TEXT("SuzieJmap="), File))
	{
		FConfigFile Local;
		Local.Read(FSzProfile::LocalIniFile());
		Local.GetString(TEXT("Local"), TEXT("Jmap"), File);
	}
	File = File.TrimQuotes().TrimStartAndEnd();
	return File.IsEmpty() ? File : FPaths::ConvertRelativePathToFull(File);
}

const FSzJmapTypes* SzJmap::Get()
{
	static FSzJmapTypes Types;
	static bool bTried = false, bOk = false;
	if (!bTried)
	{
		bTried = true;
		const FString File = ResolveFile();
		bOk = !File.IsEmpty() && Build(File, Types);
		// -SuzieDumpDerived=<dir>: writes the derived records (types.json, engine_diff.json) for inspection
		FString DumpDir;
		if (bOk && FParse::Value(FCommandLine::Get(), TEXT("SuzieDumpDerived="), DumpDir))
		{
			SaveJson(Types.Types, DumpDir / TEXT("types.json"));
			SaveJson(Types.EngineDiff, DumpDir / TEXT("engine_diff.json"));
		}
	}
	return bOk ? &Types : nullptr;
}
