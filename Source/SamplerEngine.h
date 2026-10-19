#pragma once
#include "InstrumentMap.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The sampler: voices, round robin, trigger modes, voice modes and the playback modes.
// Everything in process() runs on the audio thread: no allocation, locks or file I/O.
//
// Instrument changes arrive as a ready-built InstrumentMap through publish() (any thread).
// The audio thread picks it up at the start of the next block. Old maps go back to the loader
// thread for deletion once no voice is playing from them (popRetired).
class SamplerEngine
{
public:
    // Raw parameter values from the APVTS (see ui/params.json for ranges and units).
    struct Params
    {
        std::atomic<float>* playMode = nullptr;   // 0 one-shot, 1 loop, 2 ping-pong, 3 slice, 4 granular
        std::atomic<float>* start = nullptr;      // %, of the sample
        std::atomic<float>* end = nullptr;
        std::atomic<float>* loopStart = nullptr;
        std::atomic<float>* loopEnd = nullptr;
        std::atomic<float>* loopXfade = nullptr;  // ms
        std::atomic<float>* tune = nullptr;       // semitones
        std::atomic<float>* fine = nullptr;       // cents
        std::atomic<float>* grnPos = nullptr;     // %
        std::atomic<float>* grnSize = nullptr;    // ms
        std::atomic<float>* grnDensity = nullptr; // grains per second
        std::atomic<float>* grnSpray = nullptr;   // %
        std::atomic<float>* grnPitchRnd = nullptr;// semitones
        std::atomic<float>* grnFreeze = nullptr;  // bool
        std::atomic<float>* attack = nullptr;     // Env 1 (amp), seconds
        std::atomic<float>* decay = nullptr;
        std::atomic<float>* sustain = nullptr;    // %
        std::atomic<float>* release = nullptr;

        static Params fromApvts (juce::AudioProcessorValueTreeState&);
    };

    enum class PlayMode { oneShot, loop, pingPong, slice, granular };

    static constexpr int maxVoices = 96;
    static constexpr int maxGrains = 32;

    explicit SamplerEngine (Params);
    ~SamplerEngine();

    void prepare (double sampleRate);

    // Audio thread. Adds the voices into `out` (which the caller has cleared).
    void process (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, bool transportPlaying);

    // Any thread. Takes ownership; replaces a map that hasn't been picked up yet.
    void publish (std::unique_ptr<InstrumentMap>);
    // Loader thread. Maps the audio thread no longer uses, to be deleted. nullptr when none.
    std::unique_ptr<InstrumentMap> popRetired();

    int getActiveVoices() const noexcept { return activeVoiceCount.load(); }

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
        juce::ADSR env;
        bool killing = false;            // fast fade-out (steal, choke, mono retrigger)
        float killGain = 1;

        // granular
        Grain grains[maxGrains];
        double grainHead = 0;            // source position the cloud is centred on
        int untilNextGrain = 0;
    };

    // Parameter values read once per block
    struct BlockParams
    {
        PlayMode playMode = PlayMode::oneShot;
        float start = 0, end = 1, loopStart = 0, loopEnd = 1, loopXfadeMs = 0;
        float tuneSemis = 0;             // tune + fine + pitch bend
        float grnPos = 0.5f, grnSizeMs = 80, grnDensity = 20, grnSpray = 0, grnPitchRnd = 0;
        bool grnFreeze = false;
        juce::ADSR::Parameters env;
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
    void renderVoice (Voice&, juce::AudioBuffer<float>&, int start, int num);
    void updateRegion (Voice&) const;
    float read (const SampleData&, int channel, double pos) const noexcept;
    void triggerGrain (Voice&);

    // ---- map handover ----
    void pickUpNewMap();
    void collectDrainedMaps();
    bool pushRetired (InstrumentMap*);

    Params params;
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

    // key state
    std::array<juce::uint64, 128> keyOrder {};        // when each key went down, 0 = up
    std::array<int, 128> keyVelocity {};              // curved velocity of the key's last note-on
    std::array<double, 128> keyDownTime {};           // in samples, for release-trigger decay
    std::array<bool, 128> sustainedKey {};            // released while the sustain pedal was down
    juce::uint64 keyCounter = 0;
    bool sustainPedal = false;
    int currentKeyswitch = -1;
    float pitchBendSemis = 0;
    int lastNote = 60;                                // for glide in poly mode
    bool hasLastNote = false;

    // Round robin. The state survives instrument edits; indexed by the map's RR group.
    static constexpr int maxRRGroups = 256;
    std::array<int, maxRRGroups> rrCounter {}, rrLast {};
    double samplesSinceNoteOn = 0;
    bool idleResetDone = true, wasPlaying = false;

    double now = 0;                                   // sample clock
    juce::Random random;

    JUCE_DECLARE_NON_COPYABLE (SamplerEngine)
};
