# Third-Party Components

Every component Skywave links against or vendors, what it is used for,
and how it is licensed. Release gate 7 requires this file to cover the
full set; if you add a dependency, add it here in the same commit.

Skywave itself contains no vendored DSP, FEC, or protocol code. The
waveform, framer, LDPC encoder/decoder, ARQ, MAC, and AMC are original
to this project. The only vendored source tree is Dear ImGui.

## Vendored (in-tree source)

| Component | Version | Location | License | Used for |
|---|---|---|---|---|
| Dear ImGui | 1.91.5 | `third_party/imgui/` | MIT (`third_party/imgui/LICENSE.txt`) | The optional operator GUI, with the SDL2 + SDL_Renderer2 backends (`backends/imgui_impl_sdl2.cpp`, `backends/imgui_impl_sdlrenderer2.cpp`) |

Vendored sources are compiled with `-w`; the project's `-Wall -Wextra
-Wpedantic` applies to Skywave code only. This is set explicitly in
`CMakeLists.txt` per source file rather than by disabling warnings
globally.

## Linked at build time (system packages, found via pkg-config)

| Component | Version tested | Required? | License | Used for |
|---|---|---|---|---|
| PortAudio | 19 | **Required** | MIT | Soundcard capture and playback (`src/io/audio.cpp`) |
| Zstandard (libzstd) | 1.5.7 | **Required** | BSD-3-Clause or GPLv2 (dual) | Payload compression in the proto pipeline |
| SDL2 | 2.32.70 | Optional, `ENABLE_GUI=ON` (default) | Zlib | Window, input, and renderer behind the ImGui GUI. Configure degrades to `ENABLE_GUI=OFF` with a warning if absent. |
| Codec2 | 1.2.0 | Optional, `ENABLE_VOICE=ON` (default) | LGPL-2.1 | Live digital voice. Configure degrades to `ENABLE_VOICE=OFF` with a warning if absent. |
| libsodium | 1.0.22 | Optional, `ENABLE_CRYPTO=ON` (default **OFF**) | ISC | Session encryption. Linking it is opt-in at configure time. |

Codec2 is LGPL. Skywave links it dynamically through the system
package, so the LGPL's relinking requirement is satisfied by the normal
shared-library path. A build that statically links Codec2, or ships it
inside a single-file distribution, takes on LGPL obligations that the
default build does not; configure with `-DENABLE_VOICE=OFF` if that
matters for your distribution.

Zstandard is dual-licensed; Skywave relies on the BSD-3-Clause terms.

## Reference data transcribed from a published standard

The LDPC prototype matrices in `src/fec/ldpc_tables.cpp` are the
IEEE 802.11-2012 Annex F base matrices (rates 1/2, 2/3, 3/4, 5/6 at
N=648 and N=1944). They are numeric constants from a published
standard, not copied code. They were transcribed against the public
reference at `https://github.com/tavildar/LDPC`
(`LdpcC/WiFiLDPC.h`); the file carries that provenance note. No source
from that repository is present in this tree.

## Build and test tooling (not linked, not redistributed)

CMake 3.20+, a C++20 compiler (Apple clang 21 / GCC 12+), pkg-config,
and Python 3 for `tests/st3_hardware.py`. The ST3 hardware gate also
needs a loopback audio driver — BlackHole 2ch on macOS, VB-Cable on
Windows — which the user installs separately and which Skywave neither
bundles nor links.

## Project license

**Not yet set.** There is no `LICENSE` file at the repository root, so
the terms under which Skywave itself may be used or redistributed are
undefined. That is an owner decision, not a technical one, and it
blocks any public release. See RELEASE.md gate 7.
