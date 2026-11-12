#include "Delivery.h"

namespace lathe {

juce::String DeliveryResult::summary() const {
    juce::String t;
    t << files.size() << (files.size() == 1 ? " file:\n\n" : " files:\n\n");
    for (auto& f : files) t << juce::File(f).getFileName() << "\n";
    if (!files.isEmpty()) t << "\nin " << juce::File(files[0]).getParentDirectory().getFullPathName() << "\n";
    if (!warnings.isEmpty()) {
        t << "\nCHECK THESE\n";
        for (auto& w : warnings) t << w << "\n";
    }
    return t;
}

DeliveryTags releaseTags(const Session& s) {
    DeliveryTags t;
    t.album = s.discTitle;
    t.albumArtist = s.discPerformer;
    t.artist = s.discPerformer;
    t.genre = s.genreText;
    t.year = s.releaseDate.substring(0, 4).containsOnly("0123456789") ? s.releaseDate.substring(0, 4) : juce::String();
    return t;
}

juce::Array<DeliveryItem> trackItems(const Session& s) {
    juce::Array<DeliveryItem> out;
    const auto disc = s.toDisc();
    const auto base = releaseTags(s);
    const double secPerSector = double(cd::kSamplesPerSector) / cd::kSampleRate;
    for (size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& tr = disc.tracks[t];
        const double a = double(tr.startSector()) * secPerSector;
        const double b = double(t + 1 < disc.tracks.size() ? disc.tracks[t + 1].startSector() : disc.leadOutSector) * secPerSector;
        DeliveryItem it;
        it.range = {a, b};
        it.tags = base;
        it.tags.title = juce::String::fromUTF8(tr.text.title.c_str());
        if (tr.text.performer.size()) it.tags.artist = juce::String::fromUTF8(tr.text.performer.c_str());
        it.tags.composer = juce::String::fromUTF8((tr.text.composer.size() ? tr.text.composer : tr.text.songwriter).c_str());
        it.tags.isrc = juce::String(tr.isrc);
        it.tags.track = int(t + 1);
        it.tags.tracks = int(disc.tracks.size());
        it.name = juce::String(int(t + 1)).paddedLeft('0', 2) + " " + juce::File::createLegalFileName(it.tags.title.isNotEmpty() ? it.tags.title : "Track " + juce::String(int(t + 1)));
        out.add(it);
    }
    return out;
}

DeliveryItem programmeItem(const Session& s) {
    DeliveryItem it;
    it.range = {0.0, s.end()};
    it.tags = releaseTags(s);
    it.tags.title = s.discTitle;
    const auto stem = s.file.existsAsFile() ? s.file.getFileNameWithoutExtension() : juce::String("Programme");
    it.name = juce::File::createLegalFileName(s.discTitle.isNotEmpty() ? s.discTitle : stem);
    return it;
}

DeliveryResult runDelivery(Exporter& ex, const Session& s, const DeliveryRequest& rq, const Exporter::Keep& keep) {
    DeliveryResult res;
    juce::String why;
    if (!formatAvailable(rq.format, &why)) { res.error = why; return res; }
    if (rq.items.isEmpty()) { res.error = "Nothing to export."; return res; }
    const bool single = rq.items.size() == 1 && rq.singleFile != juce::File();
    if (!single) rq.folder.createDirectory();
    const auto f = rq.format;
    const double rate = renderRateFor(f, s.effectiveRate());
    const int n = rq.items.size();
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    for (int i = 0; i < n; ++i) {
        const auto& it = rq.items.getReference(i);
        auto out = single ? rq.singleFile.withFileExtension(formatExtension(f))
                          : rq.folder.getNonexistentChildFile(it.name, formatExtension(f), false);
        auto part = [&](double from, double span) { return [&, from, span](double p) { return keep((i + from + p * span) / n); }; };
        if (!isCompressed(f)) {
            ExportSettings es = rq.es;
            if (f != DeliveryFormat::Wav16Cd) es.shape = dsp::Ditherer::Shape::None;   // (dither is for the 16-bit CD files)
            const int bits = f == DeliveryFormat::Wav16Cd ? 16 : f == DeliveryFormat::Wav24 ? 24 : 32;
            res.error = ex.exportRangeWav(s, out, bits, es, it.range, part(0.0, 1.0), rate);
            if (res.error.isNotEmpty()) return res;
            res.files.add(out.getFullPathName());
            continue;
        }
        // Compressed: render a WAV for the encoder (24-bit for MP3, float otherwise), encode, check.
        auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lathe-encode", ".wav", false);
        ExportSettings es = rq.es;
        es.shape = dsp::Ditherer::Shape::None;
        const bool mp3 = f == DeliveryFormat::Mp3Cbr320 || f == DeliveryFormat::Mp3V0;
        res.error = ex.exportRangeWav(s, tmp, mp3 ? 24 : 32, es, it.range, part(0.0, 0.7), rate);
        if (res.error.isEmpty()) res.error = encodeFile(tmp, out, f, it.tags, [&] { return keep((i + 0.8) / n); });
        tmp.deleteFile();
        if (res.error.isNotEmpty()) return res;
        res.files.add(out.getFullPathName());
        if (f != DeliveryFormat::Alac) {
            const double tp = fileTruePeakDb(fm, out);
            if (std::isfinite(tp) && tp > 0.0)
                res.warnings.add(out.getFileName() + " peaks at +" + juce::String(tp, 2) +
                                 " dBTP after encoding, so it may clip on playback. Lowering the master by about " + juce::String(tp + 1.0, 1) +
                                 " dB for this format would leave a 1 dB margin.");
        }
        if (!keep(double(i + 1) / n)) { res.error = "Export cancelled."; return res; }
    }
    return res;
}

}  // namespace lathe
