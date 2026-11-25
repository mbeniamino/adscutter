# Changes

## 0.1.1 (2026-11-25)

- README with an overview, build instructions and a short guide.
- AppStream metadata, so software centres can list AdsCutter.
- A PKGBUILD for the AUR, which builds the release from GitHub.
- Fixed a garbled sentence in the man page.

## 0.1.0 (2026-11-21)

First release.

- Cut MPEG-TS and Matroska recordings into Matroska without re-encoding,
  with clean H.264 and HEVC joins and LOAS/LATM AAC converted to raw AAC.
- Frame-accurate navigation, a zoomable timeline, playback of the kept parts,
  undo and redo, and cut lists saved next to the recording.
- Vulkan and VAAPI hardware decoding with a software fallback, and
  deinterlacing.
- Optional ad detection from fades, separators and user-drawn regions, with
  a proposed cut list and previews of the cues and the cuts.
