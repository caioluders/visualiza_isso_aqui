# visualiza_isso_aqui

OpenGL audio visualizer with a headless EDM analysis test engine.

## Fast Algorithm Tests

The detector test path intentionally avoids the interactive app dependencies. It builds only `analysis_probe` and KissFFT, downloads or reuses EDM fixtures, converts them to WAV with `ffmpeg`, and writes a JSON quality report.

```bash
tools/run_edm_fixture_tests.py --build-dir build/probe_runner --limit-seconds 90
```

The report is written to:

```text
build/edm_fixture_report.json
```

To inspect the engine frame-by-frame from the CLI without launching the visualizer:

```bash
build/probe_runner/analysis_probe --wav path/to/file.wav --frames-jsonl
```

This emits one JSON object per analysis block with `timestamp_sec`, `rms`, low/mid/high bands, onset, spectral features, beat state, percussive ratio, and the full 16-band arrays.

Converted WAV fixtures are stored in:

```text
build/edm_fixtures/
```

In the interactive app, enable `Use audio file`, choose `Browse WAV`, and load one of those files to visually inspect the same material used by the headless detector tests.

The summary includes pass/fail plus quality scores for signal level, onset response, beat response, beat-rate control, live processing headroom, live jitter, and spectral nuance.

## Deterministic EDM Truth Fixtures

For fixture generation with known semantic ground truth, render the in-repo deterministic EDM set:

```bash
node tools/generate_edm_fixtures.js --out-dir build/generated_edm_fixtures
```

This writes:

```text
build/generated_edm_fixtures/
  *.wav
  *.truth.json
  generated_fixtures_manifest.json
```

Each `*.truth.json` contains block-aligned known values for:

- `kick`
- `bass`
- `harmonic`
- `lead`
- `air`
- `perc`
- `energy`
- `tension`
- `release`

The generated suite now carries explicit `split` and `style` metadata:

- `dev`: deterministic fixtures used for day-to-day tuning
- `holdout`: broader style fixtures used to check for overfitting

To compare the live analyzer against one generated fixture:

```bash
python3 tools/compare_generated_fixture_truth.py \
  --probe build/probe_runner/analysis_probe \
  --truth-json build/generated_edm_fixtures/semantic_build_drop.truth.json
```

To generate all deterministic fixtures and score the analyzer against all of them:

```bash
python3 tools/run_generated_fixture_truth_tests.py \
  --probe build/probe_runner/analysis_probe
```

The aggregate report is written to:

```text
build/generated_fixture_truth_report.json
```

The report includes:

- overall score across all generated fixtures
- `split_summaries.dev`
- `split_summaries.holdout`
- per-role and focus-role averages for each split

Important live metrics:

- `realtime_ratio`: total analysis time divided by processed audio duration.
- `p95_block_realtime_ratio`: 95th-percentile per-block analysis time divided by one audio block duration. This catches jitter that averages can hide.
- `max_block_realtime_ratio`: worst single-block processing spike divided by one audio block duration.
- `beat_rate_per_minute`: detector trigger rate normalized by fixture length, bounded by fixture thresholds to catch over-triggering.

To configure the small build directly:

```bash
cmake -S . -B build/probe_only -DCMAKE_BUILD_TYPE=Release -DVISUALIZA_BUILD_APP=OFF -DVISUALIZA_BUILD_TEST_ENGINE=ON
cmake --build build/probe_only --target analysis_probe -j2
```

## Interactive App

Default app builds keep FFglitch disabled to avoid compiling FFmpeg:

```bash
cmake -S . -B build/app_default -DCMAKE_BUILD_TYPE=Debug
cmake --build build/app_default --target visualiza_isso_aqui -j2
```

## Release Builds

GitHub Actions builds release archives for the supported desktop targets:

- `linux-x86_64`
- `macos-x86_64`
- `macos-arm64`

Each archive includes the visualizer app, `analysis_probe`, `assets/`, `tools/`, and this README. To publish a release with all binaries, push a version tag:

```bash
git tag -a v0.1.3 -m "v0.1.3"
git push origin v0.1.3
```

The same workflow can be run manually from the Actions tab to produce downloadable artifacts without creating a GitHub release.

macOS release apps are ad-hoc signed and verified in CI, but they are not notarized unless Developer ID credentials are added to the release workflow. If Gatekeeper still marks a downloaded build as quarantined, remove the quarantine attribute before opening:

```bash
xattr -dr com.apple.quarantine /path/to/visualiza_isso_aqui.app
```

## p5.js Processing Engine

The app includes a true Processing-family visual mode backed by p5.js. The C++ app streams live analyzer metrics to a local Node server over stdin; headless Chromium renders the p5 sketch at the current viewport or fullscreen-output resolution and streams RGBA frames back into an OpenGL texture.

```text
assets/processing/sketches/
```

Sketches are normal p5.js files:

```js
function setup() {
  createCanvas(windowWidth, windowHeight);
}

function draw() {
  const a = window.visualizaAudio;
  background(0);
  circle(width * 0.5, height * 0.5, 80 + a.kick * 220);
}
```

In the app, select `Processing` in the `Visual` panel and choose a sketch. The p5 output appears in the `Viewport`, and the same texture can be routed to fullscreen output. Available live fields include `kick`, `bass`, `harmonic`, `lead`, `air`, `percussive`, `energy`, `tension`, `drop`, `bands`, and `onsets`.

Processing mode currently requires `node` plus a Chromium-family browser on the runtime machine. Linux looks for `chromium`, `chromium-browser`, or Google Chrome on `PATH`; macOS also checks Homebrew/MacPorts node paths plus standard Google Chrome, Chromium, Microsoft Edge, and Brave `.app` locations. The renderer is headless and streams frames back into the app, so p5 output works in both the docked viewport and fullscreen output without opening a separate visible browser window.

If Processing stays on `Waiting for p5 frames`, open `Visual > Processing Runtime` and check the displayed engine log, browser log, and frame file paths. The app uses per-process ports and temp frame files so stale Node/Chrome processes from a killed app instance do not hijack the renderer.

Enable FFglitch only when needed:

```bash
cmake -S . -B build/ffglitch -DVISUALIZA_ENABLE_FFGLITCH=ON
```

## Verification

Shader validation requires `glslangValidator`:

```bash
tools/validate_shaders.py
```

CTest entry points are available when configured with default `BUILD_TESTING=ON`:

```bash
ctest --test-dir build/probe_runner --output-on-failure
```

The CTest suite validates shaders, fixture manifest schema, generated WAV loader variants, synthetic bass/body/air/silence detector nuance, and the downloaded EDM fixture analysis path.
