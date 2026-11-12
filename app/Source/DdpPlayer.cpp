#include "DdpPlayer.h"

namespace lathe {

enum { cTrk = 1, cStart, cLen, cPause, cTitle, cPerf, cIsrc, cFlags };

DdpPlayer::DdpPlayer(AudioEngine& e, const juce::File& folder, const ddp::ReadResult& r) : e_(e), folder_(folder), r_(r) {
    loaded_ = e_.loadPlayer(juce::File(juce::String::fromUTF8(r_.imageFile.c_str())));
    auto& h = table_.getHeader();
    h.addColumn("#", cTrk, 34);
    h.addColumn("Start", cStart, 78);
    h.addColumn("Length", cLen, 74);
    h.addColumn("Pause", cPause, 64);
    h.addColumn("Title", cTitle, 220);
    h.addColumn("Performer", cPerf, 150);
    h.addColumn("ISRC", cIsrc, 116);
    h.addColumn("Flags", cFlags, 60);
    h.setColour(juce::TableHeaderComponent::backgroundColourId, colours::bg);
    h.setColour(juce::TableHeaderComponent::textColourId, colours::dim);
    table_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    table_.setRowHeight(24);
    addAndMakeVisible(table_);
    for (auto* b : {&play_, &prev_, &next_, &openSession_}) { styleButton(*b, b == &play_); addAndMakeVisible(*b); }
    play_.onClick = [this] {
        if (e_.isPlayerPlaying()) e_.stopPlayer();
        else e_.playPlayer();
    };
    prev_.onClick = [this] {
        int t = currentTrack();
        double into = e_.playerPosition() - double(r_.disc.tracks[size_t(juce::jmax(0, t))].startSector()) / 75.0;
        playTrack(into > 2.0 ? t : juce::jmax(0, t - 1));
    };
    next_.onClick = [this] { playTrack(juce::jmin(int(r_.disc.tracks.size()) - 1, currentTrack() + 1)); };
    openSession_.onClick = [this] { if (onOpenAsSession) onOpenAsSession(folder_); };
    time_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::plain));
    time_.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(time_);
    status_.setColour(juce::Label::textColourId, colours::dim);
    status_.setJustificationType(juce::Justification::topLeft);
    juce::String st;
    st << (r_.checksumsChecked ? (r_.checksumsOk ? "Checksums: PASS" : "Checksums: FAIL - this DDP is damaged") : "Checksums: none found");
    st << "     UPC/EAN: " << (r_.disc.upcEan.empty() ? juce::String("(none)") : juce::String(r_.disc.upcEan));
    st << "     " << int(r_.disc.tracks.size()) << " tracks, " << msfString(double(r_.disc.leadOutSector) / 75.0);
    for (auto& n : r_.notes)
        if (juce::String(n).startsWith("Error") || juce::String(n).contains("MISMATCH")) st << "\n" << juce::String::fromUTF8(n.c_str());
    status_.setText(st, juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, r_.checksumsChecked && !r_.checksumsOk ? colours::error : colours::dim);
    addAndMakeVisible(status_);
    setSize(860, 520);
    startTimerHz(15);
}

DdpPlayer::~DdpPlayer() {
    closeList(table_);
    e_.stopPlayer();
    e_.clearPlayer();
}

int DdpPlayer::currentTrack() const {
    double t = e_.playerPosition() * 75.0;
    int cur = 0;
    for (size_t i = 0; i < r_.disc.tracks.size(); ++i)
        if (t >= double(r_.disc.tracks[i].startSector()) - 0.5) cur = int(i);
    return cur;
}

void DdpPlayer::playTrack(int t) {
    if (!juce::isPositiveAndBelow(t, int(r_.disc.tracks.size()))) return;
    e_.setPlayerPosition(double(r_.disc.tracks[size_t(t)].startSector()) / 75.0);
    e_.playPlayer();
}

juce::Rectangle<int> DdpPlayer::barArea() const { return getLocalBounds().reduced(10).removeFromTop(118).removeFromBottom(16); }

void DdpPlayer::paint(juce::Graphics& g) {
    g.fillAll(colours::bg);
    auto r = getLocalBounds().reduced(10);
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    juce::String title = juce::String::fromUTF8(r_.disc.text.title.c_str());
    juce::String perf = juce::String::fromUTF8(r_.disc.text.performer.c_str());
    g.drawText(title.isEmpty() ? folder_.getFileName() : title + (perf.isNotEmpty() ? "  -  " + perf : juce::String()), r.removeFromTop(24),
               juce::Justification::left);
    // Programme bar with track boundaries; click to locate.
    auto bar = barArea();
    double total = double(r_.disc.leadOutSector) / 75.0;
    g.setColour(colours::panel2);
    g.fillRoundedRectangle(bar.toFloat(), 3);
    if (total > 0) {
        g.setColour(colours::accent.withAlpha(0.8f));
        g.fillRoundedRectangle(bar.toFloat().withWidth(float(bar.getWidth() * juce::jlimit(0.0, 1.0, e_.playerPosition() / total))), 3);
        g.setColour(colours::bg);
        for (auto& t : r_.disc.tracks) {
            float x = bar.getX() + float(bar.getWidth() * (double(t.startSector()) / 75.0 / total));
            g.drawVerticalLine(int(x), float(bar.getY()), float(bar.getBottom()));
        }
    }
}

void DdpPlayer::mouseDown(const juce::MouseEvent& ev) {
    auto bar = barArea();
    if (bar.contains(ev.getPosition()))
        e_.setPlayerPosition(double(r_.disc.leadOutSector) / 75.0 * double(ev.x - bar.getX()) / bar.getWidth());
}

void DdpPlayer::resized() {
    auto r = getLocalBounds().reduced(10);
    r.removeFromTop(28);
    auto row = r.removeFromTop(34);
    prev_.setBounds(row.removeFromLeft(44).reduced(0, 3));
    row.removeFromLeft(4);
    play_.setBounds(row.removeFromLeft(80).reduced(0, 3));
    row.removeFromLeft(4);
    next_.setBounds(row.removeFromLeft(44).reduced(0, 3));
    row.removeFromLeft(12);
    time_.setBounds(row.removeFromLeft(320));
    openSession_.setBounds(row.removeFromRight(230).reduced(0, 3));
    r.removeFromTop(30);   // programme bar
    status_.setBounds(r.removeFromBottom(40));
    table_.setBounds(r);
}

void DdpPlayer::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) {
    bool current = e_.isPlayerPlaying() && row == currentTrack();
    g.fillAll(selected ? colours::accent.withAlpha(0.25f) : current ? colours::accent.withAlpha(0.12f) : (row % 2 ? colours::panel : colours::panel2));
}

void DdpPlayer::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) {
    auto& t = r_.disc.tracks[size_t(row)];
    int64_t next = size_t(row + 1) < r_.disc.tracks.size() ? r_.disc.tracks[size_t(row + 1)].startSector() : r_.disc.leadOutSector;
    juce::String s;
    switch (col) {
        case cTrk: s = juce::String(row + 1); break;
        case cStart: s = msfString(double(t.startSector()) / 75.0); break;
        case cLen: s = msfString(double(next - t.startSector()) / 75.0); break;
        case cPause: s = t.firstSector() < t.startSector() ? msfString(double(t.startSector() - t.firstSector()) / 75.0) : juce::String("-"); break;
        case cTitle: s = juce::String::fromUTF8(t.text.title.c_str()); break;
        case cPerf: s = juce::String::fromUTF8(t.text.performer.c_str()); break;
        case cIsrc: s = juce::String(t.isrc); break;
        case cFlags: s << (t.preEmphasis ? "PRE " : "") << (t.copyPermitted ? "DCP " : "") << (t.scms ? "SCMS" : ""); break;
        default: break;
    }
    g.setColour(colours::text);
    g.setFont(13.0f);
    g.drawText(s, 4, 0, w - 8, h, juce::Justification::centredLeft);
}

void DdpPlayer::timerCallback() {
    int t = currentTrack();
    double into = e_.playerPosition() - (r_.disc.tracks.empty() ? 0.0 : double(r_.disc.tracks[size_t(t)].startSector()) / 75.0);
    time_.setText((r_.disc.tracks.empty() ? juce::String() : "Track " + juce::String(t + 1) + "  " + (into < 0 ? "-" + msfString(-into) : msfString(into))) +
                      "    " + msfString(e_.playerPosition()),
                  juce::dontSendNotification);
    play_.setButtonText(e_.isPlayerPlaying() ? "Stop" : "Play");
    if (!loaded_) time_.setText("Couldn't open the audio image", juce::dontSendNotification);
    table_.repaint();
    repaint(barArea());
}

}  // namespace lathe
