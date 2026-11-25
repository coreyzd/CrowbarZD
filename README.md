# CrowbarZD

A Linux-native tool for compiling and decompiling Source engine models:
"Crowbar, but for Linux". Written in C++20 with Qt 6.

- **Compile**: runs a native `studiomdl` build (such as `studiomdl++` from Tools++) on a QC file,
  the way Crowbar runs `studiomdl.exe`. Before it starts, it checks that the files the QC refers to
  exist with the same letter case, a common reason Windows-made QC files fail on Linux.
- **Decompile**: turns an MDL (with its VVD and VTX) back into a QC and SMD files.
  The decompiler is a port of Crowbar's by ZeqMacaw.

## Build

Needs CMake 3.20+, a C++20 compiler and Qt 6.4+ (Widgets, Concurrent).

    cmake -S . -B build
    cmake --build build -j

This builds `build/crowbarzd` (the application) and `build/crowbarzd-cli`.
Without Qt, or with `-DCZD_BUILD_APP=OFF`, only the command-line tool is built.

## Use

Application: set the model compiler and the game folder (the one with `gameinfo.txt`) once under
Setup, then pick a QC or MDL. Dropping a `.qc` or `.mdl` on the window also works.

Both tabs take a single file, a folder, or a folder and its subfolders (the box in front of the
input path). On the Decompile tab, "Folder for each model" puts each model's files in a folder
named after it; with subfolders, the folder layout of the input is kept under the output folder.
Without it, models decompiled together share a folder and mesh files get the model's name in front,
so two models that both call their mesh `ref.smd` do not overwrite each other.

Command line:

    crowbarzd-cli decompile <model.mdl | folder> [-r] [-o <folder>] [--flat | --folder-per-model]
                            [--no-definebones] [--no-lods] [--no-anims] [--no-physics] [--no-procedural] [--no-flexes]
    crowbarzd-cli compile <model.qc | folder> [-r] -game <game folder> [-studiomdl <binary>] [-verbose]
                          [-o <work folder> | -subfolder <name>] [-- extra args]
    crowbarzd-cli info <model.mdl>

A folder works on every MDL or QC file in it; `-r` includes the folders below it.

### Where compiled models go

studiomdl always writes to `<game folder>/models/<$modelname>`. "Output to" on the Compile tab
(`-o` / `-subfolder` on the command line) chooses what happens next:

- **Game's "models" folder**: the files stay there.
- **Work folder**: the files are moved to `<work folder>/models/<$modelname>`.
- **Subfolder (of QC input)**: the files are moved to `<QC folder>/<subfolder>/models/<$modelname>`.

For the last two, a model of the same name that was already in the game folder is set aside
before the compile and put back afterwards, and folders the compile created in the game folder
are removed again if they end up empty.

## What the decompiler covers

MDL versions 44 to 48 (Source 2006 / 2007 / SDK 2013 era; GoldenEye: Source is 48).

| Written | Not written yet (reported as a warning when the model has it) |
| --- | --- |
| Reference meshes, body groups, LOD meshes and `$lod` | Axis-interpolated and aim-at procedural bones |
| Skeleton and `$definebone` | Flex rules that use DMX-only operations (written as approximations or comments) |
| Animations and `$sequence` / `$animation`: fps, loop, activity, events, blends, weight lists, nodes, root motion | Eyes or flexes on more than one body group |
| Animations and IK rules kept in an animation block file (`<model>.ani`, read from next to the MDL) | Bone controllers, LOD material replacement |
| Sequence layers (`addlayer`, `blendlayer`) | `$animblocksize` (a recompile puts the animations back inside the MDL) |
| IK: `$ikchain`, `$ikautoplaylock`, `iklock` and `ikrule` options | |
| `$jigglebone` | |
| Procedural helper bones: a VRD file and `$proceduralbones` | |
| Faces, in a `$model` block: `eyeball`, `eyelid`, `mouth`, flex shapes in a VTA file (`flex`, `flexpair`), `flexcontroller`, `localvar` and flex rules | |
| Collision models (`.phy`): physics SMD, `$collisionmodel` and `$collisionjoints` with mass, damping, joint constraints, `$concave`, `$collisiontext` | |
| `$cdmaterials`, `$texturegroup` (skins) | |
| `$attachment`, `$hboxset` / `$hbox`, `$bbox`, `$cbox` | |
| `$surfaceprop`, `$contents`, `$illumposition`, `$keyvalues`, `$poseparameter`, `$includemodel`, lighting flags | |

IK rules are written exactly as stored, with `noautoik` on every animation so the compiler does
not add its automatic "release" rules a second time.

The collision-model transform for single-solid models (props) is not Crowbar's: it was worked out
from the root bone's bind and first-frame transforms and checked against sample models, because
Crowbar's v48 formula put the collision mesh in the wrong place on the models tested.

## Test

`tests/roundtrip.py` decompiles a model, recompiles it with studiomdl, decompiles the result and
compares skeletons, animations, meshes, collision hulls, flex shapes, the model summary (`info`) and the two QC files:

    python3 tests/roundtrip.py build/crowbarzd-cli ~/.local/opt/tools++/studiomdl++ <game folder> <model.mdl>...

It compiles into a scratch copy of the game folder, never into the game itself.

## Layout

    src/core   readers (MDL, VVD, VTX), QC/SMD writers, studiomdl front end; no Qt
    src/cli    crowbarzd-cli
    src/app    Qt 6 application
    tests      round-trip test

## Credits and license

CrowbarZD is a modified work based on [Crowbar](https://steamcommunity.com/groups/CrowbarTool)
by ZeqMacaw. It is licensed under Creative Commons Attribution-ShareAlike 3.0 Unported, the same
license as Crowbar; see `LICENSE.txt`.
