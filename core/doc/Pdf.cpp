#include "Pdf.h"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace lathe::doc {

namespace {

// Helvetica and Helvetica-Bold advance widths (1/1000 em), ASCII 32..126, from the Adobe AFMs.
const short kHelv[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
    556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556,
    556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};
const short kHelvB[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
    556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333, 556, 611, 556, 611, 556, 333, 611,
    611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584};

// Width of a WinAnsi byte: accented letters take their base letter's width.
int charWidth(unsigned char c, bool bold) {
    const short* t = bold ? kHelvB : kHelv;
    auto base = [&](char b) { return int(t[b - 32]); };
    if (c >= 32 && c <= 126) return t[c - 32];
    if (c >= 0xC0 && c <= 0xC5) return base('A');
    if (c == 0xC6) return 1000;
    if (c == 0xC7) return base('C');
    if (c >= 0xC8 && c <= 0xCB) return base('E');
    if (c >= 0xCC && c <= 0xCF) return base('I');
    if (c == 0xD0) return base('D');
    if (c == 0xD1) return base('N');
    if ((c >= 0xD2 && c <= 0xD6) || c == 0xD8) return base('O');
    if (c == 0xD7) return 584;
    if (c >= 0xD9 && c <= 0xDC) return base('U');
    if (c == 0xDD) return base('Y');
    if (c == 0xDE) return base('P');
    if (c == 0xDF) return bold ? 611 : 611;
    if (c >= 0xE0 && c <= 0xE5) return base('a');
    if (c == 0xE6) return 889;
    if (c == 0xE7) return base('c');
    if (c >= 0xE8 && c <= 0xEB) return base('e');
    if (c >= 0xEC && c <= 0xEF) return 278;
    if (c == 0xF1) return base('n');
    if ((c >= 0xF2 && c <= 0xF6) || c == 0xF8) return base('o');
    if (c >= 0xF9 && c <= 0xFC) return base('u');
    if (c == 0xFD || c == 0xFF) return base('y');
    if (c == 0xA9 || c == 0xAE) return 737;
    if (c == 0x91 || c == 0x92) return bold ? 278 : 222;
    if (c == 0x93 || c == 0x94) return bold ? 500 : 333;
    if (c == 0x95) return 350;
    if (c == 0x96) return 556;
    if (c == 0x97 || c == 0x85 || c == 0x89) return 1000;
    if (c == 0xB0) return 400;
    if (c == 0xB7) return 278;
    return 556;
}

std::string escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '(' || c == ')' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 32) o += ' ';
        else o += char(c);
    }
    return o;
}

bool jpegInfo(const std::vector<uint8_t>& d, int& w, int& h, int& comps) {
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 9 < d.size()) {
        if (d[i] != 0xFF) { ++i; continue; }
        uint8_t m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { ++i; continue; }
        size_t len = (size_t(d[i + 2]) << 8) | d[i + 3];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            h = (d[i + 5] << 8) | d[i + 6];
            w = (d[i + 7] << 8) | d[i + 8];
            comps = d[i + 9];
            return w > 0 && h > 0 && (comps == 1 || comps == 3);
        }
        i += 2 + len;
    }
    return false;
}

}  // namespace

std::string Pdf::toWinAnsi(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        uint8_t c = uint8_t(in[i]);
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { out += '?'; ++i; continue; }
        if (i + size_t(len) > in.size()) { out += '?'; break; }
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (uint8_t(in[i + size_t(k)]) & 0x3F);
        i += size_t(len);
        if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) { out += char(cp); continue; }
        switch (cp) {
            case 0x20AC: out += char(0x80); break;
            case 0x2026: out += char(0x85); break;
            case 0x2018: out += char(0x91); break;
            case 0x2019: out += char(0x92); break;
            case 0x201C: out += char(0x93); break;
            case 0x201D: out += char(0x94); break;
            case 0x2022: out += char(0x95); break;
            case 0x2013: out += char(0x96); break;
            case 0x2014: out += char(0x97); break;
            case 0x2122: out += char(0x99); break;
            case 0x2117: out += "(P)"; break;
            case 0x2032: out += '\''; break;
            case 0x2033: out += '"'; break;
            default: out += '?';
        }
    }
    return out;
}

Pdf::Pdf(double w, double h) : w_(w), h_(h) {}

void Pdf::setInfo(const std::string& title, const std::string& author) { title_ = title; author_ = author; }

std::string Pdf::num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() || s == "-" ? "0" : s;
}

std::string& Pdf::cur() {
    if (pages_.empty()) newPage();
    return pages_.back();
}

void Pdf::newPage() { pages_.emplace_back(); }

void Pdf::setFont(bool bold, double size) { bold_ = bold; size_ = size; }

void Pdf::setColor(double r, double g, double b) { cur() += num(r) + " " + num(g) + " " + num(b) + " rg\n"; }
void Pdf::setStroke(double r, double g, double b) { cur() += num(r) + " " + num(g) + " " + num(b) + " RG\n"; }

double Pdf::textWidth(const std::string& utf8, bool bold, double size) {
    double w = 0;
    for (unsigned char c : toWinAnsi(utf8)) w += charWidth(c, bold);
    return w * size / 1000.0;
}

double Pdf::textWidth(const std::string& utf8) const { return textWidth(utf8, bold_, size_); }

void Pdf::text(double x, double y, const std::string& utf8, int align) {
    if (utf8.empty()) return;
    double tw = textWidth(utf8);
    if (align == 1) x -= tw / 2;
    else if (align == 2) x -= tw;
    cur() += "BT /" + std::string(bold_ ? "F2" : "F1") + " " + num(size_) + " Tf " + num(x) + " " + num(h_ - y) + " Td (" +
             escape(toWinAnsi(utf8)) + ") Tj ET\n";
}

std::string Pdf::fit(const std::string& utf8, double maxW) const {
    if (textWidth(utf8) <= maxW) return utf8;
    // Trim whole UTF-8 characters until it fits with an ellipsis.
    std::string s = utf8;
    while (!s.empty()) {
        do { s.pop_back(); } while (!s.empty() && (uint8_t(s.back()) & 0xC0) == 0x80);
        if (!s.empty() && (uint8_t(s.back()) & 0xC0) == 0xC0) s.pop_back();
        if (textWidth(s + "\xE2\x80\xA6") <= maxW) return s + "\xE2\x80\xA6";
    }
    return {};
}

std::vector<std::string> Pdf::wrap(const std::string& utf8, double maxW) const {
    std::vector<std::string> lines;
    std::string para;
    auto flushPara = [&](const std::string& p) {
        std::string line, word;
        auto pushWord = [&]() {
            if (word.empty()) return;
            std::string t = line.empty() ? word : line + " " + word;
            if (textWidth(t) <= maxW || line.empty()) line = t;
            else { lines.push_back(line); line = word; }
            word.clear();
        };
        for (char c : p) {
            if (c == ' ') pushWord();
            else word += c;
        }
        pushWord();
        lines.push_back(line);
    };
    for (char c : utf8) {
        if (c == '\n') { flushPara(para); para.clear(); }
        else para += c;
    }
    flushPara(para);
    for (auto& l : lines)
        if (textWidth(l) > maxW) l = fit(l, maxW);
    return lines;
}

void Pdf::line(double x1, double y1, double x2, double y2, double lw) {
    cur() += num(lw) + " w " + num(x1) + " " + num(h_ - y1) + " m " + num(x2) + " " + num(h_ - y2) + " l S\n";
}

void Pdf::rect(double x, double y, double w, double h, bool fill, bool stroke, double lw) {
    if (!fill && !stroke) return;
    cur() += num(lw) + " w " + num(x) + " " + num(h_ - y - h) + " " + num(w) + " " + num(h) + " re " + (fill && stroke ? "B" : fill ? "f" : "S") + "\n";
}

int Pdf::addJpeg(const std::vector<uint8_t>& jpeg) {
    Img im;
    if (!jpegInfo(jpeg, im.w, im.h, im.comps)) return -1;
    im.data = jpeg;
    images_.push_back(std::move(im));
    return int(images_.size()) - 1;
}

bool Pdf::imageSize(int id, int& w, int& h) const {
    if (id < 0 || id >= int(images_.size())) return false;
    w = images_[size_t(id)].w;
    h = images_[size_t(id)].h;
    return true;
}

void Pdf::image(int id, double x, double y, double w, double h) {
    if (id < 0 || id >= int(images_.size())) return;
    cur() += "q " + num(w) + " 0 0 " + num(h) + " " + num(x) + " " + num(h_ - y - h) + " cm /Im" + std::to_string(id) + " Do Q\n";
}

std::vector<uint8_t> Pdf::finish() {
    if (pages_.empty()) newPage();
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets;
    std::vector<std::string> objs;   // index = object number - 1, filled in order below
    // Object numbering: 1 catalog, 2 pages, 3 F1, 4 F2, 5 info, then images, then page+content pairs.
    const int firstImage = 6;
    const int firstPage = firstImage + int(images_.size());
    auto add = [&](const std::string& body) {
        offsets.push_back(out.size());
        out += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    add("<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids;
    for (size_t i = 0; i < pages_.size(); ++i) kids += std::to_string(firstPage + int(i) * 2) + " 0 R ";
    add("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages_.size()) + " >>");
    add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
    add("<< /Title (" + escape(toWinAnsi(title_)) + ") /Author (" + escape(toWinAnsi(author_)) + ") /Producer (Lathe) >>");
    std::string xobjs;
    for (size_t i = 0; i < images_.size(); ++i) {
        const auto& im = images_[i];
        std::string body = "<< /Type /XObject /Subtype /Image /Width " + std::to_string(im.w) + " /Height " + std::to_string(im.h) +
                           " /ColorSpace " + (im.comps == 1 ? "/DeviceGray" : "/DeviceRGB") +
                           " /BitsPerComponent 8 /Filter /DCTDecode /Length " + std::to_string(im.data.size()) + " >>\nstream\n";
        body.append(reinterpret_cast<const char*>(im.data.data()), im.data.size());
        body += "\nendstream";
        add(body);
        xobjs += "/Im" + std::to_string(i) + " " + std::to_string(firstImage + int(i)) + " 0 R ";
    }
    for (size_t i = 0; i < pages_.size(); ++i) {
        const int contentObj = firstPage + int(i) * 2 + 1;
        add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + num(w_) + " " + num(h_) + "] /Resources << /Font << /F1 3 0 R /F2 4 0 R >>" +
            (xobjs.empty() ? std::string() : " /XObject << " + xobjs + ">>") + " >> /Contents " + std::to_string(contentObj) + " 0 R >>");
        add("<< /Length " + std::to_string(pages_[i].size()) + " >>\nstream\n" + pages_[i] + "endstream");
    }
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
    for (auto o : offsets) {
        char b[24];
        std::snprintf(b, sizeof b, "%010zu 00000 n \n", o);
        out += b;
    }
    out += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R /Info 5 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return std::vector<uint8_t>(out.begin(), out.end());
}

bool Pdf::save(const std::string& path) {
    auto d = finish();
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
    return bool(f);
}

}  // namespace lathe::doc
