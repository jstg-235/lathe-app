#include "DeliveryPanels.h"

namespace lathe {

namespace {

// A table cell edited in place; commits on Return or focus loss.
class EditCell : public juce::Label {
public:
    std::function<juce::String()> get;
    std::function<void(const juce::String&)> set;
    EditCell() {
        setEditable(false, true, false);
        setColour(juce::Label::textColourId, colours::text);
        setColour(juce::Label::textWhenEditingColourId, colours::text);
        setColour(juce::Label::backgroundWhenEditingColourId, colours::bg);
        setColour(juce::Label::outlineWhenEditingColourId, colours::accent);
        setFont(juce::FontOptions(13.0f));
    }
    void refreshText() { setText(get ? get() : juce::String(), juce::dontSendNotification); }
    void textWasEdited() override { if (set) set(getText()); }
};

void styleLabel(juce::Label& l, const juce::String& text) {
    l.setText(text, juce::dontSendNotification);
    l.setColour(juce::Label::textColourId, colours::dim);
    l.setJustificationType(juce::Justification::centredRight);
}

void styleEditor(juce::TextEditor& e) {
    e.setColour(juce::TextEditor::backgroundColourId, colours::bg);
    e.setColour(juce::TextEditor::textColourId, colours::text);
    e.setColour(juce::TextEditor::outlineColourId, colours::line);
    e.setColour(juce::TextEditor::focusedOutlineColourId, colours::accent);
}

void styleTable(juce::TableListBox& t) {
    t.setColour(juce::ListBox::backgroundColourId, colours::bg);
    t.getHeader().setColour(juce::TableHeaderComponent::backgroundColourId, colours::panel2);
    t.getHeader().setColour(juce::TableHeaderComponent::textColourId, colours::text);
    t.setRowHeight(22);
}

juce::String mmss(double sec) {
    if (sec < 0) return "-" + mmss(-sec);
    int t = int(std::floor(sec + 1e-9));
    double frac = sec - t;
    juce::String s = juce::String(t / 60) + ":" + juce::String(t % 60).paddedLeft('0', 2);
    int tenths = int(std::floor(frac * 10 + 1e-6));
    return s + "." + juce::String(tenths);
}

juce::String js(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

}  // namespace

// ================================================================== Release info

ReleaseInfoPanel::ReleaseInfoPanel(Session& s) : s_(s) {
    auto str = [](juce::String Session::*m, Session& ss) {
        return std::make_pair(std::function<juce::String()>([&ss, m] { return ss.*m; }),
                              std::function<void(const juce::String&)>([&ss, m](const juce::String& v) { ss.*m = v; }));
    };
    auto add = [&](const juce::String& name, juce::String Session::*m, bool multi = false) {
        auto p = str(m, s_);
        addField(name, p.first, p.second, multi);
    };
    add("Artist", &Session::discPerformer);
    add("Album title", &Session::discTitle);
    add("Label", &Session::label);
    add("Catalogue no.", &Session::catalogue);
    add("DDP master ID", &Session::masterId);
    add("UPC / EAN", &Session::upcEan);
    add("Release date", &Session::releaseDate);
    add("(P) line", &Session::pLine);
    add("(C) line", &Session::cLine);
    add("Client", &Session::client);
    add("Mastered by", &Session::engineer);
    add("Genre (text)", &Session::genreText);
    add("CD-Text disc ID", &Session::cdTextDiscId);
    add("Notes", &Session::notes, true);

    styleLabel(genreLbl_, "CD-Text genre");
    addAndMakeVisible(genreLbl_);
    genre_.addItem("(none)", 1);
    for (int g = 2; g < cd::kGenreCount; ++g) genre_.addItem(js(cd::genreName(g)), g + 1);
    genre_.onChange = [this] {
        if (updating_) return;
        s_.cdTextGenre = juce::jmax(0, genre_.getSelectedId() - 1);
        if (s_.cdTextGenre == 0 && genre_.getSelectedId() == 1) s_.cdTextGenre = 0;
        if (onChange) onChange();
    };
    addAndMakeVisible(genre_);

    styleLabel(offsetLbl_, "PQ offset");
    addAndMakeVisible(offsetLbl_);
    offset_.setSliderStyle(juce::Slider::IncDecButtons);
    offset_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 60, 22);
    offset_.setRange(0, 75, 1);
    offset_.setTextValueSuffix(" frames");
    offset_.onValueChange = [this] {
        if (updating_) return;
        s_.pqOffsetFrames = int(offset_.getValue());
        if (onChange) onChange();
    };
    addAndMakeVisible(offset_);
    htoa_.setColour(juce::ToggleButton::textColourId, colours::text);
    htoa_.onClick = [this] {
        if (updating_) return;
        s_.htoa = htoa_.getToggleState();
        if (onChange) onChange();
    };
    addAndMakeVisible(htoa_);
    help_.setText("The PQ offset moves every track start (index 01) earlier on delivery so slow players don't clip the first note; "
                  "5 to 10 frames (67 to 133 ms) is typical. The DDP master ID defaults to the catalogue number. "
                  "Each product can override the catalogue number and UPC in Products & Splitting.",
                  juce::dontSendNotification);
    help_.setColour(juce::Label::textColourId, colours::dim);
    help_.setFont(juce::FontOptions(12.0f));
    help_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(help_);
    refresh();
    setSize(720, 620);
}

void ReleaseInfoPanel::addField(const juce::String& name, std::function<juce::String()> get, std::function<void(const juce::String&)> set, bool multi) {
    auto* f = fields_.add(new Field());
    f->get = std::move(get);
    f->set = std::move(set);
    styleLabel(f->label, name);
    styleEditor(f->editor);
    if (multi) { f->editor.setMultiLine(true, true); f->editor.setReturnKeyStartsNewLine(true); }
    f->editor.onTextChange = [this, f] { if (!updating_) f->set(f->editor.getText()); };
    auto commit = [this] { if (!updating_ && onChange) onChange(); };
    f->editor.onFocusLost = commit;
    if (!multi) f->editor.onReturnKey = commit;
    addAndMakeVisible(f->label);
    addAndMakeVisible(f->editor);
}

void ReleaseInfoPanel::refresh() {
    updating_ = true;
    for (auto* f : fields_)
        if (!f->editor.hasKeyboardFocus(true)) f->editor.setText(f->get(), false);
    genre_.setSelectedId(s_.cdTextGenre >= 2 ? s_.cdTextGenre + 1 : 1, juce::dontSendNotification);
    offset_.setValue(s_.pqOffsetFrames, juce::dontSendNotification);
    htoa_.setToggleState(s_.htoa, juce::dontSendNotification);
    updating_ = false;
}

void ReleaseInfoPanel::resized() {
    auto r = getLocalBounds().reduced(14);
    constexpr int lw = 120, h = 26;
    auto row = [&](juce::Label& l, juce::Component& c, int height = 26) {
        auto a = r.removeFromTop(height);
        l.setBounds(a.removeFromLeft(lw));
        a.removeFromLeft(8);
        c.setBounds(a);
        r.removeFromTop(4);
    };
    for (auto* f : fields_) {
        if (f->editor.isMultiLine()) continue;
        row(f->label, f->editor);
    }
    row(genreLbl_, genre_);
    auto a = r.removeFromTop(h);
    offsetLbl_.setBounds(a.removeFromLeft(lw));
    a.removeFromLeft(8);
    offset_.setBounds(a.removeFromLeft(150));
    a.removeFromLeft(12);
    htoa_.setBounds(a);
    r.removeFromTop(4);
    for (auto* f : fields_)
        if (f->editor.isMultiLine()) row(f->label, f->editor, 60);
    help_.setBounds(r);
}

// ================================================================== Track credits

TrackCreditsPanel::TrackCreditsPanel(Session& s) : s_(s) {
    styleTable(table_);
    auto& h = table_.getHeader();
    h.addColumn("#", 1, 32);
    h.addColumn("Title", 2, 170);
    h.addColumn("Artist", 3, 120);
    h.addColumn("Songwriter", 4, 120);
    h.addColumn("Composer", 5, 120);
    h.addColumn("Arranger", 6, 100);
    h.addColumn("Publisher", 7, 120);
    h.addColumn("PRO", 8, 60);
    h.addColumn("CD-Text message", 9, 140);
    h.addColumn("Explicit", 10, 60);
    addAndMakeVisible(table_);
    setSize(1100, 420);
}

const Marker* TrackCreditsPanel::trackMarker(int row) const {
    int n = 0;
    for (auto& m : s_.markers)
        if (m.type == Marker::TrackStart && n++ == row) return &m;
    return nullptr;
}

Marker* TrackCreditsPanel::trackMarker(int row) { return const_cast<Marker*>(static_cast<const TrackCreditsPanel*>(this)->trackMarker(row)); }

int TrackCreditsPanel::getNumRows() {
    int n = 0;
    for (auto& m : s_.markers) n += m.type == Marker::TrackStart ? 1 : 0;
    return n;
}

void TrackCreditsPanel::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) {
    g.fillAll(selected ? colours::selection : (row % 2 ? colours::panel : colours::bg));
}

juce::String TrackCreditsPanel::text(int row, int col) const {
    auto* m = trackMarker(row);
    if (!m) return {};
    switch (col) {
        case 1: return juce::String(row + 1);
        case 2: return js(m->text.title);
        case 3: return js(m->text.performer);
        case 4: return js(m->text.songwriter);
        case 5: return js(m->text.composer);
        case 6: return js(m->text.arranger);
        case 7: return js(m->publisher);
        case 8: return js(m->pro);
        case 9: return js(m->text.message);
        case 10: return m->explicitContent ? "Yes" : "";
    }
    return {};
}

void TrackCreditsPanel::setText(int row, int col, const juce::String& t) {
    auto* m = trackMarker(row);
    if (!m) return;
    auto v = t.trim().toStdString();
    switch (col) {
        case 2: m->text.title = v; break;
        case 3: m->text.performer = v; break;
        case 4: m->text.songwriter = v; break;
        case 5: m->text.composer = v; break;
        case 6: m->text.arranger = v; break;
        case 7: m->publisher = v; break;
        case 8: m->pro = v; break;
        case 9: m->text.message = v; break;
        default: return;
    }
    if (onChange) onChange();
}

void TrackCreditsPanel::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) {
    if (col != 1 && col != 10) return;
    g.setColour(col == 10 ? colours::accent : colours::dim);
    g.setFont(13.0f);
    g.drawText(text(row, col), 4, 0, w - 8, h, col == 1 ? juce::Justification::centredRight : juce::Justification::centred);
}

juce::Component* TrackCreditsPanel::refreshComponentForCell(int row, int col, bool, juce::Component* existing) {
    if (col == 1 || col == 10) { delete existing; return nullptr; }
    auto* c = dynamic_cast<EditCell*>(existing);
    if (!c) { delete existing; c = new EditCell(); }
    c->get = [this, row, col] { return text(row, col); };
    c->set = [this, row, col](const juce::String& t) { setText(row, col, t); };
    c->refreshText();
    return c;
}

void TrackCreditsPanel::cellClicked(int row, int col, const juce::MouseEvent&) {
    if (col != 10) return;
    if (auto* m = trackMarker(row)) {
        m->explicitContent = !m->explicitContent;
        table_.repaintRow(row);
        if (onChange) onChange();
    }
}

// ================================================================== Studio profile

StudioPanel::StudioPanel(StudioProfile& p) : p_(p) {
    auto setup = [this](juce::Label& l, const juce::String& t, juce::TextEditor& e, juce::String StudioProfile::*m, bool multi = false) {
        styleLabel(l, t);
        styleEditor(e);
        if (multi) { e.setMultiLine(true, false); e.setReturnKeyStartsNewLine(true); }
        e.setText(p_.*m, false);
        e.onTextChange = [this, &e, m] { p_.*m = e.getText(); };
        e.onFocusLost = [this] { if (onChange) onChange(); };
        addAndMakeVisible(l);
        addAndMakeVisible(e);
    };
    setup(nameL_, "Studio name", name_, &StudioProfile::name);
    setup(addrL_, "Address", addr_, &StudioProfile::address, true);
    setup(phoneL_, "Phone", phone_, &StudioProfile::phone);
    setup(emailL_, "Email", email_, &StudioProfile::email);
    setup(webL_, "Website", web_, &StudioProfile::website);
    styleLabel(logoL_, "Logo");
    addAndMakeVisible(logoL_);
    for (auto* b : {&chooseLogo_, &clearLogo_}) { styleButton(*b); addAndMakeVisible(*b); }
    chooseLogo_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Choose the studio logo", juce::File(), "*.png;*.jpg;*.jpeg;*.gif");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (!f.existsAsFile()) return;
            // Keep a copy in the app's support folder so moving the original doesn't break the paperwork.
            auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Lathe");
            dir.createDirectory();
            auto copy = dir.getChildFile("Studio Logo" + f.getFileExtension().toLowerCase());
            if (f != copy) { copy.deleteFile(); f.copyFileTo(copy); }
            p_.logo = copy.existsAsFile() ? copy : f;
            loadPreview();
            if (onChange) onChange();
        });
    };
    clearLogo_.onClick = [this] { p_.logo = juce::File(); loadPreview(); if (onChange) onChange(); };
    a4_.setColour(juce::ToggleButton::textColourId, colours::text);
    a4_.setToggleState(p_.a4, juce::dontSendNotification);
    a4_.onClick = [this] { p_.a4 = a4_.getToggleState(); if (onChange) onChange(); };
    addAndMakeVisible(a4_);
    loadPreview();
    setSize(600, 440);
}

void StudioPanel::loadPreview() {
    preview_ = p_.logo.existsAsFile() ? juce::ImageFileFormat::loadFrom(p_.logo) : juce::Image();
    repaint();
}

void StudioPanel::resized() {
    auto r = getLocalBounds().reduced(14);
    const int lw = 100;
    auto row = [&](juce::Label& l, juce::Component& c, int h) {
        auto a = r.removeFromTop(h);
        l.setBounds(a.removeFromLeft(lw).withHeight(26));
        a.removeFromLeft(8);
        c.setBounds(a);
        r.removeFromTop(6);
    };
    row(nameL_, name_, 26);
    row(addrL_, addr_, 64);
    row(phoneL_, phone_, 26);
    row(emailL_, email_, 26);
    row(webL_, web_, 26);
    auto a = r.removeFromTop(26);
    logoL_.setBounds(a.removeFromLeft(lw));
    a.removeFromLeft(8);
    chooseLogo_.setBounds(a.removeFromLeft(120));
    a.removeFromLeft(6);
    clearLogo_.setBounds(a.removeFromLeft(60));
    r.removeFromTop(6);
    a4_.setBounds(r.removeFromBottom(26).withTrimmedLeft(lw + 8));
}

void StudioPanel::paint(juce::Graphics& g) {
    g.fillAll(colours::panel);
    auto area = getLocalBounds().reduced(14).withTrimmedTop(26 * 5 + 64 - 26 + 6 * 6 + 32).withTrimmedBottom(32).withTrimmedLeft(108);
    g.setColour(juce::Colours::white);
    g.fillRect(area);
    if (preview_.isValid()) g.drawImageWithin(preview_, area.getX() + 6, area.getY() + 6, area.getWidth() - 12, area.getHeight() - 12, juce::RectanglePlacement::centred);
    else { g.setColour(juce::Colours::grey); g.drawText("No logo: the paperwork shows the studio name only", area, juce::Justification::centred); }
}

// ================================================================== Products

ProductsPanel::ProductsPanel(Session& s, std::function<const dsp::LoudnessMap*()> loudness) : s_(s), loudness_(std::move(loudness)) {
    list_.setColour(juce::ListBox::backgroundColourId, colours::bg);
    list_.setRowHeight(36);
    addAndMakeVisible(list_);
    for (auto* b : {&add_, &remove_, &split_, &addUnit_, &removeUnit_, &editBreak_}) {
        styleButton(*b, b == &split_);
        addAndMakeVisible(*b);
    }
    add_.onClick = [this] {
        juce::PopupMenu m;
        for (int k = 0; k < int(ProductKind::kCount); ++k) m.addItem(k + 1, productSpec(ProductKind(k)).name);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_), [this](int r) { if (r > 0) addProduct(ProductKind(r - 1)); });
    };
    remove_.onClick = [this] {
        for (int i = 0; i < s_.products.size(); ++i)
            if (s_.products[i].id == productId_) { s_.products.remove(i); break; }
        productId_ = s_.products.isEmpty() ? 0 : s_.products.getFirst().id;
        commit();
    };
    split_.onClick = [this] {
        if (auto* p = product()) {
            SplitOptions o;
            o.overlap = overlap_.getValue();
            o.atTrackMarkers = p->splitMode == 1;
            auto msg = autoSplit(s_, *p, loudness_ ? loudness_() : nullptr, o);
            summary_.setText(msg, juce::dontSendNotification);
            commit();
        }
    };
    addUnit_.onClick = [this] {
        if (auto* p = product()) {
            Unit u;
            u.start = p->units.isEmpty() ? 0.0 : p->units.getLast().end;
            u.end = s_.end();
            if (u.end - u.start < 1.0) u.start = juce::jmax(0.0, u.end - p->limit());
            p->units.add(u);
            commit();
        }
    };
    removeUnit_.onClick = [this] {
        if (auto* p = product()) {
            int r = units_.getSelectedRow();
            if (r >= 0 && r < p->units.size()) { p->units.remove(r); commit(); }
        }
    };
    editBreak_.onClick = [this] {
        if (auto* p = product()) {
            int r = juce::jmax(0, units_.getSelectedRow());
            if (r + 1 >= p->units.size()) r = p->units.size() - 2;
            if (r >= 0 && onEditBreak) onEditBreak(p->id, r);
        }
    };

    auto field = [this](juce::Label& l, const juce::String& t, juce::TextEditor& e, std::function<void(Product&, const juce::String&)> set) {
        styleLabel(l, t);
        styleEditor(e);
        addAndMakeVisible(l);
        addAndMakeVisible(e);
        auto apply = [this, &e, set] {
            if (updating_) return;
            if (auto* p = product()) { set(*p, e.getText()); commit(); }
        };
        e.onReturnKey = apply;
        e.onFocusLost = apply;
    };
    field(nameL_, "Name", name_, [](Product& p, const juce::String& t) { p.name = t.trim(); });
    field(catL_, "Catalogue", cat_, [](Product& p, const juce::String& t) { p.catalogue = t.trim(); });
    field(upcL_, "UPC", upc_, [](Product& p, const juce::String& t) { p.upc = t.trim(); });
    field(limitL_, "Max per unit", limit_, [](Product& p, const juce::String& t) {
        double v = parseTimeString(t, p.limit());
        p.maxUnit = std::abs(v - productSpec(p.kind).target) < 0.5 ? 0.0 : juce::jlimit(30.0, productSpec(p.kind).hardMax, v);
    });
    styleLabel(gapL_, "Flip gap");
    styleLabel(overlapL_, "Split overlap");
    for (auto* l : {&gapL_, &overlapL_}) addAndMakeVisible(*l);
    gap_.setSliderStyle(juce::Slider::LinearHorizontal);
    gap_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    gap_.setRange(0.0, 10.0, 0.1);
    gap_.textFromValueFunction = [](double v) { return v <= 0.0 ? juce::String("No gap") : juce::String(v, 1) + " s"; };
    gap_.onValueChange = [this] { if (!updating_) if (auto* p = product()) { p->flipGap = gap_.getValue(); } };
    gap_.onDragEnd = [this] { if (!updating_) commit(); };
    overlap_.setSliderStyle(juce::Slider::LinearHorizontal);
    overlap_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    overlap_.setRange(0.0, 30.0, 0.5);
    overlap_.setValue(5.0, juce::dontSendNotification);
    overlap_.setTextValueSuffix(" s");
    for (auto* sl : {&gap_, &overlap_}) addAndMakeVisible(*sl);
    mode_.addItem("Balanced", 1);
    mode_.addItem("Fill to track markers", 2);
    mode_.setTooltip("Balanced: units as even as possible, breaking inside music only when it must.\n"
                     "Fill to track markers: as many whole tracks per unit as fit, always breaking at a track marker.");
    mode_.onChange = [this] {
        if (updating_) return;
        if (auto* p = product()) { p->splitMode = mode_.getSelectedId() == 2 ? 1 : 0; commit(); }
    };
    addAndMakeVisible(mode_);
    autoNew_.setColour(juce::ToggleButton::textColourId, colours::text);
    autoNew_.setTooltip("On: a new product is split as soon as it's added. Off: it starts as one unit for the whole programme, "
                        "for you to split here or on the timeline (right-click the product strip).");
    autoNew_.setToggleState(s_.autoSplitNew, juce::dontSendNotification);
    autoNew_.onClick = [this] { s_.autoSplitNew = autoNew_.getToggleState(); commit(); };
    addAndMakeVisible(autoNew_);
    even_.setColour(juce::ToggleButton::textColourId, colours::text);
    even_.onClick = [this] { if (!updating_) if (auto* p = product()) { p->evenUnits = even_.getToggleState(); commit(); } };
    addAndMakeVisible(even_);

    styleTable(units_);
    auto& h = units_.getHeader();
    h.addColumn("Unit", 1, 130);
    h.addColumn("Start", 2, 80);
    h.addColumn("End", 3, 80);
    h.addColumn("Length", 4, 70);
    h.addColumn("Fade In", 5, 64);
    h.addColumn("Fade Out", 6, 64);
    h.addColumn("Tracks", 7, 140);
    h.addColumn("Fit", 8, 120);
    addAndMakeVisible(units_);
    summary_.setColour(juce::Label::textColourId, colours::dim);
    addAndMakeVisible(summary_);
    issues_.setMultiLine(true);
    issues_.setReadOnly(true);
    styleEditor(issues_);
    addAndMakeVisible(issues_);
    productId_ = s_.products.isEmpty() ? 0 : s_.products.getFirst().id;
    refresh();
    setSize(1040, 560);
}

Product* ProductsPanel::product() { return s_.findProduct(productId_); }

void ProductsPanel::addProduct(ProductKind k) {
    Product p;
    p.id = s_.nextId++;
    p.kind = k;
    static const char* names[] = {"CD", "LP", "12in 45", "10in", "7in", "Cassette"};
    p.name = names[int(k)];
    int same = 0;
    for (auto& x : s_.products) same += x.kind == k ? 1 : 0;
    if (same) p.name << " " << (same + 1);
    p.splitMode = mode_.getSelectedId() == 2 ? 1 : 0;   // the mode last shown
    juce::String msg;
    if (s_.autoSplitNew) {
        SplitOptions o;
        o.overlap = overlap_.getValue();
        o.atTrackMarkers = p.splitMode == 1;
        msg = autoSplit(s_, p, loudness_ ? loudness_() : nullptr, o);
    } else {
        singleUnit(s_, p);
        msg = p.name + ": one unit for the whole programme. Split it with Auto-Split, Add Unit, or on the timeline "
                       "(drag the unit edges in the product strip; right-click it to split or join).";
    }
    s_.products.add(p);
    productId_ = p.id;
    summary_.setText(msg, juce::dontSendNotification);
    commit();
}

void ProductsPanel::commit() {
    if (onChange) onChange();
    refresh();
}

void ProductsPanel::selectProduct(int id) {
    productId_ = id;
    refresh();
}

void ProductsPanel::refresh() {
    if (!product()) productId_ = s_.products.isEmpty() ? 0 : s_.products.getFirst().id;
    list_.updateContent();
    for (int i = 0; i < s_.products.size(); ++i)
        if (s_.products[i].id == productId_) list_.selectRow(i, true, true);
    list_.repaint();
    units_.updateContent();
    units_.repaint();
    updateFields();
}

void ProductsPanel::updateFields() {
    updating_ = true;
    autoNew_.setToggleState(s_.autoSplitNew, juce::dontSendNotification);
    if (mode_.getSelectedId() == 0) mode_.setSelectedId(1, juce::dontSendNotification);
    auto* p = product();
    for (juce::Component* c : std::initializer_list<juce::Component*>{&name_, &cat_, &upc_, &limit_, &gap_, &even_, &split_, &mode_, &addUnit_, &removeUnit_, &editBreak_, &remove_})
        c->setEnabled(p != nullptr);
    if (p) {
        name_.setText(p->name, false);
        cat_.setText(p->catalogue, false);
        upc_.setText(p->upc, false);
        limit_.setText(mmss(p->limit()), false);
        gap_.setValue(p->flipGap, juce::dontSendNotification);
        even_.setToggleState(p->evenUnits, juce::dontSendNotification);
        mode_.setSelectedId(p->splitMode == 1 ? 2 : 1, juce::dontSendNotification);
        even_.setVisible(productSpec(p->kind).sides);
        juce::String issues;
        for (auto& i : validateProduct(s_, *p)) issues << (i.error ? "ERROR: " : "Warning: ") << i.message << "\n";
        if (productSpec(p->kind).cdFormat)
            for (int u = 0; u < p->units.size(); ++u) {
                double lead = 0;
                juce::StringArray notes;
                auto d = s_.toDiscForUnit(p->units[u], u + 1, p->units.size(), lead, &notes);
                for (auto& is : cd::validate(d))
                    if (is.level == cd::Issue::Error) issues << "ERROR: " << unitLabel(*p, u) << ": " << js(is.message) << "\n";
                for (auto& n : notes) issues << n << "\n";
            }
        issues_.setText(issues.isEmpty() ? juce::String("No problems found.") : issues.trimEnd(), false);
    } else {
        issues_.setText("Add a product (CD, LP, 7\", cassette...) to cut the programme into discs or sides. "
                        "The full programme still renders as one piece for digital delivery.", false);
    }
    updating_ = false;
}

void ProductsPanel::resized() {
    auto r = getLocalBounds().reduced(10);
    auto left = r.removeFromLeft(210);
    auto lb = left.removeFromBottom(28);
    add_.setBounds(lb.removeFromLeft(120));
    lb.removeFromLeft(6);
    remove_.setBounds(lb);
    left.removeFromBottom(4);
    autoNew_.setBounds(left.removeFromBottom(24));
    left.removeFromBottom(4);
    list_.setBounds(left);
    r.removeFromLeft(10);
    auto row1 = r.removeFromTop(26);
    auto pair = [](juce::Rectangle<int>& row, juce::Label& l, juce::Component& c, int lw, int cw) {
        l.setBounds(row.removeFromLeft(lw));
        row.removeFromLeft(4);
        c.setBounds(row.removeFromLeft(cw));
        row.removeFromLeft(8);
    };
    pair(row1, nameL_, name_, 44, 110);
    pair(row1, catL_, cat_, 70, 120);
    pair(row1, upcL_, upc_, 36, 130);
    pair(row1, limitL_, limit_, 86, 70);
    r.removeFromTop(6);
    auto row2 = r.removeFromTop(26);
    pair(row2, gapL_, gap_, 60, 180);
    pair(row2, overlapL_, overlap_, 86, 180);
    even_.setBounds(row2.removeFromLeft(180));
    r.removeFromTop(6);
    auto row3 = r.removeFromTop(28);
    split_.setBounds(row3.removeFromLeft(96));
    row3.removeFromLeft(4);
    mode_.setBounds(row3.removeFromLeft(160));
    row3.removeFromLeft(8);
    addUnit_.setBounds(row3.removeFromLeft(90));
    row3.removeFromLeft(6);
    removeUnit_.setBounds(row3.removeFromLeft(100));
    row3.removeFromLeft(6);
    editBreak_.setBounds(row3.removeFromLeft(110));
    row3.removeFromLeft(10);
    summary_.setBounds(row3);
    r.removeFromTop(6);
    issues_.setBounds(r.removeFromBottom(90));
    r.removeFromBottom(6);
    units_.setBounds(r);
}

void ProductsPanel::ListModel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    if (row >= o.s_.products.size()) return;
    const auto& p = o.s_.products.getReference(row);
    g.fillAll(selected ? colours::selection : colours::bg);
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText(p.name, 8, 2, w - 16, h / 2, juce::Justification::bottomLeft);
    g.setColour(colours::dim);
    g.setFont(12.0f);
    juce::String sub = juce::String(productSpec(p.kind).name) + ", " + juce::String(p.units.size()) + " " +
                       juce::String(productSpec(p.kind).unitNoun).toLowerCase() + (p.units.size() == 1 ? "" : "s");
    g.drawText(sub, 8, h / 2, w - 16, h / 2 - 2, juce::Justification::topLeft);
}

void ProductsPanel::ListModel::selectedRowsChanged(int row) {
    if (row < 0 || row >= o.s_.products.size()) return;
    if (o.productId_ == o.s_.products[row].id) return;
    o.productId_ = o.s_.products[row].id;
    o.units_.updateContent();
    o.units_.repaint();
    o.updateFields();
    if (o.onProductSelected) o.onProductSelected(o.productId_);
}

int ProductsPanel::getNumRows() { auto* p = product(); return p ? p->units.size() : 0; }

void ProductsPanel::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) {
    g.fillAll(selected ? colours::selection : (row % 2 ? colours::panel : colours::bg));
}

juce::String ProductsPanel::unitText(int row, int col) const {
    auto* p = const_cast<ProductsPanel*>(this)->product();
    if (!p || row >= p->units.size()) return {};
    const auto& u = p->units.getReference(row);
    switch (col) {
        case 1: return unitLabel(*p, row, true);
        case 2: return mmss(u.start);
        case 3: return mmss(u.end);
        case 4: return mmss(u.length());
        case 5: return u.fadeIn > 0 ? juce::String(u.fadeIn, 1) + " s" : juce::String("-");
        case 6: return u.fadeOut > 0 ? juce::String(u.fadeOut, 1) + " s" : juce::String("-");
        case 7: {
            auto ts = unitTracks(s_, u);
            if (ts.empty()) return "-";
            juce::String t = juce::String(ts.front().number) + (ts.front().continuesFromPrevious ? " (cont.)" : "");
            if (ts.size() > 1) t << " to " << ts.back().number;
            if (ts.back().continuesOnNext) t << " (continues)";
            return t;
        }
    }
    return {};
}

void ProductsPanel::setUnitText(int row, int col, const juce::String& t) {
    auto* p = product();
    if (!p || row >= p->units.size()) return;
    auto& u = p->units.getReference(row);
    const double T = s_.end();
    switch (col) {
        case 2: u.start = juce::jlimit(0.0, u.end - 1.0, parseTimeString(t, u.start)); break;
        case 3: u.end = juce::jlimit(u.start + 1.0, T, parseTimeString(t, u.end)); break;
        case 5: u.fadeIn = juce::jlimit(0.0, u.length() - u.fadeOut, parseTimeString(t.upToFirstOccurrenceOf("s", false, true), u.fadeIn)); break;
        case 6: u.fadeOut = juce::jlimit(0.0, u.length() - u.fadeIn, parseTimeString(t.upToFirstOccurrenceOf("s", false, true), u.fadeOut)); break;
        default: return;
    }
    commit();
}

void ProductsPanel::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) {
    auto* p = product();
    if (!p || row >= p->units.size()) return;
    if (col == 8) {
        // Gauge: length against the target (green), over target (amber), over the format limit (red).
        const auto& u = p->units.getReference(row);
        const auto& spec = productSpec(p->kind);
        double frac = u.length() / spec.hardMax;
        auto bar = juce::Rectangle<float>(4, h * 0.3f, float(w - 8), h * 0.4f);
        g.setColour(colours::panel2);
        g.fillRect(bar);
        auto c = u.length() > spec.hardMax ? colours::error : u.length() > p->limit() ? colours::accent : colours::ok;
        g.setColour(c);
        g.fillRect(bar.withWidth(float(juce::jlimit(0.0, 1.0, frac) * bar.getWidth())));
        g.setColour(colours::text.withAlpha(0.7f));
        float tx = bar.getX() + float(p->limit() / spec.hardMax) * bar.getWidth();
        g.drawVerticalLine(int(tx), bar.getY() - 2, bar.getBottom() + 2);
        return;
    }
    if (col == 2 || col == 3 || col == 5 || col == 6) return;   // edit cells
    g.setColour(col == 1 ? colours::text : colours::dim);
    g.setFont(13.0f);
    g.drawText(unitText(row, col), 4, 0, w - 8, h, col == 1 || col == 7 ? juce::Justification::centredLeft : juce::Justification::centredRight);
}

juce::Component* ProductsPanel::refreshComponentForCell(int row, int col, bool, juce::Component* existing) {
    if (!(col == 2 || col == 3 || col == 5 || col == 6)) { delete existing; return nullptr; }
    auto* c = dynamic_cast<EditCell*>(existing);
    if (!c) { delete existing; c = new EditCell(); c->setJustificationType(juce::Justification::centredRight); }
    c->get = [this, row, col] { return unitText(row, col); };
    c->set = [this, row, col](const juce::String& t) { setUnitText(row, col, t); };
    c->refreshText();
    return c;
}

void ProductsPanel::cellDoubleClicked(int row, int, const juce::MouseEvent&) {
    if (auto* p = product(); p && row < p->units.size() && onLocate) onLocate(p->units[row].start);
}

// ================================================================== Break editor

BreakEditor::BreakEditor(Session& s, ThumbPool& pool, int productId, int breakIndex) : s_(s), pool_(pool), pid_(productId), k_(breakIndex) {
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(title_);
    info_.setColour(juce::Label::textColourId, colours::dim);
    addAndMakeVisible(info_);
    auto slider = [this](juce::Slider& sl, juce::Label& l, const juce::String& t, double lo, double hi, double step) {
        sl.setSliderStyle(juce::Slider::LinearHorizontal);
        sl.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
        sl.setRange(lo, hi, step);
        sl.setTextValueSuffix(" s");
        addAndMakeVisible(sl);
        l.setText(t, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, colours::dim);
        addAndMakeVisible(l);
    };
    slider(outLen_, outLenL_, "Fade out", 0.0, 60.0, 0.1);
    slider(inLen_, inLenL_, "Fade in", 0.0, 60.0, 0.1);
    slider(gap_, gapL_, "Flip gap", 0.0, 10.0, 0.1);
    slider(pre_, preL_, "Pre/post-roll", 1.0, 20.0, 0.5);
    gap_.setTextValueSuffix({});
    gap_.textFromValueFunction = [](double v) { return v <= 0.0 ? juce::String("No gap") : juce::String(v, 1) + " s"; };
    gap_.valueFromTextFunction = [](const juce::String& t) { return t.retainCharacters("0123456789.").getDoubleValue(); };
    gap_.updateText();
    pre_.setValue(4.0, juce::dontSendNotification);
    for (auto* c : {&outShape_, &inShape_}) {
        for (int i = 0; i <= 4; ++i) c->addItem(fadeShapeName(FadeShape(i)), i + 1);
        addAndMakeVisible(*c);
    }
    outShapeL_.setText("Shape", juce::dontSendNotification);
    inShapeL_.setText("Shape", juce::dontSendNotification);
    nudgeL_lbl_.setText("Move break", juce::dontSendNotification);
    for (auto* l : {&outShapeL_, &inShapeL_, &nudgeL_lbl_}) { l->setColour(juce::Label::textColourId, colours::dim); addAndMakeVisible(*l); }
    for (auto* b : {&prev_, &next_, &audition_, &stop_, &nudgeL_, &nudgeR_, &nudgeStep_, &zoomIn_, &zoomOut_}) {
        styleButton(*b, b == &audition_);
        addAndMakeVisible(*b);
    }
    outLen_.onValueChange = [this] {
        if (updating_) return;
        if (auto* u = outUnit()) { u->fadeOut = juce::jmin(outLen_.getValue(), u->length() - u->fadeIn); changed(); }
    };
    inLen_.onValueChange = [this] {
        if (updating_) return;
        if (auto* u = inUnit()) { u->fadeIn = juce::jmin(inLen_.getValue(), u->length() - u->fadeOut); changed(); }
    };
    outShape_.onChange = [this] { if (!updating_) if (auto* u = outUnit()) { u->outShape = FadeShape(outShape_.getSelectedId() - 1); changed(); } };
    inShape_.onChange = [this] { if (!updating_) if (auto* u = inUnit()) { u->inShape = FadeShape(inShape_.getSelectedId() - 1); changed(); } };
    gap_.onValueChange = [this] { if (!updating_) if (auto* p = product()) { p->flipGap = gap_.getValue(); changed(); } };
    auto nudge = [this](double dir) {
        auto* a = outUnit();
        auto* b = inUnit();
        if (!a || !b) return;
        double dt = dir * step_;
        dt = juce::jlimit(-(b->start), s_.end() - a->end, dt);
        a->end += dt;
        b->start += dt;
        changed();
    };
    nudgeL_.onClick = [nudge] { nudge(-1); };
    nudgeR_.onClick = [nudge] { nudge(+1); };
    nudgeStep_.onClick = [this] {
        step_ = step_ < 0.05 ? 0.1 : step_ < 0.5 ? 1.0 : 1.0 / 75.0;
        nudgeStep_.setButtonText(step_ < 0.05 ? "1 frame" : step_ < 0.5 ? "100 ms" : "1 s");
    };
    prev_.onClick = [this] { if (k_ > 0) setBreak(pid_, k_ - 1); };
    next_.onClick = [this] { if (auto* p = product(); p && k_ + 2 < p->units.size()) setBreak(pid_, k_ + 1); };
    audition_.onClick = [this] {
        if (auto* p = product(); p && onAudition) onAudition(makeBreakPlan(*p, k_, pre_.getValue(), pre_.getValue()));
    };
    stop_.onClick = [this] { if (onStop) onStop(); };
    zoomIn_.onClick = [this] { zoom_ = juce::jmax(2.0, zoom_ / 2); repaint(); };
    zoomOut_.onClick = [this] { zoom_ = juce::jmin(600.0, zoom_ * 2); repaint(); };
    for (auto& r : s_.regions) pool_.get(r.file, this);
    refreshControls();
    startTimerHz(30);
    setSize(980, 520);
}

Product* BreakEditor::product() { return s_.findProduct(pid_); }
Unit* BreakEditor::outUnit() { auto* p = product(); return p && k_ >= 0 && k_ < p->units.size() ? &p->units.getReference(k_) : nullptr; }
Unit* BreakEditor::inUnit() { auto* p = product(); return p && k_ + 1 < p->units.size() ? &p->units.getReference(k_ + 1) : nullptr; }

void BreakEditor::setBreak(int pid, int k) {
    pid_ = pid;
    k_ = k;
    refreshControls();
    repaint();
}

void BreakEditor::changed() {
    refreshControls();
    if (onChange) onChange();
    repaint();
}

void BreakEditor::refreshControls() {
    updating_ = true;
    auto* p = product();
    auto* a = outUnit();
    auto* b = inUnit();
    if (p && a && b) {
        title_.setText(p->name + ": " + unitLabel(*p, k_, true) + "  >  " + unitLabel(*p, k_ + 1, true), juce::dontSendNotification);
        outLen_.setValue(a->fadeOut, juce::dontSendNotification);
        inLen_.setValue(b->fadeIn, juce::dontSendNotification);
        outShape_.setSelectedId(int(a->outShape) + 1, juce::dontSendNotification);
        inShape_.setSelectedId(int(b->inShape) + 1, juce::dontSendNotification);
        gap_.setValue(p->flipGap, juce::dontSendNotification);
        double ov = a->end - b->start;
        juce::String inf;
        inf << unitLabel(*p, k_) << " ends at " << msfString(a->end) << " (" << mmss(a->length()) << " long)   "
            << unitLabel(*p, k_ + 1) << " starts at " << msfString(b->start) << " (" << mmss(b->length()) << " long)   ";
        if (ov > 0.001) inf << "Overlap " << juce::String(ov, 2) << " s";
        else if (ov < -0.001) inf << "GAP: " << juce::String(-ov, 2) << " s of music is on neither";
        else inf << "Butt break (no overlap)";
        info_.setText(inf, juce::dontSendNotification);
        info_.setColour(juce::Label::textColourId, ov < -0.001 ? colours::error : colours::dim);
    } else {
        title_.setText("No break here", juce::dontSendNotification);
        info_.setText({}, juce::dontSendNotification);
    }
    prev_.setEnabled(k_ > 0);
    next_.setEnabled(p && k_ + 2 < p->units.size());
    updating_ = false;
}

juce::Rectangle<int> BreakEditor::waveArea() const {
    return getLocalBounds().reduced(10).withTrimmedTop(74).withTrimmedBottom(100);
}

double BreakEditor::xToTime(float x) const {
    auto a = waveArea();
    return t0_ + (double(x) - a.getX()) / a.getWidth() * (t1_ - t0_);
}

float BreakEditor::timeToX(double t) const {
    auto a = waveArea();
    return float(a.getX() + (t - t0_) / (t1_ - t0_) * a.getWidth());
}

void BreakEditor::resized() {
    auto r = getLocalBounds().reduced(10);
    auto top = r.removeFromTop(24);
    zoomIn_.setBounds(top.removeFromRight(30));
    top.removeFromRight(4);
    zoomOut_.setBounds(top.removeFromRight(30));
    top.removeFromRight(12);
    next_.setBounds(top.removeFromRight(110));
    top.removeFromRight(6);
    prev_.setBounds(top.removeFromRight(110));
    title_.setBounds(top);
    info_.setBounds(r.removeFromTop(24));
    auto controls = r.removeFromBottom(92);
    auto row = [&](juce::Label& l, juce::Slider& sl, juce::Label* sl2, juce::ComboBox* c, juce::Rectangle<int>& line) {
        l.setBounds(line.removeFromLeft(80));
        sl.setBounds(line.removeFromLeft(260));
        line.removeFromLeft(8);
        if (sl2 && c) { sl2->setBounds(line.removeFromLeft(44)); c->setBounds(line.removeFromLeft(130)); line.removeFromLeft(12); }
    };
    auto l1 = controls.removeFromTop(28);
    row(outLenL_, outLen_, &outShapeL_, &outShape_, l1);
    row(gapL_, gap_, nullptr, nullptr, l1);
    controls.removeFromTop(4);
    auto l2 = controls.removeFromTop(28);
    row(inLenL_, inLen_, &inShapeL_, &inShape_, l2);
    row(preL_, pre_, nullptr, nullptr, l2);
    controls.removeFromTop(8);
    auto l3 = controls.removeFromTop(28);
    nudgeL_lbl_.setBounds(l3.removeFromLeft(80));
    nudgeL_.setBounds(l3.removeFromLeft(30));
    nudgeR_.setBounds(l3.removeFromLeft(30));
    l3.removeFromLeft(6);
    nudgeStep_.setBounds(l3.removeFromLeft(80));
    stop_.setBounds(l3.removeFromRight(70));
    l3.removeFromRight(6);
    audition_.setBounds(l3.removeFromRight(130));
}

void BreakEditor::paintLane(juce::Graphics& g, juce::Rectangle<int> area, const Unit& u, bool outgoing) {
    g.setColour(colours::bg);
    g.fillRect(area);
    // The programme's audio across the view, dim outside this unit and bright inside it.
    auto drawRegions = [&](juce::Colour c, double a, double b) {
        if (b <= a) return;
        g.saveState();
        g.reduceClipRegion(juce::Rectangle<int>(int(timeToX(a)), area.getY(), juce::jmax(1, int(timeToX(b) - timeToX(a)) + 1), area.getHeight()));
        for (auto& r : s_.regions) {
            if (r.mute || r.end() < t0_ || r.start > t1_) continue;
            auto* th = pool_.get(r.file, nullptr);
            if (!th) continue;
            double va = juce::jmax(t0_, r.start), vb = juce::jmin(t1_, r.end());
            double sa = double(r.srcIn) / r.fileRate + (va - r.start);
            auto box = juce::Rectangle<int>(int(timeToX(va)), area.getY() + 4, juce::jmax(1, int(timeToX(vb) - timeToX(va))), area.getHeight() - 8);
            g.setColour(c);
            th->drawChannels(g, box, sa, sa + (vb - va), 1.0f);
        }
        g.restoreState();
    };
    drawRegions(colours::wave.withAlpha(0.22f), t0_, t1_);
    drawRegions(colours::wave, juce::jmax(t0_, u.start), juce::jmin(t1_, u.end));
    // Gain curve.
    juce::Path p;
    const int steps = juce::jmax(50, area.getWidth() / 2);
    for (int k = 0; k <= steps; ++k) {
        double t = t0_ + (t1_ - t0_) * k / steps;
        float y = float(area.getBottom() - 3 - unitGain(u, t) * (area.getHeight() - 6));
        float x = timeToX(t);
        if (k == 0) p.startNewSubPath(x, y);
        else p.lineTo(x, y);
    }
    g.setColour(colours::envelope);
    g.strokePath(p, juce::PathStrokeType(2.0f));
    // Handles: unit edge and fade boundary.
    auto handle = [&](double t, juce::Colour c, const juce::String& label) {
        float x = timeToX(t);
        if (x < area.getX() - 2 || x > area.getRight() + 2) return;
        g.setColour(c);
        g.drawVerticalLine(int(x), float(area.getY()), float(area.getBottom()));
        g.fillRect(x - 4, float(area.getY()), 8.0f, 8.0f);
        g.setFont(11.0f);
        g.drawText(label, int(x) + 6, area.getY() + 2, 140, 14, juce::Justification::left);
    };
    auto* p0 = product();
    if (outgoing) {
        handle(u.end, colours::end, (p0 ? unitLabel(*p0, k_) : juce::String()) + " end");
        if (u.fadeOut > 0) handle(u.end - u.fadeOut, colours::accent, "fade out");
    } else {
        handle(u.start, colours::ok, (p0 ? unitLabel(*p0, k_ + 1) : juce::String()) + " start");
        if (u.fadeIn > 0) handle(u.start + u.fadeIn, colours::accent, "fade in");
    }
    g.setColour(colours::line);
    g.drawRect(area);
}

void BreakEditor::paint(juce::Graphics& g) {
    g.fillAll(colours::panel);
    auto* a = outUnit();
    auto* b = inUnit();
    if (!a || !b) return;
    double c = (a->end + b->start) * 0.5;
    t0_ = c - zoom_;
    t1_ = c + zoom_;
    auto area = waveArea();
    auto top = area.removeFromTop(area.getHeight() / 2).withTrimmedBottom(3);
    auto bottom = area.withTrimmedTop(3);
    paintLane(g, top, *a, true);
    paintLane(g, bottom, *b, false);
    // Time ruler along the top of the waveform.
    g.setColour(colours::dim);
    g.setFont(11.0f);
    const double pps = waveArea().getWidth() / (t1_ - t0_);
    double step = 120;
    for (double c : {0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0})
        if (c * pps >= 80) { step = c; break; }
    for (double t = std::ceil(t0_ / step) * step; t <= t1_; t += step) {
        float x = timeToX(t);
        g.drawVerticalLine(int(x), float(top.getY() - 6), float(top.getY()));
        g.drawText(msfString(t), int(x) + 3, top.getY() - 17, 76, 12, juce::Justification::left);
    }
    // Audition playhead.
    if (auditionPosition) {
        double t = auditionPosition();
        if (t >= t0_ && t <= t1_) {
            g.setColour(juce::Colours::white);
            g.drawVerticalLine(int(timeToX(t)), float(waveArea().getY()), float(waveArea().getBottom()));
        }
    }
}

void BreakEditor::mouseDown(const juce::MouseEvent& e) {
    auto* a = outUnit();
    auto* b = inUnit();
    if (!a || !b) return;
    auto area = waveArea();
    if (!area.contains(e.getPosition())) return;
    const bool topLane = e.y < area.getCentreY();
    const float x = float(e.x);
    auto near = [&](double t) { return std::abs(timeToX(t) - x) < 7.0f; };
    drag_ = Drag::None;
    if (topLane) {
        if (near(a->end)) drag_ = Drag::OutEnd;
        else if (a->fadeOut > 0 && near(a->end - a->fadeOut)) drag_ = Drag::OutFade;
    } else {
        if (near(b->start)) drag_ = Drag::InStart;
        else if (b->fadeIn > 0 && near(b->start + b->fadeIn)) drag_ = Drag::InFade;
    }
    if (drag_ == Drag::None && e.mods.isShiftDown()) drag_ = Drag::Both;
    dragT_ = xToTime(x);
    dragOut_ = *a;
    dragIn_ = *b;
}

void BreakEditor::mouseDrag(const juce::MouseEvent& e) {
    auto* a = outUnit();
    auto* b = inUnit();
    if (!a || !b || drag_ == Drag::None) return;
    double dt = xToTime(float(e.x)) - dragT_;
    const double T = s_.end();
    switch (drag_) {
        case Drag::OutEnd:
            a->end = juce::jlimit(juce::jmax(dragIn_.start, a->start + a->fadeIn + a->fadeOut + 1.0), T, dragOut_.end + dt);
            break;
        case Drag::OutFade:
            a->fadeOut = juce::jlimit(0.0, a->length() - a->fadeIn, dragOut_.fadeOut - dt);
            break;
        case Drag::InStart:
            b->start = juce::jlimit(0.0, juce::jmin(dragOut_.end, b->end - b->fadeIn - b->fadeOut - 1.0), dragIn_.start + dt);
            break;
        case Drag::InFade:
            b->fadeIn = juce::jlimit(0.0, b->length() - b->fadeOut, dragIn_.fadeIn + dt);
            break;
        case Drag::Both: {
            double d = juce::jlimit(-dragIn_.start, T - dragOut_.end, dt);
            a->end = dragOut_.end + d;
            b->start = dragIn_.start + d;
            break;
        }
        case Drag::None: break;
    }
    refreshControls();
    repaint();
}

void BreakEditor::mouseUp(const juce::MouseEvent&) {
    if (drag_ != Drag::None && onChange) onChange();
    drag_ = Drag::None;
}

}  // namespace lathe
