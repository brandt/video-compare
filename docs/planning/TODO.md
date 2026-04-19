# TODO

- BUG: Seeking while paused is only going to the keyframe and not then playing to the exact frame.
- BUG: Shift+A is not going to the previous frame, but instead is going to the previous keyframe.

- BUG: Backtick does not auto-align videos when the lower frame rate video is on the right like: ./video-compare testdata/sdr-2.mov testdata/sdr-1.mp4 ; Probably happens because we align the right video to the position of the left video, but the left video position does not align well with any right video position. We should either (A) align to whichever video has the higher framerate, (B) also have a way to shift the left video to the right video's position, or (C) align to one and then the other.

## WIP

## Done

- FEATURE: Add an option to crop out the black bars around videos in GPU mode.
- FEATURE: Add an option to crop out the black bars around videos in SDL mode.
