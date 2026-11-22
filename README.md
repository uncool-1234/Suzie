# Suzie

## This fork

This fork runs in profile driven mode. The generic code in `Source/Suzie` generates a game's native enums, structs, classes and delegates in memory at every editor start from the game's jmap (`-SuzieJmap=<file>`, else `[Local] Jmap=` in `<Project>/ModkitData/ModkitLocal.ini`). Everything game specific lives in a profile directory, which is not part of this repository: a profile folder named by `ModkitData/ModkitLocal.ini [Local] Profile=` or `-SuzieProfile=<dir>`, or a folder under this plugin's `Profiles` (`-SuzieProfile=<name>`; an installed build places its profile there). Generation runs at editor start when a profile is selected.

The `docs` folder of the modkit workspace describes the whole modkit: `profile-format.md`, `native-type-generation.md`, `schema-shadow-properties.md` and the others.

## Upstream jmap route

The section below is the upstream Suzie instruction set. It runs only with `-SuzieGenerate`: when no profile is selected, when the profile has no jmap configured, or with `-SuzieUpstreamJmap` (which also needs `-SuzieGenerate`). See `FSuziePluginModule::StartupModule` in `Source/Suzie/Private/SuziePlugin.cpp`.

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

- **Derivation from the jmap**: Suzie parses the game's jmap (about 8 s for a 180 MB file), compares it with the live editor reflection and derives every enum, struct, class and delegate the editor lacks (properties with exact flags, struct sizes, interfaces, default subobjects). Generation takes about 0.8 s (`Suzie.GenerateNativeTypes`). No intermediate file exists.
- **Selecting a profile**: `<Project>/ModkitData/ModkitLocal.ini [Local] Profile=<profile folder>` (the folder that holds `profile.json` and `engine.ini`) or `-SuzieProfile=<name|dir>`. With a profile selected and a jmap configured, generation runs at startup without a switch, unless a plugin listed in the profile's `conflicting_plugins` is enabled; without a jmap Suzie logs an error and generates nothing. `-DisableSuzie` turns it off. Without a profile Suzie idles (`-SuzieGenerate` still runs the upstream jmap route).
- **Shadow recipes**: engine types whose game layout differs get a recipe derived from the same jmap comparison at startup; the engine's unversioned schema hook reads the members the engine lacks into a temporary and discards them. This needs the `modkit-core` engine branch (`UObject/UnversionedSchemaOverride.h`); without it the plugin still compiles (the hooks are probed at compile time).
- **Game extension** (`SuzieExtension.h`): code that cannot be data (a struct with hand written serialization, handlers for data a game appends after the reflected part of an export) comes from a small plugin module named by the profile (`extension_module`), bound by name in `load/native_ops.json` and `load/skip.json`.
- **Gameplay tags**: the game's tag list and redirects are written from the jmap to the project's config files at startup.
- **Verification**: `Suzie.VerifyNativeTypes`, `Suzie.DumpClass`, `Suzie.DumpAll`, `Suzie.TagInfo`, and the sweep harness in `Tools/sweep`.

Documentation lives in the workspace's `docs` folder (`profile-format.md`, `native-type-generation.md`, `schema-shadow-properties.md`).

License: MIT (see LICENSE). Based on [trumank/Suzie](https://github.com/trumank/Suzie).
