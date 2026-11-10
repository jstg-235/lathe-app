// Every user action, with its menu name, category, description and default keys.
// Key assignments are editable (Help > Edit Key Commands) and saved per user.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace lathe {

namespace cmd {
enum : juce::CommandID {
    // File
    newSession = 0x1000, openSession, saveSession, saveSessionAs, importAudio, openSource,
    exportDdp, exportTrackWavs, exportProgram24, exportProgram32, batchRender, locateMissingAudio = 0x1080,
    // Edit
    undo = 0x1100, redo, splitAtPlayhead, deleteClose, deleteLeave, muteRegion, nudgeLeft, nudgeRight,
    nudgeLeftBig, nudgeRightBig, clearSelection, selectRegionRange, regionEffects, bypassRegionFx,
    restoreRegion, spectralEditor,
    // Source / destination
    sourceIn = 0x1200, sourceOut, destIn, destOut, insertEdit, overwriteEdit, replaceEdit, appendEdit, playSourceInOut,
    // Transport
    playStop = 0x1300, playSource, returnToStart, locateBackFrame, locateFwdFrame, locateBackSecond, locateFwdSecond,
    prevMarker, nextMarker, playSelection, record,
    // Markers
    addTrackMarker = 0x1400, addIndexMarker, addPauseMarker, addEndMarker,
    // View
    zoomIn = 0x1500, zoomOut, zoomFit, toggleEnvelope, toggleSnap, crossfadeEditor,
    // Tools
    analyse = 0x1600, verifyDdp, nullTest, pluginManager, audioSetup, followProjectRate, showAnalysis, references,
    listenReference, compareUnprocessed, compareLevelMatch, compareEnd, toggleLoudness, ddpPlayer, openDdpSession, controlSurfaces,
    // Help
    keyGuide = 0x1700, editKeyCommands,
    // Delivery
    products = 0x1800, breakEditor, releaseInfo, trackCredits, studioProfile, exportPaperwork,
    // Source / destination (two timelines)
    goToIn = 0x1220, goToOut, clearIn, clearOut, moveInToOut, nudgeInLeft, nudgeInRight, nudgeOutLeft, nudgeOutRight,
    nudgeSizeA, nudgeSizeB, nudgeSizeC, movePoint, replaceLeftEdit, constrainedEdit, overlayEdit, switchTimeline, linkPlay,
    clearSource, editPointToPlayhead,
    // Clipboard
    cutRange = 0x1260, copyRange, pasteInsert, pasteReplace, pasteReplaceLeft, pasteConstrained, pasteOverlay, eraseRange,
    // Transport
    playToIn = 0x1320, playFromIn, playToOut, playFromOut, playAroundEdit, repeatPlay, editPointFollowsStop, playbackSettings,
    // SRPs
    addSrp = 0x1420, addSrpText, srpsFromSelection, prevSrp, nextSrp, lockSrp, deleteSrp, deleteAllSrps,
    // Added with the key presets
    playFromPlayhead = 0x1340, playRepeatInOut, movePointIn, movePointOut, nextEdge, prevEdge, scrollLeft, scrollRight,
    zoomSelection, keyPresetLathe = 0x1720, keyPresetLegacy,
    // Edit Fade library recall (Option-0 to Option-9)
    fadeRecall0 = 0x1280, fadeRecall1, fadeRecall2, fadeRecall3, fadeRecall4, fadeRecall5, fadeRecall6, fadeRecall7, fadeRecall8, fadeRecall9,
    defaultFadeOnEdits,
    // PQ tools
    markInfo = 0x1440, segmentsToMarks, editedBlackToMarks, analogBlackToMarks, autoSpace, lockAllMarks,
    // Assembly
    textView = 0x1460, segmentGain,
    // Timeline gain
    gainOverlay = 0x1480, gainBypass, lockAllGain, clearGain,
    // Export
    exportSegments = 0x1090,
    // Metering
    meterBridge = 0x14a0, resetMeters,
};
}  // namespace cmd

struct CommandDef {
    juce::CommandID id;
    const char* name;
    const char* category;
    const char* description;
    const char* keys;   // default keys, ';'-separated JUCE descriptions, e.g. "command + Z;shift + space"
};

const std::vector<CommandDef>& allCommands();
const CommandDef* findCommand(juce::CommandID id);

// Key presets: 0 = Lathe (the defaults above), 1 = Legacy (the classic workstation layout:
// function-key transport, [ ] for In / Out, F9-F11 marks, arrows to scroll and zoom).
juce::StringArray keyPresetNames();
// The keys a command gets under a preset, as JUCE descriptions.
juce::StringArray presetKeys(int preset, const CommandDef& d);
// Replaces every assignment in `keys` with the preset's.
void applyKeyPreset(juce::KeyPressMappingSet& keys, int preset);
// A key description normalised the way a Mac keyboard sees it (Control and Command differ),
// for comparing assignments.
juce::String canonicalKey(const juce::String& description);

// The key command guide as text or HTML. With `keys` null, shows the defaults.
juce::String keyGuideText(const juce::KeyPressMappingSet* keys);
juce::String keyGuideHtml(const juce::KeyPressMappingSet* keys);
juce::String keyGuideHtmlForPreset(int preset);   // 0 Lathe, 1 Legacy

}  // namespace lathe
