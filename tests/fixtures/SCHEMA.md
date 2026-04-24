# Fixture sidecar schema

## Fixture sidecars (`tests/fixtures/sidecars/<id>.json`)

Describes a single generated fixture video. The generator writes these; the
test harness reads them to know what to download and what the expected
per-frame PTS values are.

```json
{
  "fixture_id": "lg-daylight-sdr-h264-2160p-trim124",
  "description": "Source SDR VP9 trimmed to start at source frame 124, re-encoded to h264. Plays source_frames[124..N).",

  "source": {
    "id": "lg-daylight-sdr-vp9-2160p-59.94",
    "url": "https://public-test-fixtures.s3.amazonaws.com/video-compare/sources/lg-daylight-sdr-vp9-2160p.webm",
    "sha256": "<source file sha>"
  },

  "generation": {
    "ffmpeg_version": "ffmpeg version 8.1",
    "argv": ["ffmpeg", "-y", "-i", "<source>", "-ss", "2.069", "-t", "15", "-c:v", "libx264", "-g", "250", "-bf", "2", "<out>"]
  },

  "output": {
    "url": "https://public-test-fixtures.s3.amazonaws.com/video-compare/fixtures/lg-daylight-sdr-h264-2160p-trim124.mp4",
    "sha256": "<sha of output>",
    "byte_size": 12345678
  },

  "encoded": {
    "codec": "h264",
    "pixel_format": "yuv420p",
    "resolution": [3840, 2160],
    "fps_num": 60000,
    "fps_den": 1001,
    "time_base_num": 1,
    "time_base_den": 1000,
    "keyframe_interval_frames": 250,
    "has_b_frames": true,
    "total_frames": 900,
    "first_frame_pts_us": 0,
    "last_frame_pts_us": 15014658
  },

  "content_mapping": {
    "first_source_frame_shown": 124,
    "description": "At this fixture's raw_pts=0, the frame shown corresponds to source_frame=124."
  }
}
```

The `content_mapping.first_source_frame_shown` is the **load-bearing** field.
From it and `encoded.{fps_num, fps_den, time_base_num, time_base_den}`, the
test harness computes the raw PTS corresponding to any source frame:

```python
def raw_pts_us_for_source_frame(fx, source_frame):
    rel = source_frame - fx["content_mapping"]["first_source_frame_shown"]
    if rel < 0 or rel >= fx["encoded"]["total_frames"]:
        return None
    fps_num = fx["encoded"]["fps_num"]
    fps_den = fx["encoded"]["fps_den"]
    tb_num = fx["encoded"]["time_base_num"]
    tb_den = fx["encoded"]["time_base_den"]
    # pts in stream_tb units: pts = round(rel * fps_den * tb_den / (fps_num * tb_num))
    pts_stream_tb = round(rel * fps_den * tb_den / (fps_num * tb_num))
    # Convert stream_tb units -> microseconds.
    return pts_stream_tb * 1_000_000 * tb_num // tb_den
```

## Pair sidecars (`tests/fixtures/pairs/<id>.json`)

Describes a LEFT/RIGHT pair and the source frames useful as alignment
check-points. The test harness computes the expected raw PTS for both sides
from the referenced fixture sidecars.

```json
{
  "pair_id": "lg-daylight-auto-align",
  "description": "Base (source from frame 0) vs. trimmed (source from frame 124). Right must advance ~2.069s to align with left's current.",

  "left":  "lg-daylight-sdr-h264-2160p-trim124",
  "right": "lg-daylight-hdr-vp9-2160p-base",

  "check_points": [
    {
      "source_frame": 124,
      "rationale": "First aligned frame shown at left raw_pts=0 and right raw_pts≈2069ms."
    },
    {
      "source_frame": 200,
      "rationale": "Mid-content sanity."
    }
  ]
}
```
