"""Performance regression canary for the 2160p pair.

Runs the two-press forward-alignment sequence against the full-resolution
2160p h264/VP9 pair to stress the matcher at UHD. The end-state must be
frame-exact (same contract as the 480p test), AND the total wall-clock
time must stay within a per-host baseline.

Two presses is the convergence sequence under the current directional
semantics: press 1 seeds the cache with searched_window=[0, +1]s (too
narrow for this pair's +2069 ms target, so low_confidence), press 2
extends to [0, +2]s which captures the 1.000 peak and seeks frame-exact.
A third `]` press here would iterate one frame forward to the next
at-or-above candidate (0.9994 neighbor) per the directional step-through
semantics — that's the intended behavior, not frame-exact on the target.

**Hardware-dependent baseline**: the threshold is tuned for the author's
dev machine (Apple M-series). On slower hardware this test may fail even
when the code is healthy. Run it first on a new host, observe the actual
elapsed time, and adjust BASELINE_SECONDS if needed — the test's job is
to catch *regressions* in the matcher's big-input behavior, not to enforce
a universal speed contract. Log-file artifacts on failure live in
tests/integration/_artifacts/ to help diagnose.
"""

import time

import pytest

from .harness import VideoCompareSession


# Baseline measurement on Apple M-series (Apr 2026):
#   2-press end-to-end wall-clock for 2160p h264/VP9 pair: ~5-6 s with the
#   current matcher. Threshold allows 2.5× headroom for noise on the same
#   host; a significant regression would push it well above ceiling.
BASELINE_SECONDS = 6.0
REGRESSION_CEILING_MULTIPLIER = 2.5


def test_2160p_two_press_frame_exact_and_under_budget(video_compare_binary, lg_daylight_2160p_pair):
    """Runs the full 2-press alignment at 2160p; asserts frame-exact landing
    AND total wall-clock under the regression ceiling."""
    pair = lg_daylight_2160p_pair

    with VideoCompareSession(pair) as vc:
        vc.seek_wait(timeout=20.0)

        start = time.monotonic()
        for _ in range(2):
            vc.key("]")
            # 2160p L2 drain + walk can take 1–3 s per press; give ample room
            # so the test fails on correctness, not timeout.
            vc.seek_wait(timeout=20.0)
        elapsed = time.monotonic() - start

        expected_us = pair.expected_effective_time_shift_us(source_frame=124)
        expected_s = expected_us / 1_000_000.0
        got_s = vc.get("effective_time_shift")

        assert abs(got_s - expected_s) <= 0.002, (
            f"2160p frame-exact alignment failed: got {got_s:.4f} s, expected {expected_s:.4f} s. "
            f"auto-align summaries:\n"
            + "\n".join(f"  {s}" for s in vc.auto_align_summaries())
        )

        ceiling = BASELINE_SECONDS * REGRESSION_CEILING_MULTIPLIER
        assert elapsed <= ceiling, (
            f"2160p alignment took {elapsed:.2f} s, ceiling is {ceiling:.1f} s "
            f"({BASELINE_SECONDS:.1f} s baseline × {REGRESSION_CEILING_MULTIPLIER:.1f} "
            f"regression headroom). Either the matcher slowed down on UHD inputs, "
            f"or this host is significantly slower than the baseline machine — "
            f"see module docstring for how to retune."
        )
