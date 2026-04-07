#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>
#include <atomic>

static constexpr int MAX_BEATS = 16;

//==============================================================================
class PolyrhythmProcessor : public juce::AudioProcessor
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
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
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
    // Shift all beats on a track by a semitone delta
    void  shiftTrackANotes (int semitones);
    void  shiftTrackBNotes (int semitones);
    float getSwing()            const;
    float getTrackAGate()       const;
    float getTrackBGate()       const;
    float getProbability()      const;

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

    double sampleRateVal  = 44100.0;
    bool   wasPlaying     = false;

    // Pending note-offs that extend past the current audio block
    struct PendingNoteOff
    {
        int samplesRemaining;
        int channel;
        int note;
    };
    std::vector<PendingNoteOff> pendingNoteOffs;

    void processTrack (bool        isTrackA,
                       double      barLengthPpq,
                       double      blockStartPpq,
                       double      blockEndPpq,
                       double      ppqPerSample,
                       int         numSamples,
                       int         gateSamples,
                       juce::MidiBuffer& midiOut);

    juce::Random random;

    // Simple sine-wave voice for built-in audio preview
    struct SineVoice
    {
        double phase  = 0.0;
        double inc    = 0.0;   // phase increment per sample
        float  amp    = 0.0f;
        bool   active = false;
    };
    static constexpr int NUM_VOICES = 16;
    std::array<SineVoice, NUM_VOICES> voices {};

    void triggerVoice (double frequency);
    void renderVoices (juce::AudioBuffer<float>& buffer);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolyrhythmProcessor)
};
