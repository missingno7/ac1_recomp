# AC1 controller input

The runtime exposes AC1's native Xbox controller path on Windows and Android.
It supplies controller state to the original translated game; the game's input,
camera, contextual actions and controller prompts remain original code.

## Bindings and discovery evidence

The pinned DX9 executable imports exactly three `XINPUT1_3.dll` ordinals:
2 = `XInputGetState` (IAT `0x01a47f68`), 3 = `XInputSetState`
(`0x01a47f64`), and 4 = `XInputGetCapabilities` (`0x01a47f60`).
There are no imported Enable, battery, audio or keystroke functions.
Unknown imports retain the runtime's precise fatal diagnostic.

Merely returning a connected XInput state is insufficient. The original
DirectInput callback at `0x009165c0` creates a device, requests
`DIPROP_VIDPID` (property 24), and at `0x00916643..0x0091665a` compares
vendor `0x045e`, product `0x028e`. Only that branch constructs `PadXenon`
at `0x009161a0`, using its vtable at `0x016b18ac`.
The shared discovery node supplies this identity when user 0 is connected.
The node supports discovery and reference counting; unsupported joystick
methods fail explicitly. Gameplay input is supplied through XInput.
Windows uses native DirectInput for keyboard/mouse and substitutes this discovery
node for native joystick records while an XInput source is connected.
Android supplies the same record through its portable DirectInput bridge.

`0x00402fb0` reads `[Input] SelectedInput` from the user's `Assassin.ini`
and selects a device by its registered product name. The runtime defaults this
read to `Controller (Xbox 360 For Windows)` while a pad is connected.
`AC1_PAD_AUTOSELECT=0` preserves the stored selection instead. No installed
game file or user's INI is rewritten by this policy. The selection is made at
the game's settings-read points; hot-plug between Android's physical and virtual
sources uses the same controller 0 and does not require another device selection.

The original reader at `0x0098ce60` handles connection edges, asks for
capabilities on attachment, reads all four signed axes, and checks triggers
against 30. Its face-button conversion maps A/X/Y/B into the game's internal
Button1/2/3/4 slots. The installed `DefaultBindings.map` identifies the
semantics by their keyboard/mouse equivalents. The installed English PC manual,
sections 1.2 and 1.3, describes the contextual actions. The table below combines
those local sources with the game's observed controller tutorial prompts.

| XInput control | AC1 action | On-screen control |
| --- | --- | --- |
| Left stick | Move; speed follows stick magnitude | Floating MOVE stick |
| Right stick | Camera | Relative swipe area; configurable sensitivity |
| A / PS Cross | Legs: blend in low profile, sprint/free run/jump in high profile; contextual dodge | Legs / Blend |
| B / PS Circle | Empty hand: gentle push, grab, catch ledge, contextual counter-grab | Empty hand |
| X / PS Square | Weapon hand: attack, assassinate, contextual counter | Weapon |
| Y / PS Triangle | Head: vision/Eagle Vision, synchronise, contextual interaction | Head |
| RT / R2 | Hold high profile; RT+A with movement sprints | High profile; separate RT lock toggle |
| LT / L2 | Target lock / unlock (game handles the press edge) | Target lock |
| D-pad Up | Hidden blade | Weapons popup: Blade |
| D-pad Right | Sword | Weapons popup: Sword |
| D-pad Down | Fists | Weapons popup: Fists |
| D-pad Left | Short blade / throwing knives | Weapons popup: Knives |
| Back / Select | Map | Map |
| Start | Pause | Pause |
| Right stick press | Center camera; contextual chase view | Center |

Blend shares A with Legs. It does not need an additional button. RB/LB/left
stick press remain available on physical pads and in replay but are omitted
from the default touch layout. The weapon order is corroborated directly by
`PadUp=3`, `PadRight=4`, `PadDown=5`, `PadLeft=2` in the installed keyboard
bindings: DIK 2/3/4/1 select hidden blade/sword/fists/knives.

## Providers and replay

`ac1_gamepad.h` defines 12-byte gamepad, 16-byte state, and 20-byte capabilities
layouts with compile-time size checks. Submitted source snapshots are mutex
protected. Unchanged snapshots keep the same packet; real changes and connection
edges increment it. A disconnect clears held controls. Sticks remain signed
16-bit values and triggers remain independent bytes; no deadzone is imposed.
Disconnected calls return 1167 without touching output. Invalid API arguments
return 160. Virtual capabilities report gamepad type/subtype, all implemented
button bits and full-range axes/triggers, with no vibration motors. Virtual
SetState accepts the request without vibration. Windows passes vibration through
to native XInput. Android rumble is not implemented or advertised.

Windows loads one System32 XInput DLL (1.4, 1.3, then 9.1.0) and passes native
state/capabilities/vibration through by default. Set `AC1_XINPUT_NATIVE=0` to
disable native polling for deterministic tests. Replay always isolates physical
pad input, including scripts that contain no pad commands. Platform snapshots
support four user slots; Android selects one physical controller at user 0,
retaining it until removal, and then picks another available device or the
virtual controller.

`AC1_INPUT_SCRIPT` accepts these additional commands at successful Presents:

```text
at 0 pad connect
at 1700 pad stick left 0 26000
at 1850 pad stick right 16000 0
at 1950 pad stick right 0 0
at 2050 pad trigger RT 255
at 2050 pad button A down
at 2200 pad button A up
at 2200 pad release
at 2500 pad disconnect
```

Stick arguments are raw -32768..32767 (positive Y is up); triggers use 0..255.
Button names are A/B/X/Y, UP/DOWN/LEFT/RIGHT, START/BACK, LS/RS, LB/RB.
Values hold until changed. `release` neutralises every control while keeping
connection state. `disconnect` clears controls. Existing relative `+N` Present
syntax works. Malformed pad commands fail with their script line number.
`AC1_PAD_TRACE=1` logs snapshots observed by the provider.
`tools/input/pad_gameplay.txt` bootstraps menus with the established keyboard
replay, then drives gameplay exclusively with pad commands.

## Android overlay and physical pads

`Ac1PhysicalPad` handles buttons, stick axes, analog/digital triggers, hat
directions, connection changes and removals. It normalises the full advertised
axis range without applying `MotionRange.flat`, inverts Y for XInput, and falls
back to RX/RY and BRAKE/GAS axes when appropriate. The selected device owns pad
input; its events are consumed before keyboard/mouse translation.

The Canvas view sits above the SurfaceView. Each touch pointer owns one control
until release/cancel; movement, camera, profile and actions can be held together.
The left stick floats from the initial touch within the safe movement region.
Camera swipes produce right-stick velocity, reset after 80 ms of no movement,
and reset on finger release. RT lock is visibly highlighted. The compact weapons
button toggles a D-pad popup. Hide pad leaves a small Show pad control and
disconnects the virtual source. Hide preference persists across launches.
Focus loss, pause, resize and source changes clear held controls and RT lock.
Physical-pad attachment hides the touch controller; removal restores it.
The former touch-to-Enter and joystick-to-WASD/mouse hacks are removed.

The packaged `android/app/src/main/assets/controller-layout.json` is copied on
first launch to `files/controller-layout.json` in app-private storage. Edit that
copy and restart to tune positions, radii, labels, opacity and camera sensitivity.
Coordinates are fractions of the safe rectangle; radii are fractions of its
shorter dimension. System/cutout/mandatory-gesture insets and an 8 dp margin are
excluded. Invalid layout settings log a diagnostic and use packaged defaults
without overwriting the edited file. Schema version is 1.

## Verification (2026-10-30)

`tests/run_gamepad.py` passes the raw state/packet/trigger/capability/replay
contracts, existing input-script tests, and discovery/CreateDevice/VIDPID
contracts against both the Windows and Android-portable DirectInput providers.
The existing DirectInput state/replay contracts pass. `tests/run_win32_compat.py`
passes its Windows API oracle and host/ARM64 syntax checks. These are separate
from original-game execution and hardware verification.

The first private replay, `work/gamepad/replay/`, demonstrated that adding
XInput alone did not activate the game's pad path. After discovery and selection
integration, `work/gamepad/replay2/startup.log` reaches Present 2700 at input
clock 66753 ms with every pad event consumed and no guest stop. It was terminated
deliberately at that checkpoint. The original tutorial responds to left-stick
movement, right-stick camera and RT+A, showing WALK/RUN, CAMERA and TACKLE
instructions. Captures at 1800 and 2000 explicitly show RT and stick icons.
Contextual combat and all unlocked actions are not exhaustively verified in the
restricted tutorial. Native Windows physical-controller use also needs a
controller connected; unattended evidence uses the replay source.

Private PNGs and the annotated contact sheet are under `work/gamepad/replay2/`.
The host executable builds with Make in `build/host-gamepad/`; unchanged
generated game objects were reused from the completed supervisor host build.
Ninja stalled on subprocess creation in this sandbox. Android ARM64 packaging
passes `tools/build_android.ps1 -Abi arm64-v8a -NativeBuilder Make -Make
C:/msys64/mingw64/bin/mingw32-make.exe`, with `GRADLE_USER_HOME` and
`ANDROID_USER_HOME` directed under `work/gamepad/` because the normal Gradle
cache is read-only. Native symbols and APK remain ignored build products.

Render the same layout JSON for review with:

```powershell
python -B tools/render_controller.py
python -B tools/render_controller.py --popup --output work/gamepad/overlay-weapons.png
```

The default preview is `work/gamepad/overlay.png` (3120x1440). It is a host
render on a neutral background. No phone or emulator was connected for this
task. Touch feel, cutouts/gesture navigation, physical device mappings and
hot-plug, lifecycle behavior and actual Android gameplay still need hardware
verification. There is no Android rumble implementation.

The XInput ABI and error/packet contracts follow Microsoft's
[GetState](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetstate),
[state](https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_state)
and [capabilities](https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_capabilities)
references. Recomp-kit `dx/xinput.cpp` was inspected read-only and reused under
MIT; attribution is in `src/runtime/NOTICE.md` and the provider source, with
the complete license in `src/runtime/LICENSE.recomp-kit.txt`.
