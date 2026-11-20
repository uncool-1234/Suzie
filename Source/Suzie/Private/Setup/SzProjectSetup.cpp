// Generic, game agnostic.
#include "SzProjectSetup.h"
#include "../Gen/SzProfile.h"
#include "SuziePlugin.h"
#include "Dom/JsonObject.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IDesktopPlatform.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/ModkitCore.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/AppStyle.h"
#include "UnrealEdMisc.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace SzProjectSetup
{
namespace
{
	FString Clean(const FString& In)
	{
		return In.TrimStartAndEnd().TrimQuotes().TrimStartAndEnd();
	}

	// Full path with forward slashes and no trailing slash; empty stays empty.
	FString Normalize(const FString& In)
	{
		FString Out = Clean(In);
		if (Out.IsEmpty())
		{
			return Out;
		}
		Out = FPaths::ConvertRelativePathToFull(Out);
		FPaths::NormalizeFilename(Out);
		if (Out.Len() > 3 && Out.EndsWith(TEXT("/")))
		{
			Out.LeftChopInline(1);
		}
		return Out;
	}

	FString DataDir()
	{
		return FPaths::ProjectDir() / TEXT("ModkitData");
	}

	bool IsProfileDir(const FString& Dir)
	{
		return !Dir.IsEmpty() && FPaths::FileExists(Dir / TEXT("profile.json")) && FPaths::FileExists(Dir / TEXT("engine.ini")) && FPaths::FileExists(Dir / TEXT("project.json"));
	}

	TArray<FString> FindProfileChoices()
	{
		TArray<FString> Result;
		const TSharedPtr<IPlugin> Suzie = IPluginManager::Get().FindPlugin(TEXT("Suzie"));
		if (!Suzie.IsValid())
		{
			return Result;
		}
		const FString Root = Suzie->GetBaseDir() / TEXT("Profiles");
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Root / TEXT("*")), /*Files*/ false, /*Directories*/ true);
		Names.Sort();
		for (const FString& Name : Names)
		{
			const FString Dir = Normalize(Root / Name);
			if (IsProfileDir(Dir))
			{
				Result.Add(Dir);
			}
		}
		return Result;
	}

	// The [Local] section of ModkitLocal.ini as the engine reads it: last value wins, quotes removed.
	TMap<FString, FString> ReadLocalSection()
	{
		TMap<FString, FString> Values;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Normalize(FSzProfile::LocalIniFile())))
		{
			return Values;
		}
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		FString Section;
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("//")))
			{
				continue;
			}
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				Section = Line.Mid(1, Line.Len() - 2).TrimStartAndEnd();
				continue;
			}
			int32 Eq;
			if (!Line.FindChar(TEXT('='), Eq) || Section != TEXT("Local"))
			{
				continue;
			}
			Values.Add(Line.Left(Eq).TrimStartAndEnd(), Clean(Line.Mid(Eq + 1)));
		}
		return Values;
	}

	TSharedPtr<FJsonObject> ReadJsonFile(const FString& File)
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

	// Files directly in ModkitData with the extension (case-insensitive), normalized and sorted.
	TArray<FString> DataFilesWithExtension(const FString& Extension)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(DataDir() / TEXT("*")), /*Files*/ true, /*Directories*/ false);
		Names.Sort();
		TArray<FString> Result;
		for (const FString& Name : Names)
		{
			if (FPaths::GetExtension(Name, /*bIncludeDot*/ true).Equals(Extension, ESearchCase::IgnoreCase))
			{
				Result.Add(Normalize(DataDir() / Name));
			}
		}
		return Result;
	}

	FString BuildSummary(const FState& State)
	{
		TArray<FString> Items;
		if (State.ProfileDir.IsEmpty())
		{
			Items.Add(TEXT("no profile selected"));
		}
		else if (!State.bProfileInIni)
		{
			Items.Add(TEXT("Profile: not set"));
		}
		for (const FEntry& E : State.Entries)
		{
			if (E.bValid)
			{
				continue;
			}
			FString Reason;
			if (E.bFolder)
			{
				Reason = Normalize(E.Value).IsEmpty() ? TEXT("not set") : FString::Printf(TEXT("does not contain %s"), *E.Probe);
			}
			else if (E.bEngineRejected)
			{
				Reason = TEXT("the engine did not accept this file");
			}
			else if (!E.Candidates.IsEmpty())
			{
				Reason = TEXT("several files in ModkitData, keep one");
			}
			else if (Normalize(E.Value).IsEmpty())
			{
				Reason = TEXT("not set");
			}
			else
			{
				Reason = TEXT("file not found");
			}
			Items.Add(FString::Printf(TEXT("%s: %s"), *E.Label, *Reason));
		}
		return FString::Join(Items, TEXT("; "));
	}

	// Parses "Key=value" tokens of a console line. A value runs to the next known key; quotes around it are removed.
	TMap<FString, FString> ParseOverrides(const FString& Text, const TArray<FString>& Keys)
	{
		struct FHit
		{
			int32 Pos;
			int32 ValueStart;
			FString Key;
		};
		TArray<FHit> Hits;
		for (const FString& Key : Keys)
		{
			const FString Token = Key + TEXT("=");
			int32 From = 0;
			for (;;)
			{
				const int32 Pos = Text.Find(Token, ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
				if (Pos == INDEX_NONE)
				{
					break;
				}
				if (Pos == 0 || Text[Pos - 1] == TEXT(' '))
				{
					Hits.Add({ Pos, Pos + Token.Len(), Key });
				}
				From = Pos + 1;
			}
		}
		Hits.Sort([](const FHit& A, const FHit& B) { return A.Pos < B.Pos; });
		TMap<FString, FString> Result;
		for (int32 i = 0; i < Hits.Num(); ++i)
		{
			const int32 End = i + 1 < Hits.Num() ? Hits[i + 1].Pos : Text.Len();
			Result.Add(Hits[i].Key, Clean(Text.Mid(Hits[i].ValueStart, End - Hits[i].ValueStart)));
		}
		return Result;
	}

	void LogEntries(const FState& State)
	{
		for (const FEntry& E : State.Entries)
		{
			UE_LOG(LogSuzie, Display, TEXT("Modkit project setup: %s = %s (%s)"), *E.Key, *Normalize(E.Value), E.bValid ? TEXT("valid") : TEXT("invalid"));
		}
		UE_LOG(LogSuzie, Display, TEXT("Modkit project setup: profile = %s"), State.ProfileDir.IsEmpty() ? TEXT("not selected") : *State.ProfileDir);
		if (!State.Summary.IsEmpty())
		{
			UE_LOG(LogSuzie, Display, TEXT("Modkit project setup: %s"), *State.Summary);
		}
	}

	void RunProjectSetupCommand(const TArray<FString>& Args)
	{
		FState State = Resolve();
		if (Args.IsEmpty())
		{
			LogEntries(State);
			return;
		}

		TArray<FString> Keys;
		for (const FEntry& E : State.Entries)
		{
			Keys.Add(E.Key);
		}
		const TMap<FString, FString> Overrides = ParseOverrides(FString::Join(Args, TEXT(" ")), Keys);
		for (FEntry& E : State.Entries)
		{
			if (const FString* Value = Overrides.Find(E.Key))
			{
				E.Value = *Value;
				E.bEngineRejected = false;
			}
		}
		Validate(State);
		if (!State.bComplete)
		{
			LogEntries(State);
			UE_LOG(LogSuzie, Warning, TEXT("Modkit project setup: not written, not complete"));
			return;
		}
		TMap<FString, FString> DataFiles;
		FString GameRoot;
		for (const FEntry& E : State.Entries)
		{
			if (E.bFolder)
			{
				GameRoot = Normalize(E.Value);
			}
			else
			{
				DataFiles.Add(E.Key, Normalize(E.Value));
			}
		}
		if (WriteLocalIni(State.ProfileDir, GameRoot, DataFiles))
		{
			UE_LOG(LogSuzie, Display, TEXT("Modkit project setup: wrote %s"), *Normalize(FSzProfile::LocalIniFile()));
		}
		else
		{
			UE_LOG(LogSuzie, Error, TEXT("Modkit project setup: cannot write %s"), *Normalize(FSzProfile::LocalIniFile()));
		}
	}

	FAutoConsoleCommand GProjectSetupCmd(
		TEXT("Suzie.ProjectSetup"),
		TEXT("Suzie.ProjectSetup [GameRoot=<dir>] [<DataKey>=<file> ...]: logs the project's paths; with values, sets them and writes ModkitLocal.ini when all are valid"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { RunProjectSetupCommand(Args); }));

	// ---- Setup window -----------------------------------------------------------------------------------------------

	struct FFormState
	{
		FState State;
		TWeakPtr<SWindow> Window;
		TSharedPtr<SBox> Host;
		TArray<TSharedPtr<FString>> ProfileOptions;
		FString PendingProfile;
		bool bRebuild = false;
		bool bDone = false;
		bool bSaved = false;
	};

	void BrowseEntry(const TSharedRef<FFormState>& Form, int32 Index)
	{
		IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
		if (!Desktop)
		{
			return;
		}
		const void* Parent = nullptr;
		if (const TSharedPtr<SWindow> W = Form->Window.Pin())
		{
			if (W->GetNativeWindow().IsValid())
			{
				Parent = W->GetNativeWindow()->GetOSWindowHandle();
			}
		}
		FEntry& E = Form->State.Entries[Index];
		const FString Current = Normalize(E.Value);
		if (E.bFolder)
		{
			FString Folder;
			const FString Start = FPaths::DirectoryExists(Current) ? Current : FString();
			if (Desktop->OpenDirectoryDialog(Parent, E.Label, Start, Folder))
			{
				E.Value = Normalize(Folder);
				E.bEngineRejected = false;
			}
		}
		else
		{
			const FString Start = (!Current.IsEmpty() && FPaths::FileExists(Current)) ? FPaths::GetPath(Current) : DataDir();
			const FString Types = FString::Printf(TEXT("%s (*%s)|*%s"), *E.Key, *E.Extension, *E.Extension);
			TArray<FString> Files;
			if (Desktop->OpenFileDialog(Parent, E.Label, Start, FString(), Types, 0, Files) && Files.Num() > 0)
			{
				E.Value = Normalize(Files[0]);
				E.bEngineRejected = false;
			}
		}
		Validate(Form->State);
	}

	TSharedRef<SWidget> BuildEntryRow(const TSharedRef<FFormState>& Form, int32 Index)
	{
		const FString Label = Form->State.Entries[Index].Label;
		const FString Description = Form->State.Entries[Index].Description;
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(SBox).WidthOverride(170.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Label)).ToolTipText(FText::FromString(Description))
			]
		];
		Row->AddSlot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(SEditableTextBox)
			.Text_Lambda([Form, Index]() -> FText { return FText::FromString(Form->State.Entries[Index].Value); })
			.OnTextChanged_Lambda([Form, Index](const FText& NewText)
			{
				FEntry& E = Form->State.Entries[Index];
				E.Value = NewText.ToString();
				E.bEngineRejected = false;
				Validate(Form->State);
			})
			.ToolTipText(FText::FromString(Description))
		];
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("...")))
			.ToolTipText(FText::FromString(TEXT("Browse")))
			.OnClicked_Lambda([Form, Index]() -> FReply
			{
				BrowseEntry(Form, Index);
				return FReply::Handled();
			})
		];
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
		[
			SNew(SBox)
			.WidthOverride(48.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([Form, Index]() -> FText
				{
					return FText::FromString(Form->State.Entries[Index].bValid ? TEXT("valid") : TEXT("invalid"));
				})
				.ColorAndOpacity_Lambda([Form, Index]() -> FSlateColor
				{
					return Form->State.Entries[Index].bValid ? FSlateColor(FLinearColor(0.25f, 0.7f, 0.3f)) : FSlateColor(FLinearColor(0.85f, 0.25f, 0.2f));
				})
			]
		];
		return Row;
	}

	TSharedRef<SWidget> BuildForm(const TSharedRef<FFormState>& Form)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		Box->AddSlot().AutoHeight().Padding(0, 0, 0, 12)
		[
			SNew(STextBlock).Text(FText::FromString(TEXT("Paths are not set up, please choose them below.")))
		];

		if (Form->State.ProfileDir.IsEmpty() && Form->State.ProfileChoices.Num() > 1)
		{
			Form->ProfileOptions.Reset();
			for (const FString& Choice : Form->State.ProfileChoices)
			{
				Form->ProfileOptions.Add(MakeShared<FString>(Choice));
			}
			TSharedRef<SHorizontalBox> ProfileRow = SNew(SHorizontalBox);
			ProfileRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(170.0f)
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("Profile")))
				]
			];
			ProfileRow->AddSlot().FillWidth(1.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&Form->ProfileOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
				{
					return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
				})
				.OnSelectionChanged_Lambda([Form](TSharedPtr<FString> Item, ESelectInfo::Type)
				{
					if (Item.IsValid())
					{
						Form->PendingProfile = *Item;
						Form->bRebuild = true;
					}
				})
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("Choose a profile")))
				]
			];
			Box->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
			[
				ProfileRow
			];
		}

		for (int32 i = 0; i < Form->State.Entries.Num(); ++i)
		{
			Box->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
			[
				BuildEntryRow(Form, i)
			];
		}

		TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
		Buttons->AddSlot().FillWidth(1.0f)
		[
			SNew(SSpacer)
		];
		Buttons->AddSlot().AutoWidth().Padding(0, 0, 8, 0)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Save and restart")))
			.IsEnabled_Lambda([Form]() -> bool { return Form->State.bComplete; })
			.OnClicked_Lambda([Form]() -> FReply
			{
				FString GameRoot;
				TMap<FString, FString> DataFiles;
				for (const FEntry& E : Form->State.Entries)
				{
					if (E.bFolder)
					{
						GameRoot = Normalize(E.Value);
					}
					else
					{
						DataFiles.Add(E.Key, Normalize(E.Value));
					}
				}
				if (WriteLocalIni(Form->State.ProfileDir, GameRoot, DataFiles))
				{
					UE_LOG(LogSuzie, Display, TEXT("Modkit project setup: wrote %s"), *Normalize(FSzProfile::LocalIniFile()));
					Form->bSaved = true;
					Form->bDone = true;
				}
				else
				{
					UE_LOG(LogSuzie, Error, TEXT("Modkit project setup: cannot write %s"), *Normalize(FSzProfile::LocalIniFile()));
				}
				return FReply::Handled();
			})
		];
		Buttons->AddSlot().AutoWidth()
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Cancel")))
			.OnClicked_Lambda([Form]() -> FReply
			{
				Form->bDone = true;
				return FReply::Handled();
			})
		];
		Box->AddSlot().AutoHeight().Padding(0, 12, 0, 0)
		[
			Buttons
		];
		return Box;
	}
}

FState Resolve(const FString& ProfileOverride)
{
	FState State;
	const TMap<FString, FString> Ini = ReadLocalSection();
	State.ProfileChoices = FindProfileChoices();

	const FString Override = Normalize(ProfileOverride);
	const FString IniProfile = Normalize(Ini.FindRef(TEXT("Profile")));
	State.bProfileInIni = IsProfileDir(IniProfile);
	if (!Override.IsEmpty())
	{
		if (IsProfileDir(Override))
		{
			State.ProfileDir = Override;
		}
	}
	else if (IsProfileDir(IniProfile))
	{
		State.ProfileDir = IniProfile;
	}
	else if (State.ProfileChoices.Num() == 1)
	{
		State.ProfileDir = State.ProfileChoices[0];
	}

	// Several profiles and none chosen: the first one describes the rows until the user picks.
	const FString SpecDir = !State.ProfileDir.IsEmpty() ? State.ProfileDir : (State.ProfileChoices.Num() > 0 ? State.ProfileChoices[0] : FString());
	if (SpecDir.IsEmpty())
	{
		UE_LOG(LogSuzie, Error, TEXT("Modkit project setup: no profile folder (profile.json, engine.ini, project.json) under the Suzie plugin's Profiles folder"));
		Validate(State);
		return State;
	}
	const TSharedPtr<FJsonObject> Spec = ReadJsonFile(SpecDir / TEXT("project.json"));
	if (!Spec.IsValid())
	{
		UE_LOG(LogSuzie, Error, TEXT("Modkit project setup: cannot read %s"), *(SpecDir / TEXT("project.json")));
		Validate(State);
		return State;
	}

	FEntry Game;
	Game.Key = TEXT("GameRoot");
	Game.bFolder = true;
	Game.bRequired = true;
	FString GameName;
	const TSharedPtr<FJsonObject>* GameObject = nullptr;
	if (Spec->TryGetObjectField(TEXT("game"), GameObject))
	{
		(*GameObject)->TryGetStringField(TEXT("name"), GameName);
		(*GameObject)->TryGetStringField(TEXT("probe"), Game.Probe);
	}
	Game.Label = (GameName.IsEmpty() ? TEXT("Game") : GameName) + TEXT(" folder");
	Game.Description = FString::Printf(TEXT("The folder that contains %s"), *Game.Probe);
	Game.Value = Normalize(Ini.FindRef(TEXT("GameRoot")));
	State.Entries.Add(Game);

	const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
	if (Spec->TryGetArrayField(TEXT("data_files"), Files))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Files)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}
			FEntry E;
			Obj->TryGetStringField(TEXT("key"), E.Key);
			Obj->TryGetStringField(TEXT("extension"), E.Extension);
			Obj->TryGetStringField(TEXT("description"), E.Description);
			Obj->TryGetBoolField(TEXT("required"), E.bRequired);
			if (E.Key.IsEmpty())
			{
				continue;
			}
			E.Label = E.bRequired ? E.Key : E.Key + TEXT(" (optional)");

			const FString IniValue = Normalize(Ini.FindRef(E.Key));
			const TArray<FString> Matches = DataFilesWithExtension(E.Extension);
			if (!IniValue.IsEmpty() && FPaths::FileExists(IniValue))
			{
				E.Value = IniValue;
			}
			else if (Matches.Num() == 1)
			{
				E.Value = Matches[0];
			}
			else if (Matches.Num() > 1)
			{
				E.Candidates = Matches;
			}
			else
			{
				E.Value = IniValue;
			}

			// The engine reads KeyFile at startup. A key it rejected is reported here, so the user picks another file.
			if (E.Key == TEXT("KeyFile") && Modkit::IsActive())
			{
				FAES::FAESKey Key;
				E.bEngineRejected = !IniValue.IsEmpty() && E.Value == IniValue && !Modkit::GetAesKey(Key);
			}
			State.Entries.Add(E);
		}
	}

	Validate(State);
	return State;
}

void Validate(FState& State)
{
	bool bAllValid = !State.ProfileDir.IsEmpty();
	for (FEntry& E : State.Entries)
	{
		const FString Value = Normalize(E.Value);
		if (E.bFolder)
		{
			E.bValid = !Value.IsEmpty() && FPaths::FileExists(Value / E.Probe);
		}
		else if (!Value.IsEmpty())
		{
			E.bValid = !E.bEngineRejected && FPaths::FileExists(Value);
		}
		else
		{
			E.bValid = !E.bRequired && E.Candidates.IsEmpty();
		}
		bAllValid &= E.bValid;
	}
	State.bComplete = bAllValid;
	State.Summary = BuildSummary(State);
}

bool WriteLocalIni(const FString& ProfileDir, const FString& GameRoot, const TMap<FString, FString>& DataFiles)
{
	const FString IniPath = FSzProfile::LocalIniFile();
	const FString Dir = FPaths::GetPath(IniPath);
	if (!FPaths::DirectoryExists(Dir) && !IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true))
	{
		return false;
	}

	// Managed keys and their values. A data key with an empty value is removed from the file.
	TArray<FString> Keys;
	TMap<FString, FString> Values;
	Keys.Add(TEXT("Profile"));
	Values.Add(TEXT("Profile"), Normalize(ProfileDir));
	Keys.Add(TEXT("GameRoot"));
	Values.Add(TEXT("GameRoot"), Normalize(GameRoot));
	for (const TPair<FString, FString>& Pair : DataFiles)
	{
		Keys.Add(Pair.Key);
		Values.Add(Pair.Key, Normalize(Pair.Value));
	}

	FString Text;
	const bool bExisted = FFileHelper::LoadFileToString(Text, *IniPath);
	const bool bCrLf = !bExisted || Text.Contains(TEXT("\r\n")) || !Text.Contains(TEXT("\n"));
	const FString NewLine = bCrLf ? TEXT("\r\n") : TEXT("\n");

	TArray<FString> Out;
	if (!bExisted)
	{
		Out.Add(TEXT("; Machine local paths of this project, written by the editor's Modkit project setup window."));
		Out.Add(TEXT("[Local]"));
		for (const FString& Key : Keys)
		{
			if (!Values[Key].IsEmpty())
			{
				Out.Add(Key + TEXT("=") + Values[Key]);
			}
		}
		Out.Add(FString());
	}
	else
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);

		// [Local] section: header line, the line after which missing keys go, and the keys already present.
		int32 HeaderIdx = INDEX_NONE;
		int32 EndIdx = Lines.Num();
		int32 LastKeyIdx = INDEX_NONE;
		TSet<FString> FoundKeys;
		bool bInLocal = false;
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			const FString Trimmed = Lines[i].TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("[")) && Trimmed.EndsWith(TEXT("]")))
			{
				if (bInLocal)
				{
					EndIdx = i;
					break;
				}
				if (HeaderIdx == INDEX_NONE && Trimmed == TEXT("[Local]"))
				{
					HeaderIdx = i;
					bInLocal = true;
				}
				continue;
			}
			int32 Eq;
			if (!bInLocal || Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT(";")) || Trimmed.StartsWith(TEXT("#")) || Trimmed.StartsWith(TEXT("//")) || !Trimmed.FindChar(TEXT('='), Eq))
			{
				continue;
			}
			LastKeyIdx = i;
			FoundKeys.Add(Trimmed.Left(Eq).TrimStartAndEnd());
		}

		TArray<FString> Missing;
		for (const FString& Key : Keys)
		{
			if (!FoundKeys.Contains(Key) && !Values[Key].IsEmpty())
			{
				Missing.Add(Key + TEXT("=") + Values[Key]);
			}
		}

		const bool bHasSection = HeaderIdx != INDEX_NONE;
		if (!bHasSection && !Lines.IsEmpty() && !Lines.Last().IsEmpty())
		{
			Lines.Add(FString());
		}
		if (!bHasSection && Lines.IsEmpty())
		{
			Lines.Add(FString());
		}
		const int32 InsertAfter = bHasSection ? (LastKeyIdx != INDEX_NONE ? LastKeyIdx : HeaderIdx) : INDEX_NONE;
		if (!bHasSection)
		{
			Lines.Add(TEXT("[Local]"));
			Lines.Append(Missing);
			Missing.Reset();
			Lines.Add(FString());
		}

		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			const FString& Line = Lines[i];
			if (bHasSection && i > HeaderIdx && i < EndIdx)
			{
				const FString Trimmed = Line.TrimStartAndEnd();
				int32 Eq;
				if (!Trimmed.StartsWith(TEXT(";")) && !Trimmed.StartsWith(TEXT("#")) && !Trimmed.StartsWith(TEXT("//")) && Trimmed.FindChar(TEXT('='), Eq))
				{
					const FString Key = Trimmed.Left(Eq).TrimStartAndEnd();
					if (Keys.Contains(Key))
					{
						if (!Values[Key].IsEmpty())
						{
							Out.Add(Key + TEXT("=") + Values[Key]);
						}
						continue;
					}
				}
			}
			Out.Add(Line);
			if (i == InsertAfter)
			{
				Out.Append(Missing);
			}
		}
	}

	FString Result;
	for (int32 i = 0; i < Out.Num(); ++i)
	{
		Result += Out[i];
		if (i + 1 < Out.Num())
		{
			Result += NewLine;
		}
	}
	return FFileHelper::SaveStringToFile(Result, *IniPath, FFileHelper::EEncodingOptions::ForceUTF8);
}

bool RunWindow(FState& State)
{
	const TSharedRef<FFormState> Form = MakeShared<FFormState>();
	Form->State = State;
	Form->Host = SNew(SBox).WidthOverride(640.0f);
	Form->Host->SetContent(BuildForm(Form));

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("Modkit project setup")))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		.IsTopmostWindow(true)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(12.0f)
			[
				Form->Host.ToSharedRef()
			]
		];
	Form->Window = Window;
	Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([Form](const TSharedRef<SWindow>&) { Form->bDone = true; }));
	FSlateApplication::Get().AddWindow(Window, /*bShowImmediately*/ true);

	// The window blocks startup until it closes. Slate is pumped here because the engine loop has not started.
	while (!Form->bDone && !IsEngineExitRequested())
	{
		if (Form->bRebuild)
		{
			Form->bRebuild = false;
			Form->State = Resolve(Form->PendingProfile);
			Form->Host->SetContent(BuildForm(Form));
		}
		FSlateApplication::Get().PumpMessages();
		FSlateApplication::Get().Tick();
		FPlatformProcess::Sleep(0.01f);
	}
	Window->RequestDestroyWindow();
	State = Form->State;

	if (Form->bSaved)
	{
		FUnrealEdMisc::Get().RestartEditor(/*bWarn*/ false);
		return true;
	}
	return false;
}
}
