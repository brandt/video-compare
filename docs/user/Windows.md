# Windows Tips

Tips for Windows users.

## Practical tips

### Send To Integration in Windows File Explorer

You can launch `video-compare` directly from Windows File Explorer when you only need to specify input files. Simply use:

**Right click → Send to → video-compare**

#### How it works

https://user-images.githubusercontent.com/8549626/166630445-c8c511b7-005f-48aa-83bc-0eb9676cfa2a.mp4

For quick access, select two files, right-click either one, then press:

- **N** to focus _Send to_
- **V** to select _video-compare_

#### Setup

To make _video-compare_ appear in the **Send to** menu:

1. Open the Run dialog (**Windows + R**)
2. Type `shell:sendto` and press Enter
3. Create a shortcut to `video-compare.exe` in this folder

Thanks to [couleurm](https://github.com/couleurm) for sharing this tip and providing the screen recording.

### More frontend options for Windows users

For Windows users, the community has shared several frontend options to complement the command-line functionality:

1. **Beyond Compare** integration: Launch `video-compare` directly from the interface.
2. **Total Commander** integration: Add a toolbar button to open selected videos.
3. **[VideoCompareGUI](https://github.com/TetzkatLipHoka/VideoCompareGUI)**: A standalone graphical utility that simplifies launching `video-compare`.

For details, check out the [open GitHub issue thread](https://github.com/pixop/video-compare/issues/81).
