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
    const juce::Colour MidiGreen   { 0xFF44CC66 };
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

    // Sync control positions from atomic state
    void updateGateSlider();
    void updateCutoffSlider();
    void refreshNoteDisplay();

    // The editor scales the whole layout with a transform, which leaves child bounds
    // unchanged. The pad needs the scale to judge how big it actually looks on screen.
    void setUiScale (float newScale);

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

    // Per-beat note nudge, matching the web version's pads. Hidden when the pad is too
    // narrow to draw them legibly (high beat counts in a small window).
    juce::TextButton noteDownBtn { "-" };
    juce::TextButton noteUpBtn   { "+" };

    float uiScale = 1.0f;

    /*  Layout tiers. A control is drawn only if it both fits and can be read:

          - raw width decides whether it fits, because the layout is in unscaled
            coordinates and zooming in does not create room for it
          - on-screen width decides whether it is legible, so shrinking the window
            drops controls that have become too small to hit
    */
    float onScreenWidth()   const { return (float) getWidth() * uiScale; }
    bool  showNoteButtons() const { return getWidth() >= 54 && onScreenWidth() >= 44.0f; }
    bool  showSliders()     const { return getWidth() >= 34 && onScreenWidth() >= 28.0f; }

    void changeNote (int delta);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatPadComponent)
};

//==============================================================================
// Scrollable label — responds to mouse wheel and clicks to change a value
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
class RhythmTrackComponent : public juce::Component,
                              public juce::Timer
{
public:
    RhythmTrackComponent (PolyrhythmProcessor& p, bool isTrackA);

    void paint    (juce::Graphics& g) override;
    void resized  () override;
    void timerCallback() override;   // drives flash animation at 30 Hz
    void refreshPads();
    void refreshAll();               // full resync after a global reset
    void setUiScale (float newScale);

private:
    PolyrhythmProcessor& processor;
    bool trackA;

    juce::Label      trackNameLabel;

    // MIDI channel — a button rather than a static badge, so it reflects (and can change)
    // the real parameter instead of always claiming CH 01 / CH 02
    juce::TextButton channelBtn;

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
    void updateChannelLabel();
    void showChannelMenu();
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
// Everything is laid out at a fixed base size inside this component; the editor scales it
// with an AffineTransform. That keeps one set of coordinates while still letting the user
// drag the window to make the controls larger or smaller.
class PolyrhythmContent : public juce::Component,
                          public juce::Timer
{
public:
    explicit PolyrhythmContent (PolyrhythmProcessor&);
    ~PolyrhythmContent() override;

    void paint   (juce::Graphics&) override;
    void resized () override;
    void timerCallback() override;   // updates BPM readout and MIDI device status
    void setUiScale (float newScale);

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

    // Reset everything to defaults, like the web version's RESET transport button
    juce::TextButton resetAllBtn { "RESET" };

    // Emit per-beat cutoff as CC74. Off by default — see PolyrhythmProcessor::sendCutoffCC
    juce::TextButton ccToggleBtn { "CC74" };

    // Direct MIDI output device (e.g. an IAC bus) so any DAW track can record the output
    juce::TextButton midiOutBtn { "MIDI OUT" };
    bool             midiOutConnected = false;

    void showMidiOutMenu();
    void updateMidiOutButton();
    void resetAll();

    // Rhythm tracks
    RhythmTrackComponent trackAComp;
    RhythmTrackComponent trackBComp;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolyrhythmContent)
};

//==============================================================================
class PolyrhythmEditor : public juce::AudioProcessorEditor
{
public:
    PolyrhythmEditor (PolyrhythmProcessor&);
    ~PolyrhythmEditor() override;

    void paint   (juce::Graphics&) override;
    void resized () override;

private:
    PolyrhythmProcessor& audioProcessor;
    PolyrhythmContent    content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolyrhythmEditor)
};
