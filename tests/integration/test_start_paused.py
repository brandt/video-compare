"""Guards the --start-paused black-frame regression.

History: an earlier attempt at --start-paused called `playback_.set_play(false)`
inside Display's constructor. That left the ring empty (the main loop only
advances the ring when `play_` is true or a forward-nav is pending), so the
display had no current_frame to render — a literal black window at launch.

Fix: defer the pause via `pending_startup_pause_` and apply it inside
`possibly_refresh` once the first frame has actually rendered. This test
encodes that contract as an assertion on a few socket-visible quantities
that together prove the first frame made it through the pipeline:

  - play_state == "PAUSE"
  - initialized == True (compare() has published a real snapshot)
  - frame_number > 0 (main loop ran past the first iteration)
  - right_decoded_picture_number >= 1 (at least one frame decoded on RIGHT)
"""

from .harness import VideoCompareSession


def test_start_paused_renders_first_frame_then_pauses(video_compare_binary, lg_daylight_pair):
    with VideoCompareSession(lg_daylight_pair) as vc:
        # The pipeline needs at least one frame rendered before
        # --start-paused flips to PAUSE. seek_wait polls frame_number
        # advancing + play_state==PAUSE, which is exactly this condition.
        vc.seek_wait(timeout=15.0)

        play_state = vc.get("play_state")
        initialized = vc.get("initialized")
        frame_number = vc.get("frame_number")
        left_dpn = vc.get("left_decoded_picture_number")
        right_dpn = vc.get("right_decoded_picture_number")

        assert play_state == "PAUSE", f"expected PAUSE, got {play_state!r}"
        assert initialized is True, "snapshot was never published"
        assert frame_number > 0, f"main loop didn't run past first iteration: frame_number={frame_number}"
        assert left_dpn >= 1, f"left side decoded no frames: left_decoded_picture_number={left_dpn}"
        assert right_dpn >= 1, f"right side decoded no frames: right_decoded_picture_number={right_dpn}"
