#include "AssetLoaderModel.h"

#if defined(UE_MODKIT_PACKAGE_FILTER)

#include "SuziePlugin.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const TCHAR* FilterModeName(EAssetFilterMode Mode)
	{
		switch (Mode)
		{
		case EAssetFilterMode::AllowList: return TEXT("AllowList");
		case EAssetFilterMode::DenyList: return TEXT("DenyList");
		default: return TEXT("Off");
		}
	}

	void SortNames(TArray<FName>& Names)
	{
		Names.Sort([](const FName& A, const FName& B) { return A.Compare(B) < 0; });
	}

	// Writes Text to File through a temporary file. Returns false with OutError set on failure.
	bool ReplaceFileWithText(const FString& File, const FString& Text, FString& OutError)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree*/ true);
		const FString Temp = File + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("cannot write '%s'"), *Temp);
			return false;
		}
		if (!IFileManager::Get().Move(*File, *Temp, /*Replace*/ true, /*EvenIfReadOnly*/ true))
		{
			IFileManager::Get().Delete(*Temp);
			OutError = FString::Printf(TEXT("cannot replace '%s'"), *File);
			return false;
		}
		return true;
	}
}

FString FAssetLoaderModel::LoadPremadeRegistry()
{
	TArray<FString> Candidates;
	Candidates.Add(FPaths::ProjectDir() / TEXT("EditorClientAssetRegistry.bin"));
	Candidates.Add(FPaths::ProjectDir() / TEXT("AssetRegistry.bin"));
	if (Modkit::IsActive())
	{
		Candidates.Append(Modkit::GetGameConfig().PremadeRegistryPaths);
	}
	// The first existing candidate wins, as in the engine's premade registry lookup, even if it fails to load.
	for (const FString& Path : Candidates)
	{
		if (!IFileManager::Get().FileExists(*Path))
		{
			continue;
		}
		FAssetRegistryState State;
		if (!FAssetRegistryState::LoadFromDisk(*Path, FAssetRegistryLoadOptions(), State))
		{
			UE_LOG(LogSuzie, Warning, TEXT("Asset Loader: premade asset registry '%s' exists but failed to load."), *Path);
			return FString();
		}
		SetRegistry(MoveTemp(State));
		return Path;
	}
	return FString();
}

void FAssetLoaderModel::SetRegistry(FAssetRegistryState&& State)
{
	Registry = MoveTemp(State);
	bHasRegistry = true;

	struct FBuildEntry
	{
		FAssetLoaderPackage Package;
		bool bPrimaryFound = false;
	};
	TMap<FName, FBuildEntry> Entries;
	Registry.EnumerateAllAssets([&Entries](const FAssetData& AssetData)
	{
		FBuildEntry* Entry = Entries.Find(AssetData.PackageName);
		if (!Entry)
		{
			const FName ShortName(*FPackageName::GetShortName(AssetData.PackageName));
			Entry = &Entries.Add(AssetData.PackageName);
			Entry->Package.PackageName = AssetData.PackageName;
			Entry->Package.PackagePath = FName(*FPackageName::GetLongPackagePath(AssetData.PackageName.ToString()));
			Entry->Package.AssetName = ShortName;
			Entry->Package.AssetClass = AssetData.AssetClassPath.GetAssetName();
		}
		if (!Entry->bPrimaryFound && AssetData.AssetName == FName(*FPackageName::GetShortName(AssetData.PackageName)))
		{
			Entry->bPrimaryFound = true;
			Entry->Package.AssetName = AssetData.AssetName;
			Entry->Package.AssetClass = AssetData.AssetClassPath.GetAssetName();
		}
	});

	Packages.Reset(Entries.Num());
	for (TPair<FName, FBuildEntry>& Pair : Entries)
	{
		Packages.Add(MoveTemp(Pair.Value.Package));
	}
	Packages.Sort([](const FAssetLoaderPackage& A, const FAssetLoaderPackage& B) { return A.PackageName.Compare(B.PackageName) < 0; });

	PackageIndex.Reset();
	PackageIndex.Reserve(Packages.Num());
	for (int32 Index = 0; Index < Packages.Num(); ++Index)
	{
		PackageIndex.Add(Packages[Index].PackageName, Index);
	}
}

bool FAssetLoaderModel::HasRegistry() const
{
	return bHasRegistry;
}

const TArray<FAssetLoaderPackage>& FAssetLoaderModel::GetPackages() const
{
	return Packages;
}

const FAssetLoaderPackage* FAssetLoaderModel::FindPackage(FName PackageName) const
{
	const int32* Index = PackageIndex.Find(PackageName);
	return Index ? &Packages[*Index] : nullptr;
}

EAssetLoadMode FAssetLoaderModel::GetMode() const
{
	return Mode;
}

void FAssetLoaderModel::SetMode(EAssetLoadMode NewMode)
{
	Mode = NewMode;
}

const TSet<FName>& FAssetLoaderModel::GetSelected() const
{
	return Selected;
}

bool FAssetLoaderModel::IsSelected(FName PackageName) const
{
	return Selected.Contains(PackageName);
}

void FAssetLoaderModel::SetSelected(FName PackageName, bool bSelected)
{
	if (bSelected)
	{
		Selected.Add(PackageName);
	}
	else
	{
		Selected.Remove(PackageName);
	}
}

bool FAssetLoaderModel::GetLoadDependencies() const
{
	return bLoadDependencies;
}

void FAssetLoaderModel::SetLoadDependencies(bool bLoad)
{
	bLoadDependencies = bLoad;
}

FAssetFilter FAssetLoaderModel::ComputeFilter() const
{
	FAssetFilter Filter;
	switch (Mode)
	{
	case EAssetLoadMode::All:
		Filter.Mode = EAssetFilterMode::Off;
		break;
	case EAssetLoadMode::None:
		Filter.Mode = EAssetFilterMode::AllowList;
		break;
	case EAssetLoadMode::AllExceptSelected:
		Filter.Mode = EAssetFilterMode::DenyList;
		Filter.Packages = Selected.Array();
		break;
	case EAssetLoadMode::OnlySelected:
	{
		Filter.Mode = EAssetFilterMode::AllowList;
		const TSet<FName> Closure = bLoadDependencies ? ComputeHardDependencyClosure(Selected) : Selected;
		for (const FName PackageName : Closure)
		{
			if (PackageIndex.Contains(PackageName))
			{
				Filter.Packages.Add(PackageName);
			}
		}
		break;
	}
	}
	SortNames(Filter.Packages);
	return Filter;
}

bool FAssetLoaderModel::CanApply() const
{
	return !(Mode == EAssetLoadMode::OnlySelected && Selected.Num() == 0);
}

TSet<FName> FAssetLoaderModel::ComputeHardDependencyClosure(const TSet<FName>& Roots) const
{
	TSet<FName> Visited;
	TArray<FName> Queue;
	for (const FName Root : Roots)
	{
		if (!Root.IsNone() && !Visited.Contains(Root))
		{
			Visited.Add(Root);
			Queue.Add(Root);
		}
	}
	for (int32 Index = 0; Index < Queue.Num(); ++Index)
	{
		TArray<FAssetIdentifier> Dependencies;
		const FName Current = Queue[Index];
		Registry.GetDependencies(FAssetIdentifier(Current), Dependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard);
		for (const FAssetIdentifier& Dependency : Dependencies)
		{
			if (!Dependency.PackageName.IsNone() && !Visited.Contains(Dependency.PackageName))
			{
				Visited.Add(Dependency.PackageName);
				Queue.Add(Dependency.PackageName);
			}
		}
	}
	return Visited;
}

void FAssetLoaderModel::InitializeFromFilter(const FAssetFilter& Filter)
{
	Selected.Reset();
	switch (Filter.Mode)
	{
	case EAssetFilterMode::Off:
		Mode = EAssetLoadMode::All;
		break;
	case EAssetFilterMode::AllowList:
		Mode = Filter.Packages.Num() > 0 ? EAssetLoadMode::OnlySelected : EAssetLoadMode::None;
		break;
	case EAssetFilterMode::DenyList:
		Mode = EAssetLoadMode::AllExceptSelected;
		break;
	}
	for (const FName PackageName : Filter.Packages)
	{
		Selected.Add(PackageName);
	}
}

FString FAssetLoaderModel::ModeName(EAssetLoadMode InMode)
{
	switch (InMode)
	{
	case EAssetLoadMode::All: return TEXT("All");
	case EAssetLoadMode::OnlySelected: return TEXT("OnlySelected");
	case EAssetLoadMode::AllExceptSelected: return TEXT("AllExceptSelected");
	case EAssetLoadMode::None: return TEXT("None");
	}
	return TEXT("All");
}

bool FAssetLoaderModel::ParseModeName(const FString& Name, EAssetLoadMode& OutMode)
{
	for (const EAssetLoadMode Candidate : { EAssetLoadMode::All, EAssetLoadMode::OnlySelected, EAssetLoadMode::AllExceptSelected, EAssetLoadMode::None })
	{
		if (Name.Equals(ModeName(Candidate), ESearchCase::IgnoreCase))
		{
			OutMode = Candidate;
			return true;
		}
	}
	return false;
}

bool FAssetLoaderModel::LoadState(const FString& File)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogSuzie, Warning, TEXT("Asset Loader: '%s' is not valid JSON; ignored."), *File);
		return false;
	}

	FString ModeText;
	EAssetLoadMode ParsedMode;
	if (Root->TryGetStringField(TEXT("mode"), ModeText) && ParseModeName(ModeText, ParsedMode))
	{
		Mode = ParsedMode;
	}
	bool bParsedDependencies = true;
	if (Root->TryGetBoolField(TEXT("load_dependencies"), bParsedDependencies))
	{
		bLoadDependencies = bParsedDependencies;
	}
	const TArray<TSharedPtr<FJsonValue>>* SelectedValues;
	if (Root->TryGetArrayField(TEXT("selected"), SelectedValues))
	{
		Selected.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *SelectedValues)
		{
			FString PackageName;
			if (Value.IsValid() && Value->TryGetString(PackageName) && !PackageName.IsEmpty())
			{
				Selected.Add(FName(*PackageName));
			}
		}
	}
	return true;
}

bool FAssetLoaderModel::SaveState(const FString& File, FString& OutError) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("mode"), ModeName(Mode));
	Root->SetBoolField(TEXT("load_dependencies"), bLoadDependencies);

	TArray<FString> Names;
	for (const FName PackageName : Selected)
	{
		Names.Add(PackageName.ToString());
	}
	Names.Sort();
	TArray<TSharedPtr<FJsonValue>> SelectedValues;
	for (const FString& Name : Names)
	{
		SelectedValues.Add(MakeShared<FJsonValueString>(Name));
	}
	Root->SetArrayField(TEXT("selected"), SelectedValues);

	FString Text;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root, Writer);
	Writer->Close();
	return ReplaceFileWithText(File, Text, OutError);
}

bool FAssetLoaderModel::ReadFilterFile(const FString& File, FAssetFilter& OutFilter, FString& OutError)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *File))
	{
		OutError = FString::Printf(TEXT("cannot read '%s'"), *File);
		return false;
	}
	OutFilter = FAssetFilter();
	bool bHeaderSeen = false;
	for (FString& Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		if (!bHeaderSeen)
		{
			bHeaderSeen = true;
			int32 Eq;
			FString Key;
			FString Value;
			if (Line.FindChar(TEXT('='), Eq))
			{
				Key = Line.Left(Eq);
				Value = Line.Mid(Eq + 1);
				Key.TrimStartAndEndInline();
				Value.TrimStartAndEndInline();
			}
			if (!Key.Equals(TEXT("Mode"), ESearchCase::IgnoreCase))
			{
				UE_LOG(LogSuzie, Warning, TEXT("Asset Loader: '%s' has no Mode line; treated as Off."), *File);
				break;
			}
			if (Value.Equals(TEXT("Off"), ESearchCase::IgnoreCase)) { OutFilter.Mode = EAssetFilterMode::Off; }
			else if (Value.Equals(TEXT("AllowList"), ESearchCase::IgnoreCase)) { OutFilter.Mode = EAssetFilterMode::AllowList; }
			else if (Value.Equals(TEXT("DenyList"), ESearchCase::IgnoreCase)) { OutFilter.Mode = EAssetFilterMode::DenyList; }
			else { UE_LOG(LogSuzie, Warning, TEXT("Asset Loader: '%s' has unknown mode '%s'; treated as Off."), *File, *Value); }
			continue;
		}
		OutFilter.Packages.AddUnique(FName(*Line));
	}
	SortNames(OutFilter.Packages);
	return true;
}

bool FAssetLoaderModel::WriteFilterFile(const FString& File, const FAssetFilter& Filter, FString& OutError)
{
	FString Text = TEXT("; Generated by the Asset Loader window. Read at editor start.\n");
	Text += FString::Printf(TEXT("Mode=%s\n"), FilterModeName(Filter.Mode));
	for (const FName PackageName : Filter.Packages)
	{
		Text += PackageName.ToString();
		Text += TEXT("\n");
	}
	return ReplaceFileWithText(File, Text, OutError);
}

#endif
