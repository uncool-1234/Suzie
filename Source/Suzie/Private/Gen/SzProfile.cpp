// Generic, game agnostic.
#include "SzProfile.h"
#include "SuziePlugin.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
TSharedPtr<FJsonObject> ReadJson(const FString& File)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		UE_LOG(LogSuzie, Error, TEXT("Profile: JSON parse failed: %s"), *File);
		return nullptr;
	}
	return Obj;
}

FString Resolve(const FString& Dir, const FString& Rel)
{
	if (Rel.IsEmpty())
	{
		return Rel;
	}
	return FPaths::IsRelative(Rel) ? FPaths::ConvertRelativePathToFull(Dir / Rel) : Rel;
}
}

bool FSzProfile::Load(FSzProfile& Out, bool bQuiet)
{
	FString Spec;
	if (!FParse::Value(FCommandLine::Get(), TEXT("SuzieProfile="), Spec))
	{
		FConfigFile Local;
		Local.Read(FPaths::ProjectDir() / TEXT("ModkitLocal.ini"));
		Local.GetString(TEXT("Local"), TEXT("SuzieProfile"), Spec);
	}
	Spec = Spec.TrimQuotes().TrimStartAndEnd();
	if (Spec.IsEmpty())
	{
		// No explicit Suzie profile: use the engine profile directory (-ModkitProfile= / ModkitLocal.ini [Local] Profile=) when it is a Suzie profile too.
		FString EngineProfile;
		if (!FParse::Value(FCommandLine::Get(), TEXT("ModkitProfile="), EngineProfile))
		{
			FConfigFile Local;
			Local.Read(FPaths::ProjectDir() / TEXT("ModkitLocal.ini"));
			Local.GetString(TEXT("Local"), TEXT("Profile"), EngineProfile);
		}
		EngineProfile = EngineProfile.TrimQuotes().TrimStartAndEnd();
		if (!EngineProfile.IsEmpty() && FPaths::FileExists(FPaths::ConvertRelativePathToFull(EngineProfile) / TEXT("profile.json")))
		{
			Spec = EngineProfile;
		}
	}
	if (Spec.IsEmpty())
	{
		return false;
	}
	FString Dir = Spec;
	if (!Spec.Contains(TEXT("/")) && !Spec.Contains(TEXT("\\")))
	{
		const TSharedPtr<IPlugin> Self = IPluginManager::Get().FindPlugin(TEXT("Suzie"));
		Dir = (Self.IsValid() ? Self->GetBaseDir() : FString()) / TEXT("Profiles") / Spec;
	}
	Dir = FPaths::ConvertRelativePathToFull(Dir);
	const TSharedPtr<FJsonObject> P = ReadJson(Dir / TEXT("profile.json"));
	if (!P.IsValid())
	{
		if (!bQuiet)
		{
			UE_LOG(LogSuzie, Error, TEXT("Profile: cannot read %s/profile.json"), *Dir);
		}
		return false;
	}
	Out = FSzProfile();
	Out.Dir = Dir;
	P->TryGetStringField(TEXT("id"), Out.Id);
	FString S;
	if (P->TryGetStringField(TEXT("skip"), S)) Out.SkipFile = Resolve(Dir, S);
	P->TryGetStringField(TEXT("extension_module"), Out.ExtensionModule);
	if (P->TryGetStringField(TEXT("native_ops"), S))
	{
		if (const TSharedPtr<FJsonObject> O = ReadJson(Resolve(Dir, S)))
		{
			const TArray<TSharedPtr<FJsonValue>>* Ops;
			if (O->TryGetArrayField(TEXT("structs"), Ops))
			{
				for (const TSharedPtr<FJsonValue>& V : *Ops)
				{
					const TSharedPtr<FJsonObject> E = V->AsObject();
					FString Path, OpsName;
					if (E.IsValid() && E->TryGetStringField(TEXT("path"), Path) && E->TryGetStringField(TEXT("ops"), OpsName)) Out.NativeStructOps.Add(Path, OpsName);
				}
			}
		}
	}
	if (P->TryGetStringField(TEXT("tags"), S)) Out.TagsFile = Resolve(Dir, S);
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (P->TryGetArrayField(TEXT("conflicting_plugins"), Arr))
	{
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			FConflict C;
			if (O.IsValid() && O->TryGetStringField(TEXT("plugin"), C.Plugin))
			{
				O->TryGetStringField(TEXT("disable_switch"), C.DisableSwitch);
				Out.ConflictingPlugins.Add(C);
			}
		}
	}
	if (P->TryGetStringField(TEXT("overrides"), S))
	{
		if (const TSharedPtr<FJsonObject> O = ReadJson(Resolve(Dir, S)))
		{
			const TSharedPtr<FJsonObject>* Cl;
			if (O->TryGetObjectField(TEXT("classes"), Cl) && (*Cl)->TryGetArrayField(TEXT("abstract_if_derived_from"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr) Out.AbstractIfDerivedFrom.Add(V->AsString());
			}
			const TSharedPtr<FJsonObject>* St;
			if (O->TryGetObjectField(TEXT("structs"), St) && (*St)->TryGetArrayField(TEXT("drop_super"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr) Out.DropStructSupers.Add(V->AsString());
			}
			const TSharedPtr<FJsonObject>* Row;
			if (O->TryGetObjectField(TEXT("structs"), St) && (*St)->TryGetObjectField(TEXT("row_struct"), Row))
			{
				FString Mode;
				(*Row)->TryGetStringField(TEXT("base"), Out.RowStructBase);
				(*Row)->TryGetStringField(TEXT("mode"), Mode);
				Out.bRowStructKeepSuper = Mode == TEXT("keep_super_guarded");
			}
		}
	}
	if (P->TryGetStringField(TEXT("aliases"), S))
	{
		if (const TSharedPtr<FJsonObject> O = ReadJson(Resolve(Dir, S)))
		{
			const TSharedPtr<FJsonObject>* Ty;
			if (O->TryGetObjectField(TEXT("types"), Ty))
			{
				for (const auto& KV : (*Ty)->Values) Out.Aliases.Add(KV.Key, KV.Value->AsString());
			}
			O->TryGetBoolField(TEXT("by_leaf_fallback"), Out.bAliasByLeaf);
		}
	}
	if (!Out.SkipFile.IsEmpty())
	{
		if (const TSharedPtr<FJsonObject> O = ReadJson(Out.SkipFile))
		{
			if (O->TryGetArrayField(TEXT("skip_export_classes"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					FString C;
					if (V->AsObject().IsValid() && V->AsObject()->TryGetStringField(TEXT("class"), C)) Out.SkipClasses.Add(C);
				}
			}
			if (O->TryGetArrayField(TEXT("post_serialize"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					FSzProfile::FPostSerialize H;
					if (V->AsObject().IsValid() && V->AsObject()->TryGetStringField(TEXT("class"), H.Class) && V->AsObject()->TryGetStringField(TEXT("handler"), H.Handler)) Out.PostSerializeHandlers.Add(H);
				}
			}
		}
	}
	if (!Out.TagsFile.IsEmpty())
	{
		if (const TSharedPtr<FJsonObject> O = ReadJson(Out.TagsFile))
		{
			O->TryGetStringField(TEXT("probe_tag"), Out.ProbeTag);
			O->TryGetStringField(TEXT("tags_ini"), Out.TagsIni);
		}
	}
	return true;
}
