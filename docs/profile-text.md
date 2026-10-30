# Android profile-name entry

While the game runs, the small **Keyboard** button at the upper right opens
Android's soft keyboard. It remains available when the pad is hidden or a
physical gamepad is connected. Open it on AC1's Create Profile screen, type the
name, and use the keyboard's Done/Enter action or virtual pad A to confirm.
Android Back/Escape forwards the PC Escape key and dismisses the IME; Hide
keyboard dismisses it without a game key.
The button respects the cutout/system insets and the keyboard does not resize
the rendering surface. No phone/emulator verification was performed for this
change.

## The actual PC path

The original WndProc calls `0x0077ff50` from `0x00404d10` on `WM_KEYDOWN`.
That helper calls `0x0077f9a0`, which obtains `GetKeyboardState` and invokes
`ToUnicode` (IAT `0x01a47da4` / `0x01a47da8`). The first returned UTF-16 unit
is checked against the CRT classification mask `0x107` (letters/digits), plus
explicit space, plus and minus exceptions at `0x0077ffd7..0x0077ffe7`, then
queued into the original text subsystem at `0x01a13568`. Key-up has a separate
path through `0x00780080`. Backspace/Enter/Escape are special virtual-key events.
The name is not entered through WM_CHAR or DirectInput character bytes.
DirectInput held key state is nevertheless needed for profile confirmation.

With the original controller scheme selected, the host shows an ordinary name
field and Create/Back controls. No controller character grid appeared. Pad A
confirms a completed name. No reliable profile-only text-entry signal was
identified: the WndProc's text subsystem flag also enables the general keyboard
event route. The implementation therefore uses the explicit keyboard button
instead of guessing a UI state from a global address or a timer.

## Committed text, composition and code page

`Ac1TextSurface` is the existing SurfaceView with a `BaseInputConnection`.
Composing text stays local; commit/finish sends it once. Deletion inside a
composition remains local. Deletion of committed text emits Backspace. Done
queues Enter after any pending text. Closing a connection invalidates callbacks
from that old connection; guest teardown discards unfinished composition.
Suggestions/fullscreen extraction and arbitrary selection replacement are
disabled because the original game owns the field and caret.

The runtime's ANSI code page is Windows-1252. The accepted profile repertoire
matches the portable character classification: ASCII letters/digits, space,
plus/minus, and Latin letters U+00C0..U+00FF except multiplication/division.
These UTF-16 units map losslessly to identical CP1252 byte values. The original
game still receives UTF-16 from ToUnicode and performs its normal text handling.
Other scripts, emoji, malformed surrogates and punctuation rejected by the game
are rejected as a whole commit, with a visible message; no replacement `?` or
best-fit transliteration is sent. The host capture verifies both uppercase and
the CP1252 character `ï` in `Altaïr42`.

Committed units use synthetic VK_PACKET key messages with a Unicode payload in
the existing per-message snapshot. Both window adapters return that payload
only inside the corresponding original WndProc's ToUnicode call. Native host
keyboard translation otherwise remains intact. Android physical keyboards keep
DIK state and key messages, while supplying their layout/meta-state Unicode
result; dead-key combinations and ACTION_MULTIPLE text are handled too.

Two observed AC1 behaviors matter: text queued in a single Present was consumed
in reverse order, and window-message-only Enter did not confirm the profile.
IME commits are therefore paced one UTF-16 unit per two successful Presents.
Backspace/Enter/Escape use the same key down/held DirectInput state/key up path
as PC input, held across two Presents. Text and control events share an ordered,
bounded queue so an immediate Done action cannot overtake the name. Actual
pause/input reset clears held keys and pending text.

## Replay and evidence (2026-10-30)

The replay parser accepts `text` followed by numeric UTF-16 units. It expands a
multi-unit command across two Presents per unit. `textkey BACKSPACE`,
`textkey ENTER` and `textkey ESCAPE` supply full keyboard events/state.
`tools/input/profile_name.txt` selects the controller, types `Altaïr4X`, deletes
X, adds 2, and confirms. No translated game function was edited.

Private evidence remains under `work/gamepad/text/`:

- `paced/profile_600.png`: original Create Profile field displays `Altaïr42`.
- `paced/name-detail.png`: enlarged review crop showing the diaeresis.
- `paced/profile_800.png`: main menu after the text Enter control.
- `paced/startup.log`: every supplied UTF-16 unit reaches original ToUnicode;
  all commands complete, with no guest stop before deliberate termination.
- `pad-confirm/profile_800.png`: main menu after the same name and XInput A,
  confirming compatibility with the virtual-pad button.
- `escape/profile_800.png`: Escape is forwarded but this controller-selected
  profile screen remains open, matching the observed PC key behavior. Use the
  game's Back control to leave it; Escape is not remapped to a different action.
- `replay/`: retained first attempt showing the same-frame reversal and
  message-only confirmation limitation; it is not successful-entry evidence.

The host run deliberately ends after Present 950 (the polling launcher may
observe a slightly later Present); its forced exit is not a clean game exit.
Only this task's processes were terminated. `work/host_build12.log` ended with
`Native build completed` before any task host build began. The private Make
host executable reuses that completed build's unchanged generated objects and
baseline runtime core, rebuilding the input adapters. Concurrent supervisor
runtime/graphics work is excluded from the patches and was not reset or edited.

Run the focused contracts with:

```powershell
& D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tests/run_text_input.py
& D:/Prog/dukemp_recomp/.venv/Scripts/python.exe -B tests/run_gamepad.py
```

They verify atomic validation, case/CP1252 units, pacing, queue bounds,
Backspace/Enter/Escape held state, physical DI state, parser behavior, and nested
ToUnicode calls through both actual Windows and Android window adapters. The
gamepad/parser/discovery regression suite also passes. ARM64 packaging passes
`tools/build_android.ps1 -Abi arm64-v8a -NativeBuilder Make -Make
C:/msys64/mingw64/bin/mingw32-make.exe`, with writable Gradle/Android homes under
`work/gamepad/`. APKs, symbols, captures and logs remain ignored private outputs.
Real IME variants, gesture Back, physical layouts and lifecycle behavior still
need phone testing.

The Android connection follows the platform's
[BaseInputConnection contract](https://developer.android.com/reference/android/view/inputmethod/BaseInputConnection).
