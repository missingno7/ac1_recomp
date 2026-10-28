# AC1 reference findings

Checked out under `third_party/ac-references` on 2026-10-28:

| Reference | Revision | License status |
|---|---:|---|
| [EaglePatch](https://github.com/Sergeanur/EaglePatch) | `a39dc87` | No root license file; do not copy code without clarification. `patcher/LICENSE` applies to the patcher directory. |
| [AC-Definitive](https://github.com/HenryPDT/AC-Definitive) | `91ee063` | No root license file; do not copy code without clarification. |
| [AC1-Movement-Rewritten](https://github.com/Banned445/AC1-Movement-Rewritten) | `f3d8a3b` | MIT (`LICENSE`). |

The local DX9 executable SHA-1 matches EaglePatch's supported DX9 fingerprint. In `EaglePatch/EaglePatch/src/ac1.cpp`, its DX9 address table identifies input functions `Pad_UpdateTimeStamps=0x93F990`, `Pad_ScaleStickValues=0x93FC80`, `PadXenon_ctor=0x9161A0`, and `PadProxyPC::AddPad=0x90B2F0`; the `PadProxyPC` update hook site is `0x909C90`. Its allocator references are descriptor global `0x1A1E680`, descriptor lookup `0x924070`, game allocator `0x7A4510`, and delete `0x916440`. Its DX9 fixes target MSAA at `0xE91422`, `0xE9116D`, and `0xE91178`, intro-video branch at `0x405495`, and telemetry URL pointer at `0x017382D8`. These are candidate RVAs/addresses from the patch source; confirm the image-base convention before using them in analysis.

`AC-Definitive/Plugins/AC1/AC1-EaglePatch/src/Patches/Controller.cpp` provides byte signatures for `PadProxyPC` update (`56 8B F1 8B 86 04 05 00 00 83`), Xenon-pad injection (`8B 46 04 8B 08 8B 11 6A`), and allocator setup (`51 6A 10 68 20 05 00 00`). The patch updates the keyboard and controller pads and merges their button/stick state. Its AC1 graphics source only patches MSAA and DX10 resolution enumeration; these references expose no D3D9 device-creation or presentation path.

`AC1-Movement-Rewritten/src/bindings.rs` documents additional disassembly anchors: input profile selection at `0x402FB0`, binding parse at `0x4025D0`, and key-code conversion at `0x402450`. Bindings come from `DefaultBindings.map` beside the executable and `%APPDATA%/Ubisoft/Assassin's Creed/Assassin.ini` (`[Input] SelectedInput`, `HighProfileToggle`). `src/assets/forge.rs` reads Forge v25 archives from user game data; it is a clean-room asset/runtime implementation, not code for the original game startup, saving, or rendering backends.
