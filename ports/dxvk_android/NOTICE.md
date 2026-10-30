# Renderer dependencies

The Android renderer builds official DXVK v3.1, commit
`70d7508c01201ed3d4bfb33da42ba834eafe3857`, with platform changes made by this
project in `prepare.py` and `android_wsi.cpp`. DXVK is under the zlib/libpng
license; its complete notice is in [DXVK-LICENSE](DXVK-LICENSE).

The Android surface policy and shader-generation helper retain adaptations
from codepdbh/nfsu2-recompiled, copyright codepdbh, under the MIT license in
[LICENSE.nfsu2.txt](LICENSE.nfsu2.txt).

Pinned upstream submodules retain their source notices:

| Dependency | Commit | License |
| --- | --- | --- |
| mingw-directx-headers | `9df86f2341616ef1888ae59919feaa6d4fad693d` | LGPL-2.1-or-later; see COPYING.MinGW-w64.txt and header notices |
| Vulkan-Headers | `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77` | Apache-2.0 or MIT, per-file notices |
| SPIRV-Headers | `04f10f650d514df88b76d25e83db360142c7b174` | MIT; Khronos notices |
| dxbc-spirv | `37a97745bddaf56d717253b0e4565904ce5eb06c` | MIT; copyright 2025 Philip Rebohle |
| dxbc-spirv's SPIRV-Headers | `c8ad050fcb29e42a2f57d9f59e97488f465c436d` | MIT; Khronos notices |
| libdisplay-info | `275e6459c7ab1ddd4b125f28d0440716e4888078` | MIT; copyright 2022 contributors |

The slim native Windows compatibility headers distributed with DXVK retain
their per-file Wine/LGPL and other notices. Source, generated shaders, and
compiled providers stay in ignored `work/`; do not commit them. Binary
redistributions must retain the relevant dependency notices. The separate
Mesa/Turnip and libadrenotools build and licenses belong to `ports/turnip/`.
No game assets or translated proprietary game functions belong in these patches.
