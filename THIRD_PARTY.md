# Third-Party Notices

Skywave links against or vendors the following third-party components.
License texts live with the upstream projects. Release gate 7 requires
this file to cover the full set; if you add a dependency, add it here in
the same commit.

Versions below are the ones the project is currently built and tested
against. Linked libraries come from the build machine's package manager,
so a different host may supply different versions.

## Linked libraries (required)

| Component | Version tested | License | Use |
|---|---|---|---|
| [PortAudio](https://www.portaudio.com/) | 19 | MIT-style (PortAudio license) | real-time audio I/O |
| [zstd](https://github.com/facebook/zstd) | 1.5.7 | BSD-3-Clause (dual GPLv2) | proto-pipeline payload compression |

Skywave relies on zstd's BSD-3-Clause terms.

## Linked libraries (optional, by build flag)

| Component | Flag | Version tested | License | Use |
|---|---|---|---|---|
| [libsodium](https://libsodium.org/) | `ENABLE_CRYPTO` (default **OFF**) | 1.0.22 | ISC | X25519 KEX, HKDF, AES-256-GCM |
| [Codec2](https://github.com/drowe67/codec2) | `ENABLE_VOICE` (default ON) | 1.2.0 | LGPL-2.1 | live voice vocoder (dynamically linked) |
| [SDL2](https://libsdl.org/) | `ENABLE_GUI` (default ON) | 2.32.70 | zlib | GUI window/renderer |

Configure degrades gracefully: a missing Codec2 or SDL2 turns its
feature off with a warning rather than failing the build.

**Codec2 is LGPL-2.1.** The default build links it dynamically through
the system package, which satisfies the LGPL's relinking requirement by
the normal shared-library path. A build that statically links Codec2, or
ships it inside a single-file distribution, takes on LGPL obligations
that the default build does not. Configure `-DENABLE_VOICE=OFF` if that
matters for your distribution.

## Vendored source

| Component | Version | Location | License |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.5 | `third_party/imgui/` | MIT (`third_party/imgui/LICENSE.txt`) |

This is the only vendored tree in the project. It covers the GUI plus
the SDL2 and SDL_Renderer2 backends. Vendored sources compile with `-w`,
set per source file in `CMakeLists.txt`; the project's `-Wall -Wextra
-Wpedantic` applies to Skywave code only.

KissFFT and GoogleTest appear in the early planning documents and were
**never** vendored: the FFT is in-tree mixed-radix and the test harness
is `tests/skytest.hpp`. See the ROADMAP deviations for 0.1 and 1.1.

## Standards-derived tables

- LDPC parity matrices (`src/fec/ldpc_tables.cpp`) are the QC-LDPC base
  matrices published in IEEE Std 802.11n (N=648/1944; rates 1/2, 2/3,
  3/4, 5/6), regenerated from the standard's tables. They are numeric
  constants from a published standard, not copied code. The file records
  that they were transcribed against the public reference at
  `https://github.com/tavildar/LDPC` (`LdpcC/WiFiLDPC.h`); no source
  from that repository is present in this tree.

## Build and test tooling (not linked, not redistributed)

CMake 3.20+, a C++20 compiler (Apple clang 21 / GCC 12+), pkg-config,
and Python 3 for `tests/st3_hardware.py` and `tests/check_docs_floors.py`.
The ST3 hardware gate also needs a loopback audio driver — BlackHole 2ch
on macOS, VB-Cable on Windows — which the user installs separately and
which Skywave neither bundles nor links.

Everything else in `src/`, `tests/`, and `tools/` is original to this
project.

## Skywave's own license

GPL-3.0-or-later ([LICENSE](LICENSE)), copyright The Skywave authors,
adopted 2026-10-18. Every component above is compatible with it: MIT,
BSD-3-Clause, ISC and zlib are permissive; Codec2's LGPL-2.1 allows
use from GPL-3.0 code (and the default build links it dynamically).
