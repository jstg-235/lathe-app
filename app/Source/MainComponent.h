#pragma once

#include "AudioEngine.h"
#include "Exporter.h"
#include "Session.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

namespace lathe {

class Overview;
class LevelMeters;

class MainComponent : public juce::Component,
                      public juce::TableListBoxModel,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer,
                      private juce::ChangeListener {
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    // Session lifecycle (also used by the app for autosave / recovery).
    Session& session() { return session_; }
    void sessionChanged(bool undoable = true);
    bool saveSession(bool saveAs);
    void openSession();
    void newSession();
    bool loadSessionFile(const juce::File& f);
    bool hasUnsavedChanges() const { return dirty_; }
    void writeAutosave(const juce::File& f);

    // Table
    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell(int row, int col, bool selected, juce::Component* existing) override;
    void selectedRowsChanged(int lastRow) override;
    void cellDoubleClicked(int row, int col, const juce::MouseEvent&) override;

    // Drag and drop audio files onto the window
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int, int) override;

    void setCellText(int row, int col, const juce::String& text);
    juce::String cellText(int row, int col) const;

private:
    // The plugin chain list has its own model (ListBoxModel and TableListBoxModel share names).
    struct ChainModel : juce::ListBoxModel {
        MainComponent& o;
        explicit ChainModel(MainComponent& m) : o(m) {}
        int getNumRows() override;
        void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    };
    ChainModel chainModel_{*this};

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void addFiles(const juce::Array<juce::File>& files);
    void removeSelected();
    void moveSelected(int delta);
    void addSubIndexAtPlayhead();
    void refreshAll();
    void refreshIssues();
    void showPluginScanner();
    void showPluginMenu();
    void showExportMenu();
    void exportDdp();
    void exportTrackWavs();
    void exportProgramWav();
    void analyse();
    void verifyDdp();
    void showAudioSettings();
    void undo();
    void redo();
    void pushUndo();
    ExportSettings exportSettings() const;
    void showText(const juce::String& title, const juce::String& text);
    int selectedRow() const;

    Session session_;
    AudioEngine engine_;
    Exporter exporter_{engine_};
    juce::AudioThumbnailCache thumbCache_{64};
    std::unique_ptr<Overview> overview_;
    std::unique_ptr<LevelMeters> meters_;
    juce::TableListBox table_{"Tracks", this};
    juce::ListBox chainList_{"Plugins", &chainModel_};
    juce::TextEditor issues_;
    juce::Label timeDisplay_, trackDisplay_, rateWarning_;
    juce::TextButton newBtn_{"New"}, openBtn_{"Open"}, saveBtn_{"Save"}, addBtn_{"Add Audio"},
        removeBtn_{"Remove"}, upBtn_{"Up"}, downBtn_{"Down"}, idxBtn_{"+ Index"},
        playBtn_{"Play"}, homeBtn_{"|<"}, pluginsBtn_{"Plugins..."}, exportBtn_{"Export..."},
        analyseBtn_{"Analyse"}, verifyBtn_{"Verify DDP"}, audioBtn_{"Audio Setup"},
        chainAdd_{"Add"}, chainRemove_{"Remove"}, chainBypass_{"Bypass"}, chainUp_{"Up"}, chainDown_{"Down"};
    juce::Label titleLbl_, perfLbl_, upcLbl_, clientLbl_, engLbl_, ditherLbl_;
    juce::TextEditor title_, performer_, upc_, client_, engineer_;
    juce::ComboBox dither_;
    juce::StringArray undo_, redo_;
    juce::String lastState_;
    bool dirty_ = false;
    bool updatingFields_ = false;
    double lastDeviceRate_ = 0;
    std::unique_ptr<juce::DialogWindow> scanWindow_;
    juce::ApplicationProperties props_;
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

}  // namespace lathe
