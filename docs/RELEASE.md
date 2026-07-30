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
   ThreadSanitizer on `test_st2` + `test_tcp` at minimum). Known host
   limitation (see ROADMAP deviations, 2026-07-30): Apple clang 17.0.0
   compiler-rt on Darwin 25.6 cannot start ASan (init spin) or TSan
   (init SIGSEGV) at all — on such hosts UBSan is the required gate and
   the ASan/TSan sweeps are deferred to a working toolchain.
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
4. **ST3 hardware gate** (real/virtual audio devices):
   ```
   SKW_HARDWARE_TESTS=1 ctest --test-dir build -L hardware --output-on-failure
   ```
5. **Endurance soak** (nightly-class, not per-commit):
   ```
   SKW_HARDWARE_TESTS=1 SKW_ST3_SOAK=7200 ctest --test-dir build -L hardware
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
