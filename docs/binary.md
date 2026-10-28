# Installed build

`tools/inventory_ac1.py` fingerprints every EXE and DLL in the local GOG directory, checks the DX9 executable against the published EaglePatch SHA-1, and writes detailed PE facts to ignored `work/ac1-binary-inventory.json`. It reads the game directory only. Run it with the pinned Duke Python environment:

```powershell
& 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe' tools\inventory_ac1.py
```

The selected `AssassinsCreed_Dx9.exe` is PE32/I386, 24,666,760 bytes, image base `0x00400000`, image size `0x017BD000`, with entry RVA `0x00B9F445` / VA `0x00F9F445`. SHA-256 is `2fa8d50c60398cc249c6d908de46b15f48e445dd0c605b5956e94fa2f445a713`; SHA-1 is `a9ea476e70f076709cca59a802e26ec894432120`, matching EaglePatch's GOG/Steam DX9 fingerprint. This pins the relevant third-party address references to the same executable build.

The image has `.text`, `.rdata`, `.data`, `.idata`, `.tls`, `_RDATA`, `.data1`, and `.rsrc` sections. It has no base-relocation directory and sets `IMAGE_FILE_RELOCS_STRIPPED`; preserve its preferred **guest** image base (`0x00400000`). Guest-to-host address translation allows the backing memory to live elsewhere on a 64-bit host, so no executable normalization is required for this startup experiment. It has a TLS directory with initialized data at `0x01A4A000..0x01A4A2A1`, index at `0x01A3561C`, and callback-array pointer at `0x0168010C`; the mapped callback array is zero terminated immediately, so no TLS callback was found. The load-config directory exposes an x86 SafeSEH table (`0x017D0E10`, 14,349 entries) and security cookie (`0x019C8558`). There is no PE exception directory. The report's RTTI names are decorated-name candidates found in non-executable sections; they are not a validated vtable map. Example names include `scimitar::Object`, `VideoPlayerManager`, and `SoundDareManager`.

The PE entry begins with `call 0x00FAFA0F; jmp 0x00F9F265`. The nearby bytes are in the large `.text` body and not a tiny launcher thunk. A useful first lift should include the entry and its direct-call cone; the actual initialization path still has to be recovered by execution and dispatch traces.

The DX9 executable imports 260 symbols across 17 DLL names. The operating-system surface is substantial: kernel imports include heaps, virtual memory, files, events/semaphores, threads, TLS, critical sections, timers, loader APIs, locale/profile and error handling. `USER32` supplies window/message, cursor, monitor and keyboard APIs. Graphics starts from `d3d9.dll!Direct3DCreate9` and two `d3dx9_36` helpers; input imports `DirectInput8Create` and XInput ordinals; sound imports `DSOUND` ordinals plus `WINMM`; video calls 14 `binkw32` exports. `ADVAPI32` registry/crypto, `ole32` COM, `WS2_32` and `iphlpapi` are also present. Static startup therefore needs more than kernel/CRT wrappers; implement only APIs reached on the observed startup path and keep missing calls fatal with their import name. `EAX.DLL` is a separate installed DLL with ordinal 6 used by the game.

The directory also contains `AssassinsCreed_Dx10.exe`, `AssassinsCreed_Game.exe`, the GOG wrapper DLL, Bink and EAX DLLs, language setup and uninstaller executables. Their hashes and PE headers are in the JSON inventory; only the DX9 EXE receives detailed imports/TLS/RTTI analysis. Original binaries remain outside the repository.

`work/ac1-dx9-pe.json` is the earlier analysis report. Its indirect branch count is a linear disassembly sweep and is not an exhaustive code/data classification. No full function catalog, vtable recovery, or execution trace is claimed yet.
