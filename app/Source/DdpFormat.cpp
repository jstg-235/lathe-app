#include "DdpFormat.h"

namespace lathe {

namespace {
class DdpImageReader : public juce::AudioFormatReader {
public:
    DdpImageReader(juce::InputStream* in) : AudioFormatReader(in, "DDP audio image") {
        sampleRate = 44100;
        bitsPerSample = 16;
        numChannels = 2;
        usesFloatingPointData = false;
        lengthInSamples = in->getTotalLength() / 4;
    }

    bool readSamples(int* const* dest, int numDest, int startOffset, juce::int64 startSample, int numSamples) override {
        clearSamplesBeyondAvailableLength(dest, numDest, startOffset, startSample, numSamples, lengthInSamples);
        if (numSamples <= 0) return true;
        input->setPosition(startSample * 4);
        buf_.ensureSize(size_t(numSamples) * 4);
        auto got = input->read(buf_.getData(), numSamples * 4);
        int frames = int(got / 4);
        auto* p = static_cast<const uint8_t*>(buf_.getData());
        for (int ch = 0; ch < juce::jmin(2, numDest); ++ch) {
            if (!dest[ch]) continue;
            for (int i = 0; i < frames; ++i) {
                int16_t v = int16_t(p[i * 4 + ch * 2] | (p[i * 4 + ch * 2 + 1] << 8));
                dest[ch][startOffset + i] = int(v) << 16;   // JUCE int readers are left-justified 32-bit
            }
            for (int i = frames; i < numSamples; ++i) dest[ch][startOffset + i] = 0;
        }
        return true;
    }

private:
    juce::MemoryBlock buf_;
};
}  // namespace

juce::AudioFormatReader* DdpImageFormat::createReaderFor(juce::InputStream* in, bool deleteStreamIfOpeningFails) {
    // Only accept files that are a whole number of CD sectors: plain .dat files from elsewhere won't qualify.
    if (in && in->getTotalLength() > 0 && in->getTotalLength() % 2352 == 0) return new DdpImageReader(in);
    if (deleteStreamIfOpeningFails) delete in;
    return nullptr;
}

bool sessionFromDdp(const juce::File& folder, Session& s, juce::String& error, juce::StringArray& notes) {
    auto r = ddp::readDdp(folder.getFullPathName().toStdString());
    if (!r.ok) { error = juce::String::fromUTF8(r.error.c_str()); return false; }
    for (auto& n : r.notes) notes.add(juce::String::fromUTF8(n.c_str()));
    s = Session();
    s.discTitle = juce::String::fromUTF8(r.disc.text.title.c_str());
    s.discPerformer = juce::String::fromUTF8(r.disc.text.performer.c_str());
    s.upcEan = juce::String(r.disc.upcEan);
    s.masterId = juce::String(r.disc.masterId);
    s.projectRate = 44100;
    juce::File image(juce::String::fromUTF8(r.imageFile.c_str()));
    Region reg = s.makeRegion(image, 44100, r.imageSectors * 588);
    reg.name = (s.discTitle.isNotEmpty() ? s.discTitle : folder.getFileName()) + " (DDP image)";
    reg.start = 0;
    s.regions.add(reg);
    const double frame = 1.0 / 75.0;
    for (size_t t = 0; t < r.disc.tracks.size(); ++t) {
        auto& tr = r.disc.tracks[t];
        for (auto& ix : tr.indexes) {
            double at = double(ix.sector) * frame;
            if (ix.number == 0) {
                if (t > 0) s.markers.add({s.nextId++, Marker::PauseStart, at, reg.id});
                continue;
            }
            if (ix.number >= 2) { s.markers.add({s.nextId++, Marker::Index, at, reg.id}); continue; }
            Marker m;
            m.id = s.nextId++;
            m.type = Marker::TrackStart;
            m.t = at;
            m.attachedRegion = reg.id;
            m.isrc = tr.isrc;
            m.copyPermitted = tr.copyPermitted;
            m.preEmphasis = tr.preEmphasis;
            m.scms = tr.scms;
            m.text = tr.text;
            if (m.text.title.empty()) m.text.title = "Track " + std::to_string(t + 1);
            s.markers.add(m);
        }
    }
    s.sortMarkers();
    if (!r.checksumsChecked) notes.add("No checksum file: integrity not confirmed.");
    else if (!r.checksumsOk) notes.add("CHECKSUM MISMATCH: this DDP is damaged.");
    return true;
}

}  // namespace lathe
