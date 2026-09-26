# BuffOverlay for POE2Fixer

A customizable POE2Fixer SDK v6 overlay plugin for displaying Path of Exile 2 buff state.

## Features

- Any number of independent buff trackers
- Match buff names by **Exact**, **Starts with**, or **Contains**
- Display **charges**, **custom text**, **icon**, **icon + charges**, or **icon + text**
- Custom PNG/JPG/BMP icons loaded through Windows Imaging Component
- Absolute X/Y positioning plus a draggable preview mode
- Choose what happens when a buff is missing: **hide** or **show 0**
- Optional charge summing if several matching buff entries exist
- Debug window that lists all active buff names, charges and remaining time
- Settings persisted in `Plugins/BuffOverlay/config/settings.txt`

## Example: variable numeric suffix

If the active buff is named something like:

`totem_ancestral_bond_reservation_79`

and the final number changes, configure the tracker as:

- Buff name / pattern: `totem_ancestral_bond_reservation_`
- Match mode: `Starts with`
- Display: `Charges`
- If buff is missing: `Show 0` or `Hide`

The overlay will display the SDK `Buff.Charges` value instead of depending on the numeric suffix.

## Required repository layout

This project intentionally uses the same relative SDK layout as POEFixer's ExamplePlugin:

```text
YourRepo/
├─ BuffOverlay.sln
├─ POEFixer/
│  ├─ plugin_sdk/
│  │  ├─ PluginAbi.h
│  │  └─ PluginSDK.h
│  └─ imgui/
│     ├─ imgui.cpp
│     ├─ imgui_draw.cpp
│     ├─ imgui_tables.cpp
│     ├─ imgui_widgets.cpp
│     └─ ...
└─ Plugins/
   └─ BuffOverlay/
      ├─ BuffOverlay.cpp
      └─ BuffOverlay.vcxproj
```

The easiest setup is to clone/download `POEFixer/ExamplePlugin`, then copy this package's `Plugins/BuffOverlay` folder and `BuffOverlay.sln` into that repository root.

## Build

1. Install Visual Studio 2022.
2. In Visual Studio Installer enable **Desktop development with C++**.
3. Make sure MSVC v143 and the Windows 10/11 SDK are installed.
4. Open `BuffOverlay.sln`.
5. Select `Release | x64`.
6. Build -> Build Solution.
7. Output: `x64/Release/Plugins/BuffOverlay/BuffOverlay.dll`.

## Install

Create this folder next to the POE2Fixer executable:

```text
Plugins/BuffOverlay/
```

Copy `BuffOverlay.dll` into it, start POE2Fixer, then enable **Buff Overlay** in the plugin list.

## Notes

The plugin is built against the current public POEFixer ExamplePlugin SDK v6 layout. If POEFixer changes its SDK ABI later, rebuild against the matching current `PluginSDK.h`/`PluginAbi.h` files.
