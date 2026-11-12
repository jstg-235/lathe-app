// Compressed deliverables: MP3 (LAME, run as its own program), AAC and Apple Lossless (macOS's own
// encoders, written as .m4a). Each takes a finished WAV and writes the delivery file with its tags,
// then the result is decoded again to check its true peak (lossy files can go over full scale
// where the WAV didn't).
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>

namespace lathe {

enum class DeliveryFormat { Wav16Cd = 0, Wav24, Wav32f, Mp3Cbr320, Mp3V0, Aac256, Alac };
constexpr int kNumDeliveryFormats = 7;

juce::String formatName(DeliveryFormat f);          // "MP3, 320 kbps"
juce::String formatExtension(DeliveryFormat f);     // ".mp3"
bool isCompressed(DeliveryFormat f);
// Where the audio is rendered before encoding: the rate (0 = the project's own) and whether it is
// a float WAV for an encoder or the delivery itself.
double renderRateFor(DeliveryFormat f, double projectRate);

// The tags written into compressed files (from the session's CD-Text and release details).
struct DeliveryTags {
    juce::String title, artist, album, albumArtist, composer, isrc, year, genre, comment;
    int track = 0, tracks = 0;
    juce::File cover;   // a JPEG or PNG, or none
};

// The bundled MP3 encoder: LATHE_LAME in the environment, else Lathe.app/Contents/Resources/lame,
// else next to the program. Non-existent file when there is none.
juce::File lameExecutable();
// Can this format be written here? `why` says what is missing when it can't.
bool formatAvailable(DeliveryFormat f, juce::String* why = nullptr);

// Encodes `wav` (16/24-bit or float) to `out`. Empty string = done.
juce::String encodeFile(const juce::File& wav, const juce::File& out, DeliveryFormat f, const DeliveryTags& tags,
                        const std::function<bool()>& keepGoing);

// The highest true peak in an audio file the format manager can read (dBTP; -inf when silent or unreadable).
double fileTruePeakDb(juce::AudioFormatManager& fm, const juce::File& f);

// ---- tags inside an .m4a (the iTunes "ilst" atoms), for files whose "moov" atom comes last ----
// Adds title, artist, album, track number, year, genre, ISRC (as a free-form tag) and cover art.
// Returns false (and leaves the file as it was) when the file isn't laid out that way.
bool writeM4aTags(const juce::File& m4a, const DeliveryTags& tags);
// Reads back the text of one tag (e.g. "\xa9nam" for the title), for checking.
juce::String readM4aTag(const juce::File& m4a, const char* fourcc);

}  // namespace lathe
