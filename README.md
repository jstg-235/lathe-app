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

## Products and delivery paperwork (v0.8)

- **Products & Splitting** (Delivery menu): cut one programme into CDs, 12"/10"/7" vinyl sides or
  cassette sides. Each unit is a window on the timeline with its own fade-in and fade-out, applied
  after the master chain and before dither, to that unit only; the full programme still renders as
  one piece for digital delivery. Units may overlap so music can fade out at the end of side A and
  fade back in on side B.
- **Auto-Split** balances unit lengths under each format's limit, breaking at track starts where it
  can and at the quietest moment near the ideal point where it can't (run Analyse first).
  **Fill to track markers** (v0.11) instead puts as many whole tracks on each unit as fit and always
  breaks at a track marker. **Auto-split new products** can be turned off: a new product then starts
  as one unit for the whole programme.
- **On the timeline** (v0.11): drag a unit's edges in the product strip (a butt break moves both
  sides; Snap applies); right-click the strip to split a unit at that point, join units at a break,
  auto-split either way, or show another product.
- **Break Editor** (K): the outgoing unit's fade-out and the incoming unit's fade-in over the
  waveform, draggable, with an audition that plays through the flip gap (or no gap).
- **Rendering**: a verified DDP per disc (a "(cont.)" track reusing the ISRC when a disc starts
  mid-track, 2 s pregap added automatically, -D1/-D2 master IDs) and a hi-res WAV per side.
- **Paperwork**: CD PQ sheet per disc, cutting / duplication sheet per side, label copy and a
  delivery manifest (every file with size and MD5), as PDFs with the studio name, contact details
  and logo, plus Track Metadata.csv and Delivery.json.
- **Metadata**: Release Info (label, catalogue, release date, (P)/(C), CD-Text genre and disc ID,
  PQ offset, hidden track one audio) and Track Credits (writers, arranger, publisher, PRO, explicit).
  CD-Text now includes packs 0x86 (disc ID), 0x87 (genre) and 0x8E (UPC/ISRC).

## Editing, PQ and assembly (v0.9)

- **Two timelines**: a source timeline above the destination, each with its own zoom, In / Out,
  Edit Point and lane targets (D / S / N). Play either, or link them and switch which you hear
  with Tab. Edits: Insert, Replace, Replace Left, Replace Constrained, Fit (4-point), Overlay,
  Append, with 3-point rules from either side. Cut / Copy / Paste with the same modes.
- **Edit Point** separate from the playhead; Play to / from In and Out with pre- and post-roll
  (F5-F8), Play Through Edit Point, repeat play, Find / Clear / nudge (A / B / C) / Move points,
  Move In to Out, SRPs (named reference points, lockable).
- **Edit Fade**: outgoing, incoming and the rendered result; Lock Sound, ripple / ripple until
  black; per-fade shape, length, offset, and a Custom shape set by dB down and alpha; Power Lock,
  Align Fades; per-side edit point and slip nudges with auto-audition; a ten-slot fade library
  (Option-0 to 9), slot 0 being the default crossfade put on every new edit.
- **PQ**: start, track 1, on-an-edit and end offsets, minimum index width, per-track offset;
  Mark Info (reorder tracks with their audio by dragging, lock, ISRC numbering); Segments to
  Marks, Edited Black and Analog Black to Marks; AutoSpace; warning for audio before track 1.
- **Assembly**: Shift-drag inserts, Command-drag replaces, snap to gap; track bar to reorder;
  Text View of every segment; Segment Gain (set, change, normalize, polarity).
- **Timeline gain**: nodes over the whole programme, 1 dB steps (0.1 dB with Option), groups,
  locks, bypass. Lock All Marks and Lock All Gain are global settings.
- **Lanes**: mono, stereo or 5.1. A 5.1 programme plays on six outputs and exports as a
  six-channel WAV; CD, DDP and vinyl stay stereo. Export Selected Segments writes any segment
  or In-Out as its own file.
- **Key presets**: Lathe (default) and Legacy (function-key transport, [ ] for In / Out, F9-F11
  marks), under Help > Key Preset or in Edit Key Commands.

## Metering (v0.10)

- **Meter Bridge** (Tools > Meter Bridge, Cmd-Shift-B), a window of its own for a second screen:
  - **Radar**: short-term loudness swept around a dial against the target (1 to 60 minutes per
    turn), momentary loudness on the outer ring, programme loudness against the target, LRA,
    max true peak and elapsed time.
  - **Loudness**: L / R peak bars with true-peak holds, M / S / I bars on an LU scale around
    the target, five minutes of momentary and short-term history, statistics (integrated, LRA,
    max M / S, true and sample peak per channel, PLR, PSR, overs) and compliance against a
    standard: EBU R128, ATSC A/85, ARIB, OP-59, streaming -14, Apple -16, Amazon, podcast, or
    the session's own target.
  - **Balance**: the programme's average spectrum against the mids, a target zone built from
    the session's reference tracks, six ranges (sub to air) with how far each sits from the
    zone, plain notes for the ones outside it, a mono check line and an optional 3 dB / octave
    tilt.
  - **Stereo**: goniometer, correlation with its lowest reading, width (side against mid),
    left / right balance, a minute of correlation history and correlation per band.
- The main window keeps a compact meter: peak bars, integrated loudness coloured against the
  target, momentary, short-term, LRA, true peak, PLR / PSR and correlation.

## Compare Unprocessed (v0.11)

- **W** switches between the processed mix (A) and the mix without the master plugins and region
  effects (B). Edits, gains and fades stay in both. The two play side by side, time-aligned (the
  unprocessed side is delayed by the master chain's latency; region effect latency is compensated
  per region), so the switch is instant and crossfades in 10 ms.
- A banner across the top of the plugin list shows the side you're hearing while the compare is on.
- **Level Match** (Shift-W, or the banner's button) adds C: the processed side turned down to the
  unprocessed loudness, so W switches B / C and you hear what the processing does, not just that it
  is louder. The loudness of both sides is measured as the session plays (BS.1770, the last ten
  seconds or so, pauses left out); it only ever turns the processed side down. No Analyse needed.
- **End** (Option-W) returns to normal playback. Nothing here reaches an export, and the meters
  always read the processed side (A).

## Audio Setup, input meters and Capture (v0.12)

- **Audio Setup** (Tools > Audio Setup) is now a full window in three parts:
  - **Output**: interface, sample rate, buffer size and the output pair you monitor on (with six
    outputs from that pair for a 5.1 programme, when the interface has them).
  - **Input**: the input interface, which input pairs are switched on, and the default input pair.
  - **Recording**: where recordings go (next to the session by default, in "<Session> Recordings",
    or a folder you choose, shown with its full path and the free space), the format (24-bit or
    32-bit float; this moved here from the Record window), how files are named (session, then
    Recording / Print / Capture, then date and time), the round-trip delay, and input monitoring
    through Lathe (off by default).
  - The **round-trip delay** is measured with one click on the sockets you choose, and remembered
    for that interface, output pair, input pair, sample rate and buffer size.
  - It warns when inputs and outputs are on different interfaces (their clocks drift) and when
    the interface isn't at the session's sample rate.
- **Input meters**. Whenever the inputs are armed (the Record or Capture window is open, or
  Transport > Arm Inputs, Ctrl-Shift-R), the main meter splits into **OUT | IN**. The IN side shows
  peak, true peak, a 3 s hold, the maximum, momentary loudness, and a clip light with an overs count
  that both stay lit until you click them. The Meter Bridge has a source choice: **Output**,
  **Input** or **Both**. On Both, the Loudness view shows the two sets of bars side by side and the
  gain through the outboard in LU.
- **Capture Through Outboard** (Transport menu, Ctrl-R) sends the programme through your gear and
  records what comes back:
  - **Route**: an output pair to the gear and an input pair for the return. The saved round-trip
    delay for those sockets is used; Measure now re-measures it.
  - **Level check** plays the range round and round through the gear without recording, with OUT
    and IN meters and the gain between them; it warns within 1 dB of clipping and when nothing comes
    back.
  - **What is captured**: the destination In-Out, or the whole programme when nothing is selected,
    with a 2 s pre-roll and a post-roll for the gear's tail. The channels follow the lane type:
    mono, stereo or 5.1.
  - **The result** lands on a new lane directly under the original, in sync with it: the round trip
    and the master plugins' own delay are taken off, to the sample. The pre-roll and the tail stay in
    the file behind the region's edges. The original is left playing unless "Mute the original" is
    ticked (only the captured stretch is muted).
  - **Check the capture**: switch between what was sent and what came back, in step and
    level-matched (the louder side is turned down), and run a **null test** that reports timing,
    polarity, level and what is left over. With a cable in place of the gear, nothing should be left.
  - **Safety**: free disk space is checked before it starts, the recording runs on the audio and
    disk threads (a busy screen doesn't interrupt it), and stopping early keeps what was recorded.
- While capturing, Lathe plays nothing on your output pair unless input monitoring is on, and it
  never plays the return back onto the outputs that feed the gear.

## External Gear insert, Print and Rename (v0.13)

- **External Gear** is a hardware insert for the master chain (Add under the plugin list: it is at the top of the menu). What
  reaches it goes out on an output pair to your gear; what comes back on an input pair carries on
  down the chain. It can sit anywhere in the chain, and there can be several, each on its own
  sockets. Stereo.
  - **Controls**: send level, return level, dry / wet, polarity, send and return meters with clip
    lights. The chain's Bypass button bypasses it and keeps its delay, so switching doesn't jump.
  - **Ping** measures the round trip with one click. The insert reports it as its delay, so the
    rest of the chain, Compare Unprocessed, the dry / wet mix and the meters stay lined up. It is
    saved with the insert.
  - **Safety**: the pair you monitor on is refused as a send (it would feed back) and nothing is
    ever sent to it; warnings for a clipping return, a silent return, and inputs and outputs on
    different interfaces.
- **Exports never leave the gear out.** Hardware can't be rendered faster than real time, so while
  anything still has to pass through a switched-in External Gear insert, exports and Analyse are
  refused with a message pointing to Print (or bypass the insert to export without it).
- **Print** (in the insert's window) plays the In-Out, or the whole programme, through the gear in
  real time and records what leaves the insert onto a new lane under the original, in sync to the
  sample. The printed stretch replaces the live path there:
  - the print joins the master chain *after* the insert, so the plugins above it and the gear are
    not applied twice, and the plugins below it still are;
  - unprinted material keeps going through the gear live, so a part-printed session sounds the same
    across the joins;
  - once everything is printed, exports run at full speed from the prints.
- **Switching back**: Use live gear / Use the print flips between the two (nothing is deleted),
  and Undo removes a print. **A/B** runs the live gear and its print side by side, in step, and
  switches between them at the insert, level-matched.
- **Rename Region / Audio File** (Edit menu, Ctrl-N): renames the selected region, and with the box
  ticked the audio file on disk too. Every region using the file follows it, and earlier undo steps
  keep working.
- The printed key command guide now shows Control and Command correctly.

## Stability by design

- Audio is streamed from disk on a background thread; the audio callback never touches the disk.
- Plugins are scanned in a separate helper process: a plugin that crashes or hangs only takes
  the helper down, is skipped, and the scan carries on. Plugins can be switched off in the
  Plugin Manager.
- Sessions save atomically (write-then-swap). An automatic backup is written every minute, and
  after a crash Lathe offers to restore it.
- 200 levels of undo (Cmd-Z / Cmd-Shift-Z).

## Not yet

CD-R burning, Japanese (MS-JIS) CD-Text, and a sign-off sheet.

## Building

GitHub Actions builds a universal (Apple Silicon + Intel) app on every push to `main`
and publishes `Lathe-mac.zip` to the `builds` branch. Runs on macOS 10.15 Catalina or later, Intel or Apple Silicon.

Locally: `cmake -S . -B build && cmake --build build`.

## Credits and licences

- JUCE 8 (AGPLv3 / commercial), r8brain-free-src (MIT, Aleksey Vaneev).
- DDP 2.00 field layouts follow the MIT-licensed notes at
  [ddp-reverse-eng](https://github.com/ddp-reverse-eng/ddp-reverse-eng) (spec/ddp2.md).
- Loudness filters per ITU-R BS.1770-4.

Lathe is an independent project.
