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
