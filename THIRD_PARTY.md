# Third-Party Notices

Skywave links against or vendors the following third-party components.
Version numbers are whatever the build machine's package manager provides
unless vendored; license texts live with the upstream projects.

## Linked libraries (required)

| Component | License | Use |
|---|---|---|
| [PortAudio](https://www.portaudio.com/) | MIT-style (PortAudio license) | real-time audio I/O |
| [zstd](https://github.com/facebook/zstd) | BSD-3-Clause (dual GPLv2) | proto-pipeline payload compression |

## Linked libraries (optional, by build flag)

| Component | Flag | License | Use |
|---|---|---|---|
| [libsodium](https://libsodium.org/) | `ENABLE_CRYPTO` | ISC | X25519 KEX, HKDF, AES-256-GCM |
| [Codec2](https://github.com/drowe67/codec2) | `ENABLE_VOICE` | LGPL-2.1 | live voice vocoder (dynamically linked) |
| [SDL2](https://libsdl.org/) | `ENABLE_GUI` | zlib | GUI window/renderer |

## Vendored source

| Component | Location | License |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | `third_party/imgui/` | MIT |

## Standards-derived tables

- LDPC parity matrices (`src/fec/ldpc_tables.cpp`) are the QC-LDPC base
  matrices published in IEEE Std 802.11n (N=648/1944; rates 1/2, 2/3,
  3/4, 5/6), regenerated from the standard's tables.

Everything else in `src/`, `tests/`, and `tools/` is original to this
project.
