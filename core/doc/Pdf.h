// A small PDF 1.4 writer for delivery paperwork: text in the standard Helvetica faces
// (WinAnsi encoding, so Latin-1 names print correctly), lines, filled boxes, and JPEG
// images (logos). No dependencies; coordinates are points from the top-left corner.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lathe::doc {

class Pdf {
public:
    Pdf(double pageWidth = 612, double pageHeight = 792);   // US Letter; A4 is 595.28 x 841.89

    double width() const { return w_; }
    double height() const { return h_; }
    void setInfo(const std::string& title, const std::string& author);

    void newPage();
    int pageCount() const { return int(pages_.size()); }

    void setFont(bool bold, double size);
    void setColor(double r, double g, double b);       // text and fill colour
    void setStroke(double r, double g, double b);
    // Text with its baseline at y. align: 0 left, 1 centre, 2 right (x is that edge).
    void text(double x, double y, const std::string& utf8, int align = 0);
    double textWidth(const std::string& utf8) const;    // in the current font
    static double textWidth(const std::string& utf8, bool bold, double size);
    // Shortens to fit maxWidth with an ellipsis.
    std::string fit(const std::string& utf8, double maxWidth) const;
    // Word-wraps to lines no wider than maxWidth.
    std::vector<std::string> wrap(const std::string& utf8, double maxWidth) const;

    void line(double x1, double y1, double x2, double y2, double lineWidth = 0.5);
    void rect(double x, double y, double w, double h, bool fill, bool stroke, double lineWidth = 0.5);

    // Registers a baseline JPEG (RGB or greyscale); returns an image id, or -1 if unreadable.
    int addJpeg(const std::vector<uint8_t>& jpeg);
    void image(int id, double x, double y, double w, double h);
    // Pixel size of a registered image.
    bool imageSize(int id, int& w, int& h) const;

    std::vector<uint8_t> finish();
    bool save(const std::string& path);

    // UTF-8 to WinAnsi (CP1252); characters outside it become '?', ℗ becomes "(P)".
    static std::string toWinAnsi(const std::string& utf8);

private:
    struct Img { std::vector<uint8_t> data; int w = 0, h = 0, comps = 3; };
    std::string& cur();
    static std::string num(double v);
    double w_, h_;
    bool bold_ = false;
    double size_ = 10;
    std::vector<std::string> pages_;
    std::vector<Img> images_;
    std::string title_, author_;
};

}  // namespace lathe::doc
