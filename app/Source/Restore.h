// Restoration and spectral editing of regions, non-destructively: the region's audio
// (plus a margin either side) is processed into a new 32-bit float file and the region is
// pointed at it. Undo restores the original reference; source files are never modified.
#pragma once

#include "AudioEngine.h"
#include "Session.h"
#include "Theme.h"
#include "Timeline.h"

#include "../../core/dsp/Restoration.h"

namespace lathe {

struct RestoreSettings {
    bool dc = false;
    bool hum = false;
    double humF0 = 0;   // 0 = auto
    int humHarmonics = 8;
    bool declick = true;
    double declickSensitivity = 5, declickMaxMs = 1.0;
    bool decrackle = false;
    double crackleAmount = 4;
    bool denoise = false;
    double reductionDb = 12, sensitivity = 1.2;
};

struct RestoreResult {
    bool ok = false;
    juce::String error, report;
    juce::File file;
    juce::int64 fileLength = 0, srcIn = 0, srcOut = 0;
};

// Reads [region span ∩ range] of a region's source with up to `marginSec` either side.
bool readRegionAudio(juce::AudioFormatManager& fm, const Region& r, dsp::Channels& out, juce::int64& firstSample, double marginSec);

// Processes region r. If `range` (timeline seconds) is non-empty, only that part changes;
// the rest of the region is copied bit-for-bit.
RestoreResult restoreRegion(juce::AudioFormatManager& fm, const Region& r, juce::Range<double> range, const RestoreSettings& s,
                            const dsp::NoiseProfile* noise, const juce::File& folder, const std::function<bool(double)>& progress);

RestoreResult spectralEditRegion(juce::AudioFormatManager& fm, const Region& r, double t0, double t1, double f0, double f1,
                                 dsp::SpectralMode mode, double gainDb, const juce::File& folder);

// Points the region at the processed file (keeps gain, fades, envelope, effects, markers).
void applyProcessed(Session& s, int regionId, const RestoreResult& res, const juce::String& suffix);

// ---- UI ---------------------------------------------------------------------------------

class RestorePanel : public juce::Component {
public:
    RestorePanel(const juce::String& target, bool haveProfile, bool haveSelection);
    RestoreSettings settings() const;
    bool selectionOnly() const { return selOnly_.getToggleState(); }
    void setProfileStatus(const juce::String& s) { profile_.setText(s, juce::dontSendNotification); }
    std::function<void()> onLearnNoise, onProcess;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::ToggleButton dc_, hum_, declick_, decrackle_, denoise_, selOnly_;
    juce::ComboBox humF0_;
    juce::Slider clickSens_, clickLen_, crackle_, reduction_, sens_;
    juce::Label title_, profile_, l1_, l2_, l3_, l4_, l5_;
    juce::TextButton learn_{"Learn Noise from Selection"}, process_{"Process"};
};

class SpectralView : public juce::Component {
public:
    SpectralView(AudioEngine& e, Session& s, int regionId, juce::Range<double> view);
    std::function<void(double t0, double t1, double f0, double f1, dsp::SpectralMode, double)> onApply;
    std::function<void(double from, double to)> onAudition;
    void refresh();   // re-reads the region (after an edit)
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> plot() const;
    double xToT(float x) const;
    double yToF(float y) const;
    float fToY(double f) const;
    void render();

    AudioEngine& e_;
    Session& s_;
    int id_;
    juce::Range<double> view_;   // region-local seconds shown
    double rate_ = 44100;
    juce::Image img_;
    bool logFreq_ = true;
    juce::Rectangle<float> sel_;   // in plot pixels
    juce::Point<float> dragFrom_;
    juce::TextButton attenuate_{"Attenuate"}, heal_{"Heal (remove)"}, play_{"Play Selection"}, scale_{"Log / Linear"};
    juce::Slider gain_;
    juce::Label info_;
};

}  // namespace lathe
