# AC1 guest D3D9 bridge bootstrap

This is an independently buildable Windows host bridge for AC1's 32-bit guest D3D9 ABI. Build and run its real native-D3D9 factory / adapter-count smoke test with:

```powershell
cmake -S src/graphics -B work/build/d3d9-bridge -G "MinGW Makefiles"
cmake --build work/build/d3d9-bridge
ctest --test-dir work/build/d3d9-bridge --output-on-failure
```

The bridge exposes a C ABI in `ac1_d3d9_bridge.h`. The runtime supplies checked guest memory resolution, aligned guest allocation/free, and guest-HWND to host-HWND mapping. The COM vtable token family is `0x7e000000` (D3D9 kind 1; device kind 2). `ac1_d3d9_argument_count(token)` returns exact stack-word arity including `this`, 0 for foreign tokens, -1 for malformed owned tokens, and -2 for unsupported methods. Dispatch receives arguments with guest `this` first and explicit COM arguments in source order. Dispatch returns 1 for a real native method call, 0 for a foreign token, -1 for invalid object/ABI state, and -2 for unsupported methods. Only result 1 makes EAX valid; ordinary native HRESULT failures remain normal results. `Direct3DCreate9` uses the absolute system-directory `d3d9.dll` by default. A platform factory callback can replace it if the same source is later compiled against DXVK's native headers.

The bridge implements the D3D9 methods observed in the PE analysis, marshals the 32-bit guest `D3DPRESENT_PARAMETERS` and adapter-identifier layouts, and returns the native host device as a guest object from `CreateDevice`. It preserves requested presentation values, translating only guest window handles at the host boundary. `IDirect3DDevice9` currently supports QueryInterface/AddRef/Release; other slots are classified as unsupported before argument collection and cause an explicit `-2` result with a diagnostic. The smoke test creates a real native `IDirect3D9`, dispatches `GetAdapterCount`, validates token arity/unsupported classification, releases the object, and shuts down. On the current host it reports two adapters. It does not create a device, open a window, render a frame, or prove that AC1 has reached its D3D9 import.

`GetAdapterMonitor` needs `host_monitor_to_guest` on 64-bit hosts if the guest compares monitor handles. No AC1 HWND exists in this module; the owner of the native window must supply the translation callback. The runtime import/dispatch integration is intentionally not included here.
