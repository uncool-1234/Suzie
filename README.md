# Suzie

## This fork

This fork is used by the Game modkit in profile driven mode. The generic code in `Source/Suzie` generates a game's native enums, structs, classes and delegates in memory from baked JSON (`Profiles/<Game>/data/bake/types.json`, written by the profile's baker from a jmap). Everything game specific lives in a profile directory. The Game profile is `Profiles/Game`; see [Profiles/Game/README.md](Profiles/Game/README.md). Generation runs at editor start when a profile is selected (`ModkitLocal.ini [Local] Profile=`, or `-SuzieProfile=`).

The `docs` folder of the modkit workspace describes the whole modkit: `profile-format.md`, `native-type-generation.md`, `schema-shadow-properties.md` and the others.

## Upstream jmap route

The section below is the upstream Suzie instruction set. It runs only with `-SuzieGenerate`: when no profile is selected, when the profile has no baked types file, or with `-SuzieUpstreamJmap` (which also needs `-SuzieGenerate`). See `FSuziePluginModule::StartupModule` in `Source/Suzie/Private/SuziePlugin.cpp`.

Suzie is an Unreal Engine plugin that dynamically generates UHT (Unreal Header Tool) classes at editor runtime from dumped game class definitions. This allows working with game classes in Blueprint without having to generate headers and recompile if changed.

## How It Works

Suzie works in conjunction with [jmap_dumper](https://github.com/trumank/jmap), a tool that dumps class information from running Unreal Engine games. The workflow is:

1. Use jmap_dumper to dump class definitions from a running game into a .jmap (JSON) file
2. Place the .jmap file in project's `Content/DynamicClasses` directory
3. Suzie reads this file and generates all necessary classes at editor startup and during cooking
4. All native game classes become available in the Blueprint editor

## Supported Engine Versions

Suzie has been tested on Unreal Engine 5.3 through 5.6. Other versions may require minor tweaks (please submit a PR with fixes or create an issue showing errors).

## Installation & Usage

### Step 1: Obtain Game Dump

Use [jmap_dumper](https://github.com/trumank/jmap) to dump class definitions from target game into a `.jmap.gz` (or plain `.jmap`) file.

### Step 2: Set Up Unreal Project

1. Create a new C++ Unreal Engine project:
   - Use the same engine version as the target game
   - Use the same project name as the target game

2. Create the dynamic classes directory:
```bash
mkdir -p YourProject/Content/DynamicClasses
```

3. Copy the dumped .jmap file:
```bash
cp output.jmap.gz YourProject/Content/DynamicClasses/
```

### Step 3: Install Suzie Plugin

1. Clone Suzie into project's Plugins directory:
```bash
cd YourProject/Plugins
git clone https://github.com/trumank/Suzie
```

2. Add Suzie to `.uproject` file:
```json
{
  "Plugins": [
    {
      "Name": "Suzie",
      "Enabled": true
    }
  ]
}
```

### Step 4: Build and Launch

1. Generate Visual Studio project files (right-click `.uproject` > Generate Visual Studio project files)
2. Open the `.sln` file in Visual Studio and build the solution
3. Launch the editor:
  - All native game structs, enums, and properties should now be available

---

## Profile driven generator (this repository)

Next to the jmap route above, `Source/Suzie/Private/Gen` contains a generic, profile driven generator. Everything game specific lives in a **profile** directory (`Profiles/<Game>/`); `Source/` contains no game names (`Tools/check-layers.ps1`).

- **Baked types**: the profile's baker turns a jmap into one compact JSON (types, functions, properties with exact flags, struct sizes, interfaces, default subobjects, CDO deltas). Suzie generates enums, structs, classes and delegates from it in about 1.5 s (`Suzie.GenerateNativeTypes`).
- **Selecting a profile**: `<Project>/ModkitLocal.ini [Local] Profile=<profile folder>` (the folder that holds `profile.json` and `engine.ini`) or `-SuzieProfile=<name|dir>`. With a profile selected, generation runs at startup without a switch, unless a plugin listed in the profile's `conflicting_plugins` is enabled. `-DisableSuzie` turns it off. Without a profile Suzie idles (`-SuzieGenerate` still runs the jmap route).
- **Shadow recipes**: engine types whose game layout differs are described in `schema/engine_diff.json`; the engine's unversioned schema hook reads the members the engine lacks into a temporary and discards them. This needs the `modkit-core` engine branch (`UObject/UnversionedSchemaOverride.h`); without it the plugin still compiles (the hooks are probed at compile time).
- **Game extension** (`SuzieExtension.h`): code that cannot be data (a struct with hand written serialization, handlers for data a game appends after the reflected part of an export) comes from a small plugin module named by the profile (`extension_module`), bound by name in `load/native_ops.json` and `load/skip.json`.
- **Verification**: `Suzie.VerifyNativeTypes`, `Suzie.DumpClass`, `Suzie.DumpAll`, `Suzie.TagInfo`, and the sweep harness in `Tools/sweep`.

Documentation lives in the workspace folder `<path>` (`profile-format.md`, `native-type-generation.md`, `schema-shadow-properties.md`).

License: MIT (see LICENSE and NOTICE of the upstream project).
