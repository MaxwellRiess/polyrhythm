#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
namespace Theme
{
    const juce::Colour BG          { 0xFF1A1A1A };
    const juce::Colour Panel       { 0xFF222222 };
    const juce::Colour PanelDark   { 0xFF181818 };
    const juce::Colour Accent      { 0xFFFF5500 };
    const juce::Colour AccentDim   { 0xFF803300 };
    const juce::Colour BeatBg      { 0xFF2B2B2B };
    const juce::Colour BeatBgHover { 0xFF363636 };
    const juce::Colour TextPrimary { 0xFFFFFFFF };
    const juce::Colour TextDim     { 0xFF777777 };
    const juce::Colour TextAccent  { 0xFFFF5500 };
    const juce::Colour Border      { 0xFF333333 };
    const juce::Colour Flash       { 0xFFFFFFFF };   // beat-fire pulse colour
    const juce::Colour CutoffBlue  { 0xFF5588CC };
}

//==============================================================================
class BeatPadComponent : public juce::Component
{
public:
    BeatPadComponent (std::atomic<bool>& activeRef, std::atomic<float>& velocityRef,
                      std::atomic<int>& noteRef, std::atomic<float>& gateRef,
                      std::atomic<float>& cutoffRef, int index);

    void paint   (juce::Graphics& g) override;
    void resized () override;
    void mouseDown       (const juce::MouseEvent&) override;
    void mouseEnter      (const juce::MouseEvent&) override;
    void mouseExit       (const juce::MouseEvent&) override;
    void mouseWheelMove  (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // Set flash intensity [0-1] — called by the parent track's timer
    void setFlash (float amount);

    // Sync slider positions from atomic state
    void updateGateSlider();
    void updateCutoffSlider();

    std::function<void()> onChange;

private:
    std::atomic<bool>&  active;
    std::atomic<float>& velocity;
    std::atomic<int>&   note;
    std::atomic<float>& gate;
    std::atomic<float>& cutoff;
    int   idx;
    bool  hovering    = false;
    float flashAmount = 0.0f;
    double lastWheelMs = 0.0;

    juce::Slider gateSlider;
    juce::Slider cutoffSlider;

    void changeNote (int delta);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatPadComponent)
};

//==============================================================================
// Scrollable label — responds to mouse wheel to change a value
class ScrollableNoteLabel : public juce::Component
{
public:
    ScrollableNoteLabel();

    void paint (juce::Graphics& g) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDown (const juce::MouseEvent&) override;

    std::function<void(int delta)> onNoteChange;

    void setNoteName (const juce::String& name);

private:
    juce::String currentNote;
    bool hovering = false;
    double lastWheelMs = 0.0;
    void mouseEnter (const juce::MouseEvent&) override { hovering = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovering = false; repaint(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScrollableNoteLabel)
};

//==============================================================================
// Scrollable sound type selector
class ScrollableSoundLabel : public juce::Component
{
public:
    ScrollableSoundLabel();

    void paint (juce::Graphics& g) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDown (const juce::MouseEvent&) override;

    std::function<void(int delta)> onSoundChange;

    void setSoundName (const juce::String& name);

private:
    juce::String currentSound;
    bool hovering = false;
    void mouseEnter (const juce::MouseEvent&) override { hovering = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovering = false; repaint(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScrollableSoundLabel)
};

//==============================================================================
class RhythmTrackComponent : public juce::Component,
                              public juce::Timer
{
public:
    RhythmTrackComponent (PolyrhythmProcessor& p, bool isTrackA);

    void paint    (juce::Graphics& g) override;
    void resized  () override;
    void timerCallback() override;   // drives flash animation at 30 Hz
    void refreshPads();

private:
    PolyrhythmProcessor& processor;
    bool trackA;

    juce::Label      trackNameLabel;
    juce::Label      channelLabel;

    // Beats control
    juce::Label      beatsLabel;
    juce::Label      beatCountLabel;
    juce::TextButton decBtn { "-" };
    juce::TextButton incBtn { "+" };

    // Note selector (scroll wheel)
    juce::Label          noteLabel;
    ScrollableNoteLabel  noteScroller;

    // Reset notes button
    juce::TextButton     resetNotesBtn;

    // Sound type selector (scroll wheel)
    juce::Label          soundLabel;
    ScrollableSoundLabel soundScroller;

    // Gate slider (track-level: sets all per-beat gates)
    juce::Label  gateLabel;
    juce::Slider gateSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gateAttach;

    std::array<std::unique_ptr<BeatPadComponent>, MAX_BEATS> pads;

    // Flash animation state
    uint64_t lastFireCount = 0;
    float    flashAmount   = 0.0f;
    int      flashBeatIdx  = -1;

    void changeBeatCount (int delta);
    void updateBeatCountLabel();
    void changeNote (int delta);
    void updateNoteLabel();
    void changeSoundType (int delta);
    void updateSoundLabel();
    void resetNotes();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RhythmTrackComponent)
};

//==============================================================================
class LabelledSlider : public juce::Component
{
public:
    LabelledSlider (const juce::String& labelText, juce::Colour fillColour = Theme::Accent);
    void resized() override;
    juce::Slider slider;
private:
    juce::Label  label;
    juce::Colour colour;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabelledSlider)
};

//==============================================================================
class PolyrhythmEditor : public juce::AudioProcessorEditor,
                         public juce::Timer
{
public:
    PolyrhythmEditor (PolyrhythmProcessor&);
    ~PolyrhythmEditor() override;

    void paint   (juce::Graphics&) override;
    void resized () override;
    void timerCallback() override;   // updates BPM display

private:
    PolyrhythmProcessor& audioProcessor;

    // Header — title + live BPM readout
    juce::Label titleLabel;
    juce::Label bpmValueLabel;
    juce::Label bpmUnitLabel;

    // Global controls
    LabelledSlider swingSlider { "SWING" };
    LabelledSlider probSlider  { "PROB"  };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> swingAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> probAttach;

    // Audio preview toggle
    juce::TextButton audioToggleBtn { "AUDIO" };

    // Rhythm tracks
    RhythmTrackComponent trackAComp;
    RhythmTrackComponent trackBComp;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolyrhythmEditor)
};
