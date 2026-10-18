#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>
#include <atomic>

static constexpr int MAX_BEATS = 16;

// Default editor size — the editor scales this layout to whatever size the user drags to
static constexpr int EDITOR_BASE_W = 1024;
static constexpr int EDITOR_BASE_H = 580;

//==============================================================================
class PolyrhythmProcessor : public juce::AudioProcessor,
                            private juce::AudioProcessorValueTreeState::Listener
{
public:
    PolyrhythmProcessor();
    ~PolyrhythmProcessor() override;

    //==========================================================================
    // AudioProcessor overrides
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    // These have to agree with the flags on the CMake target, or the wrapper and the
    // processor disagree about what kind of plugin this is
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return (bool) JucePlugin_IsMidiEffect; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Beat state — shared between audio thread (read) and UI thread (write)
    // Using atomics for lock-free cross-thread access
    std::array<std::atomic<bool>,  MAX_BEATS> trackAActive;
    std::array<std::atomic<bool>,  MAX_BEATS> trackBActive;
    std::array<std::atomic<float>, MAX_BEATS> trackAVelocity;
    std::array<std::atomic<float>, MAX_BEATS> trackBVelocity;
    std::array<std::atomic<int>,   MAX_BEATS> trackANotes;   // per-beat MIDI note
    std::array<std::atomic<int>,   MAX_BEATS> trackBNotes;
    std::array<std::atomic<float>, MAX_BEATS> trackACutoff;  // per-beat LP cutoff 0-1
    std::array<std::atomic<float>, MAX_BEATS> trackBCutoff;
    std::array<std::atomic<float>, MAX_BEATS> trackAGate;    // per-beat gate 0-1
    std::array<std::atomic<float>, MAX_BEATS> trackBGate;

    // Send per-beat LP cutoff as MIDI CC74 (Brightness). Off by default: when on, a CC
    // event is emitted for every note, which clutters recorded regions and moves the
    // filter on whatever instrument sits downstream.
    std::atomic<bool> sendCutoffCC { false };

    //==========================================================================
    // Scale lock, shared by both tracks. Not a parameter: changing it rewrites notes, which
    // has to happen on the message thread. Saved with the beat state.
    struct Scale
    {
        const char* name;
        int         numSteps;
        int         steps[7];   // semitone offsets from the root
    };
    static const std::array<Scale, 12>& getScales();

    std::atomic<bool> scaleEnabled { false };
    std::atomic<int>  scaleRoot    { 0 };   // 0-11, C = 0
    std::atomic<int>  scaleType    { 0 };   // index into getScales()

    bool isInScale   (int note) const;
    int  snapToScale (int note) const;          // nearest in-scale note, ties go down
    int  stepNote    (int note, int delta) const; // one semitone, or one degree when locked
    void snapAllNotesToScale();                 // message thread only

    //==========================================================================
    // Randomise per-beat values on the beats in use. Active steps, beat counts and the
    // bar are left alone. Message thread only.
    enum class RandomTarget { notes, velocity, gate, cutoff, all };
    void randomiseTrack (bool isTrackA, RandomTarget what);

    //==========================================================================
    // Direct MIDI output (e.g. an IAC Driver bus), independent of the host's own MIDI
    // routing. Lets any DAW track record the generated MIDI whatever slot we're in.
    static juce::Array<juce::MidiDeviceInfo> getMidiOutputDevices();
    void         setMidiOutputDevice (const juce::String& identifier);
    juce::String getMidiOutputIdentifier() const;
    juce::String getMidiOutputName() const;

    //==========================================================================
    // Editor size, persisted with the rest of the plugin state
    std::atomic<int> editorWidth  { EDITOR_BASE_W };
    std::atomic<int> editorHeight { EDITOR_BASE_H };

    //==========================================================================
    // Parameters (automatable)
    juce::AudioProcessorValueTreeState apvts;

    // Convenience getters (safe to call from any thread)
    int   getTrackABeatCount()  const;
    int   getTrackBBeatCount()  const;
    int   getTrackAChannel()    const;
    int   getTrackBChannel()    const;
    // Global note getters read beat 0 (used for the "all" display in the UI)
    int   getTrackANote()       const;
    int   getTrackBNote()       const;
    // Step every beat on a track up or down: by semitone, or by scale degree when the
    // scale lock is on
    void  shiftTrackANotes (int steps);
    void  shiftTrackBNotes (int steps);
    void  resetTrackANotes();
    void  resetTrackBNotes();
    float getSwing()            const;
    float getTrackAGate()       const;
    float getTrackBGate()       const;
    float getTrackAVelocity()   const;
    float getTrackBVelocity()   const;
    float getTrackACutoff()     const;
    float getTrackBCutoff()     const;
    float getProbability()      const;

    // Bumped whenever state changes from outside the editor (a project load, automation of
    // a parameter the editor lays out from), so an open editor knows to resync
    std::atomic<uint32_t> uiStateVersion { 0 };

    // Called by the editor after it edits beat state directly. Beat state is not made of
    // parameters, so the host is not told about it otherwise and may not offer to save.
    void markBeatStateEdited() { beatStateEdited.store (true); }
    bool takeBeatStateEdited() { return beatStateEdited.exchange (false); }

    // Transport status (written by audio thread, read by UI)
    std::atomic<bool>   transportPlaying { false };
    std::atomic<double> currentBpm       { 120.0 };

    // Beat-fire signals for UI pulse animation
    // UI compares against lastFireCount to detect new fires
    std::atomic<uint64_t> trackAFireCount   { 0 };
    std::atomic<uint64_t> trackBFireCount   { 0 };
    std::atomic<int>      trackACurrentBeat { 0 };
    std::atomic<int>      trackBCurrentBeat { 0 };

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void parameterChanged (const juce::String& parameterID, float newValue) override;

    std::atomic<bool> beatStateEdited { false };

    double sampleRateVal  = 44100.0;
    bool   wasPlaying     = false;

    // PPQ cursor for the scheduler. The window we search for beats runs from where the
    // previous block finished up to the end of this block, rather than from this block's
    // reported start. Host-reported positions rarely tile exactly: a gap silently drops
    // beats, an overlap fires them twice. Both show up as wrong notes in a recording.
    double nextPpq             = 0.0;
    bool   ppqCursorValid      = false;

    // A jump larger than this (in quarter notes) means the transport looped or relocated
    // rather than advanced normally, so the cursor is resynced instead of catching up.
    static constexpr double ppqDiscontinuity = 0.25;

    // Pending note-offs that extend past the current audio block
    struct PendingNoteOff
    {
        int samplesRemaining;
        int channel;
        int note;
    };
    std::vector<PendingNoteOff> pendingNoteOffs;

    // Last CC74 value sent per MIDI channel (1-16), so the CC is only emitted on change.
    // -1 = nothing sent yet.
    std::array<int, 17> lastCutoffCC {};

    void processTrack (bool        isTrackA,
                       double      barLengthPpq,
                       double      barAnchorPpq,
                       double      windowStartPpq,
                       double      windowEndPpq,
                       double      blockStartPpq,
                       double      ppqPerSample,
                       int         numSamples,
                       juce::MidiBuffer& midiOut);

    void flushPendingNoteOffs (juce::MidiBuffer& midiOut, int sampleOffset);
    void resetCutoffCC();

    // Only ever fed the notes this plugin generates. Echoing the pass-through input here
    // creates a feedback loop the moment the DAW is listening to the same port: our own
    // output returns as input and is sent straight back out again.
    void queueForExternalMidiOut (const juce::MidiBuffer& generated, int numSamples);
    void drainMidiOutQueue (bool discardAll = false);
    void publishGeneratedMidi (juce::MidiBuffer& hostBuffer, int numSamples);

    // Everything generated this block, kept separate from the host's buffer so the two
    // destinations can be fed different things
    juce::MidiBuffer generatedMidi;

    juce::Random random;

    //==========================================================================
    // Direct MIDI output device. Opened/closed on the message thread, read on the audio
    // thread, so access is guarded. Messages go out via the device's own background
    // thread rather than being written from processBlock directly.
    mutable juce::CriticalSection     midiOutLock;
    std::unique_ptr<juce::MidiOutput> midiOutDevice;
    juce::String                      midiOutIdentifier;

    // Checked by the audio thread instead of taking the lock
    std::atomic<bool> midiOutActive { false };

    /*  Generated messages cross to the sender thread through a lock-free queue. Calling
        MidiOutput::sendBlockOfMessages() straight from processBlock takes a lock and
        allocates, which is enough to stall the audio thread and produce dropouts in
        whatever instrument is downstream.
    */
    struct QueuedMidi
    {
        juce::uint8 bytes[3] {};
        juce::uint8 size     = 0;
        double      sendAtMs = 0.0;   // Time::getMillisecondCounterHiRes() clock
    };

    /*  Wall-clock time of the start of the next block, for timestamping queued messages.
        Advances by each block's duration rather than reading the clock every block, so
        notes keep their spacing when the host processes blocks in bursts. Resynced to
        the clock if it falls behind or runs too far ahead. Audio thread only.
    */
    double nextBlockMs     = 0.0;
    bool   midiClockValid  = false;
    static constexpr double maxMidiLeadMs = 500.0;

    static constexpr int midiOutQueueSize = 2048;
    juce::AbstractFifo                      midiOutFifo { midiOutQueueSize };
    std::array<QueuedMidi, midiOutQueueSize> midiOutQueue {};

    struct MidiOutSender : juce::HighResolutionTimer
    {
        explicit MidiOutSender (PolyrhythmProcessor& o) : owner (o) {}
        void hiResTimerCallback() override { owner.drainMidiOutQueue(); }
        PolyrhythmProcessor& owner;
    };

    MidiOutSender midiOutSender { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolyrhythmProcessor)
};
