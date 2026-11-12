// Exports in any delivery format: the whole programme, every track, an In-Out or chosen segments,
// as WAV (CD, 24-bit, float), MP3, AAC or Apple Lossless, tagged from the session's CD-Text.
#pragma once

#include "Encoders.h"
#include "Exporter.h"

namespace lathe {

struct DeliveryItem {
    juce::Range<double> range;   // timeline seconds
    juce::String name;           // file name, without extension
    DeliveryTags tags;
};

struct DeliveryRequest {
    DeliveryFormat format = DeliveryFormat::Wav24;
    juce::Array<DeliveryItem> items;
    juce::File folder;           // where the files go
    juce::File singleFile;       // or, for one item, exactly this file
    ExportSettings es;           // dither (16-bit), and whether the master chain runs
};

struct DeliveryResult {
    juce::String error;
    juce::StringArray files;     // full paths
    juce::StringArray warnings;  // e.g. a lossy file going over full scale
    juce::String summary() const;
};

// The tags that apply to the whole release (album, album artist, year, genre, cover).
DeliveryTags releaseTags(const Session& s);
// One item per CD track (index 01 to the next track's index 01, as on the disc), numbered and tagged.
juce::Array<DeliveryItem> trackItems(const Session& s);
// The whole programme as one item.
DeliveryItem programmeItem(const Session& s);

// Renders and encodes every item. Blocks: run it from a background job.
DeliveryResult runDelivery(Exporter& ex, const Session& s, const DeliveryRequest& rq, const Exporter::Keep& keep);

}  // namespace lathe
