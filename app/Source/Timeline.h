// The destination timeline: ruler, PQ marker strip, and lanes of regions.
// Mouse: click to locate, drag empty space to select a destination range, drag a region
// to move it (snaps to edges and the playhead), drag its edges to trim, drag the corner
// handles to fade, double-click an overlap to open the crossfade editor. With envelope
// mode on, click the gain line to add a point, drag points, Alt-click to remove.
#pragma once

#include "Session.h"
#include "Theme.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace lathe {

// Shared waveform overviews, one per source file.
class ThumbPool {
public:
    ThumbPool(juce::AudioFormatManager& fm) : fm_(fm) {}
    juce::AudioThumbnail* get(const juce::File& f, juce::ChangeListener* l);
private:
    juce::AudioFormatManager& fm_;
    juce::AudioThumbnailCache cache_{128};
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbs_;
};

class Timeline : public juce::Component, public juce::ChangeListener, private juce::ScrollBar::Listener {
public:
    Timeline(Session& s, ThumbPool& pool);
    ~Timeline() override;

    std::function<void()> onEdit;                      // session changed (commit: undo + reload)
    std::function<void(double)> onSeek;                // user located the playhead
    std::function<void(int out, int in)> onCrossfade;  // open the crossfade editor
    std::function<void(int markerId)> onMarkerSelected;
    std::function<void(int markerId)> onMarkerDoubleClicked;

    void setPlayhead(double sec, bool follow);
    double playhead() const { return playhead_; }
    juce::Range<double> selection() const { return sel_; }
    void setSelection(juce::Range<double> r) { sel_ = r; repaint(); }
    int selectedRegion() const { return selRegion_; }
    void selectRegion(int id) { selRegion_ = id; repaint(); }
    int selectedLane() const { return selLane_; }
    int selectedMarker() const { return selMarker_; }
    void selectMarker(int id) { selMarker_ = id; repaint(); }
    void setEnvelopeMode(bool on) { envelopeMode_ = on; repaint(); }
    bool envelopeMode() const { return envelopeMode_; }
    void setSnap(bool on) { snap_ = on; }

    void zoomToFit();
    void zoomBy(double factor, double aroundSec);
    void sessionUpdated();   // after external changes (undo, load)

    // Overlap (crossfade) nearest the playhead on the selected lane: {out, in} or {0,0}.
    std::pair<int, int> crossfadeNear(double t) const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scale) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

private:
    static constexpr int kRuler = 22, kMarkers = 26, kScroll = 12;
    enum class Drag { None, Locate, Select, Move, TrimStart, TrimEnd, FadeIn, FadeOut, Marker, EnvPoint };

    double xToTime(float x) const { return view0_ + double(x) / pps_; }
    float timeToX(double t) const { return float((t - view0_) * pps_); }
    int laneHeight() const;
    juce::Rectangle<int> laneBounds(int lane) const;
    int laneAt(int y) const;
    int regionAt(juce::Point<int> p) const;   // region id or 0
    int markerAt(juce::Point<int> p) const;   // marker id or 0
    int envPointAt(const Region& r, juce::Point<int> p) const;
    float envY(const Region& r, double db) const;
    double snapTime(double t, int ignoreRegion) const;
    void updateScrollbar();
    void scrollBarMoved(juce::ScrollBar*, double newStart) override;
    void paintRegion(juce::Graphics&, int index);
    void paintMarkers(juce::Graphics&);
    void paintRuler(juce::Graphics&);

    Session& s_;
    ThumbPool& pool_;
    juce::ScrollBar hscroll_{false};
    double pps_ = 20.0, view0_ = 0.0;
    double playhead_ = 0;
    juce::Range<double> sel_;
    int selRegion_ = 0, selLane_ = 0, selMarker_ = 0;
    bool envelopeMode_ = false, snap_ = true;

    Drag drag_ = Drag::None;
    int dragId_ = 0, dragPoint_ = -1;
    double dragT0_ = 0, dragOrigStart_ = 0, dragOrigEnd_ = 0;
    int dragOrigLane_ = 0;
    bool changed_ = false;
};

}  // namespace lathe
