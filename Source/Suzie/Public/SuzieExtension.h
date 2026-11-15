// Generic, game agnostic. MIT, same license as the rest of the plugin.
// Extension point for the small amount of game code a profile cannot express as data: native struct ops (a struct that is serialized
// by hand) and post-serialize handlers (data a game appends after the reflected part of an export).
//
// A game plugin module that the profile names ("extension_module" in profile.json) is loaded by Suzie right before it generates;
// its StartupModule registers an ISuzieGameExtension as modular feature. Header only: the game module needs no link dependency on Suzie.
#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"
#include "UObject/Class.h"

class FArchive;
class UObject;

/** Handler called by the loader after a cooked export of the profile's class was serialized (see UE::NativeSerialize::AddPostSerialize). */
using FSzPostSerializeFn = void (*)(UObject* Object, FArchive& Ar, int64 ExportEndOffset);

/** Named things an extension provides; the profile data (load/native_ops.json, load/skip.json) binds the names to types. */
struct FSzExtensionRegistry
{
	TMap<FName, TFunction<UScriptStruct::ICppStructOps*()>> StructOps;
	TMap<FName, FSzPostSerializeFn> PostSerialize;

	/** Registers ops for a plain C++ struct that has a TStructOpsTypeTraits specialisation (e.g. WithSerializer). */
	template <typename StructT>
	void AddStructOps(FName Name)
	{
		StructOps.Add(Name, []() -> UScriptStruct::ICppStructOps* { return new UScriptStruct::TCppStructOps<StructT>(); });
	}
	void AddPostSerialize(FName Name, FSzPostSerializeFn Fn) { PostSerialize.Add(Name, Fn); }
};

class ISuzieGameExtension : public IModularFeature
{
public:
	static FName GetModularFeatureName() { return FName(TEXT("SuzieGameExtension")); }
	/** Matches "id" of the profile.json the extension belongs to. */
	virtual FName GetProfileId() const = 0;
	virtual void Register(FSzExtensionRegistry& Registry) = 0;
};
