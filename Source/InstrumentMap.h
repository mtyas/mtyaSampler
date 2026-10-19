#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <map>
#include <memory>
#include <vector>

// The instrument JSON (layers, mapping, round robin…; schema in docs/ui-spec.md) turned into an
// immutable structure the audio thread can read without parsing, allocating or locking.
//
// A new InstrumentMap is built on the loader thread every time the instrument changes. The
// engine swaps it in atomically; voices that are still playing keep pointing into the map they
// started from until they finish (see SamplerEngine).

// One audio file in RAM, at its own sample rate (1 or 2 channels).
struct SampleData
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    juce::String path;
    bool missing = false;   // the file wasn't found: this is the stand-in tone (see SampleCache)
};
using SamplePtr = std::shared_ptr<const SampleData>;

struct Layer
{
    enum class RRMode { off, cycle, random, noRepeat };
    enum class RRReset { never, idle, transport };
    enum class Trigger { attack, release, legato, first };
    enum class Steal { oldest, quietest, lowest, highest };
    enum class VoiceMode { poly, mono, legato };

    int id = 0;
    int lo = 0, hi = 127, root = 60, vlo = 0, vhi = 127;
    float keyFade = 0, velFade = 0;         // width of the crossfade around each edge (keys / velocity steps)
    float keyTrack = 1.0f;                  // 1 = 100 %

    RRMode rrMode = RRMode::off;
    RRReset rrReset = RRReset::idle;
    int rrGroup = 0;                        // index into InstrumentMap::rrGroupSizes
    std::vector<SamplePtr> samples;         // one per round-robin variation; never empty

    float gain = 1.0f;                      // from volume dB
    float panL = 1.0f, panR = 1.0f;         // balance law, centre = unity
    int output = 0;                         // 0 = Main (multi-out comes later)

    Trigger trigger = Trigger::attack;
    float releaseGain = 0.5f;               // from releaseVolume dB
    float releaseDecayMs = 600.0f;
    float velToVol = 0.8f;

    int polyphony = 16, choke = 0;
    Steal steal = Steal::oldest;
    VoiceMode voiceMode = VoiceMode::poly;
    float glideMs = 0;

    int keyswitch = -1;                     // MIDI note, -1 = always active
    int articulation = -1;                  // index of the articulation name, -1 = none

    // The layer sounds slightly beyond its range when it has fades, so neighbours overlap.
    bool covers (int note, int velocity) const noexcept;
    // Equal-power crossfade gain for a note/velocity (1 inside the range, 0 outside the fades).
    float zoneGain (int note, int velocity) const noexcept;
};

struct InstrumentMap
{
    enum class Curve { linear, soft, hard, fixed };

    Curve velocityCurve = Curve::linear;
    std::vector<Layer> layers;              // muted layers (and non-solo layers when any is soloed) are left out
    std::vector<int> rrGroupSizes;          // per RR group: the most variations any of its layers has
    std::array<int, 128> keyswitchArticulation {};   // per note: articulation it selects, -1 = not a keyswitch
    int defaultKeyswitch = -1;              // lowest keyswitch note, active until another is pressed
    int missingSamples = 0, loadedSamples = 0;

    // Audio thread only: how many voices still point into this map.
    mutable int voiceRefs = 0;

    int applyVelocityCurve (int velocity) const noexcept;
    bool isKeyswitch (int note) const noexcept { return note >= 0 && note < 128 && keyswitchArticulation[(size_t) note] >= 0; }
};

// Loads sample files into RAM and keeps them while any map uses them, so editing the layers
// doesn't reload every file. Used from one thread only (the loader thread).
class SampleCache
{
public:
    SampleCache();

    // Finds and loads a sample named in the instrument. Names can be absolute paths or bare
    // file names; bare names are looked up in `extraFolder` (if any), then in
    // Documents/mtyaSampler/Recordings and Documents/mtyaSampler/Samples (searched recursively).
    // A file that can't be found or read gives a quiet sine at the layer's root pitch, so the
    // mapping can still be played and checked (SampleData::missing tells which).
    SamplePtr get (const juce::String& name, const juce::File& extraFolder, int rootNote);

    // Drops samples no map uses any more.
    void prune();

    static juce::Array<juce::File> defaultSearchFolders();

private:
    juce::File resolve (const juce::String& name, const juce::File& extraFolder);
    SamplePtr load (const juce::File&);
    SamplePtr missingTone (int rootNote);

    juce::AudioFormatManager formats;
    std::map<juce::String, SamplePtr> cache;          // key: full path, or "~tone:<root>"
    std::map<juce::String, juce::File> nameIndex;     // lower-case file name → file, from the search folders
    juce::uint32 nameIndexTime = 0;
};

// Builds a map from the instrument JSON. Runs on the loader thread (or in tests).
std::unique_ptr<InstrumentMap> buildInstrumentMap (const juce::var& json, SampleCache& cache);
