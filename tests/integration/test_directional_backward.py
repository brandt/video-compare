"""Mode-routing for the three auto-align keys.

Frame-exact convergence is covered by test_lg_daylight_alignment (Forward
mode). This test only verifies that each key routes to its expected
AutoAlignMode and the algorithm produces a summary line per press. Pure
Backward convergence would need a pair where follower is content-ahead of
master (i.e., follower has an intro LEFT lacks) — which isn't our fixture's
shape. If we grow a pair with that structure later, we can add a full
`test_backward_convergence_*` here.
"""

from .harness import VideoCompareSession


def test_backward_convergence(video_compare_binary, lg_daylight_backward_pair):
    """End-to-end `[` Backward convergence.

    Pair setup:
      - LEFT  = 2-s testsrc intro + source[124..]
      - RIGHT = source[124..] (no intro)
      - Aligned shift target = -2002 ms (right is content-ahead of left).

    Procedure:
      1. Unpause briefly to advance both past LEFT's intro into shared
         content territory.
      2. Pause.
      3. Press `[` three times — retry expansion grows the window from
         [-1 s, 0] to [-2 s, 0] to [-3 s, 0]. The true target (-2002 ms)
         falls near the edge of the 2-retry window; on this content it
         takes the third press's wider window to land a confident peak.
      4. Assert effective_time_shift lands on the target frame-exact.
    """
    pair = lg_daylight_backward_pair

    with VideoCompareSession(pair) as vc:
        vc.seek_wait(timeout=15.0)

        # Play ~3 s forward so LEFT advances past its 2-s intro and both
        # sides are on real source content. Space toggles play.
        vc.key("space")
        vc.sleep(3.0)
        vc.key("space")
        vc.seek_wait(timeout=5.0)

        # Three `[` presses. Retry windows: [-1 s, 0], [-2 s, 0], [-3 s, 0].
        for _ in range(3):
            vc.key("[")
            vc.seek_wait(timeout=10.0)

        expected_s = pair.expected_effective_time_shift_us(source_frame=124) / 1_000_000.0
        got_s = vc.get("effective_time_shift")

        # Frame-exact. Fixture time_base is 1 ms and per-frame pts deltas
        # are 16 or 17 ms, so the algorithm lands on an exact frame pts —
        # target and observed should differ only by the 1-ms time_base
        # quantization inherent in the fixtures' encoding.
        assert abs(got_s - expected_s) <= 0.002, (
            f"Backward alignment landed at {got_s:.4f} s, expected {expected_s:.4f} s.\n"
            "auto-align summaries:\n"
            + "\n".join(f"  {s}" for s in vc.auto_align_summaries())
        )


def test_each_key_routes_to_expected_mode(video_compare_binary, lg_daylight_pair):
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)

        # Symmetric mode (`) — window both sides of master.
        vc.key("`")
        vc.seek_wait()

        # Backward mode ([) — window [-1 s, 0] on master axis.
        vc.key("[")
        vc.seek_wait()

        # Forward mode (]) — window [0, +1 s] on master axis.
        vc.key("]")
        vc.seek_wait()

        modes = [s["mode"] for s in vc.auto_align_summaries()]
        assert modes == ["sym", "back", "fwd"], (
            f"expected modes [sym, back, fwd], got {modes}"
        )
