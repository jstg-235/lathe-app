// Reads a DDP audio image (IMAGE.DAT: headerless 16-bit little-endian stereo PCM at
// 44.1 kHz) as an ordinary audio file, so a DDP can be played, imported and edited.
#pragma once

#include "Session.h"
#include "../../core/ddp/Ddp.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace lathe {

class DdpImageFormat : public juce::AudioFormat {
public:
    DdpImageFormat() : AudioFormat("DDP audio image", ".dat") {}
    juce::Array<int> getPossibleSampleRates() override { return {44100}; }
    juce::Array<int> getPossibleBitDepths() override { return {16}; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return false; }
    juce::AudioFormatReader* createReaderFor(juce::InputStream* in, bool deleteStreamIfOpeningFails) override;
    juce::AudioFormatWriter* createWriterFor(juce::OutputStream*, double, unsigned int, int, const juce::StringPairArray&, int) override {
        return nullptr;
    }
    using AudioFormat::createWriterFor;
};

// Builds an editable session from a DDP: one region over the whole image, PQ markers
// (tracks, pauses, sub-indexes), CD-Text, ISRCs, flags and UPC. Returns false with `error`.
bool sessionFromDdp(const juce::File& ddpFolder, Session& out, juce::String& error, juce::StringArray& notes);

}  // namespace lathe
