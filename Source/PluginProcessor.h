#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>
#include <atomic>

static constexpr int MAX_BEATS = 16;

// Built-in preview sound types
enum class SoundType
{
    Sine = 0,
    Triangle,
    Square,
    Saw,
    Click,
    Noise,
    NumTypes
};

static constexpr int NUM_SOUND_TYPES = (int)SoundType::NumTypes;

inline juce::String soundTypeName (SoundType t)
{
    switch (t)
    {
        case SoundType::Sine:     return "SINE";
        case SoundType::Triangle: return "TRI";
        case SoundType::Square:   return "SQR";
        case SoundType::Saw:      return "SAW";
        case SoundType::Click:    return "CLICK";
        case SoundType::Noise:    return "NOISE";
        default:                  return "?";
    }
}

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
    std::array<std::atomic<float>, MAX_BEATS> trackACutoff;  // per-beat LP cutoff 0-1
    std::array<std::atomic<float>, MAX_BEATS> trackBCutoff;

    // Per-track preview sound type
    std::atomic<int> trackASoundType { (int)SoundType::Sine };
    std::atomic<int> trackBSoundType { (int)SoundType::Triangle };

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

    // Synth voice for built-in audio preview
    struct SynthVoice
    {
        double    phase      = 0.0;
        double    inc        = 0.0;   // phase increment per sample
        float     amp        = 0.0f;
        bool      active     = false;
        SoundType type       = SoundType::Sine;
        int       clickCount = 0;     // samples remaining for click type
        // Simple one-pole low-pass filter state
        float     lpCutoff   = 1.0f;  // 0-1
        float     lpState    = 0.0f;
    };
    static constexpr int NUM_VOICES = 16;
    std::array<SynthVoice, NUM_VOICES> voices {};

    void triggerVoice (double frequency, SoundType type, float cutoff);
    void renderVoices (juce::AudioBuffer<float>& buffer);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolyrhythmProcessor)
};
