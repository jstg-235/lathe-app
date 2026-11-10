// The Audio Setup window (interface, outputs, inputs, where and how recordings are made, the
// measured round trips) and the Capture window (send the programme through outboard and record
// the return onto a new lane, in sync).
#pragma once

#include "AudioEngine.h"
#include "Capture.h"
#include "Metering.h"
#include "Theme.h"

namespace lathe {

// "312 samples (7.1 ms)"
juce::String samplesAndMs(int samples, double rate);
// "1.2 GB"
juce::String byteSize(juce::int64 bytes);

class AudioSetupPanel : public juce::Component, private juce::ChangeListener, private juce::Timer {
public:
    AudioSetupPanel(AudioEngine& e, IoSettings& io, std::function<juce::File()> sessionFile, std::function<double()> sessionRate);
    ~AudioSetupPanel() override;
    std::function<void()> onChange;   // settings changed: save them and hand them to the engine
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override;
    void refresh();
    void applySetup(const std::function<void(juce::AudioDeviceManager::AudioDeviceSetup&)>& change);
    void changed() { if (onChange) onChange(); }
    void updateRecordingInfo();
    void updateDelayInfo();
    juce::String delayKey() const;

    AudioEngine& e_;
    IoSettings& io_;
    std::function<juce::File()> sessionFile_;
    std::function<double()> sessionRate_;
    bool refreshing_ = false, testing_ = false;
    juce::Label outHead_, inHead_, recHead_;
    juce::Label outDevL_, rateL_, bufL_, outPairL_, inDevL_, inPairsL_, defInL_, whereL_, pathL_, formatL_, nameL_, delayL_, delayInfo_, warn_;
    juce::ComboBox outDev_, rate_, buf_, outPair_, inDev_, defIn_, format_, delayOut_, delayIn_;
    juce::ToggleButton six_, nextTo_, chosen_, monitor_;
    juce::TextButton choose_{"Choose..."}, measure_{"Measure"};
    juce::OwnedArray<juce::TextButton> inPairs_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

class CapturePanel : public juce::Component, private juce::Timer {
public:
    struct Status {
        bool checkingLevels = false, capturing = false;
        double progress = 0;               // 0..1 through the range while capturing
        bool haveTake = false;             // a capture is on the timeline, ready to check
        int hearing = 0;                   // 0 = not checking, 1 = what was sent, 2 = the capture
        bool levelMatch = false, matchMeasured = false;
        double matchDifferenceDb = 0;      // sent minus capture, LU
        juce::String message;              // the last result, or a problem
    };
    struct Host {
        std::function<CapturePlan()> plan;          // from the destination In-Out as it is now
        std::function<Status()> status;
        std::function<void(bool)> levelCheck;       // loop the range through the gear, nothing recorded
        std::function<void()> capture, stop;
        std::function<void(int)> hear;              // 1 = sent, 2 = capture, 0 = end the check
        std::function<void(bool)> levelMatch;
        std::function<void()> nullTest;
        std::function<void()> settingsChanged;      // route or options changed: save
        std::function<void()> audioSetup;
        std::function<juce::File()> folder;         // where the take will be written
    };
    CapturePanel(AudioEngine& e, IoSettings& io, MeterHub& out, MeterHub& in, Host host);
    void paint(juce::Graphics&) override;
    void resized() override;
    // The round trip used for a capture on the route as it is set: the saved measurement, or
    // the interface's own figure when these sockets were never measured.
    int roundTrip(bool* measured = nullptr) const;

private:
    void timerCallback() override;
    void refreshRoute();
    void routeChanged();
    juce::String delayKey() const;

    AudioEngine& e_;
    IoSettings& io_;
    MeterHub& out_;
    MeterHub& in_;
    Host host_;
    bool testing_ = false;
    Status st_;
    CapturePlan plan_;
    juce::String what_, disk_, routeNote_, routeWarn_, levelNote_;
    bool levelHot_ = false;
    int deviceStamp_ = -1;
    juce::Label sendL_, returnL_, delayInfo_, postL_;
    juce::ComboBox send_, return_, post_;
    juce::ToggleButton mute_;
    juce::TextButton measure_{"Measure now"}, setup_{"Audio Setup..."}, check_{"Level check"}, capture_{"Capture"},
        hearSent_{"Hear what was sent"}, hearCapture_{"Hear the capture"}, match_{"Level Match"}, endCheck_{"End"}, null_{"Null test"};
    juce::Rectangle<int> whatArea_, meterArea_, progressArea_, checkArea_, routeArea_, headArea_[4];
};

}  // namespace lathe
