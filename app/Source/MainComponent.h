#pragma once

#include "AudioEngine.h"
#include "ChainPanel.h"
#include "ControlSurface.h"
#include "DdpFormat.h"
#include "DdpPlayer.h"
#include "DeliveryPanels.h"
#include "PluginManager.h"
#include "MarkInfo.h"
#include "AutoMarks.h"
#include "Restore.h"
#include "Commands.h"
#include "EditPanels.h"
#include "Exporter.h"
#include "Session.h"
#include "SourceView.h"
#include "Timeline.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

namespace lathe {

class LevelMeters;
struct RelinkState;

class MainComponent : public juce::Component,
                      public juce::TableListBoxModel,
                      public juce::FileDragAndDropTarget,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      private juce::Timer,
                      private juce::ChangeListener {
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Commands, key mappings and menus
    juce::ApplicationCommandManager& commandManager() { return commands_; }
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& ids) override;
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
    bool perform(const InvocationInfo& info) override;
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int, int) override {}

    Session& session() { return session_; }
    void sessionChanged(bool undoable = true);
    bool saveSession(bool saveAs);
    void openSession();
    void newSession();
    bool loadSessionFile(const juce::File& f);
    bool hasUnsavedChanges() const { return dirty_; }
    void writeAutosave(const juce::File& f);

    // PQ list (markers)
    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell(int row, int col, bool selected, juce::Component* existing) override;
    void cellClicked(int row, int col, const juce::MouseEvent&) override;
    void cellDoubleClicked(int row, int col, const juce::MouseEvent&) override;
    void deleteKeyPressed(int row) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    void setCellText(int row, int col, const juce::String& text);
    juce::String cellText(int row, int col) const;

private:
    struct ChainModel : juce::ListBoxModel {
        MainComponent& o;
        explicit ChainModel(MainComponent& m) : o(m) {}
        int getNumRows() override;
        void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void importFiles(const juce::Array<juce::File>& files);
    void appendSource();
    // Source-to-destination edits.
    enum class EditKind { Insert, Replace, ReplaceLeft, Constrained, Fit, Overlay };
    void sourceEdit(EditKind kind);
    void openSourceFiles(const juce::Array<juce::File>& files);
    void showSourceOpenDialog();
    void attachSource();          // after session_ is replaced or reloaded
    // Two timelines: one is active (keys, edits, Play act on it).
    void setActiveTimeline(bool source);
    Timeline& activeTimeline() { return sourceActive_ ? sourceView_.timeline() : timeline_; }
    Session& activeSession() { return sourceActive_ ? srcSession_ : session_; }
    bool activePlaying() const { return sourceActive_ ? engine_.isSourcePlaying() : engine_.isPlaying(); }
    double activePosition() const { return sourceActive_ ? engine_.sourcePosition() : engine_.positionSeconds(); }
    double markTime();                  // playhead while playing, else the Edit Point
    void locate(double t);              // active timeline: Edit Point (and playhead) to t
    void locateDest(double t);
    void togglePlay();
    void stopAll();
    void playRange(bool source, double from, double to);
    void playRelative(int which);       // 0 to In, 1 from In, 2 to Out, 3 from Out, 4 through Edit Point
    void afterStop();                   // Edit Point / playhead bookkeeping when playback ends
    // Clipboard
    void copyActive(bool cut);
    void pasteActive(PasteMode mode);
    void eraseActive();
    juce::Array<int> editLanes(const Session& s, const Timeline& t, int clipLanes) const;
    // Points
    void nudgePoint(bool in, int dir);
    void showMovePoint(int which = 0);   // 0 Edit Point, 1 In, 2 Out, 3 SRP
    void showPlaybackSettings();
    void addSrp(bool withText);
    void editSrpText(bool source, int id);
    void jumpToSrp(int dir);
    void addMarkerAtPlayhead(Marker::Type t);
    void splitAtPlayhead();
    void deleteSelection(bool ripple);
    void nudgeSelectedRegion(double dt);
    void openCrossfade(int out, int in);
    void auditionRange(double from, double to) { playRange(false, from, to); }
    void refreshAll();
    void refreshIssues();
    void showPluginScanner();
    void showPluginManager();
    void showPluginMenu();
    void showExportMenu();
    void exportDdp();
    void exportTrackWavs();
    void analyse();
    void verifyDdp();
    void showAudioSettings();
    void undo();
    void redo();
    ExportSettings exportSettings() const;
    void showText(const juce::String& title, const juce::String& text);
    void showKeyGuide();
    void showKeyEditor();
    void runNullTest();
    void jumpToMarker(int dir);
    void applyFollowRate();
    void saveKeyMappings();
    void openRegionFx(int regionId);
    void runAnalysis();
    void openDdpPlayer();
    void showRecordDialog();
    void showControlSurfaces();
    int targetRegion() const;
    void showRestore(int regionId);
    void showSpectral(int regionId);
    juce::File processedFolder() const;
    void beginRecording();
    void finishRecording();
    juce::File recordingsFolder() const;
    void openDdpAsSession(const juce::File& folder);
    void showAnalysisWindow();
    void showReferences();
    void selectReference(int index);
    void measureReference(int index);
    void toggleCompareUnprocessed();
    double referenceGainDb(juce::String* explain = nullptr) const;
    double masterLoudnessAt(double t) const;
    void showBatchDialog();
    // Delivery
    void showProducts();
    void openBreakEditor(int productId, int breakIndex);
    void breakNearPlayhead();
    void showReleaseInfo();
    void showTrackCredits();
    void showStudioProfile();
    void exportPaperworkOnly();
    void saveStudio();
    void refreshDeliveryPanels();
    DeliveryData previewDelivery() const;
    // Missing audio: search next to the session, then ask for each file still missing.
    void locateMissingAudio(bool fromMenu);
    void locateNextMissing(std::shared_ptr<RelinkState> st);
    void finishRelink(std::shared_ptr<RelinkState> st);
    int markerRowToTrackNumber(int row) const;

    Session srcSession_;
    Session session_;
    AudioEngine engine_;
    Exporter exporter_{engine_};
    ThumbPool thumbs_{engine_.formats()};
    SourceView sourceView_{srcSession_, thumbs_};
    Timeline timeline_{session_, thumbs_};
    bool sourceActive_ = false;
    Clip clip_;
    double auditionStart_ = -1;
    bool auditionSource_ = false, repeat_ = false, editFollowsStop_ = false, wasPlaying_ = false, wasSourcePlaying_ = false;
    int nudgeSize_ = 0;
    double preRoll_ = 3.0, postRoll_ = 2.0, nudgeSizes_[3] = {1.0 / 75, 0.1, 1.0};
    double lastStopDest_ = 0, lastStopSource_ = 0;
    juce::String lastSourceState_;
    FadeEditorPrefs fadePrefs_;
    FadeLibrary fadeLib_ = defaultFadeLibrary();
    void saveFadeSettings();
    void showMarkInfo();
    void runAnalogBlack();
    std::unique_ptr<juce::DocumentWindow> markInfoWindow_;
    juce::Component::SafePointer<MarkInfoPanel> markInfoPanel_;
    void crossfadeNewEdits(const juce::Array<int>& ids, Session& s);   // default crossfade on new splices
    std::unique_ptr<LevelMeters> meters_;
    juce::TableListBox pqList_{"PQ", this};
    ChainModel chainModel_{*this};
    juce::ListBox chainList_{"Plugins", &chainModel_};
    juce::TextEditor issues_;
    juce::Label timeDisplay_, trackDisplay_, rateWarning_, selDisplay_;
    juce::TextButton newBtn_{"New"}, openBtn_{"Open"}, saveBtn_{"Save"}, importBtn_{"Import"},
        envBtn_{"Envelope"}, snapBtn_{"Snap"}, lanesBtn_{"Lanes..."}, xfadeBtn_{"Crossfade..."},
        trackMk_{"+ Track"}, indexMk_{"+ Index"}, pauseMk_{"+ Pause"}, splitBtn_{"Split"},
        playBtn_{"Play"}, homeBtn_{"|<"}, pluginsBtn_{"Plugins..."}, exportBtn_{"Export..."},
        analyseBtn_{"Analyse"}, verifyBtn_{"Verify DDP"}, audioBtn_{"Audio Setup"},
        chainAdd_{"Add"}, chainRemove_{"Remove"}, chainBypass_{"Bypass"}, chainUp_{"Up"}, chainDown_{"Down"},
        zoomIn_{"+"}, zoomOut_{"-"}, zoomFit_{"Fit"};
    juce::Label titleLbl_, perfLbl_, upcLbl_, clientLbl_, engLbl_, ditherLbl_;
    juce::TextEditor title_, performer_, upc_, client_, engineer_;
    juce::ComboBox dither_;
    juce::StringArray undo_, redo_;
    juce::String lastState_;
    bool dirty_ = false, updatingFields_ = false;
    double lastDeviceRate_ = 0, auditionEnd_ = -1;
    juce::ApplicationProperties props_;
    juce::ApplicationCommandManager commands_;
    bool followRate_ = true;
    juce::String rateMessage_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Component::SafePointer<CrossfadeEditor> xfadeEditor_;
    std::unique_ptr<juce::DocumentWindow> xfadeWindow_;
    std::unique_ptr<juce::DocumentWindow> fxWindow_;
    int fxWindowRegion_ = 0;
    std::shared_ptr<dsp::AnalysisResult> analysis_, rawAnalysis_;
    bool analysisStale_ = false;
    bool compare_ = false, compareRestoreRegionBypass_ = false;
    std::unique_ptr<juce::DocumentWindow> refWindow_, analysisWindow_, playerWindow_;
    juce::Label abDisplay_;
    juce::TextButton recordBtn_{"Rec"};
    struct RecSettings {
        int firstInput = 0, bits = 24, extraLatency = 0, measured = -1;
        bool print = true, autoPunch = false, muteSources = true;
    } rec_;
    juce::File recFile_;
    juce::Range<double> recPunch_;
    std::unique_ptr<juce::DocumentWindow> recWindow_, surfaceWindow_;
    std::unique_ptr<ControlSurface> surface_;
    std::unique_ptr<juce::DocumentWindow> restoreWindow_, spectralWindow_;
    std::shared_ptr<dsp::NoiseProfile> noise_;
    StudioProfile studio_;
    std::unique_ptr<juce::DocumentWindow> pluginManagerWindow_;
    std::unique_ptr<juce::DocumentWindow> productsWindow_, breakWindow_, releaseWindow_, creditsWindow_, studioWindow_;
    juce::Component::SafePointer<ProductsPanel> productsPanel_;
    juce::Component::SafePointer<BreakEditor> breakEditor_;
    juce::Component::SafePointer<ReleaseInfoPanel> releasePanel_;
    juce::Component::SafePointer<TrackCreditsPanel> creditsPanel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

}  // namespace lathe
