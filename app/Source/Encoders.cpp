#include "Encoders.h"

#include "../../core/dsp/Meters.h"

#if JUCE_MAC
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace lathe {

juce::String formatName(DeliveryFormat f) {
    switch (f) {
        case DeliveryFormat::Wav16Cd: return "WAV, 16-bit / 44.1 kHz (as on the CD)";
        case DeliveryFormat::Wav24: return "WAV, 24-bit at the project rate";
        case DeliveryFormat::Wav32f: return "WAV, 32-bit float at the project rate";
        case DeliveryFormat::Mp3Cbr320: return "MP3, 320 kbps";
        case DeliveryFormat::Mp3V0: return "MP3, VBR V0 (about 245 kbps)";
        case DeliveryFormat::Aac256: return "AAC (.m4a), 256 kbps";
        case DeliveryFormat::Alac: return "Apple Lossless (.m4a), 24-bit at the project rate";
    }
    return {};
}

juce::String formatExtension(DeliveryFormat f) {
    switch (f) {
        case DeliveryFormat::Mp3Cbr320:
        case DeliveryFormat::Mp3V0: return ".mp3";
        case DeliveryFormat::Aac256:
        case DeliveryFormat::Alac: return ".m4a";
        default: return ".wav";
    }
}

bool isCompressed(DeliveryFormat f) { return f >= DeliveryFormat::Mp3Cbr320; }

double renderRateFor(DeliveryFormat f, double projectRate) {
    const bool family441 = std::abs(std::fmod(projectRate, 44100.0)) < 1.0;
    switch (f) {
        case DeliveryFormat::Wav16Cd: return 44100.0;
        case DeliveryFormat::Mp3Cbr320:
        case DeliveryFormat::Mp3V0:
        case DeliveryFormat::Aac256: return family441 ? 44100.0 : 48000.0;   // what these formats are made at
        default: return 0.0;                                                  // the project's own rate
    }
}

juce::File lameExecutable() {
    const auto env = juce::SystemStats::getEnvironmentVariable("LATHE_LAME", {});
    if (env.isNotEmpty() && juce::File::isAbsolutePath(env)) return juce::File(env);
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
   #if JUCE_MAC
    // Lathe.app/Contents/MacOS/Lathe -> Lathe.app/Contents/Resources/lame
    auto inBundle = exe.getParentDirectory().getSiblingFile("Resources").getChildFile("lame");
    if (inBundle.existsAsFile()) return inBundle;
   #endif
    return exe.getSiblingFile("lame");
}

bool formatAvailable(DeliveryFormat f, juce::String* why) {
    if (f == DeliveryFormat::Mp3Cbr320 || f == DeliveryFormat::Mp3V0) {
        if (!lameExecutable().existsAsFile()) {
            if (why) *why = "The MP3 encoder isn't in this copy of Lathe.";
            return false;
        }
        return true;
    }
    if (f == DeliveryFormat::Aac256 || f == DeliveryFormat::Alac) {
       #if JUCE_MAC
        return true;
       #else
        if (why) *why = "AAC and Apple Lossless use the Mac's own encoders.";
        return false;
       #endif
    }
    return true;
}

// ---------------------------------------------------------------- MP3

static juce::String encodeMp3(const juce::File& wav, const juce::File& out, DeliveryFormat f, const DeliveryTags& t,
                              const std::function<bool()>& keepGoing) {
    const auto lame = lameExecutable();
    if (!lame.existsAsFile()) return "The MP3 encoder isn't in this copy of Lathe.";
    juce::StringArray args;
    args.add(lame.getFullPathName());
    args.addArray({"--quiet", "--noreplaygain", "-q", "0"});
    if (f == DeliveryFormat::Mp3Cbr320) args.addArray({"--cbr", "-b", "320"});
    else args.addArray({"-V", "0"});
    args.addArray({"--add-id3v2", "--pad-id3v2"});
    auto tag = [&](const char* opt, const juce::String& v) {
        if (v.isNotEmpty()) { args.add(opt); args.add(v); }
    };
    tag("--tt", t.title);
    tag("--ta", t.artist);
    tag("--tl", t.album);
    tag("--ty", t.year);
    tag("--tg", t.genre);
    tag("--tc", t.comment);
    if (t.track > 0) { args.add("--tn"); args.add(juce::String(t.track) + (t.tracks > 0 ? "/" + juce::String(t.tracks) : juce::String())); }
    if (t.isrc.isNotEmpty()) { args.add("--tv"); args.add("TSRC=" + t.isrc); }
    if (t.albumArtist.isNotEmpty()) { args.add("--tv"); args.add("TPE2=" + t.albumArtist); }
    if (t.composer.isNotEmpty()) { args.add("--tv"); args.add("TCOM=" + t.composer); }
    if (t.cover.existsAsFile()) { args.add("--ti"); args.add(t.cover.getFullPathName()); }
    args.add(wav.getFullPathName());
    args.add(out.getFullPathName());
    out.deleteFile();
    juce::ChildProcess p;
    if (!p.start(args, juce::ChildProcess::wantStdErr)) return "Couldn't start the MP3 encoder.";
    while (p.isRunning()) {
        if (keepGoing && !keepGoing()) { p.kill(); out.deleteFile(); return "Export cancelled."; }
        juce::Thread::sleep(20);
    }
    const auto msg = p.readAllProcessOutput().trim();
    if (p.getExitCode() != 0 || !out.existsAsFile()) return "The MP3 encoder failed" + (msg.isNotEmpty() ? ": " + msg : juce::String("."));
    return {};
}

// ---------------------------------------------------------------- AAC / Apple Lossless (macOS)

#if JUCE_MAC
static juce::String encodeM4a(const juce::File& wav, const juce::File& out, DeliveryFormat f, const DeliveryTags& t,
                              const std::function<bool()>& keepGoing) {
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(wav));
    if (!r) return "Couldn't read " + wav.getFileName();
    const int ch = int(r->numChannels);
    const double inRate = r->sampleRate;
    AudioStreamBasicDescription dst{};
    if (f == DeliveryFormat::Aac256) {
        dst.mFormatID = kAudioFormatMPEG4AAC;
        dst.mSampleRate = std::abs(std::fmod(inRate, 44100.0)) < 1.0 ? 44100.0 : 48000.0;
    } else {
        dst.mFormatID = kAudioFormatAppleLossless;
        dst.mSampleRate = inRate;
        dst.mFormatFlags = kAppleLosslessFormatFlag_24BitSourceData;
    }
    dst.mChannelsPerFrame = UInt32(ch);
    UInt32 size = sizeof(dst);
    AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, nullptr, &size, &dst);
    out.deleteFile();
    const auto path = out.getFullPathName().toRawUTF8();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path), CFIndex(std::strlen(path)), false);
    ExtAudioFileRef ext = nullptr;
    OSStatus err = ExtAudioFileCreateWithURL(url, kAudioFileM4AType, &dst, nullptr, kAudioFileFlags_EraseFile, &ext);
    CFRelease(url);
    if (err != noErr || !ext) return "Couldn't create " + out.getFileName() + " (error " + juce::String(int(err)) + ").";
    AudioStreamBasicDescription client{};
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    client.mSampleRate = inRate;
    client.mChannelsPerFrame = UInt32(ch);
    client.mBitsPerChannel = 32;
    client.mBytesPerFrame = UInt32(4 * ch);
    client.mFramesPerPacket = 1;
    client.mBytesPerPacket = client.mBytesPerFrame;
    err = ExtAudioFileSetProperty(ext, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client);
    if (err == noErr) {
        AudioConverterRef conv = nullptr;
        UInt32 cs = sizeof(conv);
        if (ExtAudioFileGetProperty(ext, kExtAudioFileProperty_AudioConverter, &cs, &conv) == noErr && conv) {
            UInt32 q = kAudioConverterQuality_Max;
            AudioConverterSetProperty(conv, kAudioConverterCodecQuality, sizeof(q), &q);
            AudioConverterSetProperty(conv, kAudioConverterSampleRateConverterQuality, sizeof(q), &q);
            if (f == DeliveryFormat::Aac256) {
                UInt32 br = 256000;
                AudioConverterSetProperty(conv, kAudioConverterEncodeBitRate, sizeof(br), &br);
            }
            CFArrayRef none = nullptr;
            ExtAudioFileSetProperty(ext, kExtAudioFileProperty_ConverterConfig, sizeof(none), &none);
        }
    }
    juce::String result;
    if (err != noErr) result = "Couldn't set up the encoder (error " + juce::String(int(err)) + ").";
    const int block = 8192;
    juce::AudioBuffer<float> buf(ch, block);
    std::vector<float> inter(size_t(block * ch));
    for (juce::int64 pos = 0; result.isEmpty() && pos < r->lengthInSamples;) {
        const int n = int(juce::jmin<juce::int64>(block, r->lengthInSamples - pos));
        r->read(&buf, 0, n, pos, true, true);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < ch; ++c) inter[size_t(i * ch + c)] = buf.getSample(c, i);
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = UInt32(ch);
        abl.mBuffers[0].mDataByteSize = UInt32(n * ch * 4);
        abl.mBuffers[0].mData = inter.data();
        if ((err = ExtAudioFileWrite(ext, UInt32(n), &abl)) != noErr) result = "The encoder stopped (error " + juce::String(int(err)) + ").";
        pos += n;
        if (keepGoing && !keepGoing()) result = "Export cancelled.";
    }
    ExtAudioFileDispose(ext);
    if (result.isNotEmpty()) { out.deleteFile(); return result; }
    if (!writeM4aTags(out, t)) return {};   // (plays fine; only the tags couldn't be added)
    return {};
}
#endif

juce::String encodeFile(const juce::File& wav, const juce::File& out, DeliveryFormat f, const DeliveryTags& tags,
                        const std::function<bool()>& keepGoing) {
    juce::String why;
    if (!formatAvailable(f, &why)) return why;
    if (f == DeliveryFormat::Mp3Cbr320 || f == DeliveryFormat::Mp3V0) return encodeMp3(wav, out, f, tags, keepGoing);
   #if JUCE_MAC
    if (f == DeliveryFormat::Aac256 || f == DeliveryFormat::Alac) return encodeM4a(wav, out, f, tags, keepGoing);
   #endif
    return "Not a compressed format.";
}

double fileTruePeakDb(juce::AudioFormatManager& fm, const juce::File& f) {
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    if (!r) return -INFINITY;
    const int ch = int(juce::jmin(8u, r->numChannels));
    std::vector<dsp::TruePeak> tp;
    tp.resize(size_t(ch));
    for (auto& t : tp) t.prepare(r->sampleRate);
    juce::AudioBuffer<float> buf(ch, 16384);
    float mx = 0.0f;
    for (juce::int64 pos = 0; pos < r->lengthInSamples;) {
        const int n = int(juce::jmin<juce::int64>(16384, r->lengthInSamples - pos));
        r->read(&buf, 0, n, pos, true, true);
        for (int c = 0; c < ch; ++c) mx = juce::jmax(mx, tp[size_t(c)].process(buf.getReadPointer(c), n));
        pos += n;
    }
    return mx > 0.0f ? 20.0 * std::log10(double(mx)) : -INFINITY;
}

// ---------------------------------------------------------------- .m4a tags

namespace {
void put32(juce::MemoryOutputStream& o, uint32_t v) { o.writeIntBigEndian(int(v)); }

juce::MemoryBlock atom(const char* type, const juce::MemoryBlock& body) {
    juce::MemoryOutputStream o;
    put32(o, uint32_t(8 + body.getSize()));
    o.write(type, 4);
    o.write(body.getData(), body.getSize());
    return o.getMemoryBlock();
}

juce::MemoryBlock dataAtom(uint32_t kind, const void* payload, size_t size) {
    juce::MemoryOutputStream o;
    put32(o, kind);   // 1 = UTF-8 text, 0 = binary, 13 = JPEG, 14 = PNG
    put32(o, 0);      // locale
    o.write(payload, size);
    return atom("data", o.getMemoryBlock());
}

juce::MemoryBlock textTag(const char* type, const juce::String& v) {
    const auto utf8 = v.toUTF8();
    return atom(type, dataAtom(1, utf8.getAddress(), utf8.sizeInBytes() - 1));
}

juce::MemoryBlock freeformTag(const juce::String& name, const juce::String& v) {
    juce::MemoryOutputStream o;
    {
        juce::MemoryOutputStream m;
        put32(m, 0);
        m.writeString("com.apple.iTunes");
        auto mb = m.getMemoryBlock();
        mb.setSize(mb.getSize() - 1);   // (no terminating zero)
        auto a = atom("mean", mb);
        o.write(a.getData(), a.getSize());
    }
    {
        juce::MemoryOutputStream m;
        put32(m, 0);
        m.write(name.toRawUTF8(), name.getNumBytesAsUTF8());
        auto a = atom("name", m.getMemoryBlock());
        o.write(a.getData(), a.getSize());
    }
    const auto utf8 = v.toUTF8();
    auto d = dataAtom(1, utf8.getAddress(), utf8.sizeInBytes() - 1);
    o.write(d.getData(), d.getSize());
    return atom("----", o.getMemoryBlock());
}

struct Box { juce::int64 start = 0, size = 0, header = 8; char type[5] = {}; };

// The atoms directly inside [from, to) of the file.
std::vector<Box> listBoxes(juce::InputStream& in, juce::int64 from, juce::int64 to) {
    std::vector<Box> out;
    for (juce::int64 pos = from; pos + 8 <= to;) {
        in.setPosition(pos);
        Box b;
        b.start = pos;
        b.size = juce::int64(uint32_t(in.readIntBigEndian()));
        in.read(b.type, 4);
        if (b.size == 1) { b.size = in.readInt64BigEndian(); b.header = 16; }
        else if (b.size == 0) b.size = to - pos;
        if (b.size < b.header || pos + b.size > to) break;
        out.push_back(b);
        pos += b.size;
    }
    return out;
}
}  // namespace

namespace {
// Adds `delta` to every chunk offset (stco / co64) inside the moov atom held in `moov` (whole atom,
// header included). Offsets at or after `from` (where moov used to end) are the ones that move.
void shiftChunkOffsets(juce::MemoryBlock& moov, juce::int64 from, juce::int64 delta) {
    auto* d = static_cast<uint8_t*>(moov.getData());
    const auto size = juce::int64(moov.getSize());
    auto rd32 = [&](juce::int64 p) { return uint32_t(d[p]) << 24 | uint32_t(d[p + 1]) << 16 | uint32_t(d[p + 2]) << 8 | uint32_t(d[p + 3]); };
    auto wr32 = [&](juce::int64 p, uint32_t v) { d[p] = uint8_t(v >> 24); d[p + 1] = uint8_t(v >> 16); d[p + 2] = uint8_t(v >> 8); d[p + 3] = uint8_t(v); };
    std::function<void(juce::int64, juce::int64)> walk = [&](juce::int64 a, juce::int64 b) {
        for (juce::int64 p = a; p + 8 <= b;) {
            const juce::int64 len = rd32(p);
            if (len < 8 || p + len > b) return;
            const juce::String type(reinterpret_cast<const char*>(d + p + 4), 4);
            if (type == "trak" || type == "mdia" || type == "minf" || type == "stbl") walk(p + 8, p + len);
            else if (type == "stco" && len >= 16) {
                const juce::int64 n = rd32(p + 12);
                for (juce::int64 k = 0; k < n && p + 16 + 4 * k + 4 <= p + len; ++k) {
                    const auto at = p + 16 + 4 * k;
                    const juce::int64 v = rd32(at);
                    if (v >= from) wr32(at, uint32_t(v + delta));
                }
            } else if (type == "co64" && len >= 16) {
                const juce::int64 n = rd32(p + 12);
                for (juce::int64 k = 0; k < n && p + 16 + 8 * k + 8 <= p + len; ++k) {
                    const auto at = p + 16 + 8 * k;
                    const juce::int64 v = juce::int64(uint64_t(rd32(at)) << 32 | rd32(at + 4));
                    if (v >= from) {
                        const uint64_t nv = uint64_t(v + delta);
                        wr32(at, uint32_t(nv >> 32));
                        wr32(at + 4, uint32_t(nv));
                    }
                }
            }
            p += len;
        }
    };
    walk(8, size);
}
}  // namespace

bool writeM4aTags(const juce::File& m4a, const DeliveryTags& t) {
    juce::MemoryBlock file;
    if (!m4a.loadFileAsData(file)) return false;
    juce::MemoryInputStream in(file, false);
    const auto top = listBoxes(in, 0, juce::int64(file.getSize()));
    int mi = -1;
    for (int i = 0; i < int(top.size()); ++i)
        if (juce::String(top[size_t(i)].type) == "moov") mi = i;
    if (mi < 0 || top[size_t(mi)].header != 8) return false;
    const auto moov = top[size_t(mi)];
    // moov's children, without any udta already there.
    juce::MemoryOutputStream body;
    for (auto& c : listBoxes(in, moov.start + moov.header, moov.start + moov.size))
        if (juce::String(c.type) != "udta") body.write(static_cast<const char*>(file.getData()) + c.start, size_t(c.size));
    // udta > meta > (hdlr, ilst)
    juce::MemoryOutputStream ilst;
    auto add = [&](const juce::MemoryBlock& b) { ilst.write(b.getData(), b.getSize()); };
    if (t.title.isNotEmpty()) add(textTag("\xa9nam", t.title));
    if (t.artist.isNotEmpty()) add(textTag("\xa9" "ART", t.artist));
    if (t.albumArtist.isNotEmpty()) add(textTag("aART", t.albumArtist));
    if (t.album.isNotEmpty()) add(textTag("\xa9" "alb", t.album));
    if (t.composer.isNotEmpty()) add(textTag("\xa9wrt", t.composer));
    if (t.year.isNotEmpty()) add(textTag("\xa9" "day", t.year));
    if (t.genre.isNotEmpty()) add(textTag("\xa9gen", t.genre));
    if (t.comment.isNotEmpty()) add(textTag("\xa9" "cmt", t.comment));
    if (t.track > 0) {
        const uint8_t trkn[8] = {0, 0, uint8_t(t.track >> 8), uint8_t(t.track), uint8_t(t.tracks >> 8), uint8_t(t.tracks), 0, 0};
        add(atom("trkn", dataAtom(0, trkn, 8)));
    }
    if (t.isrc.isNotEmpty()) add(freeformTag("ISRC", t.isrc));
    if (t.cover.existsAsFile()) {
        juce::MemoryBlock img;
        if (t.cover.loadFileAsData(img) && img.getSize() > 8) {
            const auto* b = static_cast<const uint8_t*>(img.getData());
            const uint32_t kind = b[0] == 0x89 && b[1] == 'P' ? 14u : 13u;
            add(atom("covr", dataAtom(kind, img.getData(), img.getSize())));
        }
    }
    juce::MemoryOutputStream meta;
    put32(meta, 0);   // version and flags
    {
        juce::MemoryOutputStream h;
        put32(h, 0);
        put32(h, 0);
        h.write("mdir", 4);
        h.write("appl", 4);
        put32(h, 0);
        put32(h, 0);
        h.writeByte(0);
        auto hd = atom("hdlr", h.getMemoryBlock());
        meta.write(hd.getData(), hd.getSize());
    }
    auto il = atom("ilst", ilst.getMemoryBlock());
    meta.write(il.getData(), il.getSize());
    auto udta = atom("udta", atom("meta", meta.getMemoryBlock()));
    body.write(udta.getData(), udta.getSize());
    auto newMoov = atom("moov", body.getMemoryBlock());
    juce::int64 delta = juce::int64(newMoov.getSize()) - moov.size;
    // Anything after moov moves by delta. A "free" atom straight after it soaks the change up
    // when it is big enough; otherwise the chunk offsets that point past moov are moved too.
    juce::int64 resumeAt = moov.start + moov.size;   // where the rest of the file is copied from
    juce::MemoryBlock pad;
    const bool after = mi + 1 < int(top.size());
    if (after && delta != 0) {
        const auto& next = top[size_t(mi + 1)];
        if (juce::String(next.type) == "free" && next.size - delta >= 8) {
            juce::MemoryOutputStream f;
            put32(f, uint32_t(next.size - delta));
            f.write("free", 4);
            for (juce::int64 k = 8; k < next.size - delta; ++k) f.writeByte(0);
            pad = f.getMemoryBlock();
            resumeAt = next.start + next.size;
            delta = 0;
        } else {
            shiftChunkOffsets(newMoov, moov.start + moov.size, delta);
        }
    }
    juce::MemoryBlock result(file.getData(), size_t(moov.start));
    result.append(newMoov.getData(), newMoov.getSize());
    if (pad.getSize()) result.append(pad.getData(), pad.getSize());
    if (resumeAt < juce::int64(file.getSize()))
        result.append(static_cast<const char*>(file.getData()) + resumeAt, size_t(juce::int64(file.getSize()) - resumeAt));
    auto tmp = m4a.getSiblingFile(m4a.getFileName() + ".tagging");
    if (!tmp.replaceWithData(result.getData(), result.getSize())) return false;
    return tmp.moveFileTo(m4a);
}

juce::String readM4aTag(const juce::File& m4a, const char* fourcc) {
    juce::MemoryBlock file;
    if (!m4a.loadFileAsData(file)) return {};
    juce::MemoryInputStream in(file, false);
    std::function<juce::String(juce::int64, juce::int64, int)> walk = [&](juce::int64 a, juce::int64 b, int depth) -> juce::String {
        for (auto& x : listBoxes(in, a, b)) {
            const juce::String type(x.type);
            if (std::memcmp(x.type, fourcc, 4) == 0) {
                for (auto& d : listBoxes(in, x.start + x.header, x.start + x.size))
                    if (juce::String(d.type) == "data" && d.size > 16)
                        return juce::String::fromUTF8(static_cast<const char*>(file.getData()) + d.start + 16, int(d.size - 16));
            }
            if (depth < 6 && (type == "moov" || type == "udta" || type == "ilst"))
                if (auto v = walk(x.start + x.header, x.start + x.size, depth + 1); v.isNotEmpty()) return v;
            if (depth < 6 && type == "meta")
                if (auto v = walk(x.start + x.header + 4, x.start + x.size, depth + 1); v.isNotEmpty()) return v;
        }
        return {};
    };
    return walk(0, juce::int64(file.getSize()), 0);
}

}  // namespace lathe
