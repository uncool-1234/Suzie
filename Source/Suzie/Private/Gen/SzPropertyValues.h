// Generic, game agnostic: sets property values of objects and structs from the JSON values listed for them (the jmap's property_values).
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FProperty;
class UStruct;

/** Counters of the value passes; a pass adds to them. */
struct FSzValueStats
{
	int32 Classes = 0;           // classes whose default object has listed values
	int32 Applied = 0;           // top-level properties whose listed values were all applied
	int32 Skipped = 0;           // properties not applied: deprecated, delegate, interface, unsupported type, or instanced outside an object
	int32 InstancedApplied = 0;  // instanced top-level properties applied (their references all point into the object being set)
	int32 InstancedForeign = 0;  // instanced top-level properties left untouched: a reference is unresolved or outside the object being set
	int32 TypeMismatch = 0;      // listed values of a kind the property cannot take (left untouched)
	int32 UnresolvedObjects = 0; // object references whose object does not exist (left untouched)
	int32 CdoInternal = 0;       // object references into a class default object whose object does not exist (left untouched)
	int32 Subobjects = 0;        // default subobjects with listed values
};

/**
 * Sets the properties of Struct that Values lists, on Data (an object or struct of that class). Properties that cannot take a
 * value are left untouched. An instanced property is applied when every object reference in its value resolves to an object
 * inside Data; otherwise it is left untouched. Every top-level property that was written is appended to OutApplied. Returns true
 * when every listed value was applied.
 */
bool SzApplyPropertyValues(const UStruct* Struct, void* Data, const TSharedPtr<FJsonObject>& Values, FSzValueStats& Stats, TArray<FProperty*>* OutApplied);
