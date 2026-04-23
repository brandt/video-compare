# Tests

Two layers, one command each:

- `make test-unit` — C++ unit tests via doctest. Fast, no fixtures, no video pipeline.
- `make test-integration` — pytest driving the built `./video-compare` through its Unix-domain-socket control surface. Video fixtures downloaded from S3 (cached locally).
- `make check` — both of the above.

## Unit tests (`tests/unit/`)

Per-module doctest TUs linked into a single `video-compare-tests` binary. Each test file lists the `src/` objects it needs in a `<name>_OBJS` Makefile variable; header-only pieces (TimeShifter, FrameRing) need nothing.

Current coverage:
- `time_shifter_test.cpp` — `set_frame_shift_accumulator`, `set_static_shift_us`, effective/common-pts math.
- `structural_correlation_test.cpp` — Pearson correlation on normalized fingerprints.
- `fingerprint_test.cpp` — `compute_structural_fingerprint` via real swscale contexts.
- `frame_ring_test.cpp` — pivot_forward/backward bounds, `advance`, capacity trim.
- `packet_ring_test.cpp` — keyframe lookup, range opening on discontinuity, clear.

Add a test by dropping a `foo_test.cpp` into `tests/unit/` and (if it needs linked code) listing `src/foo/bar.o` in `foo_test_OBJS` in the Makefile's test section.

## Integration tests (`tests/integration/`)

Launches `./video-compare` per test via `VideoCompareSession`, drives it through the socket, parses logs, tears down cleanly. On assertion failure, the child's stderr log is copied into `tests/integration/_artifacts/` for post-mortem.

### Default pair

`lg_daylight_pair` pytest fixture → `lg-daylight-480p-auto-align` (6–8 MB each side, ~400 ms L2 drain per seek). Use this for any logic/behavior test.

`lg_daylight_2160p_pair` pytest fixture → `lg-daylight-auto-align` (75 MB + 38 MB, ~1.3 s L2 drain). Reserved for resolution-mismatch and performance-regression tests. The only test currently using it is `test_2160p_alignment_performance.py`.

### The harness API

```python
with VideoCompareSession(pair) as vc:
    vc.seek_wait(timeout=15.0)     # wait for first frame to render + PAUSE
    vc.key("]")
    vc.seek_wait()                 # wait for the iteration that processes the key
    shift = vc.get("effective_time_shift")
    vc.auto_align_summaries()      # list of parsed `[auto-align] mode=...` log lines
    vc.auto_align_landed_lines()   # list of post-seek verification lines
    vc.seek_timing_lines()         # list of `[seek-timing] tier=...` lines
```

`seek_wait()` is the replacement for fixed `vc.sleep(N)`: it polls `frame_number` (loop-iteration counter) + `play_state == "PAUSE"` until the key press has been processed. Hardware-neutral.

### Socket fields exposed for tests

`play_state`, `left_pts`, `right_pts`, `effective_time_shift` (common-time values),
`left_raw_pts`, `right_raw_pts` (raw, pre-shift),
`left_decoded_picture_number`, `right_decoded_picture_number`,
`frame_number`, `initialized`,
`swap`, `window_size`, `drawable_size`, `uptime`, `script_running`.

## Fixture management (`tests/fixtures/`)

Fixtures are video files produced by ffmpeg recipes in `generate.py`. Sidecar JSONs in `sidecars/` describe the generation params and include the full per-frame PTS array (the load-bearing ground-truth for frame-exact tests). Pair JSONs in `pairs/` reference two fixtures plus source-frame "check points".

Tests download fixtures from S3 on demand via `Fixture.ensure_local()`. Local cache lives in `tests/fixtures/_cache/fixtures/`; gitignored. Sha256 is verified on download and re-use.

### Regenerating fixtures

`generate.py` is a one-off, run manually when adding or updating a recipe — **not** part of the test pipeline.

```bash
# Generate one recipe locally (fixture lands in tests/fixtures/_cache/fixtures/).
python3 tests/fixtures/generate.py --recipe lg-daylight-sdr-h264-480p-trim124

# Generate + upload to S3 (requires the public-test-fixtures AWS profile).
python3 tests/fixtures/generate.py --upload
```

Sources are large (~150 MB each); they're also mirrored to S3 and only re-uploaded if asked (`--upload-sources-only`).

### Adding a new recipe

1. Add a `Recipe(...)` entry to `RECIPES` in `generate.py`.
2. Run `python3 tests/fixtures/generate.py --recipe <fixture_id>`.
3. Commit the sidecar (`tests/fixtures/sidecars/<fixture_id>.json`).
4. If it's part of a new pair, add `tests/fixtures/pairs/<pair_id>.json`.
5. Upload to S3 once (or pass `--upload` on step 2).

### Alignment ground truth

Each fixture sidecar records:
- `content_mapping.first_source_frame_shown` — the source-frame index that plays at this fixture's `raw_pts=0`.
- `encoded.frame_pts_us` — the full array of per-frame PTSes, in microseconds.

A pair sidecar lists source-frame "check points". `Pair.expected_effective_time_shift_us(source_frame=K)` returns the raw right-vs-left delta when both sides display source-frame K — exact, no tolerance-tuning by hand.

## Artifacts

Failed-run logs: `tests/integration/_artifacts/<pair-id>-<unix-ts>.log`. Gitignored. Safe to delete whenever.
