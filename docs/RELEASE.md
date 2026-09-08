# Release Checklist

Versioning: semantic, single source of truth in the top-level
`CMakeLists.txt` `project(skywave VERSION x.y.z)`. `skywave --version`
reports it (`SKW_VERSION`). Wire compatibility is governed separately by
`kWireVersion` (`core/constants.hpp`, PHY header) and
`kProtocolVersion` (`link/handshake.hpp`, handshake) — bump those ONLY
on breaking wire changes, never for releases. Both are 1.

## Gate sequence (all must pass, in order)

1. **Full suites, both flavors**
   ```
   cmake -B build            && cmake --build build -j            && ctest --test-dir build -LE hardware
   cmake -B build-crypto -DENABLE_CRYPTO=ON && cmake --build build-crypto -j && ctest --test-dir build-crypto
   ```
   `-LE hardware` keeps gate 1 and gate 4 separate on a tree configured
   with `-DSKW_HARDWARE_TESTS=ON`. Without it the real-device tests join
   the default run, and anything else touching the loopback devices at
   that moment makes one of them fail on device-open in a few seconds.
2. **Sanitizer sweeps** (`build-asan`, `build-ubsan`: same suites green;
   ThreadSanitizer on `test_st2` + `test_tcp` at minimum).

   **Configure the sanitizer trees with `-DENABLE_GUI=OFF`:**
   ```
   cmake -B build-asan -DENABLE_ASAN=ON -DENABLE_GUI=OFF \
     -DCMAKE_C_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang \
     -DCMAKE_CXX_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang++
   ```
   Every test binary links `skywave_core`, which links SDL2 when the
   GUI is on — so SDL2's library initializer runs at load time in every
   test, including ones that never touch a window. Under ASan that
   initializer takes its failure path and calls `error_dialog`, which
   opens a modal `NSAlert` and blocks in `runModal` forever. The first
   test hangs with a dialog on the operator's screen and ctest reports
   nothing at all; `sample <pid>` shows the stack ending in
   `dllinit → error_dialog → -[NSAlert runModal]`. TSan hangs the same
   way; UBSan does not. Turning the GUI off removes SDL2 from the test
   binaries entirely, which is right anyway — these sweeps target the
   modem, and the GUI has its own gate (`test_gui`, plus
   `SKW_GUI_WINDOW=1` for the real-window path).

   Toolchain
   note (see ROADMAP deviations, 2026-07-30): Apple clang 17 compiler-rt
   on Darwin 25.6 cannot start ASan/TSan at all; Command Line Tools
   26.6 (clang 21) fixes this — configure the sanitizer builds with
   `-DCMAKE_C_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang`
   (+ clang++, + `-DCMAKE_OSX_SYSROOT=.../SDKs/MacOSX.sdk`) if the
   active Xcode still ships the broken runtime.

   **Run the sanitizer suites OUTSIDE any command sandbox.** ASan's
   startup probes the filesystem, and a sandbox that blocks those
   probes makes every single test abort:
   ```
   ==NNNNN==Checking file existence is not allowed under sandbox.
   ```
   ctest reports this as `Subprocess aborted***Exception` on 40 of 40
   tests, which reads exactly like a catastrophic regression and is
   not one — the same binaries pass when run normally. If a sanitizer
   sweep fails *uniformly*, on tests that touch nothing in common,
   suspect the environment before the code: run one test directly with
   `ASAN_OPTIONS=verbosity=1` and read the first ten lines. A genuine
   ASan finding names a file, a line, and a memory operation.
3. **Conformance sweeps** — archive under `docs/conformance/`. Fading
   sweeps run each tier on its DESIGNATED channel (kFadingFloors
   convention): CCIR Moderate for the robust tier on WIDE/MID/ULTRA,
   CCIR Good for the fast tier and for ALL NARROW modes — a single
   moderate sweep across every mode "fails" Good-designated points by
   construction.
   ```
   build/bertool --preset awgn --trials 40 --csv docs/conformance/rel_awgn.csv
   build/bertool --preset moderate --trials 40 --modes 4,5,6,7 --families wide,mid,ultra --csv docs/conformance/rel_mod.csv
   build/bertool --preset good --trials 40 --modes 8,9,10,11,12,13 --csv docs/conformance/rel_good.csv
   build/bertool --preset good --trials 40 --modes 4,5,6,7 --families narrow --csv docs/conformance/rel_good_narrow.csv
   build/bertool --css --preset awgn --trials 40 --csv docs/conformance/rel_css.csv
   build/bertool --band vhf-fm --trials 40 --csv docs/conformance/rel_vhf.csv
   ```
   Every sweep must print `== all points pass ==` (the CSS sweep prints
   `== all CSS points pass ==`). **Every sweep writes a CSV.** The three
   fading commands previously had no `--csv`, so `rel_mod.csv` and
   `rel_good.csv` sat at their v1.0.0 contents while the gate was
   re-run and reported green — the archived fading evidence did not
   reflect the code being released. Check the file timestamps.
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
   linked or vendored component (it lives at the REPOSITORY ROOT,
   not in docs/, and is installed alongside them).

   The floor tables are checked mechanically, not by eye: the
   `docs_floors` ctest asserts that every OFDM floor published in
   WAVEFORM.md equals `kFloors` in `core/constants.hpp`. It runs in
   gate 1, so a floor edit that skips the doc fails the suite. Nothing
   else in the docs is machine-checked, so the rest is a read.

   **BLOCKER (open): the repository has no `LICENSE` file.** Skywave's
   own terms of use and redistribution are therefore undefined, which
   blocks any public release regardless of how the other gates read.
   This is the owner's decision to make, not a technical one, so it is
   recorded here rather than resolved: pick the license, add `LICENSE`
   at the repository root, and reference it from README.md.

   Note also that a default build links Codec2, which is LGPL-2.1.
   Dynamic linking against the system package (what the default build
   does) is fine. Static linking or a single-file distribution takes on
   LGPL obligations; configure `-DENABLE_VOICE=OFF` to avoid them. See
   THIRD_PARTY.md.

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
