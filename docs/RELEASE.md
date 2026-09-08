# Release Checklist

Versioning: semantic, single source of truth in the top-level
`CMakeLists.txt` `project(skywave VERSION x.y.z)`. `skywave --version`
reports it (`SKW_VERSION`). Wire compatibility is governed separately by
`kWireVersion` / `kProtocolVersion` (constants) — bump those ONLY on
breaking wire changes, never for releases.

## Gate sequence (all must pass, in order)

1. **Full suites, both flavors**
   ```
   cmake -B build            && cmake --build build -j            && ctest --test-dir build
   cmake -B build-crypto -DENABLE_CRYPTO=ON && cmake --build build-crypto -j && ctest --test-dir build-crypto
   ```
2. **Sanitizer sweeps** (`build-asan`, `build-ubsan`: same suites green;
   ThreadSanitizer on `test_st2` + `test_tcp` at minimum). Toolchain
   note (see ROADMAP deviations, 2026-07-30): Apple clang 17 compiler-rt
   on Darwin 25.6 cannot start ASan/TSan at all; Command Line Tools
   26.6 (clang 21) fixes this — configure the sanitizer builds with
   `-DCMAKE_C_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang`
   (+ clang++, + `-DCMAKE_OSX_SYSROOT=.../SDKs/MacOSX.sdk`) if the
   active Xcode still ships the broken runtime.
3. **Conformance sweeps** — archive under `docs/conformance/`. Fading
   sweeps run each tier on its DESIGNATED channel (kFadingFloors
   convention): CCIR Moderate for the robust tier on WIDE/MID/ULTRA,
   CCIR Good for the fast tier and for ALL NARROW modes — a single
   moderate sweep across every mode "fails" Good-designated points by
   construction.
   ```
   build/bertool --preset awgn --trials 40 --csv docs/conformance/rel_awgn.csv
   build/bertool --preset moderate --trials 40 --modes 4,5,6,7 --families wide,mid,ultra
   build/bertool --preset good --trials 40 --modes 8,9,10,11,12,13
   build/bertool --preset good --trials 40 --modes 4,5,6,7 --families narrow
   build/bertool --css --preset awgn --trials 40 --csv docs/conformance/rel_css.csv
   build/bertool --band vhf-fm --trials 40 --csv docs/conformance/rel_vhf.csv
   ```
   Every sweep must print `== all points pass ==`.
4. **ST3 hardware gate** (real/virtual audio devices). The gate is a
   CONFIGURE-time option, not an environment variable — `ctest` cannot
   enable a test that was configured `DISABLED`, and the old
   `SKW_HARDWARE_TESTS=1 ctest -L hardware` form silently ran NOTHING
   (`Not Run (Disabled)` → `No tests were found!!!`) while still exiting
   0, so the gate could appear to pass without executing:
   ```
   cmake -B build -DSKW_HARDWARE_TESTS=ON
   ctest --test-dir build -L hardware --output-on-failure
   ```
   Without the loopback devices the script exits 77 and ctest reports a
   SKIP. Equivalent direct invocation (useful when a build tree is
   configured without the option):
   ```
   tests/st3_hardware.py --binary build/skywave
   ```
5. **Endurance soak** (nightly-class, not per-commit):
   ```
   tests/st3_hardware.py --binary build/skywave --soak-sec 7200
   ```
6. **Clean-prefix install**
   ```
   cmake --install build --prefix /tmp/skywave-stage
   /tmp/skywave-stage/bin/skywave --version
   ```
   The staged binary must run from the prefix with no repo access.
7. **Docs refresh** — ROADMAP boxes/deviations current; WAVEFORM /
   PROTOCOL / CONFIGURATION match the code; THIRD_PARTY.md covers every
   linked or vendored component.

## Cutting the release

1. Bump `project(skywave VERSION …)`; commit `release: vX.Y.Z`.
2. `git tag -a vX.Y.Z -m "Skywave vX.Y.Z"`.
3. Build a source archive: `git archive --format=tar.gz -o skywave-X.Y.Z.tar.gz vX.Y.Z`.
4. Record the conformance CSVs and the suite/gate outputs alongside the
   tag (release notes).

## Support surface shipped

- `skywave` binary (modem: CLI + TCP data/control/voice ports + GUI)
- `bertool` (conformance/verification; not installed by default)
- `docs/` (installed to `share/doc/skywave`)
