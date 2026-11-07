// Delivery paperwork, written beside the rendered masters:
//  - CD PQ sheet per disc (full programme and each disc of a multi-CD product)
//  - Cutting / duplication sheet per vinyl or cassette product (one page per side)
//  - Label copy (release metadata, credits, per-product track lists)
//  - Delivery manifest (every file with size and MD5, render settings, verification)
//  - Track Metadata.csv and Delivery.json for the label's systems
// All PDFs carry the studio's name, contact details and logo.
#pragma once

#include "Exporter.h"
#include "Session.h"

namespace lathe {

struct StudioProfile {
    juce::String name, address, phone, email, website;
    juce::File logo;          // any image JUCE can read (PNG, JPEG, GIF)
    bool a4 = false;          // paper size: A4 or US Letter
    juce::var toVar() const;
    void fromVar(const juce::var& v);
    bool empty() const { return name.isEmpty() && logo == juce::File(); }
};

struct PaperworkResult {
    juce::StringArray files;  // relative to the output folder
    juce::String error;
};

PaperworkResult writePaperwork(const Session& s, const DeliveryData& d, const StudioProfile& studio, const juce::File& folder);

// Converts the logo to a JPEG on white (PDF-ready); empty if it can't be read.
std::vector<uint8_t> logoJpeg(const juce::File& f);

}  // namespace lathe
