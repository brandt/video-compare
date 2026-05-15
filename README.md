# <img src="https://github.com/user-attachments/assets/da615466-683e-4cc5-8380-32a98d743ba0" alt="Logo" width="32"/>&nbsp; video-compare

[![GitHub release](https://img.shields.io/github/release/pixop/video-compare)](https://github.com/pixop/video-compare/releases)

Split-screen video comparison tool written in C++20, utilizing FFmpeg libraries and SDL2. It provides
interactive navigation and playback controls, along with various analysis tools and customizable display options.

`video-compare` can be used to visually compare the impact of codecs, resizing algorithms, and other modifications
on two video files played in sync. The tool is versatile, allowing videos of differing resolutions, frame rates,
scanning methods, color formats, dynamic ranges, input protocols, container formats, codecs, or durations.

Thanks to FFmpeg's flexibility, `video-compare` is also capable of comparing images or image sequences.

## Installation

### Arch Linux

Install [via AUR](https://aur.archlinux.org/packages/video-compare):

```sh
git clone https://aur.archlinux.org/video-compare.git
cd video-compare
makepkg -sic
```

### Homebrew

Install [via Homebrew](https://formulae.brew.sh/formula/video-compare):

The standard Homebrew formula for `video-compare` depends on the regular `ffmpeg` formula, which currently does not include `libvmaf` support. For full VMAF support and broader codec compatibility, you must install the `ffmpeg-full` formula first, and then install `video-compare` from source. If you already have `video-compare` installed, you will need to uninstall beforehand.

TODO: Update the `video-compare` Homebrew formula to depend on `ffmpeg-full` and remove this caveat.

```sh
brew install ffmpeg-full
brew install --build-from-source video-compare
```

> [!NOTE]  
> If the `ffmpeg-full` formula is updated in the future, you may also need to reinstall `video-compare` to ensure it links against the updated `ffmpeg-full` library versions.

### Pre-compiled Windows 10 binaries

Pre-built Windows 10 x86 64-bit releases are available from [this page](https://github.com/pixop/video-compare/releases).
Download and extract the .zip-archive on your system, then run `video-compare.exe` from a command prompt.

### Compile from source

[Build it yourself](#build).

## Screenshots

Visual compare mode:
![Visual compare mode](screenshot_1.jpg?raw=true)

Subtraction mode (plus time-shift, 200% zoom, and magnification):
![Subtraction mode"](screenshot_2.jpg?raw=true)

Vertically stacked mode:
![Stacked mode"](screenshot_3.jpg?raw=true)

## Credits

`video-compare` was created by Jon Frydensbjerg (email: jon@pixop.com). The code is mainly based on
the excellent video player GitHub project: https://github.com/pockethook/player

Many thanks to the [FFmpeg](https://github.com/FFmpeg/FFmpeg), [SDL2](https://github.com/libsdl-org/SDL) and
[stb](https://github.com/nothings/stb) authors.

## Usage

Launch using the operating system's DPI setting. Video pixels are doubled on devices like a Retina 5K display;
therefore, it is the preferred option for displaying HD 1080p videos on such screens:

    video-compare video1.mp4 video2.mp4

Allow high DPI mode on systems which supports that. Video pixels are displayed "1-to-1". Useful
for e.g. displaying UHD 4K video on a Retina 5K display:

    video-compare -d video1.mp4 video2.mp4

Increase bit depth to 10 bits per color component (8 bits is the default). Fidelity is increased while
performance takes a hit. Significantly reduces visible banding on systems with a higher grade display
and driver support for 30-bit color:

    video-compare -b video1.mp4 video2.mp4

Use a specific window size instead of deriving the window size from the video dimensions. The video
frame will be scaled to fit. If either width or height is left out, the missing value will be calculated
from the other specified dimension so that aspect ratio is maintained. Useful for downscaling high resolution
video onto a low resolution display:

    video-compare -w 1280x720 video1.mp4 video2.mp4

Size the window to fit the usable display bounds while maintaining the video’s aspect ratio. This option adjusts
for elements like taskbars or OS menus. Ideal for maximizing the viewing area while keeping the video dimensions
proportional to the screen:

    video-compare -W video1.mp4 video2.mp4

Automatic in-buffer loop playback, triggered when the buffer fills or end-of-file is reached, streamlines
video analysis by eliminating the need for manual replay initiation (bidirectional "ping-pong" mode, `pp`, is
also available):

    video-compare -a on video1.mp4 video2.mp4

Shift the presentation time stamps of the right video instead of assuming the videos are aligned. A
positive amount has the effect of delaying the left video while negative values conversely delays the
right video. Useful when videos are slightly out of sync:

    video-compare -t 0.080 video1.mp4 video2.mp4

Display videos stacked vertically at full size without a slider (`hstack` for horizontal stacking is
also supported):

    video-compare -m vstack video1.mp4 video2.mp4

Preprocess one or both inputs via a list of FFmpeg video filters specified on the command line
(see [FFmpeg's video filters documentation](https://ffmpeg.org/ffmpeg-filters.html#Video-Filters)).
The Swiss Army knife for cropping/padding (comparing videos with different aspect ratios),
adjusting colors, deinterlacing, denoising, speeding up/slowing down, etc.:

    video-compare -l crop=iw:ih-240 -r format=gray,pad=iw+320:ih:160:0 video1.mp4 video2.mp4

Select a demuxer that cannot be auto-detected (such as VapourSynth):

    video-compare --left-demuxer vapoursynth script.vpy video.mp4

Explicit decoder selection for the right video:

    video-compare --right-decoder h264_cuvid video1.mp4 video2.mp4

Compare an AV1 video against itself with and without film grain synthesis applied (requires `libdav1d`; `export_side_data=film_grain` disables grain synthesis during decoding):

    video-compare --right-decoder libdav1d:export_side_data=film_grain input_av1.mkv __

By default, `--hwaccel` is `auto`, which picks the first hardware acceleration type supported by the decoder;
pass `none` to disable hardware acceleration entirely.

Set the same hardware acceleration type for both videos:

    video-compare --hwaccel cuda video1.mp4 video2.mp4

Set the hardware acceleration type for the left video only:

    video-compare --left-hwaccel videotoolbox video1.mp4 video2.mp4

Disable hardware acceleration for the right video only:

    video-compare --right-hwaccel none video1.mp4 video2.mp4

By default, HDR videos are automatically color space converted to sRGB with an initial 500-nit peak light
level. This default can be overridden with a custom peak light level, such as 850 nits. The specified peak
light level is then dynamically adjusted during decoding based on any MaxCLL metadata:

    video-compare -R 850 sdr_video.mp4 hdr_video.mp4

Map a 500-nit peak light level HDR video for an sRGB SDR display, and adjust the tone of the SDR video
to simulate the relative light level difference between the two videos on an actual HDR display:

    video-compare -T rel -L 500 hdr_video.mp4 sdr_video.mp4

Perform simpler comparison of a video with itself using double underscore (`__`) as a placeholder. This
enables tasks such as comparing the video with a time-shifted version of itself or testing various sets
of filters, without the need to enter the same, potentially long path twice:

    video-compare some/very/long/and/complicated/video/path.mp4 __

Apply common filters to both videos and extend them with additional side-specific filters using the
placeholder resolution functionality. This structure also works for demuxer, decoder, and hardware
acceleration settings:

    video-compare -i yadif,hqdn3d -l setfield=bff,__ -r __,scale=iw/2:ih/2 video1.mp4 video2.mp4

Compare a reference (left) video against multiple renditions (right) by specifying more than two input paths.
Useful for comparing a reference encode to multiple renditions (e.g. different bitrates or encoder settings),
or for comparing ground truth, input, and model output in one session. Command-line settings are shared for all
right videos, and the active right video can be switched within the UI:

    video-compare reference.mp4 rendition1.mp4 rendition2.mp4

Override any command-line option for individual right videos using the `::` separator. Global settings
apply to all right videos by default, but can be overridden per-video:

    video-compare -r yadif input.mp4 output1.mp4 \
        output2.mp4::filters=__,scale=1920:-1 \
        output3.mp4::filters=::hwaccel=videotoolbox

The above features can be combined in any order, of course. Launch `video-compare` without any arguments to
see all supported options.


## Controls

See: **[Keyboard and Mouse Controls](docs/user/keyboard-and-mouse-controls.md)**


## Building

### Requirements

Requires FFmpeg headers and development libraries to be installed, along with SDL2 and
its TrueType font rendering add on (libsdl2_ttf). SDL2 version 2.0.10 or later is now
specifically required for subpixel accuracy rendering capabilities. Users may need to
upgrade their existing SDL2 installation before compiling.

On Debian GNU/Linux the required development packages can be installed via `apt`:

```sh
apt install build-essential libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev libswresample-dev libsdl2-dev libsdl2-ttf-dev python3-pytest doctest-dev
```

On Fedora Linux the required development packages can be installed via `dnf`:

```sh
dnf install make gcc-c++ ffmpeg-devel SDL2-devel SDL2_ttf-devel python3-pytest doctest-devel
```

On macOS the required libraries can be installed via Homebrew:

```sh
brew install sdl3 sdl2_ttf ffmpeg-full nlohmann-json pytest doctest
```

### Instructions

Compile the source code via GNU Make:

```sh
make
```

The linked `video-compare` executable will be created in the source code directory. To perform a system wide installation:

```sh
make install
```

Note that root privileges are required to perform this operation in most environments (hint: use e.g. `sudo`).

## Limitations

- Audio playback is not currently on the roadmap.

## Tips

Tips for Windows users can be found in [docs/user/Windows.md](docs/user/Windows.md).

## Contributing

We're always looking for ways to improve and expand the tool. Your feedback and contributions are appreciated.
