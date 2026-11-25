# AdsCutter

AdsCutter is a Qt 6 desktop application for Linux that cuts the ads out of
DVB-T/T2 TV recordings **without re-encoding**. It reads MPEG transport streams
(`.ts`) and Matroska (`.mkv`) files and writes Matroska. Streams are copied, so
exporting takes seconds and the picture and sound are not degraded.

Each part you keep starts on a *cut point*, a frame where decoding can begin,
and can end on any frame. For H.264 and HEVC, the first picture of each part is
rewritten into a clean starting picture, so players don't show glitches at the
joins.

Ad detection is built in, but it only runs when you ask for it. It looks for
fades to black, the short black separators between spots, and changes in parts
of the picture you mark, such as the text under a channel logo. From these it
proposes a cut list that you can review and edit before exporting.

## Features

- Frame-accurate navigation: frame by frame, cut point by cut point, or in
  1 s, 10 s and 1 min steps, with a zoomable timeline.
- Video codecs: MPEG-2, H.264 and HEVC, the ones used in DVB-T and DVB-T2.
  Any audio codec FFmpeg can demux is copied as is, and LOAS/LATM AAC is
  converted to plain AAC so Matroska can store it.
- Choose which video, audio and subtitle streams to export. Teletext can't be
  stored in Matroska, so it is left out.
- Handles broadcast quirks: 33-bit timestamp wraps, clock jumps when the
  broadcaster's encoder restarts, and recordings that start mid-stream.
- Hardware-accelerated decoding through Vulkan or VAAPI, with a software
  fallback. Deinterlacing for interlaced broadcasts.
- Playback with sound, with a mode that plays only the kept parts.
- Undo and redo. The cut list is saved as JSON next to the recording.
- Optional ad detection: fades, separators and user-drawn regions, combined
  into a proposed cut list, with previews of the cues and the cuts.

## Installing

### Dependencies

- Qt 6: Core, Gui, Widgets and Multimedia. Install `qt6-svg` as well to see the
  icon on X11 and in the About box.
- FFmpeg libraries: libavformat ≥ 61, libavcodec ≥ 61, libavutil ≥ 59,
  libswscale and libswresample (FFmpeg 7 or later).
- To build: a C++20 compiler and Meson ≥ 1.3.
- To run the tests: the `ffmpeg` command-line tool with libx264 and libx265,
  `python3` and Qt Test.

### Building from source

```sh
git clone https://github.com/mbeniamino/adscutter.git
cd adscutter
meson setup build --buildtype=release
meson compile -C build
meson test -C build                 # optional
sudo meson install -C build
```

The tests use synthetic recordings that `ffmpeg` generates at build time. The
tests in the `hw` suite check Vulkan and VAAPI decoding and are skipped when
those aren't available. Use `meson test -C build --no-suite hw` to leave them
out.

### Arch Linux

`packaging/arch/PKGBUILD` builds the release tag that matches its `pkgver`
from the repository it is in:

```sh
cd packaging/arch
makepkg -si
```

## Usage

```sh
adscutter [--verbose] [recording.ts]
```

You can also drop a recording on the window. The first time you open a file,
AdsCutter indexes it, which takes one pass over the file. The index is cached,
so opening the same file again is quick.

1. **Find the parts to keep.** Move with the arrow keys: Left/Right step one
   frame and Up/Down jump to the next or previous cut point. Press **I** to mark
   the start of a part to keep and **O** to mark its end. The start always
   snaps to a cut point. Repeat for each part.
2. **Check the result.** Press **K** to play only the kept parts. The segment
   panel lists them, and Ctrl+Up/Ctrl+Down jump between their boundaries.
3. **Export.** Tick the streams to keep in the streams panel, press **Ctrl+E**
   and choose a file name. The default is `name.cut.mkv`.

The cut list is saved as `name.adscutter.json` next to the recording and is
loaded again when you reopen it.

### Finding ads automatically

Everything is in the **Analysis** menu:

- **Find Breaks by Fades** (Ctrl+Shift+A) looks for fades to black and for the
  black, silent separators between spots.
- **Draw Region…** marks part of the picture that looks different during ads,
  for example a channel logo or the text under it. Then say whether it is
  shown during the programme or during the ads. **Find Changes in Regions**
  (Ctrl+Shift+R) scans the whole recording for it. Regions can be saved as
  named presets, for example one for each channel.
- **Propose Cut List** (Ctrl+Shift+C) combines what the analyses found into a
  cut list. It replaces the current list, and Ctrl+Z undoes it.
- **P** previews the cues and **Shift+P** previews the cuts, so you can check
  each break before you export.

Regions give the most reliable results. Fades only fine-tune where a break
starts and ends.

**Help › Keyboard Shortcuts** (F1) lists every shortcut.

### Hardware decoding

**View › Hardware Decoding** chooses between Vulkan, VAAPI and software
decoding. The `ADSCUTTER_HWDEC` environment variable (`auto`, `vulkan`, `vaapi`
or `none`) overrides this setting.

### Files

| Path | Contents |
|------|----------|
| `name.adscutter.json` | Cut list, next to the recording |
| `~/.cache/adscutter/` | Packet indexes and analysis results. Safe to delete, they are rebuilt |
| `~/.config/adscutter/AdsCutter.conf` | Preferences, recent files and region presets |

`man adscutter` has the full reference.

## License

AdsCutter is released under the [MIT license](LICENSE). It uses
[Qt](https://www.qt.io/) and [FFmpeg](https://ffmpeg.org/), which have their
own licenses.
