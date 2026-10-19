#pragma once
#include "InstrumentMap.h"
#include "VoiceFilter.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The sampler: voices, round robin, trigger modes, voice modes, the playback modes, and per-voice
// sound design (filter with drive, three envelopes, four LFOs) driven by a modulation matrix.
// Everything in process() runs on the audio thread: no allocation, locks or file I/O.
//
// Instrument changes arrive as a ready-built InstrumentMap through publish() (any thread).
// The audio thread picks it up at the start of the next block. Old maps go back to the loader
// thread for deletion once no voice is playing from them (popRetired).
//
// Modulation: global sources (macros, mod wheel, aftertouch) are applied once per block, to the
// voice targets and to any parameter ("param:<id>" routes); param() returns those modulated values,
// which the FX chain also reads. Per-voice sources (Env 2/3, LFOs, velocity, key) are applied per
// voice every 32 samples.
class SamplerEngine
{
public:
    struct ParamRef
    {
        juce::String id;
        std::atomic<float>* value = nullptr;
        juce::NormalisableRange<float> range;
        bool modulatable = false;   // floats only
    };
    using ParamList = std::vector<ParamRef>;

    // Builds the list from params.json. `valueFor` gives the storage for an id: the APVTS raw value
    // in the plugin, or a test's own atomics.
    static ParamList makeParamList (const juce::var& spec, const std::function<std::atomic<float>* (const juce::String&)>& valueFor);

    enum class PlayMode { oneShot, loop, pingPong, slice, granular };

    static constexpr int maxVoices = 96;
    static constexpr int maxGrains = 32;
    static constexpr int numAuxOutputs = 3;   // "Bus 2" … "Bus 4"

    explicit SamplerEngine (ParamList);
    ~SamplerEngine();

    void prepare (double sampleRate);

    // Audio thread. Adds the voices into `main` (which the caller has cleared). Layers routed to
    // Bus 2–4 go to aux[0..2] when that pointer isn't null, otherwise to main.
    void process (juce::AudioBuffer<float>& main, juce::AudioBuffer<float>* const* aux, const juce::MidiBuffer& midi, bool transportPlaying);
    void process (juce::AudioBuffer<float>& main, const juce::MidiBuffer& midi, bool transportPlaying)
    {
        juce::AudioBuffer<float>* none[numAuxOutputs] {};
        process (main, none, midi, transportPlaying);
    }

    // Any thread. Takes ownership; replaces a map that hasn't been picked up yet.
    void publish (std::unique_ptr<InstrumentMap>);
    // Loader thread. Maps the audio thread no longer uses, to be deleted. nullptr when none.
    std::unique_ptr<InstrumentMap> popRetired();

    int getActiveVoices() const noexcept { return activeVoiceCount.load(); }

    // Audio thread: a parameter's value this block, including global modulation.
    float param (int index) const noexcept;
    float param (int index, float fallback) const noexcept { return index >= 0 ? param (index) : fallback; }
    int indexOf (const juce::String& id) const noexcept { return ids.indexOf (id); }
    const juce::StringArray& paramIds() const noexcept { return ids; }

    // Audio thread: the current instrument's FX order.
    std::array<int, 6> fxOrder() const noexcept { return map != nullptr ? map->fxOrder : std::array<int, 6> { 0, 1, 2, 3, 4, 5 }; }

    // Message thread: where the playing notes are in their samples (newest first), for the
    // waveform's playhead. Returns how many were written.
    struct Playhead { int layerId = 0, variation = 0; float position = 0; };
    static constexpr int maxPlayheads = 8;
    int getPlayheads (Playhead* out, int max) const noexcept;

    // Any thread: where an RR group's counter is (for the inspector's Position readout).
    int rrPosition (int group) const noexcept { return group >= 0 && group < maxRRGroups ? rrShown[(size_t) group].load (std::memory_order_relaxed) : 0; }

private:
    struct Grain
    {
        bool active = false;
        double pos = 0, inc = 1;
        int age = 0, length = 1;
    };

    struct Voice
    {
        bool active = false;
        const InstrumentMap* map = nullptr;
        const Layer* layer = nullptr;
        const SampleData* sample = nullptr;
        const Layer::Region* region = nullptr;   // this variation's start/end/loop points
        int variation = 0;

        int note = 60, velocity = 100;   // velocity after the curve
        juce::uint64 order = 0;          // start order, for "oldest"
        juce::uint32 eventId = 0;        // note-on event that started it (choke skips its own event)
        bool isRelease = false;          // started by a release trigger: plays through, ignores note-off
        bool keyDown = true;             // false once the note has been released

        PlayMode mode = PlayMode::oneShot;
        int sliceIndex = 0;
        double pos = 0;                  // read position in sample frames
        int direction = 1;               // ping-pong
        double regionStart = 0, regionEnd = 1;
        double loopStart = 0, loopEnd = 1, xfadeLen = 0;
        bool loopValid = false;

        float semis = 0, targetSemis = 0, glideStep = 0;   // pitch relative to the layer's root
        double rateScale = 1;            // sample rate ratio (file / host)
        double inc = 1;                  // playback increment, refreshed every few samples

        float gainL = 1, gainR = 1;
        float level = 0;                 // last envelope × gain, for "quietest"
        juce::ADSR env;                  // Env 1: amp
        bool killing = false;            // fast fade-out (steal, choke, mono retrigger)
        float killGain = 1;

        // modulation
        juce::ADSR env2, env3;
        float env1Value = 0, env2Value = 0, env3Value = 0;
        struct LfoState { double phase = 0; float value = 0, held = 0; } lfos[4];
        double age = 0;                  // samples since the note started (LFO fade-in)
        int samplesSinceMod = 0;
        float pitchMod = 0, volumeMod = 1, panModL = 1, panModR = 1, startMod = 0;
        float grainSizeMul = 1, grainDensityMul = 1, grainSpray = 0, grainPosOffset = 0, grainPitchRnd = 0;
        mtya::VoiceFilter filter;
        float cutoffOct = 0, resoSmoothed = 0;   // the knobs' values, smoothed (~10 ms) so jumps don't click
        bool filterFresh = true;

        // granular
        Grain grains[maxGrains];
        double grainHead = 0;            // source position the cloud is centred on
        int untilNextGrain = 0;
    };

    // Parameter values read once per block (already modulated by the global sources)
    struct BlockParams
    {
        PlayMode playMode = PlayMode::oneShot;
        float loopXfadeMs = 0;
        float tuneSemis = 0;             // tune + fine + pitch bend
        float grnPos = 0.5f, grnSizeMs = 80, grnDensity = 20, grnSpray = 0, grnPitchRnd = 0;
        bool grnFreeze = false;
        juce::ADSR::Parameters env, env2, env3;

        int fltType = 1;
        float cutoff = 20000, reso = 0, driveDb = 0, envAmt = 0;
        struct Lfo { int shape = 0; float rate = 1, depth = 0, phase = 0, fadeMs = 0; } lfo[4];
    };

    // Indices of the parameters the engine reads (-1 when missing)
    struct Ix
    {
        int playMode, loopXfade, tune, fine;
        int grnPos, grnSize, grnDensity, grnSpray, grnPitchRnd, grnFreeze;
        int env1[4], env2[4], env3[4];
        int fltType, fltCutoff, fltReso, fltDrive, fltEnvAmt;
        int lfo[4][5];   // shape, rate, depth, phase, fade
        int macro[4];
    };

    void readBlockParams();

    // ---- events ----
    void handleMidi (const juce::MidiMessage&);
    void noteOn (int note, int rawVelocity);
    void noteOff (int note);
    void releaseKey (int note);
    void resetRoundRobin (Layer::RRReset which);

    // ---- voices ----
    bool layerIsPlayable (const InstrumentMap&, const Layer&, int note, int velocity) const noexcept;
    int pickVariation (const Layer&, int* picks);
    Voice* startVoice (const InstrumentMap&, const Layer&, int note, int velocity, float extraGain, bool isRelease,
                       int variation, juce::uint32 eventId, float glideFrom, bool hasGlide);
    Voice* allocateVoice();
    void stealWithinLayer (const Layer&);
    void kill (Voice&);
    void freeVoice (Voice&);
    float semitonesFor (const Layer&, int note) const noexcept;
    void retarget (Voice&, int note, int velocity);

    // ---- rendering ----
    void renderVoice (Voice&, int start, int num);
    void updateRegion (Voice&) const;
    float read (const SampleData&, int channel, double pos) const noexcept;
    void triggerGrain (Voice&);
    void updateModulation (Voice&, int rampSamples);
    void startModulation (Voice&);

    // ---- map handover ----
    void pickUpNewMap();
    void collectDrainedMaps();
    bool pushRetired (InstrumentMap*);

    ParamList params;
    juce::StringArray ids;
    Ix ix {};
    std::vector<float> paramOffset;                   // global modulation per parameter (normalised)
    std::array<float, Mod::numTargets> globalMod {};  // global modulation of the voice targets
    BlockParams bp;
    double sampleRate = 44100.0;

    InstrumentMap* map = nullptr;                     // audio thread's current map
    std::atomic<InstrumentMap*> pending { nullptr };
    std::array<InstrumentMap*, 16> draining {};       // replaced, but voices may still use them
    std::array<InstrumentMap*, 32> retired {};        // ready to delete, handed to the loader thread
    juce::AbstractFifo retiredFifo { 32 };

    Voice voices[maxVoices];
    std::atomic<int> activeVoiceCount { 0 };
    juce::uint64 voiceOrder = 0;
    juce::uint32 eventCounter = 0;
    juce::AudioBuffer<float>* outputs[1 + numAuxOutputs] {};   // this block's main + aux buffers

    // key state
    std::array<juce::uint64, 128> keyOrder {};        // when each key went down, 0 = up
    std::array<int, 128> keyVelocity {};              // curved velocity of the key's last note-on
    std::array<double, 128> keyDownTime {};           // in samples, for release-trigger decay
    std::array<bool, 128> sustainedKey {};            // released while the sustain pedal was down
    juce::uint64 keyCounter = 0;
    bool sustainPedal = false;
    int currentKeyswitch = -1;
    float pitchBendSemis = 0, modWheel = 0, aftertouch = 0;
    int lastNote = 60;                                // for glide in poly mode
    bool hasLastNote = false;

    // Round robin. The state survives instrument edits; indexed by the map's RR group.
    static constexpr int maxRRGroups = 256;
    std::array<int, maxRRGroups> rrCounter {}, rrLast {};
    std::array<std::atomic<int>, maxRRGroups> rrShown {};

    // Playheads published at the end of each block (audio thread writes, message thread reads)
    std::array<std::atomic<int>, maxPlayheads> phLayer {}, phVariation {};
    std::array<std::atomic<float>, maxPlayheads> phPosition {};
    std::atomic<int> phCount { 0 };
    double samplesSinceNoteOn = 0;
    bool idleResetDone = true, wasPlaying = false;

    double now = 0;                                   // sample clock
    juce::Random random;

    JUCE_DECLARE_NON_COPYABLE (SamplerEngine)
};
