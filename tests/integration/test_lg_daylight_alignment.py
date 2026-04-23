"""Integration tests for the 3-press auto-align retry mechanism on the
lg-daylight pair.

Guards the regressions surfaced during the 2026-04-22 session:
  1. Probe coverage at ring edges — without the window-extension fix, the
     true-alignment candidate can't muster enough valid probes and the
     algorithm settles at a ring-interior local optimum.
  2. Post-walk/L1 demuxer seek direction — forward-seek to next keyframe
     polluted the ring prefetch with far-forward frames.
  3. static_shift drift — total_frames × avg_delta didn't match the exact
     landed raw PTS.

Current known gap (see test_three_press_reaches_target_frame_exact xfail):
the L2 seek path lands imprecisely on this specific fixture pair. The
mechanism tests still run green because they observe the algorithm's
decisions (retry counter, cache reuse), which are what the regressions
actually changed.
"""

import pytest

from .harness import VideoCompareSession


def test_three_press_decision_sequence(video_compare_binary, lg_daylight_pair):
    """The retry counter and decisions must follow the expected sequence:
    press 1 low_confidence (retry=0), press 2 seeks (retry=1 — cache
    reused, ring=0 confirms), press 3 is a fresh press (retry=0)."""
    pair = lg_daylight_pair

    with VideoCompareSession(pair) as vc:
        vc.seek_wait(timeout=15.0)
        assert vc.get("play_state") == "PAUSE"
        assert vc.get("effective_time_shift") == 0.0

        for _ in range(3):
            vc.key("]")
            vc.seek_wait()

        summaries = vc.auto_align_summaries()
        assert len(summaries) >= 3, (
            f"expected at least 3 auto-align summaries, got {len(summaries)}:\n"
            + "\n".join(str(s) for s in summaries)
        )

        # Press 1: fresh (retry=0), low_confidence (window [0, +1s] too
        # narrow to reach the true alignment at +2069 ms).
        assert summaries[0]["retry"] == "0"
        assert summaries[0]["decision"] == "low_confidence", (
            f"press 1 expected low_confidence, got decision={summaries[0].get('decision')}"
        )

        # Press 2: retry=1 — cache carried forward from press 1. ring=0
        # confirms the cache was reused (no new ring contribution). Press 2
        # reaches the true alignment region and seeks.
        assert summaries[1]["retry"] == "1"
        assert summaries[1]["ring"] == "0", (
            f"press 2 expected ring=0 (cache reused), got ring={summaries[1].get('ring')}"
        )
        assert summaries[1]["decision"] == "seek"
        # Windowed extension check: press 2's window expanded to [0, +2s].
        assert summaries[1]["window"] == "[0.000,2.000]s"

        # Press 3: cache cleared by the seek, retry=0 again.
        assert summaries[2]["retry"] == "0"


def test_press_2_finds_high_structural_match(video_compare_binary, lg_daylight_pair):
    """Press 2's extended window must contain a candidate that scores above
    the confidence floor. Guards the probe-coverage regression — without the
    window-extension fix, ring-edge candidates got <3 valid probes and
    scored below the floor, so the algorithm refused to seek."""
    pair = lg_daylight_pair

    with VideoCompareSession(pair) as vc:
        vc.seek_wait(timeout=15.0)
        for _ in range(2):
            vc.key("]")
            vc.seek_wait()

        summaries = vc.auto_align_summaries()
        assert len(summaries) >= 2
        best_score = float(summaries[1]["best_score"])
        # The fingerprint of the aligned content correlates strongly in this
        # scene; the pair's true alignment is at ~2069 ms and press 2's
        # [0, +2s] window comes within a frame of it. Expect ≥0.9.
        assert best_score >= 0.9, (
            f"press 2 best_score={best_score} (expected ≥0.9). "
            f"Low scores here indicate the probe-coverage fix regressed."
        )


def test_three_press_reaches_target_frame_exact(video_compare_binary, lg_daylight_pair):
    """Frame-exact endgame: after three `]` presses, effective_time_shift
    must equal the pair's declared target (right_raw - left_raw for
    source_frame=124) to within 2 ms.

    This guards four regressions together:
      - Probe coverage at ring edges (window extension using prefetch_capacity).
      - L2-path drain-to-target skipped under pure_right_frame_shift — fixed
        by exempting auto-align via pending_auto_align_verification_.active.
      - Tie-break band of 0.002 let a near-peak candidate beat the true 1.000
        peak when it was closer to current — tightened to 0.0005.
      - static_shift frame-count drift — set_static_shift_us override in the
        post-seek verification block.
    """
    pair = lg_daylight_pair

    with VideoCompareSession(pair) as vc:
        # Initial startup wait — the pipeline needs at least one frame
        # decoded + rendered before --start-paused flips to PAUSE. Use
        # seek_wait with a longer timeout to absorb that.
        vc.seek_wait(timeout=15.0)
        for _ in range(3):
            vc.key("]")
            vc.seek_wait()

        expected_us = pair.expected_effective_time_shift_us(source_frame=124)
        expected_s = expected_us / 1_000_000.0
        got_s = vc.get("effective_time_shift")

        assert abs(got_s - expected_s) <= 0.002, (
            f"Final effective_time_shift = {got_s:.4f} s, "
            f"expected {expected_s:.4f} s (source_frame=124). "
            f"auto-align summary lines:\n"
            + "\n".join(f"  {s}" for s in vc.auto_align_summaries())
        )
