// Lathe application: window, autosave every 60 s, and crash recovery on the next launch.
#include "MainComponent.h"

namespace lathe {

class LatheApp : public juce::JUCEApplication, private juce::Timer {
public:
    const juce::String getApplicationName() override { return "Lathe"; }
    const juce::String getApplicationVersion() override { return LATHE_VERSION; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLine) override {
        auto dir = appData();
        bool crashed = runningFlag().existsAsFile() && autosaveFile().existsAsFile();
        runningFlag().replaceWithText(juce::Time::getCurrentTime().toISO8601(true));

        window_ = std::make_unique<Window>();
        auto& mc = window_->main();

        auto arg = commandLine.unquoted().trim();
        if (arg.endsWithIgnoreCase(".lathe") && juce::File(arg).existsAsFile()) mc.loadSessionFile(juce::File(arg));

        if (crashed) {
            auto options = juce::MessageBoxOptions::makeOptionsYesNo(
                juce::MessageBoxIconType::QuestionIcon, "Recover your work?",
                "Lathe didn't shut down normally last time. Restore the automatic backup from " +
                    autosaveFile().getLastModificationTime().toString(true, true) + "?",
                "Restore", "Discard");
            juce::AlertWindow::showAsync(options, [this](int r) {
                if (r == 1 && window_) {
                    window_->main().loadSessionFile(autosaveFile());
                    window_->main().session().file = juce::File();   // make the user choose where to save it
                }
            });
        }
        startTimer(60 * 1000);
    }

    void shutdown() override {
        window_.reset();
        runningFlag().deleteFile();   // clean exit: no recovery prompt next time
    }

    void systemRequestedQuit() override {
        if (window_ && window_->main().hasUnsavedChanges()) {
            auto options = juce::MessageBoxOptions::makeOptionsYesNoCancel(
                juce::MessageBoxIconType::QuestionIcon, "Save changes?", "Save the session before quitting?", "Save", "Don't Save", "Cancel");
            juce::AlertWindow::showAsync(options, [this](int r) {
                if (r == 1) { if (window_->main().session().file != juce::File()) window_->main().saveSession(false); else return; }
                if (r == 0) return;   // cancel
                quit();
            });
            return;
        }
        quit();
    }

    void anotherInstanceStarted(const juce::String& commandLine) override {
        auto arg = commandLine.unquoted().trim();
        if (window_ && arg.endsWithIgnoreCase(".lathe")) window_->main().loadSessionFile(juce::File(arg));
    }

private:
    class Window : public juce::DocumentWindow {
    public:
        Window() : DocumentWindow("Lathe", juce::Colour(0xff16181c), DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            auto* mc = new MainComponent();
            setContentOwned(mc, true);
#if ! JUCE_MAC
            setMenuBar(mc);
#endif
            setResizable(true, false);
            setResizeLimits(1000, 640, 10000, 10000);
            centreWithSize(1360, 860);
            setVisible(true);
        }
        ~Window() override {
#if ! JUCE_MAC
            setMenuBar(nullptr);
#endif
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        MainComponent& main() { return *static_cast<MainComponent*>(getContentComponent()); }
    };

    void timerCallback() override {
        if (window_) window_->main().writeAutosave(autosaveFile());
    }

    static juce::File appData() {
        auto d = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Lathe");
        d.createDirectory();
        return d;
    }
    static juce::File runningFlag() { return appData().getChildFile("running.flag"); }
    static juce::File autosaveFile() { return appData().getChildFile("autosave.lathe"); }

    std::unique_ptr<Window> window_;
};

}  // namespace lathe

START_JUCE_APPLICATION(lathe::LatheApp)
