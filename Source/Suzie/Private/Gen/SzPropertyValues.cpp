#include "SzPropertyValues.h"
#include "SuziePlugin.h"

#include "Dom/JsonValue.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Class.h"
#include "UObject/PropertyOptional.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
int32 GMismatchLogged = 0;

struct FApplyCtx
{
	explicit FApplyCtx(FSzValueStats& InStats) : Stats(InStats) {}

	FSzValueStats& Stats;
	bool bRefersToDefaultObject = false; // an object reference applied for the current top-level property points into a class default object
};

const TCHAR* KindName(const TSharedPtr<FJsonValue>& V)
{
	if (!V.IsValid())
	{
		return TEXT("nothing");
	}
	switch (V->Type)
	{
	case EJson::Null:    return TEXT("null");
	case EJson::String:  return TEXT("string");
	case EJson::Number:  return TEXT("number");
	case EJson::Boolean: return TEXT("boolean");
	case EJson::Object:  return TEXT("object");
	case EJson::Array:   return TEXT("array");
	default:             return TEXT("none");
	}
}

void Mismatch(FApplyCtx& Ctx, const FString& Path, const FString& Expected, const TSharedPtr<FJsonValue>& V)
{
	Ctx.Stats.TypeMismatch++;
	if (GMismatchLogged++ < 20)
	{
		UE_LOG(LogSuzie, Log, TEXT("Property value type mismatch: %s expects %s, JSON has %s"), *Path, *Expected, KindName(V));
	}
}

bool IsInsideDefaultObject(const UObject* Obj)
{
	for (const UObject* O = Obj; O; O = O->GetOuter())
	{
		if (O->HasAnyFlags(RF_ClassDefaultObject))
		{
			return true;
		}
	}
	return false;
}

bool IsSkipped(const FProperty* Prop)
{
	return Prop->HasAnyPropertyFlags(CPF_Deprecated | CPF_InstancedReference | CPF_ContainsInstancedReference)
		|| CastField<FDelegateProperty>(Prop)
		|| CastField<FMulticastDelegateProperty>(Prop)
		|| CastField<FInterfaceProperty>(Prop);
}

bool ApplyValue(FApplyCtx& Ctx, const FProperty* Property, void* Ptr, const TSharedPtr<FJsonValue>& V, const FString& Path);
bool ApplyFields(FApplyCtx& Ctx, const UStruct* Struct, void* Data, const TSharedPtr<FJsonObject>& Values, const FString& Path, bool bTop, TArray<FProperty*>* OutApplied);

bool ApplyEnum(FApplyCtx& Ctx, const FString& Path, const FNumericProperty* Underlying, void* Ptr, const UEnum* Enum, const TSharedPtr<FJsonValue>& V)
{
	int64 Value = 0;
	if (V.IsValid() && V->Type == EJson::String)
	{
		const int32 Index = Enum->GetIndexByNameString(V->AsString(), EGetByNameFlags::None);
		if (Index == INDEX_NONE)
		{
			Mismatch(Ctx, Path, FString::Printf(TEXT("a constant of %s"), *Enum->GetPathName()), V);
			return false;
		}
		Value = Enum->GetValueByIndex(Index);
	}
	else if (V.IsValid() && V->Type == EJson::Number)
	{
		Value = (int64)V->AsNumber();
		if (!Enum->IsValidEnumValue(Value))
		{
			Mismatch(Ctx, Path, FString::Printf(TEXT("a value of %s"), *Enum->GetPathName()), V);
			return false;
		}
	}
	else
	{
		Mismatch(Ctx, Path, FString::Printf(TEXT("a constant of %s"), *Enum->GetPathName()), V);
		return false;
	}
	Underlying->SetIntPropertyValue(Ptr, Value);
	return true;
}

bool ApplyValue(FApplyCtx& Ctx, const FProperty* Property, void* Ptr, const TSharedPtr<FJsonValue>& V, const FString& Path)
{
	const EJson Kind = V.IsValid() ? V->Type : EJson::None;

	if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Property))
	{
		if (Kind == EJson::Null)
		{
			SoftProp->SetPropertyValue(Ptr, FSoftObjectPtr());
			return true;
		}
		if (Kind != EJson::String)
		{
			Mismatch(Ctx, Path, TEXT("a string"), V);
			return false;
		}
		SoftProp->SetPropertyValue(Ptr, FSoftObjectPtr(FSoftObjectPath(V->AsString())));
		return true;
	}

	if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
	{
		if (Kind == EJson::Null)
		{
			ObjProp->SetObjectPropertyValue(Ptr, nullptr);
			return true;
		}
		if (Kind != EJson::String)
		{
			Mismatch(Ctx, Path, TEXT("a string or null"), V);
			return false;
		}
		// objects are never loaded here: a reference whose object does not exist yet keeps the value it has
		UObject* Obj = nullptr;
		const FString ObjPath = V->AsString();
		if (!ObjPath.IsEmpty())
		{
			Obj = StaticFindObject(ObjProp->PropertyClass, nullptr, *ObjPath);
			if (!Obj)
			{
				Ctx.Stats.UnresolvedObjects++;
				return false;
			}
		}
		if (Obj && IsInsideDefaultObject(Obj))
		{
			Ctx.bRefersToDefaultObject = true;
		}
		ObjProp->SetObjectPropertyValue(Ptr, Obj);
		return true;
	}

	if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		if (Kind != EJson::Boolean)
		{
			Mismatch(Ctx, Path, TEXT("a boolean"), V);
			return false;
		}
		BoolProp->SetPropertyValue(Ptr, V->AsBool());
		return true;
	}

	if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Property); NumProp && !NumProp->IsEnum())
	{
		if (Kind == EJson::Number)
		{
			if (NumProp->IsFloatingPoint())
			{
				NumProp->SetFloatingPointPropertyValue(Ptr, V->AsNumber());
			}
			else
			{
				NumProp->SetIntPropertyValue(Ptr, (int64)V->AsNumber());
			}
			return true;
		}
		if (Kind == EJson::String)
		{
			// large integers are written as strings
			NumProp->SetNumericPropertyValueFromString(Ptr, *V->AsString());
			return true;
		}
		Mismatch(Ctx, Path, TEXT("a number"), V);
		return false;
	}

	if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		if (Kind != EJson::String)
		{
			Mismatch(Ctx, Path, TEXT("a string"), V);
			return false;
		}
		NameProp->SetPropertyValue(Ptr, FName(*V->AsString()));
		return true;
	}

	if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		if (Kind != EJson::String)
		{
			Mismatch(Ctx, Path, TEXT("a string"), V);
			return false;
		}
		StrProp->SetPropertyValue(Ptr, V->AsString());
		return true;
	}

	if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		if (Kind != EJson::String)
		{
			Mismatch(Ctx, Path, TEXT("a string"), V);
			return false;
		}
		TextProp->SetPropertyValue(Ptr, FText::AsCultureInvariant(V->AsString()));
		return true;
	}

	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property); EnumProp && EnumProp->GetEnum())
	{
		return ApplyEnum(Ctx, Path, EnumProp->GetUnderlyingProperty(), Ptr, EnumProp->GetEnum(), V);
	}

	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property); ByteProp && ByteProp->Enum)
	{
		return ApplyEnum(Ctx, Path, ByteProp, Ptr, ByteProp->Enum, V);
	}

	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property); StructProp && StructProp->Struct)
	{
		if (Kind != EJson::Object)
		{
			Mismatch(Ctx, Path, TEXT("an object"), V);
			return false;
		}
		return ApplyFields(Ctx, StructProp->Struct, Ptr, V->AsObject(), Path, false, nullptr);
	}

	if (const FFieldPathProperty* FieldPathProp = CastField<FFieldPathProperty>(Property))
	{
		if (Kind != EJson::String)
		{
			Mismatch(Ctx, Path, TEXT("a string"), V);
			return false;
		}
		FieldPathProp->SetPropertyValue(Ptr, TFieldPath<FProperty>(*V->AsString()));
		return true;
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	if (const FOptionalProperty* OptProp = CastField<FOptionalProperty>(Property))
	{
		if (Kind == EJson::Null)
		{
			OptProp->MarkUnset(Ptr);
			return true;
		}
		void* ValuePtr = OptProp->MarkSetAndGetInitializedValuePointerToReplace(Ptr);
		return ApplyValue(Ctx, OptProp->GetValueProperty(), ValuePtr, V, Path);
	}
#endif

	if (const FArrayProperty* ArrProp = CastField<FArrayProperty>(Property))
	{
		if (Kind != EJson::Array)
		{
			Mismatch(Ctx, Path, TEXT("an array"), V);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Elems = V->AsArray();
		FScriptArrayHelper Helper(ArrProp, Ptr);
		Helper.EmptyAndAddValues(Elems.Num());
		bool bOk = true;
		for (int32 i = 0; i < Elems.Num(); ++i)
		{
			const bool bElem = ApplyValue(Ctx, ArrProp->Inner, Helper.GetRawPtr(i), Elems[i], FString::Printf(TEXT("%s[%d]"), *Path, i));
			bOk = bOk && bElem;
		}
		return bOk;
	}

	if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		if (Kind != EJson::Array)
		{
			Mismatch(Ctx, Path, TEXT("an array"), V);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Elems = V->AsArray();
		FScriptSetHelper Helper(SetProp, Ptr);
		Helper.EmptyElements(Elems.Num());
		bool bOk = true;
		for (const TSharedPtr<FJsonValue>& Elem : Elems)
		{
			const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
			const bool bElem = ApplyValue(Ctx, SetProp->ElementProp, Helper.GetElementPtr(Index), Elem, Path);
			bOk = bOk && bElem;
		}
		Helper.Rehash();
		return bOk;
	}

	if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		if (Kind != EJson::Array)
		{
			Mismatch(Ctx, Path, TEXT("an array of [key, value] pairs"), V);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Pairs = V->AsArray();
		FScriptMapHelper Helper(MapProp, Ptr);
		Helper.EmptyValues(Pairs.Num());
		bool bOk = true;
		for (const TSharedPtr<FJsonValue>& Pair : Pairs)
		{
			if (!Pair.IsValid() || Pair->Type != EJson::Array || Pair->AsArray().Num() != 2)
			{
				Mismatch(Ctx, Path, TEXT("a [key, value] pair"), Pair);
				bOk = false;
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>& KeyValue = Pair->AsArray();
			const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
			const bool bKey = ApplyValue(Ctx, MapProp->KeyProp, Helper.GetKeyPtr(Index), KeyValue[0], Path);
			const bool bValue = ApplyValue(Ctx, MapProp->ValueProp, Helper.GetValuePtr(Index), KeyValue[1], Path);
			bOk = bOk && bKey && bValue;
		}
		Helper.Rehash();
		return bOk;
	}

	Ctx.Stats.Skipped++;
	return false;
}

bool ApplyFields(FApplyCtx& Ctx, const UStruct* Struct, void* Data, const TSharedPtr<FJsonObject>& Values, const FString& Path, bool bTop, TArray<FProperty*>* OutApplied)
{
	bool bAll = true;
	for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
	{
		FProperty* Prop = *It;
		const TSharedPtr<FJsonValue>* Found = Values->Values.Find(Prop->GetName());
		if (!Found || !Found->IsValid())
		{
			continue;
		}
		const FString PropPath = Path + TEXT(".") + Prop->GetName();
		if (IsSkipped(Prop))
		{
			Ctx.Stats.Skipped++;
			bAll = false;
			continue;
		}

		const bool bRefBefore = Ctx.bRefersToDefaultObject;
		Ctx.bRefersToDefaultObject = false;
		bool bOk = true;
		if (Prop->ArrayDim != 1)
		{
			if ((*Found)->Type != EJson::Array)
			{
				Mismatch(Ctx, PropPath, TEXT("an array"), *Found);
				bOk = false;
			}
			else
			{
				const TArray<TSharedPtr<FJsonValue>>& Elems = (*Found)->AsArray();
				const int32 Num = FMath::Min(Prop->ArrayDim, Elems.Num());
				for (int32 i = 0; i < Num; ++i)
				{
					void* ElemPtr = Prop->ContainerPtrToValuePtr<void>(Data, i);
					const bool bElem = ApplyValue(Ctx, Prop, ElemPtr, Elems[i], FString::Printf(TEXT("%s[%d]"), *PropPath, i));
					bOk = bOk && bElem;
				}
			}
		}
		else
		{
			bOk = ApplyValue(Ctx, Prop, Prop->ContainerPtrToValuePtr<void>(Data), *Found, PropPath);
		}
		const bool bRefHere = Ctx.bRefersToDefaultObject;
		Ctx.bRefersToDefaultObject = bRefBefore || bRefHere;

		if (!bOk)
		{
			bAll = false;
		}
		if (bTop)
		{
			if (bOk)
			{
				Ctx.Stats.Applied++;
			}
			if (OutApplied && !bRefHere)
			{
				OutApplied->Add(Prop);
			}
		}
	}
	return bAll;
}
} // namespace

bool SzApplyPropertyValues(const UStruct* Struct, void* Data, const TSharedPtr<FJsonObject>& Values, FSzValueStats& Stats, TArray<FProperty*>* OutApplied)
{
	if (!Struct || !Data || !Values.IsValid())
	{
		return false;
	}
	FApplyCtx Ctx(Stats);
	return ApplyFields(Ctx, Struct, Data, Values, Struct->GetPathName(), true, OutApplied);
}
