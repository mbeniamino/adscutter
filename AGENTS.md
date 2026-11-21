# AGENTS.md

Notes for coding agents working on AdsCutter.

## What it is

AdsCutter is a Qt 6 desktop app (Linux/Wayland) that cuts the ads out of DVB-T/T2
TV recordings **without re-encoding**. The input is MPEG-TS or Matroska and the output
is Matroska. Each kept part starts on a cut point, meaning a frame where decoding can start, and can
end on any frame. The original requirements are in `specs`. Ad detection
(fades/separators, watched regions, proposed cut list) is built in and runs
**only when the user asks for it**.

## Build and test

```sh
meson setup build          # once; defaults: C++20, warning_level=3, debugoptimized
meson compile -C build
meson test -C build        # whole suite (~59 tests)
meson test -C build 'export h264_hd.ts 30-70,100-149'   # one test, by name
meson test -C build --list
./build/adscutter [file]
cd packaging/arch && makepkg -si   # Arch package from the last commit
```

- Dependencies: Qt 6 (Core, Gui, Widgets, Multimedia, and Test for the GUI tests),
  FFmpeg libs (libavformat >= 61, libavcodec >= 61, libavutil >= 59, libswscale,
  libswresample). The tests also need the `ffmpeg` CLI, `python3`, libx264 and libx265.
- Test fixtures are synthetic recordings that `ffmpeg` generates at build time
  (`tests/meson.build`, `tests/make_broadcast_like.py`, `tests/neutralize_params.py`).
  Media files are never committed.
- GUI tests run with `QT_QPA_PLATFORM=offscreen` and their own `XDG_CACHE_HOME`.
- `hw_test` covers Vulkan and VAAPI. It exits with 77 (skipped) when a backend is missing and
  runs serially because the GPU is shared. Its tests are in the `hw` suite, which the
  PKGBUILD skips (`--no-suite hw`) because they test the local driver, not the package.
- Tests are plain executables with a `CHECK` macro and a failure count (see
  `tests/cutlist_test.cpp`). Shared helpers are in `tests/TestUtil.h`. There is no test
  framework, except QtTest in the GUI tests.

## Layout

- `src/media/`: no widgets, so tests and possible future CLI tools can use it.
  - Probing and indexing: `MediaFile`, `IndexBuilder`, `PacketIndex`, `IndexJob`.
    One demux pass builds a per-packet index on a unified timeline. It removes
    33-bit PTS wraps and maps clock jumps from encoder restarts onto that timeline. The index is
    cached in `~/.cache/adscutter`, keyed by path, size and mtime.
  - Cut points: `Bitstream` classifies MPEG-2, H.264 and HEVC random-access points.
  - Decoding: `RandomAccessDecoder` (seeks to a cut point, then decodes forward, with an LRU
    cache), `FrameServer` (worker thread, the latest request wins), `HwDecode` (Vulkan, then
    VAAPI, then software), `Deinterlace`, `FrameConverter`, `AudioDecoder`, `AudioOutput`.
  - Export: `Exporter`/`ExportJob` (one sequential pass, stream copy), `H264Splice`
    (turns cut points into IDR pictures, renumbers frame_num/POC, changes HEVC CRA to BLA),
    `Latm` (LOAS/LATM to raw AAC).
- `src/model/CutList`: kept segments in display positions of the main video,
  `in` snapped to cut points, with undo/redo. It is saved as JSON next to the recording.
- `src/analysis/`: `Analyzer`/`AnalysisJob`/`AnalysisData` handle fades, separators and
  loudness. `RegionDetector`/`RegionJob` handle user-drawn regions. `Proposal` turns the cues
  into a cut list.
- `src/ui/`: `MainWindow` with menus, actions and settings, plus `Timeline`, `VideoView`,
  `NavigationBar`, `Playback`, and the panels for segments, streams and regions.
- `data/`: the `.desktop` file (its name is the Wayland app_id set in `main.cpp`), the
  SVG icon and the man page `adscutter.1.in` (meson fills in the version), all installed
  by meson. Keep the man page in step with options, environment variables and file locations. `desktop-file-validate` runs as a test when available.
- `packaging/arch/PKGBUILD`: clones this repository (committed state only), versions the
  package from the meson version plus the git revision, builds with `arch-meson`.
- The version lives only in `meson.build` (`project(version:)`); it reaches the code as
  `ADSCUTTER_VERSION`.
- Build: `meson.build` lists every source by hand, so add new `.cpp` files there.
  Headers that declare `Q_OBJECT` classes must also be added to the matching
  `qt6.compile_moc(headers: ...)` list.

## Conventions

- C++20, everything in `namespace adscutter` (tests use `adscutter::test`), `#pragma once`,
  member variables `m_name`, 4-space indentation, `QStringLiteral` for literals, and `tr()`
  for user-visible strings.
- Comments explain *why*, briefly, in plain English. Keep them up to date with the code.
- Persistent preferences use `QSettings` (organization `adscutter`). The environment
  variable `ADSCUTTER_HWDEC` (`auto`, `vulkan`, `vaapi`, `none`) overrides the
  hardware decoding setting.
- Keep dependencies minimal: Qt and FFmpeg only. Don't add new libraries without asking.
- Fixes and features that touch media handling need a test on a synthetic fixture. When
  the existing fixtures can't reproduce a broadcast quirk, add a new one to `tests/meson.build`.

## Working with the user

- **Keep this file up to date**: when a change affects anything described here (build,
  layout, conventions, workflow, domain knowledge, next goal), update `AGENTS.md` in the
  same change. `CLAUDE.md` is a symlink to it.

- Work **milestone by milestone**: finish one, report the result, and wait.
- **Commit only when the user says so** (usually "commit and go on").
  Commit messages look like `Milestone N: <title>`, followed by a bullet list of the
  changes, the fixes and the new tests (see `git log`).
- Ad analysis must never run automatically.
- Don't ask for ground-truth cut lists. Tune on the synthetic fixtures and the user's real
  recordings.

## Next goal

None set. Milestone 13 (markers on the video during Preview Cues/Cuts) was the last
one. Ask the user what comes next.

## Domain knowledge (ad detection)

The reference recording is a Canale 5 (Mediaset) file: 1080p50 H.264, Vorbis audio,
GOP 96 (a cut point every 1.92 s), no channel name in the file.

- Break pattern: the programme fades to black (~0.6 s, sound down to about -50 dB but not
  silent), then a 2 s channel ident, then spots separated by 12 black, silent frames. The
  programme cuts straight back in.
- Silence on its own is useless: the programme has hundreds of short silences.
- The "5 HD" logo never goes away. The text under it is "PRIMA" during first-run programmes
  and "MEDIASET" during ads, so regions drawn on that text are reliable. The "PUBBLICITÀ"
  label is burned in by advertisers and inconsistent, so it can't be used as a region.
- Semi-transparent overlays: their edges are only visible over bright pictures. Region
  matching therefore uses edge maps, the score min(recall, precision), an overlay mask, and an
  Otsu split (fixed thresholds fail).
- When proposing a cut list, the regions decide. Fades only refine the edges, within 3 s,
  because fades on their own over-extend into dark, quiet programme openings. Undecided frames
  never count as programme ("L-frame" spots cover the logo).
