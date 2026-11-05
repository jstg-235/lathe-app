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
    exportDdp, exportTrackWavs, exportProgram24, exportProgram32,
    // Edit
    undo = 0x1100, redo, splitAtPlayhead, deleteClose, deleteLeave, muteRegion, nudgeLeft, nudgeRight,
    nudgeLeftBig, nudgeRightBig, clearSelection, selectRegionRange,
    // Source / destination
    sourceIn = 0x1200, sourceOut, destIn, destOut, insertEdit, overwriteEdit, replaceEdit, appendEdit, playSourceInOut,
    // Transport
    playStop = 0x1300, playSource, returnToStart, locateBackFrame, locateFwdFrame, locateBackSecond, locateFwdSecond,
    prevMarker, nextMarker, playSelection,
    // Markers
    addTrackMarker = 0x1400, addIndexMarker, addPauseMarker, addEndMarker,
    // View
    zoomIn = 0x1500, zoomOut, zoomFit, toggleEnvelope, toggleSnap, crossfadeEditor,
    // Tools
    analyse = 0x1600, verifyDdp, nullTest, pluginManager, audioSetup, followProjectRate,
    // Help
    keyGuide = 0x1700, editKeyCommands,
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

// The key command guide as text or HTML. With `keys` null, shows the defaults.
juce::String keyGuideText(const juce::KeyPressMappingSet* keys);
juce::String keyGuideHtml(const juce::KeyPressMappingSet* keys);

}  // namespace lathe
