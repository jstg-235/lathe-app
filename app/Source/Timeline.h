// A timeline: ruler, PQ marker strip (plus SRPs), and lanes of regions. Used twice: the
// destination (the master) and the source (material edits are taken from), each with its own
// zoom, In / Out points and Edit Point.
// Mouse: click to set the Edit Point, drag empty space to set In / Out. Drag a region to lay it
// over what is there; Shift-drag inserts it (later material moves right), Command-drag replaces
// what it covers, Option-drag keeps it on its lane. Drag a track on the track bar to reorder.
// Also: drag a region
// to move it (snaps to edges and the playhead), drag its edges to trim, drag the corner
// handles to fade, double-click an overlap to open the crossfade editor. With envelope
// mode on, click the gain line to add a point, drag points, Alt-click to remove.
#pragma once

#include "Session.h"
#include "../../core/dsp/Analyzer.h"
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
    enum class Role { Destination, Source };
    Timeline(Session& s, ThumbPool& pool, Role role = Role::Destination);
    Role role() const { return role_; }
    ~Timeline() override;

    std::function<void()> onEdit;                      // session changed (commit: undo + reload)
    std::function<void(double)> onSeek;                // user located the playhead
    std::function<void(int out, int in)> onCrossfade;  // open the crossfade editor
    std::function<void(int markerId)> onMarkerSelected;
    std::function<void(int markerId)> onMarkerDoubleClicked;
    std::function<void(int regionId)> onRegionFx;      // open the region's effects window
    std::function<void(int regionId)> onRegionRestore, onRegionSpectral;
    // Product strip (units of one product drawn under the markers); double-click a break to edit it.
    std::function<void(int productId, int breakIndex)> onBreakEdit;
    void setProductView(int productId) { productView_ = productId; resized(); repaint(); }
    int productView() const;   // the product shown (first one if none chosen), 0 = none

    void setPlayhead(double sec, bool follow);
    double playhead() const { return playhead_; }
    // In / Out points (-1 = not set). selection() is In-Out when both are set, else empty.
    juce::Range<double> selection() const { return in_ >= 0 && out_ > in_ ? juce::Range<double>(in_, out_) : juce::Range<double>(); }
    void setSelection(juce::Range<double> r) {
        if (r.isEmpty()) in_ = out_ = -1; else { in_ = r.getStart(); out_ = r.getEnd(); }
        repaint();
    }
    double inPoint() const { return in_; }
    double outPoint() const { return out_; }
    void setInPoint(double t) { in_ = juce::jmax(0.0, t); if (out_ >= 0 && out_ <= in_) out_ = -1; repaint(); }
    void setOutPoint(double t) { out_ = juce::jmax(0.0, t); if (in_ >= out_) in_ = -1; repaint(); }
    void clearInOut() { in_ = out_ = -1; repaint(); }
    // The Edit Point: where edits and pastes land and where Play starts. Separate from the
    // playhead, which shows what is being heard.
    double editPoint() const { return edit_; }
    void setEditPoint(double t, bool reveal = false);
    void reveal(double t);   // scroll so t is visible
    // The timeline keys and edits act on.
    void setActive(bool a) { if (active_ != a) { active_ = a; repaint(); } }
    bool isActive() const { return active_; }
    std::function<void()> onFocus;               // clicked: make this the active timeline
    std::function<void()> onEditPointMoved;
    int selectedSrp() const { return selSrp_; }
    void selectSrp(int id) { selSrp_ = id; repaint(); }
    std::function<void(int srpId)> onSrpEdit;    // double-click / Edit... : rename an SRP
    std::function<void(int regionId)> onSegmentGain;
    int selectedRegion() const { return selRegion_; }
    void selectRegion(int id) { selRegion_ = id; repaint(); }
    int selectedLane() const { return selLane_; }
    int selectedMarker() const { return selMarker_; }
    void selectMarker(int id) { selMarker_ = id; repaint(); }
    void setEnvelopeMode(bool on) { envelopeMode_ = on; if (on) gainMode_ = false; repaint(); }
    // Timeline gain overlay: click to add a node, drag nodes (1 dB steps, 0.1 dB with Option),
    // Option-click to delete, Shift-click to select several, Control-click for lock / group.
    void setGainOverlay(bool on) { gainMode_ = on; if (on) envelopeMode_ = false; selGain_.clear(); repaint(); }
    bool gainOverlay() const { return gainMode_; }
    bool envelopeMode() const { return envelopeMode_; }
    void setSnap(bool on) { snap_ = on; }

    // Loudness overlay from the last analysis: processed (solid) and unprocessed (dim).
    void setAnalysis(std::shared_ptr<const dsp::AnalysisResult> processed, std::shared_ptr<const dsp::AnalysisResult> raw);
    void setAnalysisStale(bool stale) { stale_ = stale; repaint(); }
    void setShowLoudness(bool b) { showLoudness_ = b; repaint(); }
    bool showLoudness() const { return showLoudness_; }
    std::function<void(double)> onProblemClicked;

    void zoomToFit();
    void zoomToRange(juce::Range<double> r);
    void scrollBy(double fractionOfWidth);
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
    static constexpr int kRuler = 22, kMarkerStrip = 26, kProductStrip = 22, kTrackBar = 14, kScroll = 12;
    int trackBarH() const { return role_ == Role::Destination ? kTrackBar : 0; }
    int trackBarY() const { return kRuler + kMarkerStrip + productH(); }
    void paintTrackBar(juce::Graphics&);
    int trackAtX(float x) const;   // track index under x on the track bar, -1 = none
    int productH() const { return role_ == Role::Destination && productView() ? kProductStrip : 0; }
    int srpAt(juce::Point<int> p) const;
    int gainNodeAt(juce::Point<int> p) const;
    float gainY(double db) const;
    double gainDbAtY(float y) const;
    void paintGain(juce::Graphics&);
    bool gainMouseDown(const juce::MouseEvent& e);
    juce::Rectangle<int> badgeBounds(int lane) const;
    void paintSrps(juce::Graphics&);
    void paintPoints(juce::Graphics&);
    int top() const { return trackBarY() + trackBarH(); }   // first lane's y
    void paintProducts(juce::Graphics&);
    int breakAt(juce::Point<int> p) const;   // break index in the shown product, -1 = none
    enum class Drag { None, Locate, Select, Move, TrimStart, TrimEnd, FadeIn, FadeOut, Marker, EnvPoint, SrpMove, TrackBar, GainNode };

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
    void paintLoudness(juce::Graphics&);
    int problemAt(juce::Point<int> p) const;   // index into processed problems, -1 = none

    Session& s_;
    ThumbPool& pool_;
    const Role role_;
    double in_ = -1, out_ = -1, edit_ = 0;
    bool active_ = false;
    int selSrp_ = 0;
    juce::ScrollBar hscroll_{false};
    double pps_ = 20.0, view0_ = 0.0;
    double playhead_ = 0;
    int selRegion_ = 0, selLane_ = 0, selMarker_ = 0;
    bool envelopeMode_ = false, snap_ = true;

    Drag drag_ = Drag::None;
    int dragId_ = 0, dragPoint_ = -1;
    double dragT0_ = 0, dragOrigStart_ = 0, dragOrigEnd_ = 0;
    int dragOrigLane_ = 0;
    Session::DropMode dropMode_ = Session::DropMode::Overlay;
    bool laneLock_ = false;
    bool gainMode_ = false;
    juce::Array<int> selGain_;                          // selected gain nodes
    juce::Array<std::pair<int, std::pair<double, double>>> gainDrag_;   // node id -> original (t, db)
    float dragY0_ = 0;
    int trackFrom_ = -1, trackTo_ = -1;
    bool changed_ = false;
    std::shared_ptr<const dsp::AnalysisResult> an_, raw_;
    bool stale_ = false, showLoudness_ = true;
    int productView_ = 0;
};

}  // namespace lathe
