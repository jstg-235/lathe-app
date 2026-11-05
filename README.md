# Lathe

A stable, 64-bit Mac app for CD premastering: assemble tracks, set PQ codes, ISRC and CD-Text,
run your AU/VST3 mastering chain, and deliver a verified DDP 2.0 image for replication.

## What it does (v0.1)

- **Assembly**: drag in WAV / AIFF / FLAC files at any sample rate. Per-track gap, pause (index 00),
  gain, fade in/out, crossfades (negative gap), and sub-indexes (press **I** at the playhead).
- **PQ / metadata**: ISRC, UPC/EAN (check digit verified), DCP and pre-emphasis flags, disc and
  track CD-Text (title, performer). Red Book rules are checked live at the bottom of the window.
- **Processing**: AU and VST3 insert chain with bypass and full state recall. Plugins are run with
  delay compensation on export. NaN/inf output from a misbehaving plugin is silenced, not passed on.
- **Export**: DDP 2.0 (DDPID, DDPMS, PQDESCR, IMAGE.DAT, CDTEXT.BIN, MD5 + CRC32 checksums, cue
  sheet, PQ log with QC numbers), track WAVs 16/44.1, programme WAVs at 24-bit or 32-bit float.
  Every DDP is re-read and checksummed right after it's written.
- **QC**: BS.1770-4 / EBU R128 loudness (momentary, short-term, integrated, LRA), 4x oversampled
  true peak, and full-scale sample counts, live and as an offline report per track.
- **Verify DDP**: open any DDP folder (from Lathe or another tool) to check its checksums and read
  back the PQ list and CD-Text.

Signal path on export: clips → plugin chain at project rate → r8brain SRC to 44.1 kHz →
TPDF dither (flat, light or strong noise shaping) → 16-bit.

## Stability by design

- Audio is streamed from disk on a background thread; the audio callback never touches the disk.
- The plugin scanner remembers a plugin that crashes it and skips it next time.
- Sessions save atomically (write-then-swap). An automatic backup is written every minute, and
  after a crash Lathe offers to restore it.
- 200 levels of undo (Cmd-Z / Cmd-Shift-Z).

## Not yet

CD-R burning, DDP playback, spectral repair/noise reduction, and 0x8E (UPC/ISRC) CD-Text packs.

## Building

GitHub Actions builds a universal (Apple Silicon + Intel) app on every push to `main`
and publishes `Lathe-mac.zip` to the `builds` branch. Requires macOS 11 or later.

Locally: `cmake -S . -B build && cmake --build build`.

## Credits and licences

- JUCE 8 (AGPLv3 / commercial), r8brain-free-src (MIT, Aleksey Vaneev).
- DDP 2.00 field layouts follow the MIT-licensed notes at
  [ddp-reverse-eng](https://github.com/ddp-reverse-eng/ddp-reverse-eng) (spec/ddp2.md).
- Loudness filters per ITU-R BS.1770-4.

Lathe is an independent project.
