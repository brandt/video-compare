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
