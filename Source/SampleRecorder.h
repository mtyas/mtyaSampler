#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <map>

// The "create your own samples" assistant (Create page).
//
// For every take in the plan it runs, sample-accurately on the audio thread:
//   announce  → spoken prompt from the voice pack (or the UI speaks it when no pack is installed)
//   tone      → reference sine at the note's pitch
//   countin   → clicks (plus spoken numbers when the voice pack has them)
//   record    → captures the plugin's audio input; stops at the max length or after silence
//   save      → waits while the message thread trims the take and writes the WAV
// then moves to the next take, waits for review, or finishes.
//
// Threading
//  * The plan is built and swapped on the message thread under `planLock`; the audio thread only
//    try-locks it (a missed block just outputs no guide audio).
//  * UI commands reach the audio thread through a lock-free FIFO.
//  * The recording buffer is allocated when a session starts, never on the audio thread.
//  * The audio thread publishes its state through atomics that getState() reads.
class SampleRecorder final : private juce::Timer
{
public:
    SampleRecorder();
    ~SampleRecorder() override;

    void prepare (double sampleRate, int maxBlockSize);

    // Audio thread. `input` holds the plugin's input channels (it may have 0 channels).
    // Guide audio (and the input, when monitoring is on) is added to `output`.
    void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int numSamples);

    // Message thread. Commands: start(plan) pause resume retake skip next goto(index) stop monitor(bool)
    juce::var command (const juce::String& name, const juce::var& arg);

    // Message thread. Snapshot for the UI (same shape as publish() in ui/js/mock-recorder.js).
    juce::var getState() const;

    // Message thread. Called once per finished take: saved, empty or skipped.
    std::function<void (const juce::var&)> onTake;

    bool hasVoicePack() const noexcept { return ! voiceClips.empty(); }
    static juce::File defaultRecordingFolder();
    static juce::File voicePackFolder();

private:
    enum class Status { idle, running, paused, review, done };
    enum class Phase { idle, announce, tone, countin, record, save };
    enum class Cmd { pause, resume, retake, skip, next, gotoTake, stop, afterSave };

    struct Take
    {
        int note = 60, rr = 0, v = 0;
        juce::String file;
        std::vector<int> prompt;      // voice clip indices; empty = the UI speaks
    };

    struct Plan
    {
        std::vector<Take> takes;
        juce::File folder;
        int inputMode = 0;            // 0 stereo, 1 left, 2 right
        juce::String normalize = "session";
        double announceSec = 2.2, toneSec = 1.5, beatSec = 0.6, maxSec = 6.0;
        int countBeats = 3;
        float threshold = 0.008f;     // linear
        int silenceMs = 900, prerollMs = 20, fadeMs = 60;
        bool autoAdvance = true, pitched = true, voiceInUi = true;
        std::array<int, 9> countClips {};   // spoken numbers, index 1..8; -1 = none
    };

    struct VoiceClip { juce::AudioBuffer<float> audio; double sampleRate = 44100.0; };

    // message thread
    void timerCallback() override;
    void loadVoicePack();
    std::vector<int> promptFor (const juce::String& text) const;
    juce::var startSession (const juce::var& planJson);
    void handleFinishedTake();
    void normalizeSession();
    void push (Cmd c, int arg = 0);

    // audio thread
    void applyCommands();
    void enterPhase (Phase);
    void finishTake (bool advance);
    void goTo (int index);
    juce::int64 lengthOf (Phase) const;
    float guideSample();
    void publish();

    // ---- shared ----
    juce::SpinLock planLock;
    std::unique_ptr<Plan> plan;
    juce::AbstractFifo cmdFifo { 64 };
    std::array<std::pair<Cmd, int>, 64> cmdBuffer {};

    std::atomic<int> pubStatus { 0 }, pubPhase { 0 }, pubTake { 0 }, pubCount { 0 };
    std::atomic<int> savedCount { 0 }, lastSaved { -1 }, pendingSkip { -1 };
    std::atomic<juce::int64> pubPos { 0 }, pubLen { 0 }, pubRecLen { 0 };
    std::atomic<float> inLevel { 0.0f };
    std::atomic<bool> clipFlag { false }, monitor { false }, takeReady { false }, heard { false };
    static constexpr int numLivePeaks = 200;
    std::array<std::atomic<float>, numLivePeaks> livePeaks {};

    // ---- audio thread only (or message thread while holding planLock) ----
    double sampleRate = 44100.0;
    Status status = Status::idle;
    Phase phase = Phase::idle;
    int take = 0;
    juce::int64 pos = 0, len = 0, recPos = 0, maxRecSamples = 0, silentSamples = 0;
    juce::AudioBuffer<float> recBuffer;    // allocated by startSession
    juce::AudioBuffer<float> preroll;      // ring buffer, allocated by prepare
    int prerollWrite = 0;
    float envelope = 0.0f, envDecay = 0.999f;
    double tonePhase = 0.0, toneDelta = 0.0;
    int promptIndex = 0; double promptPos = 0.0; juce::int64 promptGap = 0;
    int countClip = -1; double countPos = 0.0;
    bool pauseAfterSave = false;

    // ---- voice pack (loaded once in the constructor, read-only afterwards) ----
    std::vector<VoiceClip> voiceClips;
    std::map<juce::String, int> voiceIndex;

    // ---- message thread only ----
    juce::AudioFormatManager formats;
    struct SavedTake { juce::File file; float peak; };
    std::vector<SavedTake> savedTakes;
    int emptyRetries = 0, emptyRetryTake = -1;
    bool sessionNormalized = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleRecorder)
};
