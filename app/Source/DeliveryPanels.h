// Delivery windows: release info, track credits, studio profile, products & splitting,
// and the break editor (the crossfade editor's counterpart for side / disc breaks).
#pragma once

#include "Paperwork.h"
#include "Products.h"
#include "Session.h"
#include "Theme.h"
#include "Timeline.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace lathe {

// A label + editor grid for release-level metadata, PQ offset and HTOA.
class ReleaseInfoPanel : public juce::Component {
public:
    explicit ReleaseInfoPanel(Session& s);
    std::function<void()> onChange;
    void refresh();
    void resized() override;
    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }

private:
    struct Field {
        juce::Label label;
        juce::TextEditor editor;
        std::function<juce::String()> get;
        std::function<void(const juce::String&)> set;
    };
    void addField(const juce::String& name, std::function<juce::String()> get, std::function<void(const juce::String&)> set, bool multi = false);
    Session& s_;
    juce::OwnedArray<Field> fields_;
    juce::Label genreLbl_, offsetLbl_, help_;
    juce::ComboBox genre_;
    juce::Slider offset_;
    juce::ToggleButton htoa_{"Hidden track one audio (audio before track 1 is intended)"};
    bool updating_ = false;
};

// Per-track credits table (writers, arranger, publisher, PRO, message, explicit).
class TrackCreditsPanel : public juce::Component, public juce::TableListBoxModel {
public:
    explicit TrackCreditsPanel(Session& s);
    // Close the list while this panel is still whole: a cell being edited at close otherwise
    // reaches back into a half-destroyed list (a crash on macOS).
    ~TrackCreditsPanel() override { closeList(table_); }
    std::function<void()> onChange;
    void refresh() { table_.updateContent(); table_.repaint(); }
    void resized() override { table_.setBounds(getLocalBounds().reduced(8)); }
    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }

    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell(int row, int col, bool selected, juce::Component* existing) override;
    void cellClicked(int row, int col, const juce::MouseEvent&) override;

    juce::String text(int row, int col) const;
    void setText(int row, int col, const juce::String& t);

private:
    Marker* trackMarker(int row);
    const Marker* trackMarker(int row) const;
    Session& s_;
    juce::TableListBox table_{"Credits", this};
};

// Studio name, contact and logo for the paperwork (stored per user, not per session).
class StudioPanel : public juce::Component {
public:
    explicit StudioPanel(StudioProfile& p);
    std::function<void()> onChange;
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void loadPreview();
    StudioProfile& p_;
    juce::Label nameL_, addrL_, phoneL_, emailL_, webL_, logoL_;
    juce::TextEditor name_, addr_, phone_, email_, web_;
    juce::TextButton chooseLogo_{"Choose Logo..."}, clearLogo_{"Clear"};
    juce::ToggleButton a4_{"A4 paper (otherwise US Letter)"};
    juce::Image preview_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

// Products & splitting: the product list, each product's units, auto-split and checks.
class ProductsPanel : public juce::Component, public juce::TableListBoxModel {
public:
    ProductsPanel(Session& s, std::function<const dsp::LoudnessMap*()> loudness);
    // Close the list while this panel is still whole: a cell being edited at close otherwise
    // reaches back into a half-destroyed list (a crash on macOS).
    ~ProductsPanel() override { closeList(units_); }
    std::function<void()> onChange;                          // commit (undo + reload)
    std::function<void(int productId, int breakIndex)> onEditBreak;
    std::function<void(double)> onLocate;
    std::function<void(int productId)> onProductSelected;
    void refresh();
    void selectProduct(int id);
    int selectedProduct() const { return productId_; }
    void resized() override;
    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }

    // Units table
    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell(int row, int col, bool selected, juce::Component* existing) override;
    void cellDoubleClicked(int row, int col, const juce::MouseEvent&) override;
    juce::String unitText(int row, int col) const;
    void setUnitText(int row, int col, const juce::String& t);

private:
    struct ListModel : juce::ListBoxModel {
        ProductsPanel& o;
        explicit ListModel(ProductsPanel& p) : o(p) {}
        int getNumRows() override { return o.s_.products.size(); }
        void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
        void selectedRowsChanged(int lastRowSelected) override;
    };

    Product* product();
    void addProduct(ProductKind k);
    void commit();
    void updateFields();

    Session& s_;
    std::function<const dsp::LoudnessMap*()> loudness_;
    int productId_ = 0;
    ListModel listModel_{*this};
    juce::ListBox list_{"Products", &listModel_};
    juce::TextButton add_{"Add Product..."}, remove_{"Remove"}, split_{"Auto-Split"}, addUnit_{"Add Unit"}, removeUnit_{"Remove Unit"},
        editBreak_{"Edit Break..."};
    juce::Label nameL_, catL_, upcL_, limitL_, gapL_, overlapL_, summary_;
    juce::TextEditor name_, cat_, upc_, limit_;
    juce::Slider gap_, overlap_;
    juce::ToggleButton even_{"Even number of sides"}, autoNew_{"Auto-split new products"};
    juce::ComboBox mode_;
    juce::TableListBox units_{"Units", this};
    juce::TextEditor issues_;
    bool updating_ = false;
};

// Side / disc break: the outgoing unit's fade-out and the incoming unit's fade-in over the
// programme waveform, with audition through the flip gap.
class BreakEditor : public juce::Component, public juce::ChangeListener, private juce::Timer {
public:
    BreakEditor(Session& s, ThumbPool& pool, int productId, int breakIndex);
    ~BreakEditor() override { stopTimer(); }
    std::function<void()> onChange;
    std::function<void(const BreakPlan&)> onAudition;
    std::function<void()> onStop;
    std::function<double()> auditionPosition;   // timeline seconds being heard, -1 = none
    void setBreak(int productId, int breakIndex);
    int breakIndex() const { return k_; }
    int productId() const { return pid_; }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

private:
    void timerCallback() override { repaint(); }
    Product* product();
    Unit* outUnit();
    Unit* inUnit();
    void refreshControls();
    void changed();
    juce::Rectangle<int> waveArea() const;
    double xToTime(float x) const;
    float timeToX(double t) const;
    void paintLane(juce::Graphics&, juce::Rectangle<int> area, const Unit& u, bool outgoing);

    Session& s_;
    ThumbPool& pool_;
    int pid_, k_;
    double t0_ = 0, t1_ = 1;
    juce::Label title_, outLenL_, inLenL_, outShapeL_, inShapeL_, gapL_, preL_, info_;
    juce::Slider outLen_, inLen_, gap_, pre_;
    juce::ComboBox outShape_, inShape_;
    juce::TextButton prev_{"< Prev Break"}, next_{"Next Break >"}, audition_{"Audition Flip"}, stop_{"Stop"},
        nudgeL_{"<"}, nudgeR_{">"}, nudgeStep_{"1 frame"}, zoomIn_{"+"}, zoomOut_{"-"};
    juce::Label nudgeL_lbl_;
    double step_ = 1.0 / 75.0;
    double zoom_ = 30.0;   // seconds shown either side of the break
    enum class Drag { None, OutEnd, InStart, OutFade, InFade, Both } drag_ = Drag::None;
    double dragT_ = 0;
    Unit dragOut_, dragIn_;
    bool updating_ = false;
};

}  // namespace lathe
