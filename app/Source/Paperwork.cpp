#include "Paperwork.h"

#include "../../core/doc/Pdf.h"
#include "../../core/util/Hash.h"

#include <juce_graphics/juce_graphics.h>

namespace lathe {

juce::var StudioProfile::toVar() const {
    auto* o = new juce::DynamicObject();
    o->setProperty("name", name);
    o->setProperty("address", address);
    o->setProperty("phone", phone);
    o->setProperty("email", email);
    o->setProperty("website", website);
    o->setProperty("logo", logo.getFullPathName());
    o->setProperty("a4", a4);
    return o;
}

void StudioProfile::fromVar(const juce::var& v) {
    name = v["name"].toString();
    address = v["address"].toString();
    phone = v["phone"].toString();
    email = v["email"].toString();
    website = v["website"].toString();
    auto l = v["logo"].toString();
    logo = l.isNotEmpty() && juce::File::isAbsolutePath(l) ? juce::File(l) : juce::File();
    a4 = bool(v["a4"]);
}

std::vector<uint8_t> logoJpeg(const juce::File& f) {
    if (!f.existsAsFile()) return {};
    auto img = juce::ImageFileFormat::loadFrom(f);
    if (!img.isValid()) return {};
    // Composite onto white (PDF JPEGs have no alpha) and keep it a sensible size.
    const int maxDim = 900;
    double scale = juce::jmin(1.0, double(maxDim) / juce::jmax(img.getWidth(), img.getHeight()));
    int w = juce::jmax(1, int(img.getWidth() * scale)), h = juce::jmax(1, int(img.getHeight() * scale));
    juce::Image flat(juce::Image::RGB, w, h, true, juce::SoftwareImageType());
    {
        juce::Graphics g(flat);
        g.fillAll(juce::Colours::white);
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImage(img, 0, 0, w, h, 0, 0, img.getWidth(), img.getHeight());
    }
    juce::MemoryOutputStream mo;
    juce::JPEGImageFormat jpg;
    jpg.setQuality(0.92f);
    if (!jpg.writeImageToStream(flat, mo)) return {};
    auto* p = static_cast<const uint8_t*>(mo.getData());
    return std::vector<uint8_t>(p, p + mo.getDataSize());
}

namespace {

std::string u8(const juce::String& s) { return s.toStdString(); }
juce::String js(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

juce::String msf(int64_t sectors) { return juce::String(cd::Msf::fromSectors(sectors).str()); }
juce::String mmss(double sec) {
    int t = int(std::llround(sec));
    if (t >= 3600) return juce::String(t / 3600) + ":" + juce::String((t / 60) % 60).paddedLeft('0', 2) + ":" + juce::String(t % 60).paddedLeft('0', 2);
    return juce::String(t / 60) + ":" + juce::String(t % 60).paddedLeft('0', 2);
}
juce::String hms(double sec) {
    int t = int(std::floor(sec));
    int cs = int(std::floor((sec - t) * 100));
    return juce::String(t / 3600) + ":" + juce::String((t / 60) % 60).paddedLeft('0', 2) + ":" + juce::String(t % 60).paddedLeft('0', 2) + "." +
           juce::String(cs).paddedLeft('0', 2);
}
juce::String db(double v, const char* unit) { return std::isfinite(v) ? juce::String(v, 1) + " " + unit : juce::String("--"); }
juce::String sizeText(juce::int64 b) {
    if (b >= (1LL << 30)) return juce::String(double(b) / double(1LL << 30), 2) + " GB";
    if (b >= (1LL << 20)) return juce::String(double(b) / double(1LL << 20), 1) + " MB";
    if (b >= 1024) return juce::String(double(b) / 1024.0, 1) + " KB";
    return juce::String(b) + " B";
}
juce::String md5Of(const juce::File& f) {
    juce::FileInputStream in(f);
    if (!in.openedOk()) return {};
    util::Md5 m;
    juce::HeapBlock<char> buf(1 << 20);
    for (;;) {
        auto n = in.read(buf.get(), 1 << 20);
        if (n <= 0) break;
        m.update(buf.get(), size_t(n));
    }
    return m.hexDigest();
}
juce::String fadeText(double len, FadeShape s) { return len > 0 ? juce::String(len, 1) + " s " + fadeShapeName(s).toLowerCase() : juce::String("none"); }

// Programme tracks with their credits (from the session's track markers).
struct Credit {
    int number = 0;
    juce::String title, performer, songwriter, composer, arranger, publisher, pro, isrc, message;
    bool explicitContent = false;
    double start = 0, length = 0;
};

std::vector<Credit> credits(const Session& s) {
    Session c = s;
    c.sortMarkers();
    auto ranges = c.trackRanges();
    std::vector<Credit> out;
    int i = 0;
    for (auto& m : c.markers) {
        if (m.type != Marker::TrackStart) continue;
        Credit k;
        k.number = i + 1;
        k.title = js(m.text.title);
        k.performer = js(m.text.performer).isNotEmpty() ? js(m.text.performer) : s.discPerformer;
        k.songwriter = js(m.text.songwriter);
        k.composer = js(m.text.composer);
        k.arranger = js(m.text.arranger);
        k.message = js(m.text.message);
        k.publisher = js(m.publisher);
        k.pro = js(m.pro);
        k.isrc = js(cd::normaliseIsrc(m.isrc));
        k.explicitContent = m.explicitContent;
        if (i < ranges.size()) { k.start = ranges[i].getStart(); k.length = ranges[i].getLength(); }
        out.push_back(k);
        ++i;
    }
    return out;
}

// ------------------------------------------------------------------ page layout

struct Col {
    juce::String title;
    double width;   // points
    int align = 0;  // 0 left, 2 right
};

class Sheet {
public:
    Sheet(const StudioProfile& st, const std::vector<uint8_t>& logo, const juce::String& docTitle, const juce::String& project)
        : pdf_(st.a4 ? 595.28 : 612, st.a4 ? 841.89 : 792), st_(st), title_(docTitle), project_(project) {
        logoId_ = logo.empty() ? -1 : pdf_.addJpeg(logo);
        pdf_.setInfo(u8(docTitle + (project.isNotEmpty() ? " - " + project : juce::String())), u8(st.name));
        generated_ = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M");
    }

    double left() const { return 40; }
    double right() const { return pdf_.width() - 40; }
    double contentWidth() const { return right() - left(); }
    doc::Pdf& pdf() { return pdf_; }

    void startPage(const juce::String& subtitle = {}) {
        if (pageOpen_) footer();
        pdf_.newPage();
        pageOpen_ = true;
        ++page_;
        double top = 36;
        double headerH = 54;
        if (logoId_ >= 0) {
            int iw = 1, ih = 1;
            pdf_.imageSize(logoId_, iw, ih);
            double h = 50, w = h * iw / ih;
            if (w > 170) { w = 170; h = w * ih / iw; }
            pdf_.image(logoId_, left(), top, w, h);
            headerH = juce::jmax(headerH, h);
        }
        // Studio block, right-aligned.
        double y = top + 11;
        pdf_.setColor(0, 0, 0);
        if (st_.name.isNotEmpty()) { pdf_.setFont(true, 12); pdf_.text(right(), y, u8(st_.name), 2); y += 13; }
        pdf_.setFont(false, 8);
        pdf_.setColor(0.25, 0.25, 0.25);
        juce::StringArray lines;
        lines.addLines(st_.address);
        juce::String contact;
        for (auto& c : {st_.phone, st_.email, st_.website})
            if (c.isNotEmpty()) contact << (contact.isEmpty() ? "" : "  |  ") << c;
        if (contact.isNotEmpty()) lines.add(contact);
        for (auto& l : lines)
            if (l.trim().isNotEmpty()) { pdf_.text(right(), y, u8(l.trim()), 2); y += 10; }
        y = juce::jmax(y, top + headerH) + 10;
        // Title bar.
        pdf_.setColor(0.12, 0.12, 0.14);
        pdf_.rect(left(), y, contentWidth(), 24, true, false);
        pdf_.setColor(1, 1, 1);
        pdf_.setFont(true, 12);
        pdf_.text(left() + 8, y + 16, u8(title_.toUpperCase()));
        if (subtitle.isNotEmpty()) { pdf_.setFont(false, 10); pdf_.text(right() - 8, y + 16, u8(subtitle), 2); }
        pdf_.setColor(0, 0, 0);
        y_ = y + 36;
    }

    void finish() { if (pageOpen_) footer(); pageOpen_ = false; }
    bool save(const juce::File& f) { finish(); return pdf_.save(f.getFullPathName().toStdString()); }

    void ensure(double h) { if (y_ + h > pdf_.height() - 56) startPage(contSubtitle_); }
    void setContinuationSubtitle(const juce::String& s) { contSubtitle_ = s; }
    double& y() { return y_; }

    void heading(const juce::String& t) {
        ensure(30);
        y_ += 4;
        pdf_.setFont(true, 10);
        pdf_.setColor(0, 0, 0);
        pdf_.text(left(), y_ + 9, u8(t.toUpperCase()));
        pdf_.setStroke(0.6, 0.6, 0.6);
        pdf_.line(left(), y_ + 13, right(), y_ + 13, 0.5);
        y_ += 22;
    }

    // Label / value pairs in two columns.
    void fields(const std::vector<std::pair<juce::String, juce::String>>& kv) {
        const double colW = contentWidth() / 2, labelW = 92;
        size_t i = 0;
        while (i < kv.size()) {
            ensure(14);
            for (int c = 0; c < 2 && i < kv.size(); ++c, ++i) {
                double x = left() + c * colW;
                pdf_.setFont(false, 8);
                pdf_.setColor(0.4, 0.4, 0.4);
                pdf_.text(x, y_ + 9, u8(kv[i].first));
                pdf_.setFont(true, 9);
                pdf_.setColor(0, 0, 0);
                auto v = kv[i].second.isEmpty() ? juce::String("-") : kv[i].second;
                pdf_.text(x + labelW, y_ + 9, pdf_.fit(u8(v), colW - labelW - 8));
            }
            y_ += 14;
        }
        y_ += 6;
    }

    void table(const std::vector<Col>& cols, const std::vector<std::vector<juce::String>>& rows, double fontSize = 8) {
        double total = 0;
        for (auto& c : cols) total += c.width;
        const double scale = contentWidth() / total;
        const double rowH = fontSize + 6;
        auto header = [&]() {
            ensure(rowH * 2);
            pdf_.setColor(0.9, 0.9, 0.92);
            pdf_.rect(left(), y_, contentWidth(), rowH + 2, true, false);
            pdf_.setColor(0, 0, 0);
            pdf_.setFont(true, fontSize);
            double x = left();
            for (auto& c : cols) {
                double w = c.width * scale;
                pdf_.text(c.align == 2 ? x + w - 4 : x + 4, y_ + rowH - 2, pdf_.fit(u8(c.title), w - 8), c.align);
                x += w;
            }
            y_ += rowH + 2;
        };
        header();
        bool shade = false;
        for (auto& r : rows) {
            if (y_ + rowH > pdf_.height() - 56) { startPage(contSubtitle_); header(); }
            if (shade) { pdf_.setColor(0.97, 0.97, 0.98); pdf_.rect(left(), y_, contentWidth(), rowH, true, false); }
            shade = !shade;
            pdf_.setColor(0, 0, 0);
            pdf_.setFont(false, fontSize);
            double x = left();
            for (size_t c = 0; c < cols.size(); ++c) {
                double w = cols[c].width * scale;
                if (c < r.size()) pdf_.text(cols[c].align == 2 ? x + w - 4 : x + 4, y_ + rowH - 4, pdf_.fit(u8(r[c]), w - 8), cols[c].align);
                x += w;
            }
            y_ += rowH;
        }
        pdf_.setStroke(0.75, 0.75, 0.75);
        pdf_.line(left(), y_, right(), y_, 0.5);
        y_ += 10;
    }

    void paragraph(const juce::String& t, double size = 8.5, bool bold = false) {
        pdf_.setFont(bold, size);
        pdf_.setColor(0, 0, 0);
        for (auto& l : pdf_.wrap(u8(t), contentWidth())) {
            ensure(size + 4);
            pdf_.setFont(bold, size);
            pdf_.text(left(), y_ + size, l);
            y_ += size + 3.5;
        }
        y_ += 4;
    }

    // Ruled lines for handwritten notes.
    void notesBox(const juce::String& label, int lines) {
        ensure(18 + lines * 18);
        pdf_.setFont(false, 8);
        pdf_.setColor(0.4, 0.4, 0.4);
        pdf_.text(left(), y_ + 9, u8(label));
        y_ += 14;
        pdf_.setStroke(0.7, 0.7, 0.7);
        for (int i = 0; i < lines; ++i) { y_ += 18; pdf_.line(left(), y_, right(), y_, 0.4); }
        y_ += 10;
    }

private:
    void footer() {
        pdf_.setFont(false, 7);
        pdf_.setColor(0.45, 0.45, 0.45);
        const double y = pdf_.height() - 28;
        pdf_.setStroke(0.8, 0.8, 0.8);
        pdf_.line(left(), y - 9, right(), y - 9, 0.4);
        pdf_.text(left(), y, u8(project_.isNotEmpty() ? project_ : title_));
        pdf_.text(pdf_.width() / 2, y, u8("Generated " + generated_ + " by Lathe"), 1);
        pdf_.text(right(), y, u8("Page " + juce::String(page_)), 2);
    }

    doc::Pdf pdf_;
    const StudioProfile& st_;
    juce::String title_, project_, generated_, contSubtitle_;
    int logoId_ = -1, page_ = 0;
    bool pageOpen_ = false;
    double y_ = 0;
};

juce::String projectName(const Session& s) {
    juce::String p = s.discPerformer;
    if (s.discTitle.isNotEmpty()) p << (p.isEmpty() ? "" : " - ") << s.discTitle;
    return p.isEmpty() ? juce::String("Untitled") : p;
}

juce::String yesNo(bool b) { return b ? "Yes" : "No"; }

// ------------------------------------------------------------------ CD PQ sheet

void pqSheet(Sheet& sh, const Session& s, const cd::Disc& disc, const juce::String& productLabel, int discNo, int discCount,
             const juce::String& catalogue, const juce::String& upc, const juce::String& ddpPath, bool verified, const juce::String& imageMd5,
             const juce::StringArray& notes, const QcReport* qc, const juce::String& dither, double lead) {
    juce::String sub = productLabel + (discCount > 1 ? "  |  Disc " + juce::String(discNo) + " of " + juce::String(discCount) : juce::String());
    sh.setContinuationSubtitle(sub + " (cont.)");
    sh.startPage(sub);
    sh.heading("Release");
    sh.fields({{"Artist", s.discPerformer},
               {"Title", js(disc.text.title).isNotEmpty() ? js(disc.text.title) : s.discTitle},
               {"Label", s.label},
               {"Catalogue no.", catalogue},
               {"UPC / EAN", upc},
               {"Master ID", js(disc.masterId)},
               {"Client", s.client},
               {"Mastered by", s.engineer}});
    sh.heading("Disc");
    int indexes = 0;
    for (auto& t : disc.tracks) indexes += juce::jmax(0, int(t.indexes.size()) - (t.find(0) ? 2 : 1));
    const int64_t total = disc.leadOutSector;
    sh.fields({{"Total time", msf(total) + "  (" + mmss(double(total) / 75.0) + ")"},
               {"Tracks", juce::String(int(disc.tracks.size())) + (indexes ? ", " + juce::String(indexes) + " sub-index(es)" : juce::String())},
               {"Lead-out", msf(total)},
               {"PQ offset", s.pqOffsetFrames > 0 ? juce::String(s.pqOffsetFrames) + " frame(s) on track starts" : juce::String("None")},
               {"Hidden track (HTOA)", yesNo(s.htoa && !disc.tracks.empty() && disc.tracks[0].startSector() > 150)},
               {"CD-Text", disc.hasCdText() ? juce::String("Yes (Latin-1, English)") : juce::String("No")},
               {"Format", "CD-DA, 44.1 kHz / 16-bit, DDP 2.00"},
               {"Dither", dither},
               {"DDP", ddpPath.isEmpty() ? juce::String("Not rendered yet (preliminary sheet)")
                                         : ddpPath.fromLastOccurrenceOf("/", false, false) + (verified ? "  (verified)" : "  (NOT VERIFIED)")},
               {"Image MD5", imageMd5}});
    if (lead > 0) sh.paragraph("A " + juce::String(lead, 2) + " s pregap of digital silence was added before this disc's first track.");

    sh.heading("Tracks (times are absolute from the start of the disc, MM:SS:FF)");
    bool anyArtist = false;
    for (auto& t : disc.tracks) anyArtist = anyArtist || (!t.text.performer.empty() && js(t.text.performer) != s.discPerformer);
    std::vector<Col> cols = {{"#", 22, 2}, {"Title", anyArtist ? 150.0 : 210.0}};
    if (anyArtist) cols.push_back({"Artist", 90});
    for (auto c : std::vector<Col>{{"ISRC", 82}, {"Pause", 54, 2}, {"Start", 54, 2}, {"Length", 54, 2}, {"Flags", 40}})
        cols.push_back(c);
    if (qc && qc->tracks.size() == int(disc.tracks.size())) cols.push_back({"LUFS / dBTP", 64, 2});
    std::vector<std::vector<juce::String>> rows;
    juce::StringArray indexLines;
    for (size_t i = 0; i < disc.tracks.size(); ++i) {
        const auto& t = disc.tracks[i];
        int64_t next = i + 1 < disc.tracks.size() ? disc.tracks[i + 1].startSector() : total;
        juce::String flags;
        if (t.preEmphasis) flags << "PRE ";
        if (t.copyPermitted) flags << "DCP ";
        if (t.scms) flags << "SCMS";
        std::vector<juce::String> r = {juce::String(int(i + 1)).paddedLeft('0', 2), js(t.text.title)};
        if (anyArtist) r.push_back(js(t.text.performer));
        r.push_back(js(t.isrc));
        r.push_back(t.find(0) ? msf(t.find(0)->sector) : juce::String());
        r.push_back(msf(t.startSector()));
        r.push_back(msf(next - t.startSector()));
        r.push_back(flags.trim().isEmpty() ? juce::String("-") : flags.trim());
        if (qc && qc->tracks.size() == int(disc.tracks.size()))
            r.push_back(std::isfinite(qc->tracks[int(i)].lufs) ? juce::String(qc->tracks[int(i)].lufs, 1) + " / " + juce::String(qc->tracks[int(i)].truePeakDb, 1) : juce::String("--"));
        rows.push_back(r);
        for (auto& ix : t.indexes)
            if (ix.number >= 2) indexLines.add("Track " + juce::String(int(i + 1)) + " index " + juce::String(ix.number).paddedLeft('0', 2) + " at " + msf(ix.sector));
    }
    sh.table(cols, rows);
    if (!indexLines.isEmpty()) { sh.heading("Sub-indexes"); sh.paragraph(indexLines.joinIntoString("\n")); }

    if (disc.hasCdText()) {
        sh.heading("CD-Text");
        std::vector<std::pair<juce::String, juce::String>> kv = {{"Album title", js(disc.text.title)}, {"Album artist", js(disc.text.performer)}};
        if (!disc.discId.empty()) kv.push_back({"Disc ID", js(disc.discId)});
        if (disc.genreCode > 0 || !disc.genreText.empty()) kv.push_back({"Genre", js(cd::genreName(disc.genreCode)) + (disc.genreText.empty() ? "" : " / " + js(disc.genreText))});
        if (!disc.text.songwriter.empty()) kv.push_back({"Songwriter", js(disc.text.songwriter)});
        if (!disc.text.composer.empty()) kv.push_back({"Composer", js(disc.text.composer)});
        if (!disc.text.arranger.empty()) kv.push_back({"Arranger", js(disc.text.arranger)});
        if (!disc.text.message.empty()) kv.push_back({"Message", js(disc.text.message)});
        sh.fields(kv);
    }
    auto issues = cd::validate(disc);
    juce::StringArray all = notes;
    for (auto& i : issues) all.add(juce::String(i.level == cd::Issue::Error ? "ERROR: " : "Warning: ") + js(i.message));
    sh.heading("Validation");
    sh.paragraph(all.isEmpty() ? juce::String("Red Book checks passed: no errors or warnings.") : all.joinIntoString("\n"));
}

// ------------------------------------------------------------------ cutting sheet

void cuttingSheet(Sheet& sh, const Session& s, const Product& p, const std::vector<const UnitRender*>& units, const juce::File& folder) {
    const auto& spec = productSpec(p.kind);
    const bool tape = p.kind == ProductKind::Cassette;
    for (auto* u : units) {
        juce::String sub = p.name + "  |  " + unitLabel(p, u->unit, true);
        sh.setContinuationSubtitle(sub + " (cont.)");
        sh.startPage(sub);
        sh.heading("Release");
        sh.fields({{"Artist", s.discPerformer},
                   {"Title", s.discTitle},
                   {"Label", s.label},
                   {"Catalogue no.", p.catalogue.isNotEmpty() ? p.catalogue : s.catalogue},
                   {"UPC / EAN", p.upc.isNotEmpty() ? p.upc : s.upcEan},
                   {"Mastered by", s.engineer}});
        sh.heading(tape ? "Side" : "Side and lacquer");
        const auto f = folder.getChildFile(u->path);
        std::vector<std::pair<juce::String, juce::String>> kv = {
            {"Format", juce::String(spec.name)},
            {"Side", unitLabel(p, u->unit, true) + " (" + juce::String(u->unit + 1) + " of " + juce::String(p.units.size()) + ")"},
            {"Side length", mmss(u->window.length()) + "  (target " + mmss(p.limit()) + ")"},
            {"Programme time", hms(u->window.start) + " to " + hms(u->window.end)},
            {"Fade in", fadeText(u->window.fadeIn, u->window.inShape)},
            {"Fade out", fadeText(u->window.fadeOut, u->window.outShape)},
            {"Loudness", db(u->lufs, "LUFS")},
            {"True peak", db(u->truePeakDb, "dBTP")},
            {"File", u->path},
            {"Resolution", juce::String(u->rate / 1000.0, 1) + " kHz / " + juce::String(u->bits) + "-bit WAV"},
            {"File MD5", md5Of(f)},
            {"File size", f.existsAsFile() ? sizeText(f.getSize()) : juce::String("-")}};
        if (!tape) kv.push_back({"EQ", "Flat (RIAA at the cutter)"});
        sh.fields(kv);
        if (u->unit > 0 && p.units[u->unit - 1].end > u->window.start + 0.01)
            sh.paragraph("Overlap: the first " + juce::String(p.units[u->unit - 1].end - u->window.start, 1) + " s of this side repeat the end of " +
                         unitLabel(p, u->unit - 1) + " so the music can fade back in.");
        if (u->unit + 1 < p.units.size() && u->window.end > p.units[u->unit + 1].start + 0.01)
            sh.paragraph("Overlap: the last " + juce::String(u->window.end - p.units[u->unit + 1].start, 1) + " s of this side continue on " +
                         unitLabel(p, u->unit + 1) + ".");

        sh.heading("Tracks (times from the start of the side)");
        std::vector<std::vector<juce::String>> rows;
        int band = 0;
        for (auto& t : unitTracks(s, u->window)) {
            juce::String note;
            if (t.continuesFromPrevious) note << "continued from previous side";
            if (t.continuesOnNext) note << (note.isEmpty() ? "" : "; ") << "continues on next side";
            rows.push_back({juce::String(++band), juce::String(t.number), t.title, mmss(t.start), mmss(t.end - t.start), t.isrc, note});
        }
        sh.table({{"Band", 30, 2}, {"Trk", 26, 2}, {"Title", 190}, {"Start", 44, 2}, {"Length", 44, 2}, {"ISRC", 80}, {"Note", 120}}, rows);
        if (!tape) {
            const juce::String blank = "______________________";
            sh.fields({{"Matrix / run-out", blank}, {"Lacquer no.", blank}, {"Cut level", blank}, {"Groove pitch", blank}});
            sh.notesBox("Cutting notes", 4);
        } else {
            const juce::String blank = "______________________";
            sh.fields({{"Tape type", blank}, {"Noise reduction", blank}, {"Reference level", blank}, {"Leader", blank}});
            sh.notesBox("Duplication notes", 4);
        }
    }
}

// ------------------------------------------------------------------ label copy

void labelCopy(Sheet& sh, const Session& s, const std::vector<Credit>& cr, const DeliveryData& d) {
    sh.setContinuationSubtitle(projectName(s) + " (cont.)");
    sh.startPage(projectName(s));
    sh.heading("Release");
    juce::String genre = cd::genreName(s.cdTextGenre);
    if (s.genreText.isNotEmpty()) genre << (genre.isEmpty() ? "" : " / ") << s.genreText;
    sh.fields({{"Artist", s.discPerformer},
               {"Title", s.discTitle},
               {"Label", s.label},
               {"Catalogue no.", s.catalogue},
               {"UPC / EAN", s.upcEan},
               {"Release date", s.releaseDate},
               {"Genre", genre},
               {"Total time", mmss(s.end())},
               {"(P) line", s.pLine},
               {"(C) line", s.cLine},
               {"Mastered by", s.engineer},
               {"Client", s.client}});
    sh.heading("Tracks and credits");
    std::vector<std::vector<juce::String>> rows;
    bool anyExplicit = false;
    for (auto& c : cr) {
        juce::StringArray writers;
        for (auto& w : {c.songwriter, c.composer}) if (w.isNotEmpty() && !writers.contains(w)) writers.add(w);
        rows.push_back({juce::String(c.number), c.title + (c.explicitContent ? " [E]" : ""), c.performer, writers.joinIntoString(", "), c.arranger,
                        c.publisher, c.pro, c.isrc, mmss(c.length)});
        anyExplicit = anyExplicit || c.explicitContent;
    }
    sh.table({{"#", 20, 2}, {"Title", 120}, {"Artist", 80}, {"Writers", 90}, {"Arranger", 60}, {"Publisher", 70}, {"PRO", 40}, {"ISRC", 78}, {"Time", 36, 2}}, rows, 7.5);
    if (anyExplicit) sh.paragraph("[E] = explicit content.");

    for (int pi = 0; pi < s.products.size(); ++pi) {
        const auto& p = s.products.getReference(pi);
        if (p.units.isEmpty()) continue;
        sh.heading(p.name + " - " + productSpec(p.kind).name + (p.catalogue.isNotEmpty() ? "  (" + p.catalogue + ")" : juce::String()));
        std::vector<std::vector<juce::String>> pr;
        for (int ui = 0; ui < p.units.size(); ++ui) {
            const auto& u = p.units.getReference(ui);
            bool first = true;
            for (auto& t : unitTracks(s, u)) {
                juce::String title = t.title;
                if (t.continuesFromPrevious) title << " (cont.)";
                if (t.continuesOnNext) title << " (continues)";
                pr.push_back({first ? unitLabel(p, ui, true) : juce::String(), juce::String(t.number), title, mmss(t.end - t.start)});
                first = false;
            }
            pr.push_back({juce::String(), juce::String(), "Total " + unitLabel(p, ui), mmss(u.length())});
        }
        sh.table({{productSpec(p.kind).sides ? "Side" : "Disc", 100}, {"Trk", 30, 2}, {"Title", 280}, {"Time", 50, 2}}, pr);
    }
    if (s.notes.isNotEmpty()) { sh.heading("Notes"); sh.paragraph(s.notes); }
    juce::ignoreUnused(d);
}

// ------------------------------------------------------------------ manifest

void manifest(Sheet& sh, const Session& s, const StudioProfile& st, const DeliveryData& d, const juce::StringArray& files, const juce::File& folder,
              juce::Array<juce::var>& jsonFiles) {
    sh.setContinuationSubtitle(projectName(s) + " (cont.)");
    sh.startPage(projectName(s));
    sh.heading("Delivery");
    sh.fields({{"Project", projectName(s)},
               {"Client", s.client},
               {"Label", s.label},
               {"Catalogue no.", s.catalogue},
               {"Mastered by", s.engineer},
               {"Studio", st.name},
               {"Delivered", juce::Time::getCurrentTime().toString(true, true)},
               {"Folder", folder.getFileName()}});
    sh.heading("Render");
    juce::StringArray lines;
    lines.addLines(d.settingsText.trim());
    lines.add("Engine: 64-bit float mix, region effects and master chain");
    if (d.fullDdp) lines.add(juce::String("Full-programme DDP: ") + (d.ddpVerified ? "written and verified" : "NOT VERIFIED"));
    for (auto& u : d.units)
        lines.add(u.longLabel + ": " + (u.disc.tracks.empty() ? juce::String("side file") : juce::String("DDP")) + ", " + mmss(u.window.length()) + ", " +
                  db(u.lufs, "LUFS") + ", " + db(u.truePeakDb, "dBTP") + (u.verified ? "" : "  (NOT VERIFIED)"));
    lines.add("Programme loudness: " + db(d.qc.integratedLufs, "LUFS") + ", true peak " + db(d.qc.truePeakDb, "dBTP") + ", LRA " + juce::String(d.qc.lra, 1) + " LU");
    sh.paragraph(lines.joinIntoString("\n"));
    sh.heading("Files");
    std::vector<std::vector<juce::String>> rows;
    juce::int64 totalBytes = 0;
    for (auto& rel : files) {
        auto f = folder.getChildFile(rel);
        if (!f.existsAsFile()) continue;
        auto md5 = md5Of(f);
        totalBytes += f.getSize();
        rows.push_back({rel, sizeText(f.getSize()), md5});
        auto* o = new juce::DynamicObject();
        o->setProperty("path", rel);
        o->setProperty("bytes", f.getSize());
        o->setProperty("md5", md5);
        jsonFiles.add(o);
    }
    sh.table({{"File", 300}, {"Size", 60, 2}, {"MD5", 170}}, rows, 7);
    sh.paragraph(juce::String(int(rows.size())) + " files, " + sizeText(totalBytes) + ". Verify with: md5 -r <file> (macOS) or md5sum <file>.");
}

juce::String csvField(const juce::String& v) {
    if (v.containsAnyOf(",\"\n\r")) return "\"" + v.replace("\"", "\"\"") + "\"";
    return v;
}

}  // namespace

PaperworkResult writePaperwork(const Session& s, const DeliveryData& d, const StudioProfile& st, const juce::File& folder) {
    PaperworkResult res;
    auto dir = folder.getChildFile("Paperwork");
    if (!dir.createDirectory()) { res.error = "Couldn't create " + dir.getFullPathName(); return res; }
    const auto logo = logoJpeg(st.logo);
    const auto project = projectName(s);
    auto saveSheet = [&](Sheet& sh, const juce::String& name) {
        auto f = dir.getChildFile(juce::File::createLegalFileName(name) + ".pdf");
        if (!sh.save(f)) { res.error = "Couldn't write " + f.getFullPathName(); return false; }
        res.files.add(f.getRelativePathFrom(folder));
        return true;
    };

    // 1. CD PQ sheets.
    if (d.fullDdp) {
        Sheet sh(st, logo, "CD PQ Sheet", project);
        pqSheet(sh, s, d.disc, "Full programme", 1, 1, s.catalogue, s.upcEan, d.files.isEmpty() ? juce::String() : juce::String("DDP"), d.ddpVerified, {},
                d.discNotes, d.files.isEmpty() ? nullptr : &d.qc, d.dither, 0);
        if (!saveSheet(sh, "PQ Sheet - " + (s.catalogue.isNotEmpty() ? s.catalogue : juce::String("CD")))) return res;
    }
    for (int pi = 0; pi < s.products.size(); ++pi) {
        const auto& p = s.products.getReference(pi);
        std::vector<const UnitRender*> units;
        for (auto& u : d.units)
            if (u.product == pi) units.push_back(&u);
        if (units.empty()) continue;
        if (productSpec(p.kind).cdFormat) {
            Sheet sh(st, logo, "CD PQ Sheet", project);
            for (auto* u : units)
                pqSheet(sh, s, u->disc, p.name, u->unit + 1, p.units.size(), p.catalogue.isNotEmpty() ? p.catalogue : s.catalogue,
                        p.upc.isNotEmpty() ? p.upc : s.upcEan, u->path, u->verified, u->imageMd5, u->notes, nullptr, d.dither, u->lead);
            if (!saveSheet(sh, "PQ Sheet - " + p.name)) return res;
        } else {
            Sheet sh(st, logo, p.kind == ProductKind::Cassette ? "Cassette Duplication Sheet" : "Vinyl Cutting Sheet", project);
            cuttingSheet(sh, s, p, units, folder);
            if (!saveSheet(sh, (p.kind == ProductKind::Cassette ? "Duplication Sheet - " : "Cutting Sheet - ") + p.name)) return res;
        }
    }

    // 2. Label copy.
    const auto cr = credits(s);
    {
        Sheet sh(st, logo, "Label Copy", project);
        labelCopy(sh, s, cr, d);
        if (!saveSheet(sh, "Label Copy")) return res;
    }

    // 3. CSV of track metadata.
    {
        juce::String csv = "Track,Title,Artist,ISRC,Songwriter,Composer,Arranger,Publisher,PRO,Explicit,Start,Length";
        for (auto& p : s.products) csv << "," << csvField(p.name);
        csv << "\r\n";
        for (auto& c : cr) {
            csv << c.number << "," << csvField(c.title) << "," << csvField(c.performer) << "," << c.isrc << "," << csvField(c.songwriter) << ","
                << csvField(c.composer) << "," << csvField(c.arranger) << "," << csvField(c.publisher) << "," << csvField(c.pro) << ","
                << (c.explicitContent ? "Yes" : "No") << "," << hms(c.start) << "," << mmss(c.length);
            for (auto& p : s.products) {
                juce::StringArray where;
                for (int ui = 0; ui < p.units.size(); ++ui)
                    for (auto& t : unitTracks(s, p.units.getReference(ui)))
                        if (t.number == c.number) where.add(unitLabel(p, ui, true));
                csv << "," << csvField(where.joinIntoString(" / "));
            }
            csv << "\r\n";
        }
        auto f = dir.getChildFile("Track Metadata.csv");
        if (!f.replaceWithText(csv, false, false, "\r\n")) { res.error = "Couldn't write " + f.getFullPathName(); return res; }
        res.files.add(f.getRelativePathFrom(folder));
    }

    // 4. Manifest (lists the masters plus the paperwork written so far) and the JSON.
    juce::StringArray all = d.files;
    for (auto& f : res.files) all.addIfNotAlreadyThere(f);
    juce::Array<juce::var> jsonFiles;
    {
        Sheet sh(st, logo, "Delivery Manifest", project);
        manifest(sh, s, st, d, all, folder, jsonFiles);
        if (!saveSheet(sh, "Delivery Manifest")) return res;
    }
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("format", "lathe-delivery");
        root->setProperty("version", 1);
        root->setProperty("generated", juce::Time::getCurrentTime().toISO8601(true));
        root->setProperty("studio", st.toVar());
        auto* rel = new juce::DynamicObject();
        rel->setProperty("artist", s.discPerformer);
        rel->setProperty("title", s.discTitle);
        rel->setProperty("label", s.label);
        rel->setProperty("catalogue", s.catalogue);
        rel->setProperty("upc", s.upcEan);
        rel->setProperty("releaseDate", s.releaseDate);
        rel->setProperty("pLine", s.pLine);
        rel->setProperty("cLine", s.cLine);
        rel->setProperty("genre", s.genreText.isNotEmpty() ? s.genreText : juce::String(cd::genreName(s.cdTextGenre)));
        rel->setProperty("client", s.client);
        rel->setProperty("engineer", s.engineer);
        rel->setProperty("lengthSeconds", s.end());
        rel->setProperty("integratedLufs", std::isfinite(d.qc.integratedLufs) ? juce::var(d.qc.integratedLufs) : juce::var());
        rel->setProperty("truePeakDb", std::isfinite(d.qc.truePeakDb) ? juce::var(d.qc.truePeakDb) : juce::var());
        root->setProperty("release", rel);
        juce::Array<juce::var> tracks;
        for (auto& c : cr) {
            auto* o = new juce::DynamicObject();
            o->setProperty("number", c.number);
            o->setProperty("title", c.title);
            o->setProperty("artist", c.performer);
            o->setProperty("isrc", c.isrc);
            o->setProperty("songwriter", c.songwriter);
            o->setProperty("composer", c.composer);
            o->setProperty("arranger", c.arranger);
            o->setProperty("publisher", c.publisher);
            o->setProperty("pro", c.pro);
            o->setProperty("explicit", c.explicitContent);
            o->setProperty("start", c.start);
            o->setProperty("length", c.length);
            tracks.add(o);
        }
        root->setProperty("tracks", tracks);
        juce::Array<juce::var> prods;
        for (int pi = 0; pi < s.products.size(); ++pi) {
            const auto& p = s.products.getReference(pi);
            auto* o = new juce::DynamicObject();
            o->setProperty("name", p.name);
            o->setProperty("format", juce::String(productSpec(p.kind).name));
            o->setProperty("catalogue", p.catalogue);
            o->setProperty("upc", p.upc);
            juce::Array<juce::var> ul;
            for (int ui = 0; ui < p.units.size(); ++ui) {
                const auto& u = p.units.getReference(ui);
                auto* uo = new juce::DynamicObject();
                uo->setProperty("label", unitLabel(p, ui, true));
                uo->setProperty("start", u.start);
                uo->setProperty("end", u.end);
                uo->setProperty("fadeIn", u.fadeIn);
                uo->setProperty("fadeOut", u.fadeOut);
                for (auto& r : d.units)
                    if (r.product == pi && r.unit == ui) {
                        uo->setProperty("path", r.path);
                        uo->setProperty("verified", r.verified);
                        if (std::isfinite(r.lufs)) uo->setProperty("integratedLufs", r.lufs);
                        if (std::isfinite(r.truePeakDb)) uo->setProperty("truePeakDb", r.truePeakDb);
                        if (!r.disc.tracks.empty()) {
                            juce::Array<juce::var> pq;
                            for (size_t i = 0; i < r.disc.tracks.size(); ++i) {
                                const auto& t = r.disc.tracks[i];
                                auto* to = new juce::DynamicObject();
                                to->setProperty("track", int(i + 1));
                                to->setProperty("title", js(t.text.title));
                                to->setProperty("isrc", js(t.isrc));
                                to->setProperty("start", msf(t.startSector()));
                                if (t.find(0)) to->setProperty("pause", msf(t.find(0)->sector));
                                pq.add(to);
                            }
                            uo->setProperty("pq", pq);
                            uo->setProperty("leadOut", msf(r.disc.leadOutSector));
                        }
                    }
                juce::Array<juce::var> ts;
                for (auto& t : unitTracks(s, u)) {
                    auto* to = new juce::DynamicObject();
                    to->setProperty("track", t.number);
                    to->setProperty("title", t.title);
                    to->setProperty("start", t.start);
                    to->setProperty("length", t.end - t.start);
                    to->setProperty("continuedFromPrevious", t.continuesFromPrevious);
                    to->setProperty("continuesOnNext", t.continuesOnNext);
                    ts.add(to);
                }
                uo->setProperty("tracks", ts);
                ul.add(uo);
            }
            o->setProperty("units", ul);
            prods.add(o);
        }
        root->setProperty("products", prods);
        if (d.fullDdp) {
            juce::Array<juce::var> pq;
            for (size_t i = 0; i < d.disc.tracks.size(); ++i) {
                const auto& t = d.disc.tracks[i];
                auto* to = new juce::DynamicObject();
                to->setProperty("track", int(i + 1));
                to->setProperty("title", js(t.text.title));
                to->setProperty("isrc", js(t.isrc));
                to->setProperty("start", msf(t.startSector()));
                if (t.find(0)) to->setProperty("pause", msf(t.find(0)->sector));
                pq.add(to);
            }
            root->setProperty("cdPq", pq);
        }
        root->setProperty("files", jsonFiles);
        auto f = dir.getChildFile("Delivery.json");
        if (!f.replaceWithText(juce::JSON::toString(juce::var(root)))) { res.error = "Couldn't write " + f.getFullPathName(); return res; }
        res.files.add(f.getRelativePathFrom(folder));
    }
    return res;
}

}  // namespace lathe
