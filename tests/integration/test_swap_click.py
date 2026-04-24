"""Phase 2 of the swap rollout — shift-click on the timeline scopes its
seek to the visually-right side.

The click handler is `mouse_x / window_width` (see
`Display::handle_mouse_button_event`); any left-click with Shift held
qualifies — there's no separate timeline widget, so the y coordinate
doesn't matter. That makes window layout irrelevant here.

Without swap, the scoped side is the underlying RIGHT pipeline. Under
swap (visually-right == underlying LEFT), it's LEFT. Both cases should:
  - leave the stationary side's pts approximately unchanged;
  - change effective_time_shift by a non-trivial amount.
"""

from .harness import VideoCompareSession


def _shift_click(vc, frac_x: float) -> None:
    width = vc.get("window_size")["w"]
    click_x = int(width * frac_x)
    # y doesn't matter; the handler uses mouse_x / window_width only.
    vc.call({
        "cmd": "mouse", "action": "button", "button": "left",
        "down": True, "x": click_x, "y": 100, "mods": ["shift"],
    })
    vc.call({
        "cmd": "mouse", "action": "button", "button": "left",
        "down": False, "x": click_x, "y": 100, "mods": ["shift"],
    })
    vc.seek_wait(timeout=10.0)


def test_shift_click_scopes_to_visual_right(video_compare_binary, lg_daylight_pair):
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)

        def state() -> dict:
            return {
                "left_raw": vc.get("left_raw_pts"),
                "right_raw": vc.get("right_raw_pts"),
                "shift": vc.get("effective_time_shift"),
            }

        # --- Case A: no swap, shift-click at 50% ---
        assert vc.get("swap") is False
        before_a = state()
        _shift_click(vc, 0.5)
        after_a = state()

        # LEFT (stationary under no-swap shift-click) must not have moved.
        assert abs(after_a["left_raw"] - before_a["left_raw"]) < 0.05, (
            f"no-swap: LEFT must stay put, got "
            f"{before_a['left_raw']:.3f} -> {after_a['left_raw']:.3f}"
        )
        # effective_time_shift must change meaningfully (click at 50 % of the
        # duration drives the right side somewhere in the middle).
        assert abs(after_a["shift"] - before_a["shift"]) > 0.5, (
            f"no-swap: effective_time_shift should change, got "
            f"{before_a['shift']:.3f} -> {after_a['shift']:.3f}"
        )

        # --- Case B: enable swap, shift-click at 25% ---
        vc.key("s")
        vc.seek_wait(timeout=5.0)
        assert vc.get("swap") is True
        before_b = state()
        _shift_click(vc, 0.25)
        after_b = state()

        # Under swap, RIGHT (the underlying pipeline) is the stationary
        # master — its raw pts must not have moved.
        assert abs(after_b["right_raw"] - before_b["right_raw"]) < 0.05, (
            f"swap: RIGHT must stay put, got "
            f"{before_b['right_raw']:.3f} -> {after_b['right_raw']:.3f}"
        )
        # shift must change, in the opposite direction from case A (Case A
        # increased; 25 % is behind where Case A landed, so shift decreases).
        assert abs(after_b["shift"] - before_b["shift"]) > 0.5, (
            f"swap: effective_time_shift should change, got "
            f"{before_b['shift']:.3f} -> {after_b['shift']:.3f}"
        )
