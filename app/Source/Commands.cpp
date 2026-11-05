#include "Commands.h"

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
        {cmd::exportDdp, "Export DDP Image...", "File", "Write and verify a DDP 2.0 master for replication", "command + E"},
        {cmd::exportTrackWavs, "Export Track WAVs...", "File", "16-bit / 44.1 kHz WAV per track, cut at the PQ marks", "command + shift + E"},
        {cmd::exportProgram24, "Export Programme WAV (24-bit)...", "File", "The whole programme at the project rate, 24-bit dithered", ""},
        {cmd::exportProgram32, "Export Programme WAV (32-bit float)...", "File", "The whole programme at the project rate, 32-bit float", ""},
        {cmd::batchRender, "Render Album (Batch)...", "File", "DDP plus WAV / FLAC deliverables in one go, with a QC and checksum report", "command + R"},
        // Edit
        {cmd::undo, "Undo", "Edit", "Undo the last change", "command + Z"},
        {cmd::redo, "Redo", "Edit", "Redo the last undone change", "command + shift + Z"},
        {cmd::splitAtPlayhead, "Split at Playhead", "Edit", "Cut the selected region (or all regions under the playhead) in two", "B"},
        {cmd::deleteClose, "Delete (Close Gap)", "Edit", "Delete the selected region or marker; later material moves up", "delete;backspace"},
        {cmd::deleteLeave, "Delete (Leave Gap)", "Edit", "Delete the selected region; leave silence in its place", "shift + delete;shift + backspace"},
        {cmd::muteRegion, "Mute / Unmute Region", "Edit", "Silence the selected region without deleting it", "U"},
        {cmd::nudgeLeft, "Nudge Region Left", "Edit", "Move the selected region one CD frame (1/75 s) earlier", "option + cursor left"},
        {cmd::nudgeRight, "Nudge Region Right", "Edit", "Move the selected region one CD frame later", "option + cursor right"},
        {cmd::nudgeLeftBig, "Nudge Region Left 100 ms", "Edit", "Move the selected region 100 ms earlier", "option + shift + cursor left"},
        {cmd::nudgeRightBig, "Nudge Region Right 100 ms", "Edit", "Move the selected region 100 ms later", "option + shift + cursor right"},
        {cmd::clearSelection, "Clear Selection", "Edit", "Deselect regions and the destination range", "escape"},
        {cmd::selectRegionRange, "Select Region Range", "Edit", "Set the destination In/Out to the selected region", "command + A"},
        {cmd::regionEffects, "Region Effects...", "Edit", "Open the selected region's own plugin chain", "F"},
        {cmd::bypassRegionFx, "Bypass All Region Effects", "Edit", "A/B every region's effects at once (the master chain stays on)", "shift + F"},
        // Source / destination
        {cmd::sourceIn, "Mark Source In", "Source / Destination", "Set the Source In point at the source playhead", "I"},
        {cmd::sourceOut, "Mark Source Out", "Source / Destination", "Set the Source Out point at the source playhead", "O"},
        {cmd::destIn, "Mark Destination In", "Source / Destination", "Set the destination In point at the playhead", "shift + I"},
        {cmd::destOut, "Mark Destination Out", "Source / Destination", "Set the destination Out point at the playhead", "shift + O"},
        {cmd::insertEdit, "Insert", "Source / Destination", "Insert the source In-Out at the destination In; later material moves right", "T"},
        {cmd::overwriteEdit, "Overwrite", "Source / Destination", "Overwrite the destination from its In for the source length", "shift + T"},
        {cmd::replaceEdit, "Replace (4-Point)", "Source / Destination", "Replace destination In-Out with source In-Out; later material moves to fit", "R"},
        {cmd::appendEdit, "Append as Track", "Source / Destination", "Add the source In-Out at the end as a new track", "A"},
        {cmd::playSourceInOut, "Play Source In-Out", "Source / Destination", "Audition the marked part of the source", "option + spacebar"},
        // Transport
        {cmd::playStop, "Play / Stop", "Transport", "Play or stop the destination (the master)", "spacebar"},
        {cmd::playSource, "Play / Stop Source", "Transport", "Play or stop the Source window", "shift + spacebar"},
        {cmd::returnToStart, "Return to Start", "Transport", "Move the playhead to the top and reset the meters", "home;return"},
        {cmd::locateBackFrame, "Back One Frame", "Transport", "Move the playhead back 1/75 s", "cursor left"},
        {cmd::locateFwdFrame, "Forward One Frame", "Transport", "Move the playhead forward 1/75 s", "cursor right"},
        {cmd::locateBackSecond, "Back One Second", "Transport", "Move the playhead back 1 s", "shift + cursor left"},
        {cmd::locateFwdSecond, "Forward One Second", "Transport", "Move the playhead forward 1 s", "shift + cursor right"},
        {cmd::prevMarker, "Previous Marker", "Transport", "Jump to the previous PQ marker", "cursor up"},
        {cmd::nextMarker, "Next Marker", "Transport", "Jump to the next PQ marker", "cursor down"},
        {cmd::playSelection, "Play Destination In-Out", "Transport", "Play the selected destination range", "command + spacebar"},
        {cmd::record, "Record / Stop Recording", "Transport", "Record the inputs, or print the session through outboard and record the return", "shift + R"},
        // Markers
        {cmd::addTrackMarker, "Add Track Marker", "Markers", "New CD track (index 01) at the playhead", "M"},
        {cmd::addIndexMarker, "Add Index Marker", "Markers", "New sub-index in the current track at the playhead", "shift + M"},
        {cmd::addPauseMarker, "Add Pause Marker", "Markers", "Start of the pause (index 00) before the next track", "P"},
        {cmd::addEndMarker, "Add End-of-Disc Marker", "Markers", "Set the lead-out (end of programme) at the playhead", "E"},
        // View
        {cmd::zoomIn, "Zoom In", "View", "Zoom the timeline in around the playhead", "command + =;command + +"},
        {cmd::zoomOut, "Zoom Out", "View", "Zoom the timeline out", "command + -"},
        {cmd::zoomFit, "Zoom to Fit", "View", "Show the whole programme", "command + 0"},
        {cmd::toggleEnvelope, "Gain Envelope Mode", "View", "Edit clip gain envelopes on the timeline", "G"},
        {cmd::toggleSnap, "Snap", "View", "Snap edits to region edges, markers and the playhead", "N"},
        {cmd::crossfadeEditor, "Crossfade Editor", "View", "Open the crossfade editor for the edit nearest the playhead", "X"},
        {cmd::toggleLoudness, "Loudness Overlay", "View", "Show short-term loudness, track LUFS / true peak and problem flags on the timeline", "L"},
        // Tools
        {cmd::analyse, "Analyse Programme", "Tools", "Loudness map, per-track stats and problem scan (clips, overs, DC, phase, dropouts) of the master and the unprocessed mix", "command + shift + A"},
        {cmd::showAnalysis, "Analysis Results...", "Tools", "Show the last analysis: summary and a clickable problem list", "command + shift + L"},
        {cmd::references, "References...", "Tools", "Load reference tracks for level-matched A/B", "command + shift + R"},
        {cmd::listenReference, "A/B: Listen to Reference", "Transport", "Switch between the master and the active reference (level-matched, 10 ms crossfade)", "Q"},
        {cmd::compareUnprocessed, "A/B: Compare Unprocessed", "Transport", "Hear the mix without master and region effects, level-matched to the master", "W"},
        {cmd::verifyDdp, "Verify DDP...", "Tools", "Check a DDP folder's checksums and read back its PQ", "command + shift + V"},
        {cmd::ddpPlayer, "DDP Player...", "Tools", "Play and check a finished DDP exactly as the plant will read it", "command + shift + P"},
        {cmd::openDdpSession, "Open DDP as Session...", "File", "Turn a DDP back into an editable session (PQ, CD-Text, ISRC) for revisions", "command + option + O"},
        {cmd::nullTest, "Null Test / Compare Files...", "Tools", "Align two renders and measure exactly how they differ", "command + shift + N"},
        {cmd::pluginManager, "Plugin Manager...", "Tools", "Scan for and manage AU / VST3 plugins", ""},
        {cmd::controlSurfaces, "Control Surfaces / MIDI...", "Tools", "Mackie Control, HUI and MIDI-learn for any command", ""},
        {cmd::audioSetup, "Audio Setup...", "Tools", "Choose the audio interface and buffer size", "command + ,"},
        {cmd::followProjectRate, "Follow Project Sample Rate", "Tools", "Switch the interface to the project's rate so playback is never resampled", ""},
        // Help
        {cmd::keyGuide, "Key Command Guide", "Help", "Every command and its keys", "F1;command + shift + /"},
        {cmd::editKeyCommands, "Edit Key Commands...", "Help", "Change, add or remove key assignments", "command + option + K"},
    };
    return defs;
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

static juce::StringArray keysFor(const CommandDef& d, const juce::KeyPressMappingSet* keys) {
    juce::StringArray out;
    if (keys) {
        for (auto& k : keys->getKeyPressesAssignedToCommand(d.id)) out.add(macLabel(k));
    } else {
        for (auto& s : juce::StringArray::fromTokens(d.keys, ";", ""))
            if (s.trim().isNotEmpty()) out.add(macLabel(juce::KeyPress::createFromDescription(s.trim())));
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
