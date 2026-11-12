#include "Commands.h"

#include <set>

namespace lathe {

const std::vector<CommandDef>& allCommands() {
    static const std::vector<CommandDef> defs{
        // File
        {cmd::newSession, "New Session", "File", "Start an empty session", "command + N"},
        {cmd::openSession, "Open Session...", "File", "Open a .lathe session", "command + O"},
        {cmd::saveSession, "Save Session", "File", "Save the session", "command + S"},
        {cmd::saveSessionAs, "Save Session As...", "File", "Save the session under a new name", "command + shift + S"},
        {cmd::importAudio, "Import Audio as Tracks...", "File", "Add audio files to the end, each with a track marker", "command + I"},
        {cmd::openSource, "Open in Source Window...", "File", "Load a file into the Source window for auditioning and editing", "command + shift + O"},
        {cmd::locateMissingAudio, "Locate Missing Audio...", "File", "Find audio files the session can't find (moved, renamed, or on another computer)", "command + shift + M"},
        {cmd::exportDdp, "Export DDP Image...", "File", "Write and verify a DDP 2.0 master for replication", "command + E"},
        {cmd::exportTrackWavs, "Export Track WAVs...", "File", "16-bit / 44.1 kHz WAV per track, cut at the PQ marks", "command + shift + E"},
        {cmd::exportProgram24, "Export Programme WAV (24-bit)...", "File", "The whole programme at the project rate, 24-bit dithered", ""},
        {cmd::exportProgram32, "Export Programme WAV (32-bit float)...", "File", "The whole programme at the project rate, 32-bit float", ""},
        {cmd::exportSegments, "Export Selected Segments...", "File", "Each selected segment as its own file, in any format (WAV, MP3, AAC, Apple Lossless)", "command + option + E"},
        {cmd::exportInOut, "Export In-Out...", "File", "The In-Out of the active timeline as one file, in any format", ""},
        {cmd::exportTracksAs, "Export Tracks As...", "File", "Every CD track as its own file in any format, tagged from CD-Text", ""},
        {cmd::exportProgrammeAs, "Export Programme As...", "File", "The whole programme as one file in any format, tagged", ""},
        {cmd::batchRender, "Render Album (Batch)...", "File", "DDP plus WAV / FLAC deliverables in one go, with a QC and checksum report", "command + R"},
        // Edit
        // Delivery
        {cmd::products, "Products & Splitting...", "Delivery", "Cut the programme into CDs, LP sides or tape sides, each with its own fades", "command + shift + U"},
        {cmd::breakEditor, "Break Editor", "Delivery", "Edit the side / disc break nearest the playhead: fades, overlap and flip audition", "K"},
        {cmd::releaseInfo, "Release Info...", "Delivery", "Label, catalogue number, release date, (P)/(C), CD-Text genre and disc ID, PQ offset, HTOA", "command + shift + I"},
        {cmd::trackCredits, "Track Credits...", "Delivery", "Per-track writers, arranger, publisher, PRO, explicit flag and CD-Text message", "command + shift + T"},
        {cmd::studioProfile, "Studio Profile...", "Delivery", "Studio name, contact details and logo for the paperwork", ""},
        {cmd::exportPaperwork, "Export Paperwork...", "Delivery", "PQ sheets, cutting sheets, label copy and manifest from the session (no render)", "command + option + P"},
        {cmd::undo, "Undo", "Edit", "Undo the last change", "command + Z"},
        {cmd::redo, "Redo", "Edit", "Redo the last undone change", "command + shift + Z"},
        {cmd::splitAtPlayhead, "Split at Playhead", "Edit", "Cut the selected region (or all regions under the playhead) in two", "B"},
        {cmd::deleteClose, "Delete (Close Gap)", "Edit", "Delete the selected region or marker; later material moves up", "delete;backspace"},
        {cmd::deleteLeave, "Delete (Leave Gap)", "Edit", "Delete the selected region; leave silence in its place", "shift + delete;shift + backspace"},
        {cmd::muteRegion, "Mute / Unmute Region", "Edit", "Silence the selected region without deleting it", "U"},
        {cmd::renameRegion, "Rename Region / Audio File...", "Edit", "Rename the selected region, and its audio file on disk if you choose (the session follows the file)", "control + N"},
        {cmd::nudgeLeft, "Nudge Region Left", "Edit", "Move the selected region one CD frame (1/75 s) earlier", "option + cursor left"},
        {cmd::nudgeRight, "Nudge Region Right", "Edit", "Move the selected region one CD frame later", "option + cursor right"},
        {cmd::nudgeLeftBig, "Nudge Region Left 100 ms", "Edit", "Move the selected region 100 ms earlier", "option + shift + cursor left"},
        {cmd::nudgeRightBig, "Nudge Region Right 100 ms", "Edit", "Move the selected region 100 ms later", "option + shift + cursor right"},
        {cmd::clearSelection, "Clear Selection", "Edit", "Deselect regions and the destination range", "escape"},
        {cmd::selectRegionRange, "Select Region Range", "Edit", "Set the destination In/Out to the selected region", "command + A"},
        {cmd::regionEffects, "Region Effects...", "Edit", "Open the selected region's own plugin chain", "F"},
        {cmd::restoreRegion, "Restore Region...", "Edit", "Declick, decrackle, denoise, hum and DC removal on the selected region (non-destructive)", "command + shift + D"},
        {cmd::spectralEditor, "Spectral Editor...", "Edit", "See the selected region as a spectrogram and heal or attenuate any sound in it", "command + shift + F"},
        {cmd::textView, "Text View...", "Edit", "Every segment as a list: times, gain, fades, polarity; select several for Segment Gain", "command + T"},
        {cmd::segmentGain, "Segment Gain...", "Edit", "Set, change or normalize the gain of the selected segment(s), and their polarity", "command + shift + G"},
        {cmd::bypassRegionFx, "Bypass All Region Effects", "Edit", "A/B every region's effects at once (the master chain stays on)", "shift + F"},
        // Source / destination
        {cmd::sourceIn, "Mark In", "Source / Destination", "Set the In point on the active timeline (at the playhead while playing, otherwise the Edit Point)", "I"},
        {cmd::sourceOut, "Mark Out", "Source / Destination", "Set the Out point on the active timeline (at the playhead while playing, otherwise the Edit Point)", "O"},
        {cmd::destIn, "Mark Destination In", "Source / Destination", "Set the destination In point, whichever timeline is active", "shift + I"},
        {cmd::destOut, "Mark Destination Out", "Source / Destination", "Set the destination Out point, whichever timeline is active", "shift + O"},
        {cmd::goToIn, "Find In", "Source / Destination", "Move the Edit Point (and playhead) to the In point of the active timeline", "option + I"},
        {cmd::goToOut, "Find Out", "Source / Destination", "Move the Edit Point (and playhead) to the Out point of the active timeline", "option + O"},
        {cmd::clearIn, "Clear In", "Source / Destination", "Remove the In point of the active timeline", "control + I"},
        {cmd::clearOut, "Clear Out", "Source / Destination", "Remove the Out point of the active timeline", "control + O"},
        {cmd::moveInToOut, "Move In to Out", "Source / Destination", "Step the In-Out window forward: In moves to Out, Out moves on by the same length", "option + shift + O"},
        {cmd::nudgeInLeft, "Nudge In Earlier", "Source / Destination", "Move the In point earlier by the current nudge size", ","},
        {cmd::nudgeInRight, "Nudge In Later", "Source / Destination", "Move the In point later by the current nudge size", "."},
        {cmd::nudgeOutLeft, "Nudge Out Earlier", "Source / Destination", "Move the Out point earlier by the current nudge size", "option + ,"},
        {cmd::nudgeOutRight, "Nudge Out Later", "Source / Destination", "Move the Out point later by the current nudge size", "option + ."},
        {cmd::nudgeSizeA, "Nudge Size A", "Source / Destination", "Use nudge size A (set in Playback & Nudge Settings)", "control + 1"},
        {cmd::nudgeSizeB, "Nudge Size B", "Source / Destination", "Use nudge size B", "control + 2"},
        {cmd::nudgeSizeC, "Nudge Size C", "Source / Destination", "Use nudge size C", "control + 3"},
        {cmd::movePoint, "Move Point...", "Source / Destination", "Move the Edit Point, In, Out or selected SRP by an exact amount or to an exact time", "control + M"},
        {cmd::editPointToPlayhead, "Edit Point to Playhead", "Source / Destination", "Put the Edit Point where the playhead is (e.g. after stopping)", "control + E"},
        {cmd::insertEdit, "Insert", "Source / Destination", "Insert the source In-Out at the destination In (or Edit Point); later material moves right", "T"},
        {cmd::overwriteEdit, "Replace", "Source / Destination", "Replace the destination from its In (or Edit Point) for the source length", "shift + T"},
        {cmd::replaceLeftEdit, "Replace Left", "Source / Destination", "Replace so the source ends at the destination Out (back-timed from the Out)", "option + T"},
        {cmd::constrainedEdit, "Replace Constrained", "Source / Destination", "Replace exactly the destination In-Out; the source is cut to fit, nothing moves", "control + T"},
        {cmd::replaceEdit, "Fit (4-Point Replace)", "Source / Destination", "Replace destination In-Out with source In-Out; later material moves to fit", "R"},
        {cmd::overlayEdit, "Overlay", "Source / Destination", "Lay the source over the destination at its In (overlaps crossfade, nothing is removed)", "option + shift + T"},
        {cmd::appendEdit, "Append as Track", "Source / Destination", "Add the source In-Out at the end as a new track", "A"},
        {cmd::playSourceInOut, "Play Source In-Out", "Source / Destination", "Audition the marked part of the source", "option + spacebar"},
        {cmd::switchTimeline, "Switch Source / Destination", "Source / Destination", "Make the other timeline active (during linked play, hear the other one)", "tab"},
        {cmd::linkPlay, "Link Source and Destination Play", "Source / Destination", "Play both timelines together, in step, hearing the active one", "shift + tab"},
        {cmd::clearSource, "Clear Source Timeline", "Source / Destination", "Remove everything from the source timeline", ""},
        // Clipboard
        {cmd::cutRange, "Cut", "Edit", "Copy the In-Out of the active timeline (targeted lanes) and remove it; later material moves up", "command + X"},
        {cmd::copyRange, "Copy", "Edit", "Copy the In-Out (or the selected region) of the active timeline", "command + C"},
        {cmd::pasteInsert, "Paste Insert", "Edit", "Paste at the In (or Edit Point); later material moves right", "command + V"},
        {cmd::pasteReplace, "Paste Replace", "Edit", "Paste over what is there, from the In (or Edit Point)", "command + option + V"},
        {cmd::pasteReplaceLeft, "Paste Replace Left", "Edit", "Paste so it ends at the Out (or Edit Point)", "command + option + control + V"},
        {cmd::pasteConstrained, "Paste Constrained", "Edit", "Paste into the In-Out only; the clip is cut to fit, nothing moves", "command + control + V"},
        {cmd::pasteOverlay, "Paste Overlay", "Edit", "Paste on top without removing anything (overlaps crossfade)", "command + option + shift + V"},
        {cmd::eraseRange, "Erase In-Out (Leave Gap)", "Edit", "Remove the In-Out on targeted lanes and leave silence", "option + delete;option + backspace"},
        // Transport
        {cmd::playStop, "Play / Stop", "Transport", "Play or stop the active timeline from its Edit Point (both, when linked)", "spacebar"},
        {cmd::playSource, "Play / Stop Source", "Transport", "Play or stop the Source window", "shift + spacebar"},
        {cmd::returnToStart, "Return to Start", "Transport", "Move the playhead to the top and reset the meters", "home;return"},
        {cmd::locateBackFrame, "Back One Frame", "Transport", "Move the playhead back 1/75 s", "cursor left"},
        {cmd::locateFwdFrame, "Forward One Frame", "Transport", "Move the playhead forward 1/75 s", "cursor right"},
        {cmd::locateBackSecond, "Back One Second", "Transport", "Move the playhead back 1 s", "shift + cursor left"},
        {cmd::locateFwdSecond, "Forward One Second", "Transport", "Move the playhead forward 1 s", "shift + cursor right"},
        {cmd::prevMarker, "Previous Marker", "Transport", "Jump to the previous PQ marker", "cursor up"},
        {cmd::nextMarker, "Next Marker", "Transport", "Jump to the next PQ marker", "cursor down"},
        {cmd::playSelection, "Play In-Out", "Transport", "Play the In-Out of the active timeline", "command + return;command + spacebar"},
        {cmd::playToIn, "Play to In", "Transport", "Play the pre-roll up to the In point (Edit Point if no In)", "F5"},
        {cmd::playFromIn, "Play from In", "Transport", "Play from the In point for the post-roll", "F6"},
        {cmd::playToOut, "Play to Out", "Transport", "Play the pre-roll up to the Out point", "F7"},
        {cmd::playFromOut, "Play from Out", "Transport", "Play from the Out point for the post-roll", "F8"},
        {cmd::playAroundEdit, "Play Through Edit Point", "Transport", "Play from the pre-roll before the Edit Point to the post-roll after it", "F9"},
        {cmd::repeatPlay, "Repeat Play", "Transport", "Loop In-Out play and the play-to / play-from commands", "option + R"},
        {cmd::editPointFollowsStop, "Edit Point Follows Playhead on Stop", "Transport", "When on, stopping moves the Edit Point to where you stopped; when off, Play returns to the Edit Point", ""},
        {cmd::playbackSettings, "Playback & Nudge Settings...", "Transport", "Pre-roll, post-roll and the three nudge sizes", ""},
        {cmd::record, "Record / Stop Recording", "Transport", "Record the inputs, or print the session through outboard and record the return", "shift + R"},
        {cmd::capture, "Capture Through Outboard...", "Transport", "Send the In-Out (or the whole programme) through your gear and record the return onto a new lane, in sync", "control + R"},
        {cmd::armInputs, "Arm Inputs (Input Meters)", "Transport", "Measure the inputs without recording: the meter splits into OUT and IN", "control + shift + R"},
        // Markers
        {cmd::addTrackMarker, "Add Track Marker", "Markers", "New CD track (index 01) at the playhead", "M"},
        {cmd::addIndexMarker, "Add Index Marker", "Markers", "New sub-index in the current track at the playhead", "shift + M"},
        {cmd::addPauseMarker, "Add Pause Marker", "Markers", "Start of the pause (index 00) before the next track", "P"},
        {cmd::addEndMarker, "Add End-of-Disc Marker", "Markers", "Set the lead-out (end of programme) at the playhead", "E"},
        {cmd::markInfo, "Mark Info...", "Markers", "Every track: PQ offsets, per-track offset and lock, titles, ISRCs; drag to reorder tracks with their audio", "command + M"},
        {cmd::segmentsToMarks, "Segments to Start Marks", "Markers", "A track start at the beginning of every region on lane 1 that doesn't have one", ""},
        {cmd::editedBlackToMarks, "Edited Black to Marks", "Markers", "A track end and the next track's start around every gap between regions", "shift + F12"},
        {cmd::analogBlackToMarks, "Analog Black to Marks...", "Markers", "Find silence inside the audio and mark tracks around it", ""},
        {cmd::autoSpace, "AutoSpace Tracks", "Markers", "Make every gap between tracks the same (Mark Info sets the gap); marks move with their audio", ""},
        {cmd::lockAllMarks, "Lock All Marks", "Markers", "Every mark keeps its time when audio is moved, rippled or edited", ""},
        {cmd::addSrp, "Add SRP", "Markers", "Drop a reference point (not a PQ mark) at the playhead / Edit Point on the active timeline", "S"},
        {cmd::addSrpText, "Add SRP with Text...", "Markers", "Drop a reference point and name it", "shift + S"},
        {cmd::srpsFromSelection, "SRPs from Selection", "Markers", "SRPs at the In and Out (or the selected region's edges)", "option + S"},
        {cmd::prevSrp, "Previous SRP", "Markers", "Move the Edit Point to the previous SRP", "option + cursor up"},
        {cmd::nextSrp, "Next SRP", "Markers", "Move the Edit Point to the next SRP", "option + cursor down"},
        {cmd::lockSrp, "Lock / Unlock SRP", "Markers", "A locked SRP keeps its time when material ripples", ""},
        {cmd::deleteSrp, "Delete SRP", "Markers", "Remove the selected SRP", ""},
        {cmd::deleteAllSrps, "Delete All SRPs", "Markers", "Remove every SRP on the active timeline", ""},
        // View
        {cmd::zoomIn, "Zoom In", "View", "Zoom the timeline in around the playhead", "command + =;command + +"},
        {cmd::zoomOut, "Zoom Out", "View", "Zoom the timeline out", "command + -"},
        {cmd::zoomFit, "Zoom to Fit", "View", "Show the whole programme", "command + 0"},
        {cmd::toggleEnvelope, "Gain Envelope Mode", "View", "Edit clip gain envelopes on the timeline", "G"},
        {cmd::toggleSnap, "Snap", "View", "Snap edits to region edges, markers and the playhead", "N"},
        {cmd::crossfadeEditor, "Edit Fade...", "Edit Fade", "Open Edit Fade for the edit (or region fade-in / fade-out) nearest the Edit Point: the sides, the result, shapes, nudges and the fade library", "X"},
        {cmd::fadeRecall0, "Recall Default Crossfade", "Edit Fade", "Apply library slot 0 (the default crossfade) to the fade nearest the Edit Point, or the one open in Edit Fade", "option + 0"},
        {cmd::fadeRecall1, "Recall Fade 1", "Edit Fade", "Apply library slot 1 to the edit nearest the Edit Point, or the one open in Edit Fade", "option + 1"},
        {cmd::fadeRecall2, "Recall Fade 2", "Edit Fade", "Apply library slot 2 to the edit nearest the Edit Point, or the one open in Edit Fade", "option + 2"},
        {cmd::fadeRecall3, "Recall Fade 3", "Edit Fade", "Apply library slot 3 to the edit nearest the Edit Point, or the one open in Edit Fade", "option + 3"},
        {cmd::fadeRecall4, "Recall Fade 4", "Edit Fade", "Apply library slot 4 to the edit nearest the Edit Point, or the one open in Edit Fade", "option + 4"},
        {cmd::fadeRecall5, "Recall Fade 5", "Edit Fade", "Apply library slot 5 to the edit nearest the Edit Point, or the one open in Edit Fade", "option + 5"},
        {cmd::fadeRecall6, "Recall Fade 6", "Edit Fade", "Apply library slot 6 to the edit nearest the Edit Point, or the one open in Edit Fade", "option + 6"},
        {cmd::fadeRecall7, "Recall Fade 7", "Edit Fade", "Apply library slot 7 to the edit nearest the Edit Point, or the one open in Edit Fade", "option + 7"},
        {cmd::fadeRecall8, "Recall Fade 8", "Edit Fade", "Apply library slot 8 to the edit nearest the Edit Point, or the one open in Edit Fade", "option + 8"},
        {cmd::fadeRecall9, "Recall Fade 9", "Edit Fade", "Apply library slot 9 to the edit nearest the Edit Point, or the one open in Edit Fade", "option + 9"},
        {cmd::defaultFadeOnEdits, "Crossfade New Edits", "Edit Fade", "Put the default crossfade (library slot 0) on every new edit, centred so nothing moves", ""},
        {cmd::gainOverlay, "Timeline Gain Overlay", "View", "Draw and edit gain across the whole programme: click to add a node, drag (1 dB, Option 0.1 dB), Option-click to delete", "option + G"},
        {cmd::gainBypass, "Bypass Timeline Gain", "View", "Hear the programme without the timeline gain (the nodes stay)", "option + shift + G"},
        {cmd::lockAllGain, "Lock All Gain", "View", "Every timeline gain node stays put and can't be changed", ""},
        {cmd::clearGain, "Clear Timeline Gain", "View", "Remove every timeline gain node (locked ones stay)", ""},
        {cmd::toggleLoudness, "Loudness Overlay", "View", "Show short-term loudness, track LUFS / true peak and problem flags on the timeline", "L"},
        // Tools
        {cmd::analyse, "Analyse Programme", "Tools", "Loudness map, per-track stats and problem scan (clips, overs, DC, phase, dropouts) of the master and the unprocessed mix", "command + shift + A"},
        {cmd::meterBridge, "Meter Bridge...", "Tools", "Loudness radar, loudness bars and history, statistics and compliance, tonal balance against references, stereo scope", "command + shift + B"},
        {cmd::resetMeters, "Reset Meters", "Tools", "Start every meter reading again (integrated, range, maxima, spectrum average)", "command + option + R"},
        {cmd::showAnalysis, "Analysis Results...", "Tools", "Show the last analysis: summary and a clickable problem list", "command + shift + L"},
        {cmd::references, "References...", "Tools", "Load reference tracks for level-matched A/B", "command + shift + R"},
        {cmd::listenReference, "A/B: Listen to Reference", "Transport", "Switch between the master and the active reference (level-matched, 10 ms crossfade)", "Q"},
        {cmd::compareUnprocessed, "A/B: Compare Unprocessed", "Transport", "Switch between the processed mix and the mix without master and region effects (instant, time-aligned)", "W"},
        {cmd::compareLevelMatch, "A/B: Level Match Processed", "Transport", "While comparing, turn the processed side down to the unprocessed loudness (measured as it plays), so W switches B / C", "shift + W"},
        {cmd::compareEnd, "A/B: End Compare", "Transport", "Back to normal playback: processed, no level match", "option + W"},
        {cmd::verifyDdp, "Verify DDP...", "Tools", "Check a DDP folder's checksums and read back its PQ", "command + shift + V"},
        {cmd::ddpPlayer, "DDP Player...", "Tools", "Play and check a finished DDP exactly as the plant will read it", "command + shift + P"},
        {cmd::openDdpSession, "Open DDP as Session...", "File", "Turn a DDP back into an editable session (PQ, CD-Text, ISRC) for revisions", "command + option + O"},
        {cmd::nullTest, "Null Test / Compare Files...", "Tools", "Align two renders and measure exactly how they differ", "command + shift + N"},
        {cmd::pluginManager, "Plugin Manager...", "Tools", "Scan for AU / VST3 plugins (in a crash-proof helper) and activate or deactivate them", ""},
        {cmd::controlSurfaces, "Control Surfaces / MIDI...", "Tools", "Mackie Control, HUI and MIDI-learn for any command", ""},
        {cmd::audioSetup, "Audio Setup...", "Tools", "Choose the audio interface and buffer size", "command + ,"},
        {cmd::followProjectRate, "Follow Project Sample Rate", "Tools", "Switch the interface to the project's rate so playback is never resampled", ""},
        {cmd::playFromPlayhead, "Play from Playhead", "Transport", "Play from where the playhead is instead of the Edit Point", ""},
        {cmd::playRepeatInOut, "Play In-Out Repeating", "Transport", "Loop the In-Out of the active timeline until you stop", ""},
        {cmd::movePointIn, "Move In Point...", "Source / Destination", "Move the In point by an exact amount or to an exact time", ""},
        {cmd::movePointOut, "Move Out Point...", "Source / Destination", "Move the Out point by an exact amount or to an exact time", ""},
        {cmd::nextEdge, "Edit Point to Next Region Edge", "Transport", "Move the Edit Point to the next region start or end", ""},
        {cmd::prevEdge, "Edit Point to Previous Region Edge", "Transport", "Move the Edit Point to the previous region start or end", ""},
        {cmd::scrollLeft, "Scroll Left", "View", "Scroll the active timeline earlier", ""},
        {cmd::scrollRight, "Scroll Right", "View", "Scroll the active timeline later", ""},
        {cmd::zoomSelection, "Zoom to In-Out", "View", "Fit the In-Out (or the selected region) to the window", ""},
        // Help
        {cmd::keyPresetLathe, "Key Preset: Lathe", "Help", "Use Lathe's default key assignments", ""},
        {cmd::keyPresetLegacy, "Key Preset: Legacy", "Help", "Use the classic workstation layout (function-key transport, [ ] for In / Out, F9-F11 marks)", ""},
        {cmd::keyGuide, "Key Command Guide", "Help", "Every command and its keys", "F1;command + shift + /"},
        {cmd::editKeyCommands, "Edit Key Commands...", "Help", "Change, add or remove key assignments", "command + option + K"},
    };
    return defs;
}

juce::String canonicalKey(const juce::String& description) {
    auto parts = juce::StringArray::fromTokens(description.trim().toLowerCase(), "+", "");
    parts.trim();
    parts.removeEmptyStrings();
    if (parts.isEmpty()) return {};
    const juce::String keyName = parts[parts.size() - 1];
    parts.remove(parts.size() - 1);
    for (auto& m : parts) m = m == "ctrl" ? "control" : m == "alt" ? "option" : m == "cmd" ? "command" : m;
    parts.sort(false);
    return parts.joinIntoString("+") + "+" + juce::KeyPress::createFromDescription(keyName).getTextDescription().toLowerCase();
}

juce::StringArray keyPresetNames() { return {"Lathe", "Legacy"}; }

namespace {
// The Legacy layout. Commands not listed keep their Lathe keys unless a Legacy key takes them.
const std::vector<std::pair<juce::CommandID, const char*>>& legacyKeys() {
    static const std::vector<std::pair<juce::CommandID, const char*>> k{
        // File
        {cmd::newSession, "command + N"}, {cmd::openSession, "command + O"}, {cmd::saveSession, "command + S"},
        {cmd::openDdpSession, "command + option + O"}, {cmd::exportDdp, ""},
        // Playback
        {cmd::playStop, "spacebar"}, {cmd::playSelection, "command + spacebar;command + control + F5;command + return"},
        {cmd::playFromPlayhead, "option + spacebar"}, {cmd::playRepeatInOut, "command + option + spacebar;command + option + return"},
        {cmd::playToIn, "F5"}, {cmd::playFromIn, "F6"}, {cmd::playToOut, "F7"}, {cmd::playFromOut, "F8"},
        {cmd::playSourceInOut, ""}, {cmd::playAroundEdit, ""},
        // Edit
        {cmd::undo, "command + Z"}, {cmd::redo, "command + shift + Z"}, {cmd::clearSelection, "command + D;control + \\"},
        {cmd::cutRange, "command + X"}, {cmd::copyRange, "command + C"}, {cmd::pasteReplace, "command + V"},
        {cmd::pasteInsert, "command + option + V"}, {cmd::pasteOverlay, "shift + V"},
        {cmd::deleteClose, "delete;backspace"}, {cmd::eraseRange, "option + delete;option + backspace"},
        {cmd::crossfadeEditor, "control + G"},
        {cmd::overwriteEdit, "F1"}, {cmd::insertEdit, "F2"},
        {cmd::replaceLeftEdit, "option + F1"}, {cmd::constrainedEdit, "shift + F1"}, {cmd::replaceEdit, "control + F1"}, {cmd::overlayEdit, "option + F2"},
        {cmd::sourceIn, "["}, {cmd::sourceOut, "]"}, {cmd::selectRegionRange, "control + option + [;command + \\"},
        {cmd::clearIn, "control + ["}, {cmd::clearOut, "control + ]"},
        {cmd::movePointIn, "option + ["}, {cmd::movePointOut, "option + ]"},
        {cmd::editPointToPlayhead, "\\"},
        {cmd::nextMarker, "shift + F6"}, {cmd::prevMarker, "shift + command + F6"},
        {cmd::nextEdge, "shift + F5"}, {cmd::prevEdge, "shift + command + F5"},
        {cmd::goToIn, "shift + F7"}, {cmd::goToOut, "shift + command + F7"},
        {cmd::nextSrp, "shift + F10"}, {cmd::prevSrp, "shift + command + F10"},
        // View
        {cmd::gainOverlay, "option + G"}, {cmd::textView, "option + T"}, {cmd::scrollRight, "cursor right"}, {cmd::scrollLeft, "cursor left"},
        {cmd::zoomIn, "cursor down;command + =;command + +"}, {cmd::zoomOut, "cursor up;command + -"},
        {cmd::zoomSelection, "command + G"}, {cmd::zoomFit, "command + E"},
        // CD prep
        {cmd::addTrackMarker, "F9"}, {cmd::addPauseMarker, "F10"}, {cmd::addIndexMarker, "F11"},
        {cmd::markInfo, "command + M"}, {cmd::editedBlackToMarks, "shift + F12"},
        // System
        {cmd::audioSetup, "option + A;command + ,"},
        {cmd::nudgeSizeA, "control + 1"}, {cmd::nudgeSizeB, "control + 2"}, {cmd::nudgeSizeC, "control + 3"},
    };
    return k;
}
}  // namespace

juce::StringArray presetKeys(int preset, const CommandDef& d) {
    auto split = [](const char* keys) {
        juce::StringArray a = juce::StringArray::fromTokens(keys, ";", "");
        a.trim();
        a.removeEmptyStrings();
        return a;
    };
    if (preset != 1) return split(d.keys);
    std::set<juce::String> taken;
    for (auto& [id, keys] : legacyKeys()) {
        if (id == d.id) return split(keys);
        for (auto& k : split(keys)) taken.insert(canonicalKey(k));
    }
    juce::StringArray out;
    for (auto& k : split(d.keys))
        if (taken.count(canonicalKey(k)) == 0) out.add(k);
    return out;
}

void applyKeyPreset(juce::KeyPressMappingSet& keys, int preset) {
    for (auto& d : allCommands()) keys.clearAllKeyPresses(d.id);
    for (auto& d : allCommands())
        for (auto& k : presetKeys(preset, d)) keys.addKeyPress(d.id, juce::KeyPress::createFromDescription(k));
}

const CommandDef* findCommand(juce::CommandID id) {
    for (auto& d : allCommands())
        if (d.id == id) return &d;
    return nullptr;
}

// Mac-style label, e.g. "⌥⇧←", the same on every platform the guide is generated on.
static juce::String macLabel(const juce::KeyPress& k) {
    juce::String s;
    auto m = k.getModifiers();
    if (m.isCtrlDown() && !m.isCommandDown()) s << juce::String::charToString(0x2303);
#if JUCE_MAC
    if (m.isCtrlDown() && m.isCommandDown()) s << juce::String::charToString(0x2303);
#endif
    if (m.isAltDown()) s << juce::String::charToString(0x2325);
    if (m.isShiftDown()) s << juce::String::charToString(0x21e7);
    if (m.isCommandDown()) s << juce::String::charToString(0x2318);
    const int code = k.getKeyCode();
    juce::String key;
    if (code == juce::KeyPress::spaceKey) key = "Space";
    else if (code == juce::KeyPress::returnKey) key = "Return";
    else if (code == juce::KeyPress::escapeKey) key = "Esc";
    else if (code == juce::KeyPress::backspaceKey) key = juce::String::charToString(0x232b);
    else if (code == juce::KeyPress::deleteKey) key = juce::String::charToString(0x2326);
    else if (code == juce::KeyPress::leftKey) key = juce::String::charToString(0x2190);
    else if (code == juce::KeyPress::rightKey) key = juce::String::charToString(0x2192);
    else if (code == juce::KeyPress::upKey) key = juce::String::charToString(0x2191);
    else if (code == juce::KeyPress::downKey) key = juce::String::charToString(0x2193);
    else if (code == juce::KeyPress::homeKey) key = "Home";
    else if (code == juce::KeyPress::endKey) key = "End";
    else if (code == juce::KeyPress::tabKey) key = "Tab";
    else if (code >= juce::KeyPress::F1Key && code <= juce::KeyPress::F16Key) key = "F" + juce::String(code - juce::KeyPress::F1Key + 1);
    else key = juce::String::charToString(juce::CharacterFunctions::toUpperCase(juce::juce_wchar(code)));
    return s + key;
}

// The same label straight from a key description ("control + shift + R"). A KeyPress can't be
// used for this off the Mac: there Control and Command are one modifier, and every Control key
// would print as Command.
static juce::String macLabelFor(const juce::String& description) {
    auto parts = juce::StringArray::fromTokens(description.trim().toLowerCase(), "+", "");
    parts.trim();
    parts.removeEmptyStrings();
    if (parts.isEmpty()) return {};
    const auto keyName = parts[parts.size() - 1];
    parts.remove(parts.size() - 1);
    const bool ctrl = parts.contains("control") || parts.contains("ctrl"), alt = parts.contains("option") || parts.contains("alt");
    const bool shift = parts.contains("shift"), cmd = parts.contains("command") || parts.contains("cmd");
    juce::String s;
    if (ctrl) s << juce::String::charToString(0x2303);
    if (alt) s << juce::String::charToString(0x2325);
    if (shift) s << juce::String::charToString(0x21e7);
    if (cmd) s << juce::String::charToString(0x2318);
    return s + macLabel(juce::KeyPress(juce::KeyPress::createFromDescription(keyName).getKeyCode()));
}

static int guidePreset = 0;   // which preset a guide without live keys shows

static juce::StringArray keysFor(const CommandDef& d, const juce::KeyPressMappingSet* keys) {
    juce::StringArray out;
    if (!keys && guidePreset != 0) {
        for (auto& s : presetKeys(guidePreset, d)) out.add(macLabelFor(s));
        return out;
    }
    if (keys) {
        for (auto& k : keys->getKeyPressesAssignedToCommand(d.id)) out.add(macLabel(k));
    } else {
        for (auto& s : juce::StringArray::fromTokens(d.keys, ";", ""))
            if (s.trim().isNotEmpty()) out.add(macLabelFor(s.trim()));
    }
    return out;
}

static juce::StringArray categories() {
    juce::StringArray c;
    for (auto& d : allCommands()) c.addIfNotAlreadyThere(d.category);
    return c;
}

juce::String keyGuideText(const juce::KeyPressMappingSet* keys) {
    juce::String s;
    s << "LATHE KEY COMMANDS\n";
    s << (keys ? "Current assignments" : "Default assignments") << ". Change them in Help > Edit Key Commands.\n";
    for (auto& cat : categories()) {
        s << "\n" << cat.toUpperCase() << "\n";
        for (auto& d : allCommands()) {
            if (cat != d.category) continue;
            auto k = keysFor(d, keys).joinIntoString("  or  ");
            s << "  " << juce::String(d.name).paddedRight(' ', 36) << (k.isEmpty() ? juce::String("-") : k).paddedRight(' ', 22) << d.description << "\n";
        }
    }
    return s;
}

juce::String keyGuideHtmlForPreset(int preset) {
    guidePreset = preset;
    auto h = keyGuideHtml(nullptr);
    guidePreset = 0;
    if (preset == 1) h = h.replace("Default assignments", "The Legacy preset (Help &rsaquo; Key Preset: Legacy)");
    return h;
}

juce::String keyGuideHtml(const juce::KeyPressMappingSet* keys) {
    auto esc = [](juce::String t) { return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"); };
    juce::String h;
    h << "<!doctype html><html><head><meta charset='utf-8'><title>Lathe Key Commands</title><style>"
         "body{font:14px -apple-system,Helvetica,Arial,sans-serif;margin:32px;color:#1c1d20;max-width:980px}"
         "h1{font-size:22px;margin:0 0 4px}p.sub{color:#666;margin:0 0 20px}"
         "h2{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:#a06d12;margin:26px 0 6px;border-bottom:1px solid #ddd;padding-bottom:4px}"
         "table{border-collapse:collapse;width:100%;table-layout:fixed}td{padding:5px 8px;vertical-align:top;border-bottom:1px solid #f0f0f0}"
         "td.k{white-space:nowrap;width:150px}kbd{font:13px -apple-system,Helvetica,sans-serif;background:#f3f3f5;border:1px solid #d6d6db;border-radius:4px;padding:1px 6px;margin-right:4px}"
         "td.n{width:260px;font-weight:600}td.d{color:#555}"
         ".cols{columns:1}@media print{body{margin:12mm}h2{break-after:avoid}tr{break-inside:avoid}}"
         "</style></head><body>";
    h << "<h1>Lathe Key Commands</h1><p class='sub'>" << (keys ? "Current assignments" : "Default assignments")
      << ". Change any of them in <b>Help &rsaquo; Edit Key Commands</b>. "
         "&#8984; Command &nbsp; &#8997; Option &nbsp; &#8679; Shift &nbsp; &#8963; Control</p>";
    for (auto& cat : categories()) {
        h << "<h2>" << esc(cat) << "</h2><table>";
        for (auto& d : allCommands()) {
            if (cat != d.category) continue;
            juce::String k;
            for (auto& key : keysFor(d, keys)) k << "<kbd>" << esc(key) << "</kbd>";
            h << "<tr><td class='k'>" << (k.isEmpty() ? juce::String("&ndash;") : k) << "</td><td class='n'>" << esc(d.name)
              << "</td><td class='d'>" << esc(d.description) << "</td></tr>";
        }
        h << "</table>";
    }
    h << "</body></html>";
    return h;
}

}  // namespace lathe
