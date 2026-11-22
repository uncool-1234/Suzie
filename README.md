# Suzie

## This fork

This fork targets Unreal Engine 5.7.4 and adds a profile driven generator: at every editor start it generates a game's native enums, structs, classes and delegates in memory from the game's jmap. Everything game specific lives in a **profile** folder, which is not part of this repository; a game's profile is maintained with that game's modding project. How it works, how to select a profile and what a profile folder holds: [Profile driven generator](#profile-driven-generator-this-repository) below.

## Upstream jmap route

The rest of this section and the installation steps are the upstream Suzie instructions (Content/DynamicClasses, a project named like the game). In this fork that route runs only with `-SuzieGenerate`: when no profile is selected, when the profile has no jmap configured, or with `-SuzieUpstreamJmap` (which also needs `-SuzieGenerate`). See `FSuziePluginModule::StartupModule` in `Source/Suzie/Private/SuziePlugin.cpp`.

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

Next to the jmap route above, `Source/Suzie/Private/Gen` contains a generic, profile driven generator. Everything game specific lives in a **profile** folder outside this repository; this repository contains no game names (`Tools/check-layers.ps1`).

- **Derivation from the jmap**: Suzie parses the game's jmap (about 8 s for a 180 MB file), compares it with the live editor reflection and derives every enum, struct, class and delegate the editor lacks (properties with exact flags, struct sizes, interfaces, default subobjects). Generation takes about 0.8 s (`Suzie.GenerateNativeTypes`). No intermediate file exists.
- **Selecting a profile**: `<Project>/ModkitData/ModkitLocal.ini [Local] Profile=<profile folder>` (the folder that holds `profile.json` and `engine.ini`) or `-SuzieProfile=<dir>`; `-SuzieProfile=<name>` looks in this plugin's `Profiles` folder (none ship with this repository; an installed engine build can carry one there). The jmap comes from `-SuzieJmap=<file>` or `[Local] Jmap=` of the same file. The editor's **Modkit project setup** window writes `ModkitLocal.ini`: it opens when a path is missing or invalid (`Source/Suzie/Private/Setup/SzProjectSetup.cpp`; console command `Suzie.ProjectSetup`). With a profile selected and a jmap configured, generation runs at startup without a switch, unless a plugin listed in the profile's `conflicting_plugins` is enabled; without a jmap Suzie logs an error and generates nothing. `-DisableSuzie` turns it off. Without a profile Suzie idles (`-SuzieGenerate` still runs the upstream jmap route).
- **Shadow recipes**: engine types whose game layout differs get a recipe derived from the same jmap comparison at startup; the engine's unversioned schema hook reads the members the engine lacks into a temporary and discards them. This needs an engine with the unversioned schema override hook (`UObject/UnversionedSchemaOverride.h`, not in stock Unreal Engine); without it the plugin still compiles (the hooks are probed at compile time) and shadow recipes are off.
- **Game extension** (`SuzieExtension.h`): code that cannot be data (a struct with hand written serialization, handlers for data a game appends after the reflected part of an export) comes from a small plugin module named by the profile (`extension_module`), bound by name in `load/native_ops.json` and `load/skip.json`.
- **Gameplay tags**: the game's tag list and redirects are written from the jmap to the project's config files at startup.
- **Verification**: `Suzie.VerifyNativeTypes`, `Suzie.DumpClass`, `Suzie.DumpAll`, `Suzie.TagInfo`, and the sweep harness in `Tools/sweep`.

### Profile folder

| File | Read by | Purpose |
|---|---|---|
| `profile.json` | Suzie | entry point: `id`, `description`, paths of the data files below, `extension_module`, `conflicting_plugins` (relative paths resolve against the folder) |
| `engine.ini` | the engine's modkit hooks | container probing, index cipher steps, shader library, layout tolerance, plugin content roots |
| `project.json` | Modkit project setup window | `engine_id`, the data files a project folder holds (key file, jmap) and the game folder probe |
| `schema/aliases.json` | Suzie | game type path to editor type path |
| `schema/overrides.json` | Suzie | generation policy: abstract rules, flags, row structs |
| `load/skip.json` | Suzie | exports skipped on load; post-serialize handlers provided by the extension |
| `load/native_ops.json` | Suzie | structs whose C++ struct ops come from the extension |
| `tags/tags.json` | Suzie | project relative path of the written tag list, probe tag |
| `sweeps/`, `data/golden/` | `Tools/sweep` | optional test references: asset lists and a golden `Suzie.DumpAll` dump |
| `Plugin/<Module>` | UE | the extension plugin named by `extension_module` |

A profile may hold further files that its engine or packaging scripts read.

License: MIT (see LICENSE). Based on [trumank/Suzie](https://github.com/trumank/Suzie).
