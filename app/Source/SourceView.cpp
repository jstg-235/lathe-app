#include "SourceView.h"

namespace lathe {

SourceView::SourceView(Session& source, ThumbPool& pool) : s_(source), timeline_(source, pool, Timeline::Role::Source) {
    title_.setText("Source", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, colours::dim);
    addAndMakeVisible(title_);
    struct B { juce::TextButton* b; const char* tip; };
    const B buttons[] = {
        {&open_, "Add audio files to the end of the source timeline"},
        {&clear_, "Remove everything from the source timeline"},
        {&play_, "Play / stop the source from its Edit Point"},
        {&in_, "Mark the source In"},
        {&out_, "Mark the source Out"},
        {&insert_, "Insert the source In-Out at the destination In (or Edit Point); later material moves right"},
        {&replace_, "Replace the destination from its In (or Edit Point) for the source length"},
        {&replaceLeft_, "Replace so the source ends at the destination Out (back-timed)"},
        {&constrained_, "Replace exactly the destination In-Out; the source is cut to fit"},
        {&fit_, "Replace the destination In-Out with the source In-Out; later material moves to fit"},
        {&overlay_, "Lay the source over the destination at its In (overlaps crossfade)"},
        {&append_, "Add the source In-Out at the end of the destination as a new track"},
        {&link_, "Play source and destination together, in step; switch which you hear with Tab"},
        {&zoomIn_, "Zoom in"},
        {&zoomOut_, "Zoom out"},
        {&fitView_, "Show the whole source"},
    };
    for (auto& x : buttons) {
        styleButton(*x.b, false);
        x.b->setTooltip(x.tip);
        addAndMakeVisible(*x.b);
    }
    link_.setClickingTogglesState(true);
    auto call = [](std::function<void()>& f) { if (f) f(); };
    open_.onClick = [this, call] { call(onOpen); };
    clear_.onClick = [this, call] { call(onClear); };
    play_.onClick = [this, call] { call(onPlay); };
    in_.onClick = [this, call] { call(onMarkIn); };
    out_.onClick = [this, call] { call(onMarkOut); };
    insert_.onClick = [this, call] { call(onInsert); };
    replace_.onClick = [this, call] { call(onReplace); };
    replaceLeft_.onClick = [this, call] { call(onReplaceLeft); };
    constrained_.onClick = [this, call] { call(onConstrained); };
    fit_.onClick = [this, call] { call(onFit); };
    overlay_.onClick = [this, call] { call(onOverlay); };
    append_.onClick = [this, call] { call(onAppend); };
    link_.onClick = [this, call] { call(onLinkChanged); };
    zoomIn_.onClick = [this] { timeline_.zoomBy(1.5, timeline_.editPoint()); };
    zoomOut_.onClick = [this] { timeline_.zoomBy(1 / 1.5, timeline_.editPoint()); };
    fitView_.onClick = [this] { timeline_.zoomToFit(); };
    addAndMakeVisible(timeline_);
}

void SourceView::paint(juce::Graphics& g) { g.fillAll(colours::bg); }

void SourceView::resized() {
    auto r = getLocalBounds();
    auto bar = r.removeFromTop(26);
    title_.setBounds(bar.removeFromLeft(56));
    auto place = [&](juce::TextButton& b, int w) { b.setBounds(bar.removeFromLeft(w).reduced(0, 2)); bar.removeFromLeft(3); };
    place(open_, 62);
    place(clear_, 48);
    bar.removeFromLeft(6);
    place(play_, 50);
    place(link_, 44);
    bar.removeFromLeft(6);
    place(in_, 34);
    place(out_, 38);
    bar.removeFromLeft(6);
    place(insert_, 54);
    place(replace_, 62);
    place(replaceLeft_, 86);
    place(constrained_, 86);
    place(fit_, 70);
    place(overlay_, 60);
    place(append_, 60);
    for (auto* b : {&fitView_, &zoomOut_, &zoomIn_}) { b->setBounds(bar.removeFromRight(b == &fitView_ ? 36 : 26).reduced(0, 2)); bar.removeFromRight(3); }
    r.removeFromTop(2);
    timeline_.setBounds(r);
}

}  // namespace lathe
